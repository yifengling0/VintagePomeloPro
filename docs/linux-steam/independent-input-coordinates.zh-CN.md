# 独立游戏窗口的输入坐标

Mini 上尖塔 2 的画面已独立呈现，但触屏和虚拟触摸板仍同时偏移。设备证据显示输入覆盖层为 `80,0 2400x1600`，游戏 surface 为 `2560x1430`；原生输入持续记录 `TARGET-FALLBACK`，使用了桌面根的映射。

## 修复合同

- DesktopLayer / WineWindow 在真实 XComponent 的 onAreaChange 上报告物理像素区域。Area.globalPosition 是所属窗口内的位置，叠加当前父窗口 origin 得到屏幕位置；不再从全局桌面设置猜测切边或标题栏高度。
- 保留的输入子窗口跟随该区域，直接触屏 windowX/windowY、虚拟光标绘制及注入使用同一个 surface 物理坐标空间。
- 测量按 desktop / window-toplevel 区分并带页面实例身份，旧页面销毁不能清掉新页面，后台桌面回调不能覆盖前台游戏。窗口操作串行并检查代次，切换或隐藏后旧回调不得重新移动覆盖层。
- findToplevelAt 接受可选 presentationToplevel；独立游戏留在所属窗口，现有两参数桌面调用保持兼容。独立游戏的 pointer、scroll 和 keyboard 使用自己的 renderer / surface，不能再次套用桌面根的缩放和命中。renderer 尚未建立时也不能借用另一窗口的 renderer。
- 不增加固定偏移、游戏名特例或 CPU 图像读回。

## 回归入口

本地与 CI 的 `make test-game-runtime` 均运行 surface bounds 和 mouse input 回归。前者覆盖 Mini decor、切边、虚拟光标/直接触屏共同原点、移动、旋转、全屏、无效测量、旧页面、异步切换及隐藏；后者覆盖物理输入校准和现有手势。

`python3 host_tests/gpu_scene_input_test.py` 编译生产 InputManager / compositor 并运行独立窗口 pointer、focus 和 scroll 回归（case 21）。`--isolated-input-baseline` 用 b5edc6db 的旧 InputManager 验证同一用例确实失败；修复后必须通过。窗口图层和桌面命中仍纳入既有 case 20。

以上主机结果不等于真机验收。安装包来源、签名、Mini 实际点击和生命周期结果记录在工作区 artifacts/hsl-sync，未执行的游戏/性能验收继续保留。
