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

Mini 已安装当前调试签名包，基本窗口、中文菜单与输入已有局部验证。Wayland 真机复验已通过：系统浏览器地址栏复制的 `https://example.com/` 经真实 PasteButton 点击后出现在 Wine 记事本；Wine 记事本中新输入的 `WineWaylandClipboardConfirmed123` 经 Ctrl+C 后，在鸿蒙原生 SearchField 中 Ctrl+V 得到完全相同文本。这仅验收按钮授权粘贴和 Wine 向系统复制，没有验收持续自动读取。证据见 `validation/x11-mini-clipboard-open.json`。

下列项目仍未完整验收，不得引用旧性能或 fullscreen BADMODE 记录替代：READ_PASTEBOARD 持续授权、系统打开关闭/回退/文件映射、双路线 DPI 全矩阵及输入/退出重入、各图形后端矩阵、固定负载 Heaven 预热后 3×60 秒对比、M80 及 Edge/PC 场景。

完整模型测试未通过：旧 `run_host_window_task_tests.cjs` 调用了现有生产类中不存在的 `expectSteamGame`、`setSteamGameCloser`，后续模型用例未执行。相关生产类不在本次系统打开修复中修改。详情见 `validation/x11-host-tests.json`。

## 系统打开修复与验证

原先 OPEN 已到达 ArkTS，但保存的 EntryAbility context 在桌面前台运行时处于后台，系统返回 201。`a2b9306c` 改为复用 WineWindowManager 既有的前台桌面/游戏窗口上下文选择，排除已销毁窗口，保留 HTTP openLink、文件只读授权和异步失败提示。

候选已在 Mini 调试签名覆盖安装。Wine 运行对话框打开 `https://example.com/` 后，真实系统浏览器显示 Example Domain 与对应地址，确认不是仅进入请求队列。关闭开关、无法映射、失败通知、旧回调失效和文件 URI 授权已有主机行为测试；文件和 mailto 的真机场景尚未验收。

## 当前调试包

`VPP-2.0.3-x11-upstream-sync-debug-signed.hap`：627763847 字节，SHA-256 `bef04aad68d7372e5e57ee5fba3b0b1e6a7afba68c53f891d4286b34f8eaa8ae`，应用源码 `a2b9306c81444cf9e2ee8de88d903b0c15cfa16d`。签名、新包图形契约、X11 40 ELF/PE/XKB/locale 闭包校验均通过。

上一份已通过剪贴板修复验收的 `8bac145f…` 包保留为 previous 回退包；main 原始回退包继续引用现有 multi-gamepad 任务目录，不再复制。

当前方案保持候选状态；本报告不构成发布或性能提升结论。
