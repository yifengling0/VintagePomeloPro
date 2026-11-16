# 2.0.4 用户测试版本

版本：2.0.4 / 2000040。独立分支 `release/2.0.4`，本轮不合入 main、不推送、不上传市场。

## 整合范围

- 纳入 X11 分支至 `e9925b0c`：HiDPI、失焦按键释放、X11/Wayland 剪贴板及默认关闭的系统打开。剪贴板通过系统 PasteButton 授权粘贴；不宣称已获得受限的后台自动读取权限。
- 纳入 `fba878fe`、`9f2fe8c5` 的 Zink buffer 偏移、线性图像隔离 staging 和局部更新修复，保留原有合批上传。
- 纳入 `9db27bc7` 的 DXVK9 交换链修复，窗口模式不再额外轮换前缓冲。原 DLL 与修复 DLL 已分别做 Mini PAL4 对照；本次将热修复正式放入嵌套运行时。
- 纳入 `f9ebe484` 的 WineD3D legacy GLSL 修复：specular alpha 隐式雾读取旧语法已声明的 `gl_FrontSecondaryColor`。候选补丁配方增加 0086，更新历史、源码锁、两套 PE DLL 及 prefix 内置 DLL 身份。
- 保留现有本地 UI、会员两种购买方式、多手柄、Proton/FEX、Gecko 双架构和 Mono。默认 DX9/VirGL、DX11/DXVK 1.10；专业玩家可按游戏切换 DXVK9 或 Zink。
- 启动阶段改为一行真实状态，阶段变化时淡入淡出并上下切换；不播放伪造日志或估计百分比。耗时超过 20 秒显示实际等待时长，保留错误、返回和重试；Deck 游戏封面继续保留。

## 已有证据与边界

WineD3D 主机真实生成器 96 组判据通过，旧实现负对照失败 2 组。Mini Zink 像素探针验证了修复 shader，原 shader 因未声明变量编译失败。

PAL4 的 Zink 白地面仍属未验收问题：雾修复不能单独解决；强制立即上传后画面正常，但恢复原合批库也正常，反向对照未复现。因此不关闭合批、不宣称根因已找到，也不宣称 FPS 提升。VirGL 仍为这款游戏的建议默认。

临时 draw/geometry/toggle probe、逐 draw 读像素、诊断开关和 immediate-upload 库不进入产品。旧记录不能代替整合包的真机验收。多手柄仍缺多只实体手柄验证；M80/Edge/PC 本轮未验收。

## 可复现与交付

应用源码身份和复用运行时来源分别记录。Wine recipe 必须从上游锁和完整补丁历史重放；保留各独立组件 pin，不替换 Wine gitlink。复用 Wine 编译缓存前，对发布基线的两套 stripped DLL 逐字节检查；只重编此次修改的生成器对象并重链 ARM64/ARM64EC、i386。

运行时、Mesa 顶层/嵌套副本、DXVK 清单、WineD3D 清单、wine.inf 身份及发行 lock 必须相互匹配。最终包的源码收据、精确成员、签名和设备结果见 `artifacts/v2.0.4/manifest.json`；主机预检记录在同目录，不以历史包结果宣称新包通过。
