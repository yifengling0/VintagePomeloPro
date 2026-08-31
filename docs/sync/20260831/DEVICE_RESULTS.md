# 本轮设备验证记录

状态：进行中，不能据此宣布全套回归通过。

## 包与设备条件

- 产品基线 main：`2c0436360ad3f821afe3fd6ea29d76e89f3781e7`。
- 基线签名 HAP：1.3.3 ARM64，SHA-256 `2bcc61e7124480810bdb03e4ce98321984e79972c856dfad6da84ab4e52a0bf2`。RC 源码 `358e3147` 与 main 只差 CI/docs，已核对运行时代码等价。
- 候选源码：`d28d1a02c42f87f6cb7a37f130ad35fefb668205`；1.3.3 双 ABI，481284048 bytes；SHA-256 `8c3239aa2a4fcc73deec13a2ec5993ac3d5684dfc42fd7007049207c83129a61`。
- 真机 ARM64、系统 API26；包 target/min API23。构建支持 x86_64 不等于在 x86_64 设备上验证。Windows guest x86/x64 与 HAP ABI 也不是同一概念。
- 通过 `install -r` 保留用户数据；无卸载、无重置 prefix。运行锁保持亮屏。原屏幕超时 600000ms，测试完成后释放临时锁；原产品渲染模式 DXVK 2.6，自动化后需恢复。
- 截图、完整日志、安装与签名验证输出放在忽略目录 `.hvigor/outputs/sync-master-20260831/`。不提交设备标识、证书或用户资料。

## 已有证据

1. 基线与候选的主页/设置/库页面保持产品 UI；源代码保护范围另由 audit.py 校验。
2. 候选 Modern cube 从 frame 24211 持续推进，窗口最大化、F1 D3D9、F2 D3D11 可见，regress=0。五轮 maximize/restore 后返回启动器，再点运行卡片恢复，frame 37200 仍正常。这些是功能证据，不能当作严格性能 A/B。
3. 候选短探针 run `phase2-20260901-001243-01-core-reuse`：audio-x64/audio-x86 PASS；opengl-x86 PASS；opengl-x64 的程序结果 PASS（1297 frames、fallback=false）和四象限画面 PASS，但 NCP 日志出现 `serial=10 gl=0x505 drops=1`，严格 host-summary 为 FAIL。
4. 基线复测 run `phase2-20260901-001819-01-opengl-reuse`：opengl-x86 PASS；opengl-x64 同样完成 1297 frames、四象限正确、fallback=false，但出现 `serial=374 gl=0x505 drops=1`，严格 host-summary 为 FAIL。这仅证实基线也存在问题，不能证明候选所有失败频率/恢复时间都等价。
5. 第一组基线 GL 稳态测量 `baseline-a1` 出现 `serial=3755 gl=0x505 drops=1`，严格 runner 记 INCONCLUSIVE；原始时间序列保留，后续描述性分析不得改写为无告警通过。

音频 PASS 指 guest 提交、host 消费及非零 RMS；没有人耳确认时不声称可听性、音色或延迟通过。当前没有实体手柄输入/马达证据。

## 仍需完成

- x64/x86 GL 持续运行至少十分钟，窗口/后台恢复与错误恢复情况。
- 三组交替基线/候选性能样本；同 workload、尺寸、backend、观测设置；保留温度/电源条件和告警，不筛选最好的重试结果。
- Legacy 与 Modern 各自帧推进，不能把 Modern 下 D3D9/D3D11 切换当成两个 DXVK 版本通过。
- 真实游戏、菜单/popup、滚动、触摸/触摸板、IME、窗口/后台/热启动动作。
- 实体手柄方向、断连释放、重连、游戏震动；不存在的游戏/硬件单独标未覆盖。

最终验收应逐项更新本文件和 STATUS，并保留失败和限制，不抹去先前严格检查结果。
