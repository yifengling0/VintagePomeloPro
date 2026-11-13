# FEX x87 / RichMan8 交接证据（2026-10-06）

先读 [当前完整报告](../../fex-x87-bottleneck-handoff-20261006.md)。本目录将暂停调查时的必要文本证据和实验代码发布到 Git，供下一位模型继续定位；本次发布没有新增真机测试或运行时修复。

## 已证实和仍待验证

RichMan8 主线程首次人物窗口出现约 1024 万次/s 的 x87 helper 回退，重复窗口约 333 万次/s。同进程历史 JIT map 的可见栈把部分回退关联到 `strat game.exe` 和 Miles 的 `mssmp3.asi`。计数与可见调用栈均不能换算成精确 CPU 时间份额。

固定 FEX 版本在 MinGW 下明确禁用 broken 的 `preserve_all`，普通 helper 调用需要更多寄存器保存恢复。这是有源码支持的放大因素，尚未量化为所有 32 位游戏低帧率的根因。不要强行开启该 ABI，也不要降低 x87 精度、TSO、SMC 或画质。

`FEX_EXACTSTORE=1` 候选仅对规范、可精确写回正常 float/double 的 ext80 值走整数快路，其余保留旧 helper；默认关闭，未接入生产构建。367680 个输入、48 组结果的 on/off 校验一致，微基准有改善。第一候选只有一次 off→on 游戏对照；无 FloatDiagnostics 的干净候选只有 on 冷/暖窗口，off 未完成。人物缺块、长片头以及跨游戏性能差距仍未解决。

## 文件对应关系

| 路径 | 用途与限制 |
| --- | --- |
| [main-proton-isolation-20261006](main-proton-isolation-20261006/) | 同包 Box/FEX、浮点计数及诊断补丁；不代表 main 完整架构 A/B |
| [fex-exact-store-20261006](fex-exact-store-20261006/) | 第一候选完整 off/on 人物窗口、native x86 参考；仍编入默认关闭的浮点诊断代码 |
| [fex-exact-clean-20261006](fex-exact-clean-20261006/) | 无 FloatDiagnostics 的候选、probe、GL 汇总、guest 调用归因；游戏只有 on 数据 |
| [fex-softfloat-lto-20261006](fex-softfloat-lto-20261006/) | 前序 ThinLTO/CPU 结果和边界补丁；保留当轮实验限制 |
| [fex-jit-symbols-20261006/symbolize_jit_profile.py](fex-jit-symbols-20261006/symbolize_jit_profile.py) | 调用归因脚本依赖；有独立 `--self-test` |
| [lineage](lineage/) | 实验基线的源码总差异、前置补丁、尚未整合的本地源码快照；不自动应用到生产 |
| [history](history/) | 10 月 5–6 日的图形复制、低地址映射、JIT、文件接口及控制器调查过程；结论以当前报告为准 |
| [provenance.json](provenance.json) | 被复制文件的原始路径、原始 SHA256、筛选后 SHA256 与转换记录 |
| [SHA256SUMS.txt](SHA256SUMS.txt) | 仅覆盖本次实际发布的 evidence 文件，排除清单自身 |

`exact-cpu-current.tsv` 是第一候选当轮 CPU 结果；误取历史数据的 `exact-cpu.tsv` 未发布。FPS 数据只保留 `[GL-PERF]` 行；浮点数据只保留 `[FEX-FLOAT-*]` 行；GL 汇总输入保留 `scope=opengl_unix` 行及前后重叠锚点。筛选规则记录在 provenance 中。宿主 120 帧窗口的最低 FPS 不是点击延迟，也不是 GPU 时间；嵌套 GL 指标不能直接相加。

普通文本统一 LF 并整理尾空白；`.patch` 保留原始字节，便于核对实验补丁身份与重放。补丁中的上下文空行本来就含有一个空格，目录级 `.gitattributes` 仅豁免归档补丁的空白检查，不影响生产源文件。

HAP、DLL/EXE、原始 Hiperf `.data`、截图、完整日志、源码树和构建目录继续保留在本地 `F:/VintagePomelo-Workspace/workspace_temp/`，未进入本次提交。原始调用归因输入哈希在 `fallback-caller-attribution.json`；复跑需取回 `fex-softfloat-lto-20261006/avatars-{cold,warm}-load.data` 及配对的 `*-symbols.map`，放到本目录的同名子目录。禁止跨进程或跨会话混用 JIT map。

## 最短复核路径

以下离线命令只读取已发布数据并重算汇总，不操作平板：

```sh
cd docs/architecture/evidence/fex-x87-20261006
sha256sum -c SHA256SUMS.txt
python3 fex-exact-clean-20261006/validate-probe.py
python3 main-proton-isolation-20261006/summarize-float.py
python3 fex-exact-store-20261006/summarize.py
python3 fex-exact-clean-20261006/summarize.py
python3 fex-exact-clean-20261006/summarize-gl.py
python3 fex-jit-symbols-20261006/symbolize_jit_profile.py --self-test
```

保留的 `build.sh`、`validate-source.py`、`inspect-pe-math.py` 和历史准备脚本记录原实验路径，不是从 evidence 直接启动的完整构建流程；缺失的 HAP、源码或原始采样应先按原路径恢复。设备脚本已去掉序列号，运行前设置 `VP_HDC_TARGET`；坐标、日志日期、游戏路径和 PID 必须按当前现场复核。

## 在独立源码中检查候选

`lineage/fex-experimental-baseline.patch` 是固定上游 `86ff33bbe299cd8959a6610198c169b67ec419db` 到保存的 ThinLTO 实验基线的 **FEX 自身源码总差异**，包含文件接口、JIT 诊断和 native COFF 边界等前置状态。它已经在临时目录重放，并与保存基线的 22 个变化文件逐字节核对；随后重放 clean exact-store，三个变化文件与实际候选一致。External 等 Git 子模块和 `.orig/.rej` 备份不包含在总差异中，依赖子模块需按固定上游记录初始化；本次未重新构建完整依赖树。

前置补丁单独归档用于审阅。**总差异和单项前置补丁选择一条路径，不要叠加应用。** `pending-tracked-source.patch` 是发布时本地尚未提交的宿主/构建变更快照；其中控制器等历史修复只是包基线来源，需单独评审后整合。

以下为后续构建参考，本次未执行。须在 Linux/ext4 权威仓库根目录、配置好 `scripts/env.sh` 的工具链后，使用全新的源码目录和 BUILD_DIR；不要覆盖现有 thirdparty 或复用旧 Wine 对象：

```sh
analysis_root="$PWD/workspace_temp/dot-x87-repro-20261006"
evidence_root="$PWD/docs/architecture/evidence/fex-x87-20261006"
test ! -e "$analysis_root"
git clone --no-hardlinks https://github.com/FEX-Emu/FEX.git "$analysis_root/source"
git -C "$analysis_root/source" checkout --detach 86ff33bbe299cd8959a6610198c169b67ec419db
git -C "$analysis_root/source" submodule update --init --recursive
patch -d "$analysis_root/source" -p1 < "$evidence_root/lineage/fex-experimental-baseline.patch"
patch -d "$analysis_root/source" -p1 < "$evidence_root/fex-exact-clean-20261006/fex-exact-store.patch"
export BUILD_DIR="$analysis_root/build"
test ! -e "$BUILD_DIR"
source scripts/env.sh
export PATH="$LLVM_MINGW/bin:$PATH"
cmake -S "$analysis_root/source" -B "$BUILD_DIR/fex-pe" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$analysis_root/source/Data/CMake/toolchain_mingw.cmake" \
  -DENABLE_LTO=True -DMINGW_TRIPLE=aarch64-w64-mingw32 -DBUILD_TESTING=False
cmake --build "$BUILD_DIR/fex-pe" -j12 --target wow64fex
```

此构建仍默认关闭 exact-store，运行时才用 `FEX_EXACTSTORE=1`。本轮实际安装 DLL 和 HAP 身份在 `fex-exact-clean-20261006/package-identity.json`；不要用仅相同版本号确认实验包。游戏验证仍需先完成 clean off 对照，再做反序、温度匹配和重复测量，最后才考虑整合到生产脚本。下一步优先量化 helper ABI 保存恢复成本以及游戏/MP3 解码热点；缺图需另查最终 draw 的资源内容和裁剪状态。
