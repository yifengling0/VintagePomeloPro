# 本次测试发现的限制与定向后续任务

这是问题记录，不把已有缺陷或测试工具限制隐去，也不默认扩大本次上游移植范围。

## I1：OpenGL 0x505 间歇性丢帧

候选与冻结基线的 x64 短探针均出图并完成四象限校验，但严格日志门禁因 `gl=0x505` 的 blit-drop 告警失败；首组稳定采样两边各一条相同告警。三组采样和长跑已在 DEVICE_RESULTS 中补齐：后两组完成的样本无该告警，原 B3 readiness 失败单独保留；x64 长跑与 x86 后台阶段仍有告警。beb 的五轮 x86 卡片恢复成功，但整段有 10 行告警、末次 drops=1080。不能因基线复现就判定频率/恢复完全一致，也不把改善 root 卡片的 ArkTS 修复当 GL 错误修复。

定向任务建议：Terra 只读 `graphics/virgl_surface_presenter.*`、错误退避 policy、guest/host transport 和对应单次日志；先判断错误归属、是否遗留 GL error、分配/重建期间的失败及恢复时间。不得直接清掉错误检查、删除日志门禁或把失败行过滤掉来取得 PASS。若需修改 guest，另开子模块任务，本轮 pins 保持不动。

## I2：同进程 Wine 引擎重建后的 IME 注册

实测候选：先通过 `observe-product-summary` 启动 GL，再用无 LAB 参数的正常 notepad Want；产品策略切换按既有设计重启了 Wine 引擎。新桌面/记事本能运行，英文按键输入有效，但中文 commit 未落入 Wine。完全 force-stop 应用再冷启动 notepad，启用宿主键盘后中文“冷启动中文”成功上屏。

源码证据：`input/text_input.cpp` 的 `Register` 在 `global_` 非空时直接返回；本单例没有对应 Unregister/Shutdown。`compositor/wayland_server.cpp::Stop` 销毁 display，但没有清理这个单例保存的 global、event source、pipe 和排队状态。日志显示第二轮没有新的 TextInput object/register/armed 处理，冷启动则恢复。此段实现与 main `2c043636` 一致（仅搬移 include 路径）。

基线设备对照已补齐：主进程 19182 的 LAB GL/War3 会话，01:21:40 经无 LAB notepad Want 发生图形策略切换和引擎重建；notepad 新窗口可用，点击剪贴板候选“原版重启中文”后正文仍空，随后 `baseline-ascii` 正常上屏。force-stop 后主进程 23192 冷启动 notepad，“原版冷启动中文”真正上屏。`baseline-ime-restart.log`、`baseline-ime-restart-chinese-result.jpeg`、`baseline-ime-restart-ascii-result.jpeg`、`baseline-ime-cold-chinese-result.jpeg` 保留。因此重启后中文失效已经有基线运行证据，不能宣称本轮整体 IME 门禁无缺陷通过。

定向任务建议：为 TextInputManager 设计显式 display 生命周期；在事件线程停止后的安全位置解除 pipe event source/FD，清理旧 resource/focus/pending op，并允许新 display 再注册。审查资源销毁回调与互斥锁的顺序；旧队列不能投递到新会话。测试至少覆盖 register→客户绑定→中文→stop→register→中文、重复 stop、挂起 commit、焦点 surface 销毁。不要只把 `global_=nullptr` 当作完整修复。

本次无须改产品 UI。若纳入修复，必须是独立提交、双 ABI Native/HAP 重建和真机重启复测；现有搬移审计的固定检查点与新语义修复证据要分开，不能放宽断言掩盖变化。

## I3：测试方法与硬件限制

- UI 自动化的英文 inputText 是按键注入；中文 inputText 会先放入宿主剪贴板，需点输入法候选条才能验证 commit。不能把剪贴板候选可见当作 Wine 上屏。
- 用新的 Want 改变 LAB/profile 会触发既有引擎重启，不能把这样被中断的 GL 会话算成十分钟长跑。第二个测试程序若需要共存，使用同一 backend 和相同 LAB 参数，并核对进程/窗口未重建。
- 目前实体手柄/马达未被检测到；硬件方向、断连释放及震动未实测。host 模型测试或手机振动不替代实体手柄验收。
- x86_64 HAP 已编译并做包检查，但没有 x86_64 Harmony 设备运行证据。

## I4：第三组候选冷启动未创建桌面

`candidate-b3` 在 180 秒 readiness 门禁失败，451 秒截图仍为“启动桌面”。主进程与 Explorer 存活，Wayland 统计始终为零 surface/toplevel；Wine stderr 记录 Explorer guest 在 dlopen 的 SIGSEGV 及 Wine 异常处理恢复信息。现有证据不能认定异常是直接根因，也不能认定这是本次合并引入或基线已有。

已保留 `candidate-b3-failed.json`、startup/stderr 日志和现场截图；未重置 prefix、未更改参数。一次单独编号的 force-stop 冷启动 `candidate-b3-retry1` 正常创建桌面并完成测量。后者不覆盖首次失败，不能用它计算“全部启动成功”。产品启动代码保留原有十分钟 root watchdog，性能工具的 180 秒 readiness 是更短的测试门禁。

定向排查：先比较同包重复冷启动与基线的失败率、Explorer/loader 与 wineserver 生命周期、安装替换后的进程清退；保留 HAP 身份和失败样本。不要扩大重试次数、删 watchdog、跳过桌面根要求或重置用户数据来获得通过。

## I5：War3 共存全屏的右侧拖边与首次触摸高亮

候选在 x86 GL 窗口共存时启动 War3 1.24.4 OpenGL，全屏右侧延伸出拖边，退出后 GL 标题栏右端裁切，重新最大化/还原后恢复。部分菜单第一次 `uitest click` 只高亮，第二次才进入。候选仍能进入 Booty Bay、框选五个单位、发出巡逻命令并观察单位移动，F10/结束/退出有效。

冻结基线在相同 LAB、backend、x86 GL 共存与游戏参数下复现同样的右侧拖边、退出后 GL 边框裁切，以及“单人模式”第一次触摸高亮、第二次进入；证据为 `baseline-war3-ready`、`single-tap`、`second-tap`、`single-menu`、`exit` 的截图和 `baseline-war3-graphics.log`。这支持“已存在的表现”，不证明所有游戏几何与输入均正确。真实物理鼠标和无共存场景未以此替代验收。

后续分开排查 GPU/SHM 组合画面的边界与触摸按下/移动/释放次序；保留前景/根窗口几何和输入日志，不用裁剪截图或取消全屏来掩盖问题。

## I6：产品路径的诊断 x86 程序运行卡片未恢复

候选主进程 27821 以无 LAB Want 启动 `C:/smoke/x86/winehua_graphics_smoke.exe`。root #1 已建立并出图，随后 Back→运行中→“图形测试”卡片停留在启动器，没有引擎重建。01:25:24.969 日志先出现 `reconcile removes session=desktop pid=0 state=running`，01:25:24.997 才进入 READY。Native explorer 登记为 `@engine/explorer`，ArkTS `getWineSession('desktop')` 不能靠这个键补回已丢的 root 会话；内置图形测试的相对 executable 与诊断 x86 绝对路径也不是同一精确路径，点击走新启动等待 root 的路径。

01:30 同 backend、无 LAB 的 notepad Want 能通过新窗口 auto-promote 恢复桌面，原 GL session/key 未重建。这个诊断恢复不是卡片恢复通过。外部直接启动未导出的 DesktopAbility 被系统拒绝，未改 exported、权限或其他访问控制。

基线控制：主进程 41748、相同产品 x86 GL Want，01:42–01:43 的 `baseline-root-card-*` 样本保留了 Wine 桌面与图形测试两个卡片，未出现 root 的提前删除；点击图形测试虽按内置相对路径另启一个程序，但能返回桌面。因此不能把候选的卡住直接归为基线已有缺陷。

修复提交 `beb00711` 只调整 AppSessionService 的 reconcile：PREPARING 时查询 Native 当前 desktop root；仅当它是正 ID 且与现有 pid=0 会话的 toplevelId 精确匹配时保留。停止、切换、错误、root 消失/更换、查询失败及已退出游戏进程仍被清理。没有全局跳过 reconcile、修改 UI/exported/协议或导入另一套 process registry。

新增测试运行实际转译后的服务源码，按手机日志的 root callback→reconcile→READY 顺序执行；旧实现失败、新实现通过，并覆盖失效 root 与退出进程。`make test-model` 通过。新 HAP 的全部 121 个 Native 库和嵌套 Wine 数据与 d28d1a02 逐字节相同，只有 ArkTS 内容变化；包/签名校验通过。手机循环恢复结果以 DEVICE_RESULTS 第 15 项为准，不能删除旧失败样本。

## I7：Legacy DXVK 的 D3D9 初始化失败（原版已复现）

候选 beb00711、主进程 55651，以产品 `dxvk_legacy` 和 x64 switch cube 的 `--d3d11` 冷启动，D3D11 出图；先触摸窗口取得焦点，再按 F1，标题出现 `init D3D9 failed: HRESULT 0x8876086a`，窗口白屏。标题 frame 仍快速增长，因此仅凭 frame/“regress 0”不能判定真实绘制通过。F2 切回 D3D11 后恢复正常图像；最大化和经桌面卡返回也可用。

冻结原版同参数、主进程 63216，在可见 D3D11 后触摸窗口并按 F1，复现相同 HRESULT 和白屏。证据 `root-fix-legacy-d3d9-failure.log`、`root-fix-legacy-d3d9-focused.jpeg`、`baseline-legacy-d3d9.log/.jpeg`。这是两版均失败的 Legacy/D3D9 覆盖，不能写成 Legacy 全面通过，也不说明新包的 Modern 结果。

后续定向检查 cube 的 D3D9 CreateDevice 参数与 Legacy 的设备能力/格式/交换链创建日志；对照 Modern。先分辨探针兼容性、驱动能力与 Wine/DXVK 行为，再决定是否需要独立子模块修复。保持当前 pins，不通过关闭默认 batching、忽略 HRESULT 或只计空循环帧来获取 PASS。
