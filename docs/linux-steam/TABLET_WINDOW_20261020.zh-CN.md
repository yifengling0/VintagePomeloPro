# 平板 Steam 窗口尺寸与大屏往返验证

2026-10-20。在 `main_proton_steamarm64` / `dea33e75` 的工作树实现并验证，仅操作 USB 平板。候选 v82 调试签名 HAP SHA-256：`ad643a1e7a18f02843e9130bf0f4c73d16f5cc1b5860a8febe52aac02edee5c3`。手机保留原状态，本轮未操作。

## 修正

- Wine HWND 从实际 work area 计算普通启动矩形：首选 1024×640、16:10、最多占可用宽高的 90%，不足时等比缩小并居中。先取得真实客户区再启动 Xwayland，不再用固定的 1280×800 强行拉大宿主。
- 原生 Steam 的普通／最大化／全屏请求增加序号和 Wine 应答。模式与本次序号匹配后才调整 Xwayland，避免拿过渡中的旧矩形调整客户端。窗口撤回只表示切换过程，不临时退回普通模式。
- Wine/Wayland 可能在 SetWindowPos 后完成较早的 fullscreen configure；普通恢复矩形需连续两次检查匹配才应答。错误尺寸最多重发 20 次 SetWindowPos，不用固定等待猜测恢复完成。
- 实际错位来自合成器：全屏将 toplevel 锚定原点，退出时 Wine 报告的是原有坐标，位置同步判断其未变化，合成器就留在左上角。现在仅对有效的外部窗口绑定保存全屏前的合成器位置和 Wine 坐标快照，退出时恢复，resize 重绑保留快照。普通未绑定 Wine 游戏沿用原行为。
- X11 的 ConfigureRequest 在被约束或尺寸不变时也回复 synthetic ConfigureNotify；普通主窗约束在真实 root 区域，Steam 等面积替换窗口按堆叠顺序选择。没有按窗口标题强制所有窗口铺满。

## 平板实机结果

官方 Linux ARM64 Steam、外置 GNU runtime，CEF GPU 默认关闭，Xwayland 内容绑定同一 Wine HWND；未使用 Windows Steam、Windows Steam DLL、QEMU 或 GNU Wine。

| 场景 | 实测 |
| --- | --- |
| 普通冷启动 | 1024×640，居中，进入真实客户端与游戏库 |
| 普通 → 大屏 → 普通 | 大屏 1200×800，恢复 1024×640，实际显示回到切换前位置 |
| 拖动普通窗后再进入／退出大屏 | 第二轮恢复到拖动后的实际位置；未退到 Wine 旧坐标或左上角 |
| 最大化／还原 | 1200×800 → 1024×640，恢复拖动后位置 |
| 直接 `-gamepadui` 冷启动再退出大屏 | 1200×800 → 1024×640，居中，游戏库可操作 |
| 大屏退出后的双指滚动 | 系统注入两指触摸使左侧库列表持续滚动；延迟截图的列表裁剪逐像素一致，没有回弹；反向手势可以返回 |

实际位置用截图边界核验，不只使用 PE 数值。v82b 第一轮前后物理矩形都是 `(354,158)-(2402,1438)`；拖动后及第二轮大屏还原、最大化还原均为 `(220,184)-(2268,1464)`，内容尺寸均为 2048×1280 物理像素（逻辑客户区 1024×640）。证据和比较结果存于本地 artifact，含账号的原始截图不纳入仓库。

## 自动验证与构建

- v82 ArkTS/native 增量编译、打包、调试签名与覆盖安装通过。按既有 `STEAMOHOS_PIN_ENTRY_BASELINE=1` 规则固定 EntryAbility；身份记录保留源码／打包输入差异。本轮没有编辑 EntryAbility。
- native 生产合成／输入测试 20/20 通过；external-window-host 专项通过，覆盖拖动坐标与 Wine 坐标不一致、全屏重绑、重复请求、退出恢复及未绑定游戏不受影响。
- GNU X11 桌面回归 3 项通过：小启动窗、超大主窗约束、synthetic ConfigureNotify、替换窗口与大屏切换。
- ArkTS WindowHost 状态测试通过：请求／应答、过渡期禁止 resize、最小化恢复、焦点、关闭与代次；PE 使用 `-Wall -Wextra -Werror` 编译通过。输入回归详见 [输入报告](TABLET_INPUT_20261020.zh-CN.md)。
- Mesa gitlink 保持 `557fa3e781ea6e78fcff3ee759c25f60c3055064`，原有 libdrm 工作树状态未动。

## 边界与保留问题

分屏、更小宿主区域及旋转未在真机验收，尺寸算法通过不等于这些系统场景均已通过。CEF GPU 仍默认关闭；主题、手机游戏启动和 GNU 原生 Vulkan 是各自独立的后续任务。

v82 首轮在 GNU robust-owner IPC 预检失败，实际 Steam 未启动；确认子进程全部退出后，v82b 和 v82c 均正常启动。此次窗口修复没有修改或绕开 IPC 预检；其偶发失败另记为待复查，不能以旧的 live 文件作为当前启动证明。切换后网页内容仍需客户端加载，窗口位置与网页加载分别验收。

当前保留 v82 候选与 v77 回退包、身份 JSON 和必要小型证据，删除失败中间 HAP 及重复 unsigned。源码尚未为本轮新建 commit/push。依赖和下一阶段设计分别见 [第三方清单](THIRD_PARTY_RUNTIME.zh-CN.md)、[DroidDeck 移植设计](DROIDDECK_UI_RESEARCH.zh-CN.md)。
