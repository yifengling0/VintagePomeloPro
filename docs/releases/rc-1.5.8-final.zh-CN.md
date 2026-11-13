# rc-1.5.8-final 收口记录

日期：2026-11-12。此标签保存最后一个保留 Linux Steam 的 VPP 源码节点。后续产品路线收敛到 OHOS Wine、Windows Steam 与 HmosDeck；完整 Linux 支持由 HSL 承担。

## 冻结源码

- 标签 `rc-1.5.8-final` 指向 `67ea17f72d0b8a4d25eedcd75d3a7b837cd3ffbf`，已推送 origin。
- 当前产品代码已在 `main_proton_steamarm64` 提交推送；标签直接指向已实际构建安装的提交，不为收口伪造功能改动。
- 新分支 `feature/v2-ohos-wine-winsteam` 从标签建立。本次仅新增规划/验收文档，产品仍为 `1.5.8 / 1005080`，未开始 Linux 拆除或 2.0 改号。
- 已用 Git ancestry 验证 `rc-1.5.7`、`425a4088`、`6b967cee`、`8da32e9c` 在冻结提交历史内。保留 Windows Steam 受控启动、库低性能设置、缓存 CM 限时与 helper 升级、默认软件 CEF，以及后续输入/窗口/生命周期修复。
- [来源清单](evidence/rc-1.5.8-final-source.json) 固定 73 个子模块身份、168 个 patch SHA-256；本次逐个重查 patch，全部匹配。其 releaseTag 为空是因为原清单生成于打标签前，保留原记录。

## 本地与真机证据

本地独立 source-integrity 检出 HEAD 同为 `67ea17f7`。完整 `make test-game-runtime` PASS，生产组件整组来源检查、定向 ASan/UBSan 路径测试及 HAP 构建已通过；它们不代替真机游戏与支付验收。

| 成果 | 身份 |
| --- | --- |
| unsigned HAP | 878792571 bytes；SHA-256 `bfdfe83f2bab21de16bb07a2d55be402ea1901445756569ba709f884572a88af` |
| Mini 签名 HAP | 881774486 bytes；SHA-256 `c7b4ad276ce5c2c66ff45ef41572846ab45c5489b4e346e9a3dfa2c0c142ce39` |
| 签名检查 | release/internaltesting；profile/证书、签名、payload inventory PASS；不等于 AppGallery 发布验证 |
| 完整回归日志 | 20446 bytes；SHA-256 `6e3a40e9fe1c8db3db5db7243e61fc6b3aa78e9a0dda7614469c18306bcc6313` |

USB Mini 已覆盖安装上述 67 包并保留数据。全新卸载/重新下载组件的操作发生在本轮较早版本，随后增量更新到 67，不能称 67 再次完全清空安装。

- Linux Steam 登录/主库的系统标题、状态/导航栏已隐藏，宿主区域为 2560×1600；Steam 自己的菜单仍属于客户端界面。
- 仙剑4：普通 Steam 库 Play，动态菜单、新游戏片头多场景视频、实时3D剧情与中文、Enter 推进、Home→HmosDeck→继续恢复均短测通过；HmosDeck 关闭后游戏窗口/会话消失，Steam 保留。未打开旧存档，真实退出码未知，不记 exit0。
- 仙剑1：67 的 Wine 准备完成后遇到 Steam Cloud Out of Date，取消，没有覆盖冲突。95ff 的片头/新故事历史结果不替代最新67完整启动验收。
- Banana：67 普通库启动进入主界面；Unity x86_64/FEX，OpenGL 初始化失败后 Vulkan/FEX thunks/Venus/Maleoon910 初始化成功。实际 ELF 文件头未直接读取。游戏内 Quit 已进入 Unity shutdown callbacks，但停在退出阶段，需强退；根因未定。
- 尖塔2由用户延期。本次没有用 Linux 成绩替代 Windows 版或完整游玩验收。

临时 Banana Launch Options 已从 FEX GDB/日志参数恢复为原先空值，UI 已确认；诊断游戏已结束，无 GDB socket。正常 Steam Exit 后 HmosDeck 运行列表为空。Wine 的空闲宿主子进程仍可见，未宣称整组运行库全部退出。原日志包含账号数据，仅保留本地有界记录，不公开原日志/截图。

## 已知问题与 CI

- Banana 正常 Quit/close 卡住、历史间歇 Steam signal11/启动失败和真实退出结果不明继续记录；路线冻结不代表修复。
- 最新67尚未重做 Windows Steam 的完整游戏/下载/helper生命周期；旧性能证据有适用范围，见 2.0 计划。
- 同源码分支 CI [43158316892](https://github.com/yifengling0/VintagePomeloPro/actions/runs/43158316892)：记录时会员、GNU 清洁依赖检查成功，HAP 编译仍在运行；不能写成全绿。
- 标签 CI [43165968197](https://github.com/yifengling0/VintagePomeloPro/actions/runs/43165968197)：会员、GNU 清洁依赖检查成功，但在编译前的 `Verify source and release versions` 失败。`scripts/verify_ci_release.py:39` 只接受精确 `rc-<versionName>`，因此拒绝用户指定的 `rc-1.5.8-final`。
- 本次不修改源码/发布脚本，不移动标签，不把应用版本伪改为 `1.5.8-final`，不跳过校验发布。2.0 P0 需支持明确的 final 标签后缀，同时保持版本/commit不匹配拒绝；冻结提交重新自动发布需使用经审查、独立于历史源码版本的发布流程，不能声称已发布 Release。
- Windows 工作区 `artifacts/release-1.5.8` 仍是较旧 `373ce532` 的正式交付。本次未重打 APP，不能把它标为最新67或标签成果。

## 开发工作树边界

开发检出中的 box64、dxvk、glib、gstreamer、pcre2、virglrenderer、wine-valve、wlroots、xserver 有既有 build/patch overlay，未 reset、stash 或直接提交子模块。

逐文件核对160个 tracked dirty 文件：152个内容与独立产品检出一致；VirGL 余下8个差异是本地 flight recorder、GPU checkpoint 和 phone shader replay 诊断。独立产品检出的 VirGL effective-file hashes 全部符合已提交来源锁。诊断文件和未跟踪头文件保留原处，没有混入生产标签，也没有被清理掉。不能把这些历史 dirty 误报为“全工作树 clean”，也不能把诊断差异冒称已发布功能。

完整机器可读结果见 [验收/冻结记录](evidence/rc-1.5.8-final-validation.json)。后续实施见 [2.0.0 详细计划](../plans/v2-ohos-wine-winsteam.zh-CN.md)。
