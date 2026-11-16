# WineHua X11 同步：当前进度与验收边界

分支 `feature/x11-upstream-sync`，基于 VPP `main@7683a58b`。版本保持 2.0.3，尚未合入 main、上架或向 WineHua 推送。

## 已移植与保留

按 WineHua `master@40b74771` 相对 `a001e6d0` 的功能差异移植 HiDPI、输入失焦释放、X11/Wayland 文本剪贴板和可关闭的系统打开；旧 `feature/proton-x11@22c46eb1` 未新增提交。

保留当前 Proton/FEX 来源、DXVK 1.10 默认行为、DXVK9、VirGL、Zink、本地桌面生命周期、UI、会员及多手柄代码。未替换为上游旧 Wine gitlink，未采用整树删除对象文件的构建方式。多实体手柄仍未实测。

## 剪贴板权限核查

当前 `entry/src/main/module.json5` 未声明 `ohos.permission.READ_PASTEBOARD`，也未弹窗申请该权限。现用调试 Profile 经签名校验，其 ACL 只有 `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY`，没有剪贴板读取权限。

当前宿主文本读取的可用入口是 `InputOverlay.ets` 中的系统 `PasteButton`：用户真实点击安全控件后，由 `X11ClipboardService.prepareSystemPaste()` 读取并准备 guest selection，然后对仍然有效的窗口发送粘贴快捷键。该临时授权不能作为持续自动读取权限的验收依据。普通系统变化监听中的读取可能被拒绝，拒绝不应影响游戏启动。

官方 [申请访问剪贴板权限](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/get-pastedata-permission-guidelines) 将 READ_PASTEBOARD 定义为受限 user_grant 权限。自动读取需要 AGC 申请相应 Profile、module.json5 声明及用户弹窗授权。当前仅完成核查，未向 AGC 提交权限申请，不能声称完整自动双向同步已通过。

Wine 向鸿蒙系统剪贴板写入使用 `setData()`，不需要该读取权限；这个方向的失败需独立检查协议和传输。权限拒绝、服务不可用及超时均不得阻断 Wine 启动。

## Wayland 复制修复

- 合成器保留 Wine 特殊 MIME `application/x.winewayland.tag`；新 selection 读取完成前及键盘失焦时不向 Wine 发布空 selection，保护延迟渲染的剪贴板所有者。
- 当前 Proton `is_winewayland_clipboard_hwnd()` 的类名常量缺少 NUL。修复前打包 ELF 中类名之后紧接非零 UTF-16 字符，无法与完整类名正确匹配，影响 core data-device 模式下每进程管理窗口的创建。补丁 `0085-wayland-clipboard-window-class-termination.patch` 补齐终止符。
- 行为回归直接编译生产补丁内的识别函数，覆盖准确类名、相近类名、其他窗口及无效 HWND，并启用 ASan/UBSan。新 ELF 同时检查终止符。
- 只增量编译 `wayland_data_device.o` 并重链接 `winewayland.so`；记录内其他完整构建产物哈希保持不变。来源与产物见 `validation/x11-clipboard-component.json`。

## 验证状态

主机完整 preflight 为 274 项通过；此前 X11 集成、原生增量构建、ArkTS、OPEN transport、剪贴板生命周期及最终包成员校验通过。新增剪贴板窗口识别回归通过。构建结果和真机验收分别记录。

Mini 已安装过本分支调试签名包，基本窗口、中文菜单与输入已有局部验证。修复 0085 前 Wayland Wine→系统复制仍失败；0085 的最终真机复验尚待记录。安全控件读取与完整持续授权需分开验收。

下列项目仍未完整验收，不得引用旧性能或 fullscreen BADMODE 记录替代：HTTP `openLink` 真机结果、系统打开关闭/回退/文件映射、双路线 DPI 全矩阵及输入/退出重入、各图形后端矩阵、固定负载 Heaven 预热后 3×60 秒对比、M80 及 Edge/PC 场景。

当前方案保持候选状态；本报告不构成发布或性能提升结论。
