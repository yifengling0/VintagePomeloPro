# Gamescope 移植记录：来源迁入与 OHOS 编译边界

基线为 VPP `release/2.0.4@043a1cc0`，工作分支为 `feature/gamescope-port`。
HSL 参考提交为 `46fe1daabd26085c617c74033f470ac9e5d912b1`，
Gamescope 源码固定于 `e6674a98118ca7b098b98a3874b8f195ae1bf750`。
本阶段没有改动产品启动、Game 页面、现有图形后端、用户配置或正式 2.0.4 包。

## 已迁入和验证

- 10 份上游源码归档锁，总压缩输入 165,486,506 字节。复用 HSL 已有归档，逐项核验大小与 SHA-256，无重复下载。
- HSL 已提交的 12 个补丁，保留顺序、来源与每项源文件应用前后的 SHA-256。
- 单独迁入 HSL 未提交的补丁 0013 和 `HslShmUpload.h`：最多三个可复用 staging slot，GPU 完成后才允许覆盖；每份目标纹理独立持有，保留队列依赖；大图和 DMA-BUF 路线保留同步边界。
- 共用描述符、shader 编译、图像协议与窗口关联源码作为来源记录迁入。`HslBackend.cpp` 的 GNU Xlib WSI 和 `HslImage.cpp` 的私有 GNU Control 传输目前仍是**参考实现**，不能作为 VPP 已接通的 OHOS 输出或资源导入。
- `make gamescope-source` 已在现有 `winehua-dev` 容器及 Linux ext4 工作树上通过 13 项严格补丁重放，关闭 patch fuzz，并核验所有前后文件身份。再次执行核验完整源码清单，拒绝未知改动。
- `make test-gamescope-shm` 通过实际迁入上传 owner 的主机行为测试：120 次复用、4 帧排队压力、5 种分配故障、尺寸变化、同步路线及析构排空。
- `make gamescope-ohos-probe` 使用 OHOS SDK 编译真实 `src/color_helpers.cpp` 和 `src/Utils/Process.cpp`，两者均产出 AArch64 ELF 对象；这不证明完整 Gamescope 已链接，也不证明 fork 等进程操作可在应用沙箱运行。
- 现有 Windows Steam 主机检查和产品源码载荷边界检查通过；现有 WoW64 explicit buffer flush 的 7 项测试通过。后者需指定实际只读 Wine 源码 `/shared/thirdparty/wine-valve`；本地空的子模块目录不能作为该测试输入，未修改共享 Wine 树。

详细来源在 `migration-sources.json`。原始 SHA-256 与 VPP 调整后的 SHA-256 分开保存。
HSL 的设备记录保存为 `hsl-shm-evidence-20261117.json`，只作为迁移依据，不计入 VPP 验收。

## HSL 未提交改动取舍

| 改动 | 本阶段处理 | 原因与边界 |
| --- | --- | --- |
| 有界 SHM staging 复用与队列依赖 | 已吸收并通过 VPP 主机测试 | 通用上传所有权修复；VPP 设备性能和异常退出仍需独立验收 |
| MIT-SHM、CEF GPU 默认值和 child 环境 | 保留合同，未复制 GNU 启动脚本 | 依赖 HSL 私有 GNU SysV IPC；VPP 使用 OHOS Wine 与 Windows Steam，需验证自身跨进程 SHM 能力 |
| OpenGL WoW64 explicit mapped-range flush | 本地已覆盖，无重复移植 | VPP `patches/wine/0035-opengl-wow64-explicit-buffer-flush.patch` 已包含显式范围发布与 unmap 语义，且有现有构建接线和测试 |
| Steam 导航、X11 SHM 诊断脚本 | 作为证据和后续用例参考 | 不引入 VPP 产品运行时或正式包 |
| 构建后存储维护接线 | 沿用各工程现有策略 | HSL 仍由自身任务维护；本轮没有修改或提交 HSL 原工作树 |
| 编辑器状态和临时截图 | 不迁入 | 与运行时修复无关 |

HSL 在审查期间仍有其他任务更新，来源清单记录的是本轮捕获版本；不得把后续工作树状态自动视为本次已审查代码。

## 原生构建与呈现尚待完成

`native-probe.json` 记录的是实测编译与 pkg-config 元数据检查。现有 OHOS host 前缀缺少
`xdamage`、`xtst`、`xres`、`xmu`、`libdisplay-info`、`libdecor-0`、`luajit`、
`libinput`、`libudev` 元数据。它们需要分别处理；缺少元数据不等于系统接口无法实现，
也不能以 GNU `.so` 填补 OHOS 链接依赖。

后续最小平台差异如下：

1. 在 Meson 中明确禁用 OHOS 不使用的 DRM/VR 全局 libinput/udev 入口；保留真实失败与能力检查。
2. 根据实际采用的 backend 拆分 Wayland/libdecor、脚本和显示器信息依赖，补齐真正需要的 OHOS 库。不能以空实现或伪造 `.pc` 声称可用。
3. 将 HSL 输出队列和 swapchain 生命周期接到 OHOS NativeWindow Vulkan surface，替换 GNU Xlib WSI。
4. 使用 VPP NativeChildProcess 和现有 Xwayland FD 交接；禁用通用 fork/exec 启动路径，回传真实启动与退出结果。
5. 将 HSL 窗口内容关联和共享图像生命周期接到 VPP NativeBuffer/fence 合同，逐项验证 Direct、Venus、VirGL。GPU 游戏不以全帧 CPU 读回替代正式图像交接。
6. 原生最终输出通过后，接入统一会话解析和独立 Game 页面，再开放已验证的 FSR、限帧及游戏覆盖设置。

当前没有完整 Gamescope OHOS 链接产物、NativeChildProcess 入口、VPP 游戏画面、Game 模式 HAP 或 FPS 对比。
PAL4、Heaven、DX11、五轮启动退出、三轮 resize、Steam 派发、输入和性能均未在 VPP 验收。
未合并 main、未推送、未上架，也没有替换正式交付包。
