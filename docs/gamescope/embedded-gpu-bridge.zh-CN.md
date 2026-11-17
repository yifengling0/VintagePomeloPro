# Gamescope GPU 桥与 FSR 接入

工作分支 `feature/gamescope-port`。VPP 保留 Wine/FEX、窗口和输入管理，
Game 会话将原始游戏图像直接交给 Gamescope 的 Vulkan 合成器与官方 EASU/RCAS。
不在 App 中运行 Gamescope main，不创建第二个 XWM 或输入投递链。

## 实现

- `VppGamescopeEmbeddedStart/Present/Stop` 是有版本大小检查的原生接口。
  平板和手机均在 App 进程内使用；不依赖 NCP。独立 core 的 OHIPC/SHM 诊断保留。
- 复用现有 DirectDesktopConsumer、GraphicsBroker 和场景快照，沿用窗口、
  代际与客户区身份。Game 最终输出由 core 独占 NativeWindow，
  VPP 不先生成一张完整游戏画面给 core 二次放大。
- NativeBuffer 导入 core 自己的 VkDevice，不传递别的设备上的 VkImage。
  ready fence 用临时 SYNC_FD semaphore 导入；FOREIGN 队列获取和归还均有屏障。
  输出槽 fence 保护命令与图像引用；导出的 completion fence 随原队列归还。
  未采样/背景帧保留原始 ready fence。resize/错误/退出排空，普通 GPU 帧不 waitIdle。
- GLES 翻转和客户区裁切在源分辨率通过 core GPU pass 处理；结果按槽缓存。
  后续 UI 层保持原顺序，多层场景用 core 有界分批合成。SHM 每帧限 32 MiB，
  协议拒绝截断文本像素、重复 NativeBuffer、无效尺寸、非有限坐标和越界裁切。
- FSR 只对放大的首个游戏内容层执行官方 EASU/RCAS；菜单/弹窗后合成。
  等尺寸与缩小时沿线性采样。设置保存供下次会话生效，默认仍是线性。
  锐化与 HSL 相同：界面越大越锐，传 core 的 RCAS 值为 `20 - 界面值`。
  不改变游戏分辨率、Wine DPI 或用户图形后端。

## 当前验证边界

OHOS core 和 App native 编译通过。Game 模型、FSR/限帧位域、含 fast-math 的
输入契约行为测试、92 项合成器回归通过。此记录在首次设备安装前写入；
真机画面、FSR 实际派发、重复启停及性能结论以任务目录最新验收记录为准。

轴交换的原始 GPU buffer transform 尚未接入本桥；不把它标为已验收。
X11 Game 专用客户区裁切、手机、Steam、Heaven/DX11、完整资源与 3×60 秒
性能对照仍需独立验证。本次不合 main、不推送、不上架。
