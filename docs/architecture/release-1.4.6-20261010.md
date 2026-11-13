# VintagePomeloPro 1.4.6 发布包

- 版本名称：`1.4.6`，版本码：`1004060`。
- bundle：`com.vintage.pomelopro`；`proRelease / release`；`debug=false`；ARM64。
- 生成未签名 HAP 和 AppGallery 正式签名 APP。未上传商店，按用户要求暂停真机操作，由后续测试者继续回归。
- 源码：`feature/main_proton`，HEAD `f47090663537b1e70ad1ebe2f76bd2c34cfbb6af`，包含本地尚未提交的修复。这是构建时的源码快照；源码发布范围见本文末尾。

## 本版保留的运行路径与修复

1. Proton / Wine ARM64 与 FEX 的 32 / 64 位执行路径，既有 x87 性能设置和低地址共享映射。
2. 自动 Direct Vulkan 选择及 Venus 回退策略；保留减少复制的路径。
3. 既有 OpenGL 3.0 / 3.1、UBO、sRGB、透明度及图层呈现修复，不扩大实际硬件能力声明。
4. Steam DX11 GPU 多窗口呈现：隐藏窗口提前创建 WSI、drawable 独立 producer、完整身份及代次校验。
5. 点击及拖拽开始前刷新目标窗口，防止静止光标沿用后台 Steam 的缓存编号；移动缓存最多 32ms，拖拽释放继续配对。
6. 输入透明的纯 GPU 子面将事件交给真实父窗口；普通菜单保留独立命中。
7. Direct FPS 使用真实父窗口身份，多个 drawable 每个输出帧只计一次。

发布前将已验证构建中的 `winewayland.so` 和 `win32u.so` 同步到打包输入。除这两份库外，底层 Wine / FEX / Mesa / Vulkan 资产与 v5 测试包保持一致；release strip 的非加载 ELF 元数据变化不影响此比对。

## 构建与校验

- Hvigor `assembleApp` 成功，ArkTS 和原生 release 代码均进入包。
- 正式 APP 容器、签名 HAP、嵌入的 release / app_gallery profile 通过官方验证，证书与配置匹配。
- 未签名 HAP 已确认无签名；APP 内嵌 HAP 与它的业务内容一致。
- 包内版本号、版本码、debug 标志、架构及纯数字版本格式通过校验。
- Wine ARM64 runtime closure、运行库来源和 zlib 检查通过。
- 所有 rawfile 及预编译库的加载代码与 v5 基线匹配；新编译的宿主库与当前 release 产物匹配。
- 当前源构建签名配置已恢复；密钥和私有签名材料未放入交付文件。

## 真机验证范围与接续测试

既有候选包的 PE32 / PE64 并发窗口探针通过，生产代码的旧输入缓存问题已复现，修复后的点击 / 拖拽与 input region 回归通过。

本轮核对设备原生库身份匹配 v5，并观察到 Steam Direct 的 `directGameCpuReadBytes=0`、`directGameCpuUploadBytes=0`。大屏加载动画的帧率不能作为正常导航性能结论。

需要后续测试者继续确认：Steam 启动游戏后首次点击和拖拽不会提升后台窗口；普通 Steam / 大屏模式连续浏览 30–45 秒；Wine 开始菜单和窗口显示 / 隐藏切换；PAL4、War3 等已有游戏回归。此次正式交付包未再次安装验证，不宣称所有游戏或 Steam 导航帧率已达标。

Steam 客户端的历史版本解压补丁仍为独立分发内容，不在 HAP / APP 中替换用户游戏目录。

完整身份及 SHA256 见同目录 `manifest.json`、`tested-input-identity.json` 和 `SHA256SUMS.txt`。

## 源码发布前复核（2026-10-10）

本次发布包含 1.4.6 版本信息、宿主源码、Wine/Mesa/VirGL 生产补丁、构建入口、专项回归及对应诊断说明和脱敏证据。上架 APP、HAP、预编译库、签名材料、构建目录及 workspace_temp 保留本地，不加入 Git。子模块固定提交不变，其本地草稿不整体提交；生产改动通过主仓库补丁入口重建。

54 个已注册 Wine 补丁从固定基础重放后，50 个文件与 release 实际使用的 `wine-direct-drawable-source-20261010` 完全一致。Mesa 8 个补丁 / 23 个文件、VirGL renderer 4 个补丁 / 3 个文件重放后分别与已验证源码一致。子模块原始 dirty 工作树不是 release 的 Wine 输入，不能把其额外实验混入发布。

Direct viewport/会话策略、显示节拍、PE image policy、Broker argv、legacy alpha-test、GPU front/pbuffer storage、FEX fault boundaries、layered child、early WSI、drawable IPC、GPU scene/input、client remap、Broker startup 和 guest graphics build identity 专项通过。VirGL UBO、conditional render、socket transaction 与 11 项共享映射在 WSL 通过。生产 ArkTS 输入回归通过，旧版可复现静止光标点击提升后台 Steam，修复版覆盖点击、拖拽、释放配对、缓存到期及时钟回退。

Docker 的新 glibc 与旧 Mesa C11 thread shim 冲突、挂载目录 Git ownership 检查及 Node 路径导致的首轮测试阻塞，均换用正确测试环境后通过；未改动产品代码或图形默认值来绕过。没有追加全量测试或最终发行包真机回归，原有设备验证边界继续保留。

版本：`1.4.6 / 1004060`。已交付产物 SHA256：

| 产物 | SHA256 |
| --- | --- |
| 未签名 HAP | `ecf34affe12fc3f5f8240b58937081dfa18bfbb6af3ebdcd393baa925cfaf1fd` |
| AppGallery APP | `fa104aebe32fd7380ba77243f64472a2fd9cd2b3ed0dd6afd3637916b82c9a27` |
