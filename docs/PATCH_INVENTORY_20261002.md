# 补丁与改动总清单（2026-10-02 第三版，提交前对抗性自查后）

## 0. Git 现状

- 分支 **feature/main_proton**（不是 main）；上一条交付提交 `b3377b8b`；本次在其上新增
  一条**暂存提交**（三组补丁 + 构建注册 + 评审文档，见 `git log` 顶部）。main 未被触碰。
- 本次提交内容：
  - `patches/wine/0013a-…diagnostics.patch`、`0013b-…frame-guard.patch`、`0013c-…stack-top-pad.patch`；
  - `scripts/build_wine.sh`（幂等注册三行）；`AppScope/app.json5`（proton.22 / 1004023）；
  - `docs/PATCH_INVENTORY_20261002.md`、`docs/RPGXP_FIX_STATUS_FOR_REVIEW_20261002.md`；
  - wine 子模块**保持 gitlink**（不提交子模块；其 dirty 8 文件由 0011/0012/0013a/b/c 完整复现）。
- **全链反向校验**：12 个注册补丁（0001,0002,0006-0012,0013a/b/c）逐一 `patch -R --dry-run` 全部 OK
  → 补丁集合 == 当前 dirty 树，可精确重放/回退。（本轮曾发现 0013a 初版把新函数插进
  0002 的 hunk 上下文区导致其反向校验失败——已把 `ohos_smc_read_mem` 挪到
  `ohos_smc_dump_stack` 之后并重生成补丁，全链恢复 OK。此坑已在代码注释中标注。）
- 恢复快照（含被替换的合并版 0013）：`staging-snapshot/`。
- 设备：已装 **proton.22**（versionCode 1004023，SHA256 `ac1797db…98d73`；
  proton.21 已被本轮自查修复替换）。

## 1. Wine 补丁链（build_wine.sh 注册、幂等应用）

| 编号 | 主题 | 来源 | 验证状态 | 建议 |
| --- | --- | --- | --- | --- |
| 0001 | win32u 外部字体注册修复 | 交接前基线 | 已随交付验证 | 保留 |
| 0002 | ntdll 信号安全栈读取 | 交接前基线 | 已随交付验证 | 保留（注意其 hunk 上下文区，勿贴边插码） |
| 0006–0010 | 桌面/窗口/Wayland/WSI 等 5 项 | 交接前基线 | 已随交付验证 | 保留 |
| 0011 | wineboot 持久化 prefix 完成握手 | 交接交付 | 真机冷启动已验 | 保留 |
| 0012 | wineserver 锁内注册表迁移 | 交接交付 | 真机已验 | 保留 |
| **0013a** | RPGXP 取证诊断组 | 本会话 | 已产生 E1–E4；**会扰动时序** | 暂存；合并前加运行期开关 |
| **0013b** | wow64 分发帧候选校验 | 本会话 | **从未触发，未验证**；含"立即终止"策略 | 暂存；终止语义需单独设计/测试 |
| **0013c** | wow32 栈顶容忍页 pad | 本会话 | 生效过但收益未证明；无条件时有破坏史 | **隔离，不并入**；撤除见 #4 |

## 2. 三包明细与专家三条意见的落实

### 0013a — ntdll/ohos RPGXP 取证诊断（仅日志）
内容：[WOW-TEB]（栈界+vma_top pad 自证+CPU 区+SEH 链有界遍历）、[SMC-ANOMALY]（跳出解密族
的首个故障：精确指令字节/全寄存器/guest EIP/映射）、[EXIT-FAULT]（abort_thread 回带
status/caller/tid/last_seq）。
- **专家意见落实（8 字节对齐）**：`ohos_smc_read_stack()` 会把地址向下按 8 对齐，用它读
  未对齐 EIP 指令会整体偏移且不报警。已新增 **`ohos_smc_read_mem()`（地址原样、返回实际
  字节数）**，所有指令字节与 SEH 节点读取改走它（初版 p19 的 pc=…7bb0/7e02b0 恰好 8 对齐
  所以结果正确，但属侥幸）。
- 风险：探针对 SIGSEGV 路径有时序扰动（p18/p19 结果差异即为证据）。用途=取证；上游化前必须加开关。

### 0013b — wow64 分发帧候选校验（含“立即终止”）
内容：候选 esp 验证（帧 ⊆ [StackLimit, StackBase)），失败则 `GetCurrentProcess()` +
STATUS_STACK_OVERFLOW。
- **专家意见落实（行为定性）**：句柄替换**不是**无影响改动——`(HANDLE)0` 时
  NtTerminateProcess 失败返回、代码继续写帧（旧行为）；伪句柄则立即终止进程。该分支
  至今零触发；作为“修复”提交前必须按“异常帧不可写 ⇒ 立即终止进程”单独设计语义并测试
  （何时可以杀进程/退出码/debugger 与客户端交互），或整体移除。代码注释已按此改写。
- 风险：未验证 + 立即终止语义未设计。默认处置：隔离。

### 0013c — wow32 栈顶容忍页 pad（EXPERIMENTAL）
内容：32 位栈多保留 64K 已提交页并把报告 StackBase 回调；env 门控
`WINEHUA_WOW_STACK_TOP_PAD`（"1" 或按 argv[0] 子串）。
- **专家意见落实（撤除完整性）**：top_pad 参数（`virtual_alloc_thread_stack` 第 7 参）、
  4 处调用点、`unix_private.h` 声明全部只在该补丁内 → **撤除 = 删本补丁文件 + build_wine.sh
  一行**，不留残余管线。
- 风险：收益未证明（p19 风暴照旧）；无条件启用曾使 Steam 无法启动。默认处置：隔离。

### 已移出的改动（专家意见落实）
- **`virtual_setup_exception` 可写性校验已从树中还原**（未命中任何路径的通用栈处理改动，
  避免扩大到其他程序；本清单上一版的该项作废）。对应地，本轮 diff 只含上列三包。

## 3. 非 wine 改动

| 改动 | 文件 | 性质 | 处置 |
| --- | --- | --- | --- |
| 版本号 → 1004022 / 1.4.5-proton.21 | AppScope/app.json5 | 本地真机迭代计数 | 合并前按主线版本策略清理 |
| 注册 0013a/b/c | scripts/build_wine.sh | 构建接线 | 与三包去留绑定（pad 撤除=删 0013c 行） |
| pal4-build-proton9..21.sh / want / hdc 脚本 | workspace_temp（仓库外） | 本地工具 | 不涉及 |

## 4. 撤除 pad 的标准动作（专家要求）

```bash
# 1) 从构建链摘除
编辑 scripts/build_wine.sh：删除 0013c 两行（注释 + ensure_wine_patch）
# 2) 删除补丁文件（已快照于 staging-snapshot/，需要实验时可取回）
rm patches/wine/0013c-ntdll-wow32-stack-top-pad.patch
# 3) 从工作树还原 pad 代码（top_pad 参数/调用点/声明一并消失）
cd thirdparty/wine-valve
patch -p1 --batch -R < /path/to/staging-snapshot/0013c-ntdll-wow32-stack-top-pad.patch
# 4) 重建验证（全链反向校验应继续 OK，因为其它补丁不依赖它）
```

## 5. 三种“暂存”操作（按需选择，当前维持 A）

- **A（现状，推荐）**：不提交；三包独立、全链反向校验通过、快照在手。
- **B（继续开发）**：把 0013a（+可选 EXIT-FAULT 单列）提交到 feature/main_proton 并标注验证状态；0013b/c 留工作树或独立分支。
- **C（彻底回退工作树，仅留补丁）**：
  ```bash
  cd /home/liufeng/src/vpp-proton
  git -C thirdparty/wine-valve checkout -- .
  git checkout -- AppScope/app.json5 scripts/build_wine.sh
  # 三包保留在 patches/wine/，下次构建自动重放
  ```

## 6. 一句话结论

主线安全区 = 0001–0012。本轮 0013a/b/c 全部定性为**暂存件**：
0013a 是取证工具（有用但需加开关）；0013b 未触发、其“立即终止”语义待专门设计；
0013c 收益未证明、随时可整体撤除（撤除配方见 #4）。三者均不进 main。

## 7. 提交前对抗性自查（本轮新增，proton.22）

### 已发现并修复（3 项）

| # | 问题 | 性质 | 处置 |
| --- | --- | --- | --- |
| S1 | **诊断被 WINEDEBUG=-all 静默吞掉**：实测 p19 游戏私日志 `err:` 计为 0（p8 时代还能出），而 0013b 的 `dispatch32 layout` / `unrecoverable` 用的是 ERR 通道 → “等它触发就能看到”的承诺失效 | 我方诊断自身缺陷 | 既已改 `__wine_dbg_output` 直写（与 FEX 启动横幅同机制、不受 WINEDEBUG 门控），`_vsnprintf`(ntdll 导出) 组装 |
| S2 | **异常快照的 maps 重读无并发保护**：`ohos_dump_anomaly` 在 `ohos_smc_log_enter` 保护区之外裸调 `ohos_smc_reload_maps()`，与其它信号处理器的 maps 缓存读并行 → 缓存撕裂 | 我方诊断自身缺陷 | 增加 `ohos_maps_busy` test-and-set 串行化；拿不到则跳过归属行 |
| S3 | **va_list 误用**（S1 修复自查中抓到）：`_snprintf(buf, n, fmt, ap)` 把 va_list 当普通变参传 → 未定义 | 我方代码 bug | 改用 `_vsnprintf`；截断时显式端点置零 |

### 未修复、留给专家的疑点清单（按风险降序）

1. **0013b 的 frame_size 语义未经实证**：`offsetof+context_length`（上游 asm 版用的是
   `max(0x5c0, context_ex->All.Length)` 保守值）。RtlGetExtendedContextLength 在
   FEX/该配置下返回什么、与后续 `RtlInitializeExtendedContext` 实际写入范围是否一致，
   无运行证据（该路径零触发）。
2. **0013b 终止后仍会继续执行**：`NtTerminateProcess(GetCurrentProcess(),…)` 预期不返回；
   但一旦返回（任何原因），代码会继续向已验证失败的帧目标写入 → 回到嵌套递归。
   建议专家评估在终止调用后加 `__builtin_unreachable`/死循环，或把该分支整体移除。
3. **诊断扰动时序**：p18(905 fault) 与 p19(1774 fault) 同代码不同结果，探针改变信号路径
   时序是已知副作用——据此对比行为差异时必须以“无探针构建”为基准。
4. **0013a 与 0002 存在补丁级依赖**：`struct iovec` 的 `#include <sys/uio.h>` 由 0002 提供，
   0013a 自身不含之；单独应用 0013a 到干净树会编不过（组合应用无碍）。
5. **pad 的线程一致性**：`main_argv` 在极早期线程（如 LdrInitializeThunk 创建的
   apc_worker）栈分配时可能尚未就绪 → 同一进程内可能主线程有 pad、早期线程没有；
   决策日志只打第一份，混态不可见。影响面小（布局偏移），但属未验证行为。
6. **异常启发式偏游戏专用**：`decrypt_family>=64 且 addr 超出 [0x10000000,0x11000000)`
   针对 SteamStub 形态；其它进程理论上可误触（配额 4 兜底，仅多几行日志）。
7. **pad 语义本身**：报告 StackBase 之上的 64K 是“已提交但不在栈视图内”的窗口
   （guest 越界写会静默成功）；env 门控 + 随时可撤，但若保留需在文档中说明该语义。
8. **风暴写手在 guest 侧**：本轮零 `dispatch32 layout` 行证明风暴完全发生在
   FEX-JIT 执行的 32 位 ntdll 派发代码里——**0013b 不在该故障链上**（对 RPGXP
   现状无作用）；若未来要收口风暴，修补点在 guest 可见的 32 位派发/栈增长路径。
   这不算缺陷，但决定了 0013b 的“修复”名分不成立，只应作防御性改动评估。
