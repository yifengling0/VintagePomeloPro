# VPP 2.0 实施记录

## 2026-11-12：P0 CI、版本和 Windows 基线

- 从 `feature/v2-ohos-wine-winsteam@f990b176` 继续，不移动 `rc-1.5.8-final`。
- 新分支 push 和面向该分支的 PR 纳入现有 HAP workflow，保留原会员与 GNU 过渡检查。
- versionName `2.0.0`，versionCode `2000000`；bundle/签名身份不变。当前为过渡开发版，Linux UI/后端还未移除。
- `verify_ci_release.py` 只额外接受精确 `rc-<应用版本>-final`，保留 commit、版本和来源校验；错误版本、额外后缀和错误 commit 仍拒绝。
- 新入口 `make test-windows-steam` 校验生产 helper 来源/PE/HAP资源、客户端参数、事务导入恢复与实际 Steam 服务。`make test-game-runtime` 在开头调用同一入口，后续删除 GNU 测试时也保留 Windows 回归。
- 定向验证：`host_tests/ci_release_test.py` 17 项 PASS；Windows runtime 身份3项、C++参数、VDF/path/argv与14种事务恢复、实际服务的中文多库/缓存/安装状态/进程归属/冷热启动等 PASS。主机回归不代替真机下载、游戏和后台恢复。
- 完整回归、2.0 HAP 编译和 Mini Windows Steam 实测：待执行，不先标记 P0 全部完成。

历史 tag 的工作流文件仍是 67 的版本。修改新分支验证器不会自动改变旧标签工作流；重新运行旧 tag 原 job 仍会拒绝 final 后缀。修复适用于新提交上的标签；冻结源码重发布仍须有独立、可追溯的发布验证流程，禁止移动标签或跳过校验。

存储：核对任务现存两份较新 HAP、读取 module 身份并验证 SHA 后，清理12份旧 `app.hackeris.winehua / 1.0.13 / debug=true` 安装候选，回收4164315504 bytes。保留 VPP 当前67/c09测试包、旧任务较新两包、所有用户数据/备份、正式发布包、源码和共享缓存。构建复用 E 盘既有 WSL增量目录，未新建完整源码或运行库副本。

恢复执行核对：67ea17f7 的分支 CI 43158316892 已成功；final 标签 CI 43165968197 仅在源版本验证阶段失败。当前 Wayland close 生命周期回归通过，focus fixture ABI 已包含在历史提交 efb8ae2c。开发树的 Docker 子模块绝对 Git 路径和 VirGL 实验 overlay 影响全量主机回归，因此后续使用既有 source-integrity 独立检出及增量缓存验收；保留开发子模块，不重置实验文件。P0 定向回归与完整产品验收分开记录。

## P1：Windows Steam UI 与服务解耦

- HmosDeck 不再订阅或初始化 Linux session/library/installer。游戏列表从现有 Windows Steam 目录生成，保留原始 AppDescriptor（用户兼容设置、中文/私有库路径和启动参数）。
- SteamClientService 只负责 Windows 客户端导航，导入期间阻止启动、重复启动互斥、失败释放锁；游戏和退出仍交给既有 AppSession/HostWindowTasks，保留开始前反馈、正常关闭与强退。
- 组件页改为 Windows ZIP 导入/更新、实际目录扫描、CEF 偏好和启动入口；经典界面、HmosDeck 和未安装空态都指向同一管理页。删除 LinuxSteamPanel/ProtonRuntimePanel，旧 pages/LinuxSteam 路由只显示退役说明与 Windows 管理，绝不初始化旧运行库。
- 公共封面服务脱离 Linux 模型；复用历史公开封面缓存，避免重复下载。AppID 验证统一为 uint32 边界；手机/平板布局逻辑保留。
- Windows 回归新增实际客户端服务与 Deck 路由/列表/手动扫描测试，并纳入 make test-windows-steam 和 CI 共用入口。原 Linux 专用后端测试仍保留到 P2/P3，已退役双客户端 UI 用新产品行为测试替换。
- 验证：P0 在既有独立检出运行完整 make test-game-runtime 通过；本批 Windows helper、C++参数、事务导入、中文多库、服务、Deck 路由和布局回归通过。剩余 Linux 后端专用 Deck 测试与 preflight 单测通过。P1 完整 HAP、远端 CI 和 Mini 验收待后续记录。
- 此提交只完成 UI/服务拆分；GNU native、旧诊断入口和载荷尚在，不能作为最终 2.0 发布包。用户 Linux 库/前缀/存档没有清理或迁移。


## P2/P3：本地退役 Linux 运行线

- 遵照用户新要求：停止线上 CI，本批仅本地提交、调试与构建，暂不推远端。
- 删除 Linux Steam/GNU Proton ArkTS 服务、模型、原生 loader/broker/NAPI 与 CMake targets；旧路由保留说明，不读取或删除旧用户库、前缀、配置或存档。
- WineEngine 不再准备 Linux Steam bridge；删去 0065/0069 Linux 注册表桥补丁及组装注入。0069-wow64-current-thread-context 等公共修复保留，不能按数字前缀批量删补丁。只在隔离产品树精确逆转这两份桥补丁，未改开发树 vendor 修改。
- Windows Steam 和 OHOS Wine 使用既有 AppSession 输入/会话管理；删除仅供 Linux broker 的外部窗口与虚拟输入所有者，保留鼠标表面边界、逐窗口输入空间和手机/平板布局修复。
- 通用 NativeBuffer 探针移入 direct/，不再依赖 Linux 原生客户端目录。保留 ARM64EC FEX、DXVK/WineD3D/VKD3D、图形/媒体/字体闭包、会员功能和 Windows ZIP 导入。
- Make/assemble/package/preflight 解除 Linux 下载和离线包依赖。构建前只清理本检出的已退役生成资源和旧 CMake 模块，不清理共享编译缓存或设备用户数据；最终 HAP 检查拒绝 Linux rawfile、GNU helper、lsteamclient 和桥接目录，并确认核心 Wine/FEX/Windows 模块。
- 为本地增量树建立 local-effective-source 来源记录，包含所有 vendor 有效差异与新增源码字节，并在构建期间重验。它不冒充干净检出；默认 CI 干净源码门禁保持严格。Wine 受影响构建使用固定 build/wine-v2 独立身份，复用其他依赖缓存。
- 已验证：ArkTS 编译通过；make test-game-runtime 完整通过（Windows 导入/多库/冷热服务/Deck、鼠标漂移回归、字体、窗口/图形生命周期、IME、诊断导出）。本批最终 HAP、源码来源闭包与 Mini 实测仍待下列后续记录。

## 本地升级与最终载荷检查补齐

- 第一次完整本地构建完成 Wine 重编译和 252 项预检，组装在旧检查器仍要求 linux-steam-bridge.json 时停止；修复检查器，改为拒绝已退役内容，同时保留 Wine/FEX/图形/媒体闭包检查。
- 最终 HAP 检查新增 libentry/libsmoke_napi 的 Linux NAPI 残留与 kernelbase 注册表桥代码检查；负向 fixture 通过。
- 修复受管 FEX/ICU 同尺寸升级被跳过的问题：按 64 KiB 分块比较内容，短读明确失败，临时复制校验成功后再原子替换。升级配对、同尺寸变化、用户 native ICU 保留及复制/rename 失败重试回归通过。
- 覆盖升级仅定点清除 files/wine 内的两份旧 lsteamclient.dll 与 linux-steam-bridge.json。遇到符号链接不遍历，不清理旧 Linux 游戏库、前缀、账号配置和存档；新安装、重复调用、失败重试、符号链接和用户数据保留回归通过。
- 启动超时/取消、受管 CPU、会员结算 UI/活动及旧运行时清理测试纳入同一 make test-game-runtime 入口。最新 ArkTS 编译、完整重跑、最终 HAP 及真机结果以实际后续记录为准，不把主机 fixture 当游戏或支付实测。
- 全程本地提交与构建，不 push、不发起线上 CI。复用 source-integrity 独立检出及依赖缓存；受影响 Wine 编译固定在 build/wine-v2，不伪造旧缓存身份。

### 最新本地主机回归（325887c8）

- 完整 make test-game-runtime 返回 0：Windows helper/导入事务/中文多库/冷热客户端/Deck，输入表面与鼠标，字体/图形/窗口生命周期，19 项生产 Wine 启动案例，CPU/ICU 升级、退役文件清理、会员、IME 和有界诊断导出全部通过。
- 2.0.0 / 2000000 release ArkTS 与 HAP 组装成功（app.debug=false）。新产品边界检查首次误写了 wineboot 路径，现按既有 assemble 和启动器使用的 bin/wineboot.exe 修正；没有删除该必需检查，其他架构/依赖校验继续保留。
- 已对真实 HAP 运行新边界检查通过，包含嵌套 Wine runtime 和实际 native 模块。最终来源/ABI/配对收据在本地构建任务目录生成，不以本条记录替代完整 gate。
- Mini 当前由 HSL 聊天进行真机测试；本聊天仅做了只读目标/状态确认，没有安装、停止会话或输入。2.0 的安装、WinSteam 普通入口、游戏及性能真机验收仍待设备交接。
