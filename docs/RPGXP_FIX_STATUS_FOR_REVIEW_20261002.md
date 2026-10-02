# RPG Maker XP 启动修复：现状、已做修改与存疑清单（2026-10-01 深夜，供专家评审）

> **修订 v4（专家三轮的定向取证已按处方完成，proton.19 单次运行，全部原始数据在此）**
>
> 先更正 v3 的两处口径（专家指出，均接受）：
> - **"旧螺旋消失"是过度声明**。v3 依据的是 p18 私有日志的 905 行截取；本轮
>   取证运行（p19）在同一进程里完整跑到结束：seq905 异常后，**旧风暴立即
>   恢复**（seq906 起 616×addr0 + 125×0x230b38 + 123×0x230cb4 的底页写，
>   合计 1774 次故障）。两版差异（p18 停在 905 / p19 冲到 1774）最可能是
>   诊断代码改变了信号路径的时序竞争结果——**诊断会扰动现象，单次运行的
>   "消失"不可作数**。
> - code=2 不是"写堆失败"的证据（仅= SEGV_ACCERR 访问权限错误）；
>   `result=not_mine` 是路由标记，非致命原因（早前成功继续的故障同样带它）。
>
> **本轮按处方交付的三组证据（p19 运行，`WINEHUA_STEAM_BOUNDARY_TRACE=1`）**：
>
> **E1. seq905 跳变现场（专家要的 PC/PC+4 指令、寄存器、映射、guest EIP）**
> ```
> [FAULT-MIN] seq=905 code=2 addr=0x5c8cb17e pc=0x7ff5a57bb0 lr=0x7ffffe010c
>             sp=0x1001ff2e0  (pc 在 FEXMemJIT; 904→905 pc 偏离 +0x31C 而非 +0x320)
> [SMC-ANOMALY-CODE] instr @pc: ldapurb w20,[x4] ; add w20,w20,w4 ; stlurb w20,[x4] ; ldapurb w20,[x4]
>   （原始码 0x19400094 / 0x0b040294 / 0x19000094 / 0x19400094, llvm 反汇编确认）
> [SMC-ANOMALY-REGS] x0=7ff5a57ba0 x1=7ff5a57b40 x2=4 x3=1001ff268 x4=5c8cb17e x5=101cf910
>   x6=fffff958 x7=655ae8a7 x8=00330114 x9=101cf013 x10=6f0fb866 x11=101cf23d x13=101cf26c x14=110006c80
>   x16=6fffea7584 x17=1001ff240 x19=1001ff320 x20=00655ae8 x21=101cf26c x23=36444d41
>   x24=6ffff92000 x25=14052d580 x27=000006a7 x28=130001140 x29=1001ff3a0 x30=7ffffe010c
> [SMC-ANOMALY-GUEST] cpu_eip=7bd5eb50 cpu_esp=0032fd10 cpu_ebp=00000000
> [SMC-MAP] addr=0x5c8cb17e  101f8000-68000000 ---p      ← PROT_NONE 保留区(1.4GB), 权限错误来源
> [SMC-MAP] pc=0x7ff5a57bb0  7ff57e0000-7ff67e0000 rwxp [anon:FEXMemJIT]
> ```
> 事实：故障指令为 **x4 = 0x5c8cb17e 上的字节加载（LSE acquire 形态，FEX 仿真
> 序列）**；目标落在 PROT_NONE 保留区（不是 DRM 页、不是栈、不是堆数据页）；
> 寄存器里 x13/x21=0x101cf26c 是上一迭代地址、x9=0x101cf013 不变。x4 的
> 生成来源在 pc 之前的 JIT 块里，本轮没采到（下一次可加“跳变前 N 条的
> 块级轨迹”或直接离线反汇编该 JIT 块）。
>
> **E2. SEH 链原始数据（seq906 现场，专家"帧覆盖节点"推演的对照物）**
> ```
> seh0[0032fa88] = 0032fb50, 0032fb50   ← handler 槽=栈地址(垃圾), 链头已异常
> seh1[0032fb50] = 7ac4e7a9, 7ac4e799   ← 该节点两字段都是 0x7a.. 代码区值
> seh2[7ac4e7a9] = ff121286, ffbfbfbf   ← 越界/填充形态
> ```
> 0x32fb50 正是专家指出的"首帧覆盖点"；现在能看到该处两字段已被写成
> 代码区指针（非栈帧结构），head 也从 0x32fb50 移到了 0x32fa88。
> 本轮仍**没有捕获到"从有效值变为 0"的那一次写**（需要在覆盖发生前暂停
> 或记录写前值），此机制仍属未坐实。
>
> **E3. 退出链路（专家要的 status/caller/最后故障序号，已不再缺失）**
> ```
> [EXIT-FAULT] who=abort_thread status=c00000fd tid=31362 last_seq=905
>              last_addr=5c8cb17e last_pc=7ff5a57bb0 caller=0x7f65e64ca4
> [steam-thread] server killed waiting thread
> [EXIT-FAULT] who=abort_thread status=00000001 tid=31355 last_seq=1774
>              last_addr=100100e90 last_pc=6fffeb7eb8 caller=0x7f65e8ddd0
> ```
> 含义：风暴中**另一线程**（31362，win 0b38）先在 32 位栈底不可恢复
> （`unrecoverable thread stack overflow`，status=c00000fd）干净终止；主线程
> （31355）风暴到 seq1774 踩进 64 位栈保护页（last_addr=0x100100e90 位于
> stack64 前两页）而终止（status=1）。启动器侧（hilog）：
> `[ProcReg] complete pid=31355 name=RPGXP.exe exit=1 source=ncp-exit`。
> **进程退出码=1；两条线程终止路径都有明确写手与状态。**
>
> **E4. pad 在私有日志内自证**：`[SMC-MAP] vma_top=0x32f000 00232000-00340000 rwxp`
> —— 栈视图上界（0x340000）高于报告 StackBase（0x330000）恰 0x10000，与
> shared stderr 的 `argv0=...RPGXP.exe pad=0x10000` 决策行互相印证。
>
> **本轮代码改动（proton.20，versionCode 1004021）**：仅一处——wow64/syscall.c
> 终止分支的句柄从 `(HANDLE)0` 改为 `GetCurrentProcess()`（不依赖 NULL 的
> 终止当前进程语义，属代码卫生，该分支至今未被触发过、未验证）。其余
> 同 p19。**未做任何新的"放宽权限/扩大 pad"改动。**
>
> **仍开放的关键问题**：
> 1. x4=0x5c8cb17e 的生成路径（需要跳变前的 JIT 块级轨迹或离线反汇编）。
> 2. 该字节加载为何指向 PROT_NONE 保留区：guest 自身算错 vs FEX 地址生成
>    （两种可能都还不能排除；x2=4、x13=0x101cf26c、x7/x20 等可作重算输入）。
> 3. 风暴的写手已定位在 **FEX-JIT 执行的 32 位 ntdll 派发代码**（无任何
>    `dispatch32 layout` 行 → 从未经过 wow64.dll 的 call_user_exception_dispatcher；
>    即先前在那里的边界修补对本次风暴仍不适用）。若最终要收口风暴，修补点
>    在 guest 可见的 32 位派发/栈增长路径，而非 wow64.dll。
> 4. E2 的覆盖时刻仍未捕获（写前值需要新探针）。
> 5. 建议按专家处方做 p19/p20 的 pad 关/开/关对照（本轮只跑了 pad=开）。
>
> 基线（v1-v3）原文与 patch 0013 明细见下文；**口径以本 v4 为准**。

> **修订 v3（专家二轮意见落实 + proton.18 首次有效验证）**
>
> 专家二轮四点意见全部落实为 proton.18（versionCode 1004019）：
> 1. 帧校验改为"**整个帧 [stack, esp) ⊆ 已提交可写区 [StackLimit, StackBase)**"
>    ——与 host 页大小无关，不再以 DeallocationStack+固定偏移做下界；
>    删除 NtProtectVirtualMemory 路径（它是保护修改而非提交，且信号上下文
>    成败不可控）；低于 StackLimit 一律按不可恢复栈溢出干净终止。
> 2. 新增**同次异常布局日志**（每进程前 8 次 dispatch32 打印：最终选用 esp
>    及来源、ctx.Esp、SystemReserved1[0]、deal/limit/base、frame/rec/context
>    地址、frame_size、context_length、seh_head、code/eip）——用于坐实覆盖
>    SEH 节点的是哪个写操作（专家地址推演指出 ExceptionFlags 假说对不上：
>    ESP=0x32fd10→flags@0x32f9dc、ESP=0x330004→flags@0x32fcd0，均非 0x32fb54）。
> 3. 诊断读取全部改经 `ohos_smc_read_stack()`（process_vm_readv 内核中介，
>    坏页安全失败）：cpu_eip 指令字节、SEH 链逐节点——杜绝诊断自身递归。
> 4. 关于"Steam 卡住"：不再声称与修改无关；已确认 proton.17/18 两个会话
>    Steam 未完成启动期间，新代码**零触发**（无 dispatch32 layout 行），但
>    p16/p18 同条件对照仍待做。
>
> **proton.18 首次有效验证（应用重启后的新会话，pad 决策日志实证）**：
> - `argv0=C:\...\RPGXP.exe argv1=(null) pad=0x10000` —— **pad 经 argv[0]
>   门控首次真正生效**（专家判断正确：栈分配前 exe 在 argv[0]）。
> - Steam 拉起的游戏进程（pid 10899）：**旧死亡螺旋完全消失** —— 905 次
>   fault 全部为解密期静默 SMC 写（0x101cf2xx 逆序），无 0x330000 首发越界、
>   无 615 次地址 0 执行风暴、无 0x230ad4/0x230c50 底页递归、**零异常分发**
>   （dispatch32 layout 一次未打）。
> - 修正后的 CPU 区读数正常：`cpu_esp=0032fd10`（握手后 Esp）、
>   `cpu_eip=7bd5eb50`（32 位 ntdll 真实代码，bytes=55 89 e5 56 83 ec 18...
>   合理的函数序言）——进一步佐证 v1 的"字段错位"是诊断假象。
> - **新前沿**：进程存活到解密接近完成处，终止于 seq 905 —— 解密器向
>   `0x5c8cb17e`（约 1.5GB，疑似堆区）写入 fault，pc 仍在 JIT、x9 仍为
>   0x101cf013（解密源指针），随后 abort-thread，期间**没有任何 wow64 异常
>   分发**。这是下一个需要定位的现场（该写的性质、页面归属、为何未被
>   静默解决也未分发）。
> - 对照数据：直启路径（env 未传、pad=0）的游戏进程 11452 与历史 7 次
>   pad-off 运行同环境，无私有日志可比；严格的 pad 关/开/关 A/B 与
>   p16/p18 Steam 卡住对照仍待执行。
>
> 当前最准确的结论：诊断错误与异常分发缺陷已纠正并**部分验证**（pad 生效、
> 旧螺旋消除为 proton.18 单次运行实证，待复跑确认）；首发 AV 随 pad 生效而
> 未再出现，但其根源（guest 首压栈为何落在栈顶之上）仍未解释；SEH 覆盖的
> 精确机制因零分发而未获得直接证据；新前沿 0x5c8cb17e 待查。

## proton.18 相对 proton.17 的修改（专家二轮意见）

见顶部修订 v3 第 1-3 条；补丁仍为 `patches/wine/0013-*.patch`（未提交 git）。

---
（以下 v2/v1 原文供对照，结论以顶部修订为准）
> - "CPU 区字段被写乱（Eip=Esp 值/Esp=0）"——是我方诊断代码把 CPU 区基址
>   多偏了 12 字节（手写 16 对齐，正确应为 `get_cpu_area()` 的
>   TYPE_ALIGNMENT(I386_CONTEXT)=4 对齐），真实 Esp=0x32FD10 本来就正确。
> - "SegSs=0x2B 异常"——ARM64 WOW64 初始化本就使用该值，应与同现场
>   ss32_sel 比较；原叙事不成立（但候选目标验证修复本身仍正确且有效）。
> - "首发 AV 是压栈"——仅凭重建出的 ESP 不足以判定，需指令字节等现场
> （proton.17 已加 cpu_eip 处 16 字节指令转储）。
> - "NtProtectVirtualMemory 在嵌套信号中不生效"——真正原因是帧目标边界
> 错误：把 DeallocationStack 当合法下界，漏掉首页不可访问的事实，
> 0x230ad4（距起点仅 0xad4）被错误放行；且未检查 API 返回值。均已修。

## proton.17（versionCode 1004018，本轮最新）相对 proton.16 的修改

1. `ohos_virtual.c`：CPU 区读取改用 `get_cpu_area(IMAGE_FILE_MACHINE_I386)`
   （修正 12 字节偏移）；新增 cpu_eip 处 16 字节指令转储；新增 SEH 链有界
   遍历（≤16 节点，逐节点打印 [next,handler]，链向低地址走即停）——直接
   验证"帧覆盖 SEH 节点"机制在现场的发生过程。
2. `wow64/syscall.c`：合法帧范围下界改为 DeallocationStack+0x1000（首页
   不可访问）；`NtProtectVirtualMemory` 返回值检查，失败即 ERR+终止，
   成功才更新 StackLimit。
3. `thread.c`：pad 门控匹配 `main_argv[0]`（原 [1]，专家指出栈分配前 argv
   已重建、exe 在 [0]——决策日志已实测确认 argv0=完整 exe 路径）；新增
   首次分配时的决策日志 `[thread] WineHua wow stack pad: env= argv0= argv1= pad=`。

## proton.17 验证状态：被 Steam 会话自身卡住（未完成）

部署后两次 want（含一次补发）约 15 分钟内 Steam 未完成启动/未处理
-applaunch（relay steam.exe 正常退出、webhelper renderer 正常拉起、所有
Steam 进程 pad=0 即行为未变——与 proton.13/15 的 pad 影响不同，本次是
登录/启动时序问题）。**游戏进程未拉起，修复效果未验证**。复跑方式：
Steam 就绪后重发 want（命令同文档末尾，env 含
WINEHUA_WOW_STACK_TOP_PAD=RPGXP），然后
`hdc file recv -b com.vintage.pomelopro /data/storage/el2/base/temp/wine-native-<pid>.log`。
观察点：①`wow stack pad` 行中 RPGXP.exe 是否 pad=0x10000；②0x330000 首发
fault 是否消失；③SEH 链遍历输出是否出现 handler=0 节点及其位置；④若递归
仍发生，应看到新的 "no dispatch frame target above guard page" 或
"failed to commit dispatch frame pages" ERR（单次干净终止替代 252 层递归）。

---
（以下为 v1 原文，供对照；结论以上方修订为准）

## 一句话现状

**游戏仍无法启动**（Steam 正常拉起 RPGXP.exe，~4 秒后退出，编辑器未出现）。已修掉 wow64 异常分发层的三个确定性缺陷（消除了"1712 层无限递归"中最恶劣的部分），但**首发 AV 的根因（guest 以 esp=栈顶+0x14 运行首个压栈）尚未定位**；针对它的栈顶容忍页方案因影响 Steam 全家而经历三次门控迭代，最新 exe 名门控版已部署但**未验证生效**。

## 已确认的事实链（全部有日志证据）

三次基线运行（proton.7×2、proton.8×2）+ 五轮修复验证（proton.9-16）：

1. **引擎确认是 FEX**（`starting FEX based libwow64fex.dll`、`HODLL=libwow64fex.dll`）。"wow64" 是 Wine 新 wow64 架构必需的 32 位分发层（wow64.dll），FEX 以 wow64cpu 身份插在其下，不是引擎选错。
2. **壳自解密阶段正常**：RPGXP.exe（SteamStub 打包，ImageBase=0x400000、入口在 .bind、运行时被重定位到 0x10000000）就地解密的数百次写 RX 页 fault 均被 Wine SMC 机制静默修复（每次 fault 后跟 WX-MPROTECT）。
3. **首发 AV（未解决）**：seq 837，guest JIT 指令写 0x330000。0x330000 恰为 32 位栈视图 [0x230000, 0x330000) 顶界的下一字节；FEX 重建的 32 位上下文显示 esp=0x330004=StackBase+4、ebp=0x101cf013（解密指针）→ 判读为"esp 比栈顶高 0x14 处的首次压栈"（**此判读依赖 FEX 重建上下文的正确性，是存疑点**）。
4. **SEH 链本身健康**：首发时 `seh_head=0x32fb50, next=0xffffffff, handler=0x7bd733d0`（wine builtin）；更晚的现场显示游戏自己的 handler（0x7ab2b399）。"链损坏/NULL handler 链头"的早期假设已被否定。
5. **风暴传播**：首发 AV 分发后，32 位 SEH walk 过程中反复出现 `calling handler at 00000000`（signal_i386.c 帧链 walk 打印）→ 在地址 0 执行 → 新 AV → 再分发，615 次，每次消耗 guest 栈约 0x6A0，ebp 从 0x32f990 降到 0x2315d0。
6. **递归放大器（已修）**：guest 栈耗尽后，异常分发帧写入栈底 PROT_NONE 页（vprot=0、无 GUARD），guest ESP 不变 → 每次写 fault → 无限嵌套（256 层）→ native 64 位栈溢出 → abort_thread。
7. **CPU 区怪象（未解）**：解密期间 wow CPU 区（I386_CONTEXT 交换区）内容为 `Eip=0x32FD10、Esp=0`。0x32FD10 恰是 wow64 thread_init 推入 0xdeadbabe 握手标记的槽地址（=握手完成后的 Esp 值）。即 **Eip 槽收到了 Esp 的值、Esp 槽为 0**——像一次字段错位/混淆的写入。已核对 mingw-w64 与 Wine 的 WOW64_CONTEXT 布局逐字段一致（Eip@0xB8、Esp@0xC4），排除头文件错位。
8. **Steam 对地址空间布局变化敏感**：栈顶 pad 对 Steam 全家生效时（proton.13 无条件版、proton.15 env=1 修复条件后），Steam 无法完成启动/处理 -applaunch（15 分钟无游戏拉起）；pad 不生效时（proton.14，因主线程 reserve_size==0 条件 bug）Steam 正常。另外 SteamService.exe 反复重启在旧会话（proton.7 时代）就存在（旧日志 158 次 spawn），是否与 pad 相关未定。

## 已做修改清单（patch 0013，均已登记 scripts/build_wine.sh，wine 子模块 dirty 未提交 git）

### A. dlls/wow64/syscall.c — call_user_exception_dispatcher（i386 分支）【核心修复】

- 原缺陷 1（确定）：`esp = SegSs != ss32_sel ? TEB32->SystemReserved1[0] : ctx.Esp` —— FEX 重建上下文 SegSs 常为 0x2B（数据段），fallback 字段在 wow64 下为 0，`esp - 帧大小` 下溢回绕（实测 0xfffffb44），异常帧写野地址 → 无限嵌套。
- 修改：候选目标验证——依次试 ctx.Esp、SystemReserved1[0]，凡算出的帧目标落在 [DeallocationStack, StackBase) 内即采用；都不行则 ERR + NtTerminateProcess(STATUS_STACK_OVERFLOW)；目标低于 StackLimit 时 NtProtectVirtualMemory 提交并更新 StackLimit。
- 验证结论：**回绕消除（proton.11/12 对比 proton.10 确认）**；但 252 次 0x230ad4 底层递归仍在 → **NtProtectVirtualMemory 在嵌套信号上下文（PE wow64 信号内）疑似不生效，终止分支从未触发**。这两点是存疑修改。

### B. dlls/ntdll/unix/virtual.c — virtual_setup_exception 增长后可写性校验

- 逻辑镜像同文件 last-page 不可恢复分支。**实际未命中任何路径**（递归写入者走 A 路径、不经过本函数）——保留无害，但属于"没打中的修补"。

### C. dlls/ntdll/unix/ohos_virtual.c — 诊断增强（确定性有用）

- `[WOW-TEB]`：每进程前 16 次 SIGSEGV 现场打印 wow TEB 三元组 + 32 位 ExceptionList 链头两字 + wow CPU 区实时 Eip/Esp（本条产出了事实 4/7）。
- 独立 8 次配额的三类死亡螺旋现场触发器：写 StackBase 之上（首发）、地址 0 执行（风暴）、写 [DeallocationStack, StackLimit)（递归放大，附 pc/lr maps 归属）。

### D. dlls/ntdll/unix/thread.c + virtual.c + unix_private.h — 32 位栈顶容忍页【三次迭代，当前未验证】

思路：多保留一个 64K 粒度、整体已提交，报告给 guest 的 StackBase/初始 Esp 不变，让"栈顶上方的首次压栈"落在可写页。

| 版本 | 门控 | 结果 |
| --- | --- | --- |
| proton.13 | 无条件 | **确认有害**：Steam 无法处理 -applaunch，回退 |
| proton.14 | env=1 | pad 未生效（我方条件 bug：主线程 reserve_size==0 被排除），Steam 正常，游戏照旧崩 |
| proton.15 | env=1 + 修条件（virtual_alloc_thread_stack 新增 top_pad 参数） | pad 生效但对 Steam 全家生效 → **Steam 又卡死** |
| proton.16 | env 值为主程序路径子串匹配（=RPGXP，大小写不敏感；"1"=无条件） | 代码已确认在包内（strings 验证）；Steam 正常拉起游戏（pid 30093），**但 0x330000 fault 仍存在 → pad 在游戏进程仍未生效，原因未明** |

virtual_alloc_thread_stack 签名从 6 参改为 7 参（+top_pad），thread.c 四处调用点已同步。

## 关键未解问题（建议专家按此优先级）

1. **Q1 guest esp=StackBase+0x14 的来源**（根因）。候选：32 位启动握手（wow64/syscall.c `thread_init` 的 5 个 0xdeadbabe 握手 dword → 32 位 LdrInitializeThunk → signal_i386.c `signal_start_thread`/`NtContinue`）在 FEX 翻译下的行为；或 FEX SRA/上下文重建错误（现场 ebp=解密指针本身就可疑）。注意 PAL2/PAL4 正常运行——同为 32 位却无此问题，差异点未知。
2. **Q2 CPU 区 {Eip=Esp值, Esp=0} 的写入者**。FEX 的 BTCpuSetContext 走 RtlWow64SetThreadContext（wine 实现，按字段名复制，不可能错位）——是谁以错位方式直写了 CPU 区？（也可能是"CPU 区本就该过期、该值另有含义"——但 Eip=握手标记槽地址过于巧合。）
3. **Q3 proton.16 exe 名门控为何未生效**。loader.c:2401 处已在用 main_argv[1]、2548 处 init_thread_stack 理论可见；需确认主线程栈分配时 main_argv 是否已赋值（loader.c:2641 的赋值在后面，但 2401 的使用说明另有早期赋值路径 env.c:601）。若 pad 实际生效而 0x330000 写仍在 → **esp 压栈判读本身错误**（该写可能是解密循环对某缓冲区的写，esp=0x330004 是 FEX 重建噪声）——Q1 需重评。
4. **Q4 `calling handler at 00000000` 的 NULL handler 帧从哪来**（帧链 walk 中的某个帧 Handler=0；链头有效）。候选：分发自身压入的哨兵帧被调用 / 壳安装的帧在其 handler 代码尚未解密时被提前触发（Windows 上无此窗口，因为解密期间不产生 AV）。
5. **Q5 pad 全局生效时 Steam 卡死的机制**（地址空间布局变化的哪个具体差异破坏 Steam 启动）。

## 版本与设备状态

- 设备：MatePad（USB 5KPBB25818203996），当前安装 **proton.16**（versionCode 1004017），应用运行中（Steam 已登录，游戏拉起即崩）。prefix/登录态/共享目录完好（appmanifest StateFlags=4）。
- 分支：`feature/main_proton`（WSL /home/liufeng/src/vpp-proton；本轮 wine 侧修改全部未提交，patch 文件已入库登记）。
- 复现入口：want 启动 `steam.exe -applaunch 235900`（命令见 commands.jsonl / 此前报告）；游戏私有日志 `hdc file recv -b com.vintage.pomelopro /data/storage/el2/base/temp/wine-native-<pid>.log`。

## 证据索引（本轮新增）

- `p9run/wine-native-{25776,40702? 详见下表}.log` 等各轮游戏进程日志；`p16chk/ntdll.so`（proton.16 包内提取物，strings 已验证含新代码）。
- 各轮构建日志 `build/host-tests/sign-proton{9..16}.log`、HAP 与 SHA256SUMS。
- 基线分析：`RPGXP_CRASH_CONFIRMED_20261001.md`（崩溃链）；`steam-rpgxp-32045.log`（proton.7 基线）；`wine-stderr-steam-shared.log`（86MB，含 trace+seh 的 guest 上下文）。

| 轮次 | HAP | 游戏进程 pid | 现象要点 |
| --- | --- | --- | --- |
| proton.8 基线 | (交接时已装) | 40702/41783 | 1712 faults：615×addr0 + 256×0x230ad4/0x230c50 + 栈底对 |
| proton.9 | 仅诊断 | 25776 | 同基线（确认 [WOW-TEB] 链头有效） |
| proton.10 | esp 视图内校验 | 64619 | 回绕消除；615 风暴 + 新 255×0xfffffb44 回绕递归 |
| proton.11 | 候选目标验证 | 37061 | 回绕消除；风暴+底页递归恢复 252×0x230ad4 |
| proton.12 | +cpu Eip/Esp | 5588 | 同上；拿到 CPU 区 Eip=0x32FD10/Esp=0 |
| proton.13 | pad 无条件 | （Steam 卡死未拉起） | Steam 15 分钟不处理 -applaunch |
| proton.14 | pad env=1（条件bug未生效） | 28343 | Steam 正常；游戏 1708 faults 照旧 |
| proton.15 | pad env=1 生效 | （Steam 卡死未拉起） | 确认 pad 影响Steam |
| proton.16 | pad 按 exe 名 | 30093 | Steam 正常拉起游戏；0x330000 fault 仍在（pad 未生效原因待查） |

## 给专家的最短路径

1. 看 `p9run/wine-native-30093.log`（最新）+ `steam-rpgxp-32045.log`（基线）的 `[WOW-TEB]`/`[FAULT-MIN]` 序列。
2. 读 wine: `dlls/wow64/syscall.c`（call_user_exception_dispatcher、thread_init）、`dlls/ntdll/unix/signal_arm64.c`（init_syscall_frame i386 分支）、`signal_i386.c`（32 位 LdrInitializeThunk/signal_start_thread）；FEX: `Source/Windows/WOW64/Module.cpp`（Context namespace、BTCpuSet/GetContext）。
3. 存疑修改就是本文"已做修改清单"A 的提交/终止分支、D 全部——欢迎推翻，patch 0013 可整体或部分回退（幂等登记，`patch -R` 可逆）。
