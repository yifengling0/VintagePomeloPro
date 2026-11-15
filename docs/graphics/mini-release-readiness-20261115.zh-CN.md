# Mini 后端与购买修复发布整理（2026-11-15）

本次整理用于 2.0.2 / 2000020 的正式发布准备。用户已部署购买修复云函数，Mini r17 已覆盖安装并启动，用户反馈“这次可以了”。购买验收来源是用户反馈；本轮未代为付款或查看订单。

## 发布行为

保留 DXVK 1.10 默认设置，默认 D3D9 走 WineD3D/VirGL，D3D11 走 DXVK。系统及单游戏可选择 VirGL、DXVK9、Zink，游戏可跟随系统。DXVK9 当前提供 x86 DLL；Zink 自动启用已验证的兼容修复、批量上传和 GPU 呈现，界面不暴露内部参数。不同游戏仍可能有性能和兼容性差别，没有宣称统一提升 30% 或提升 OpenGL 版本。

连续包月和终身买断均保留。自建续费提醒故障不再阻止购买；官方商品、价格、账号、协议、订单核验和重复购买保护继续生效。云端提醒服务自身的可用性仍独立报告。

## 产品清理

- 移除测试 Want 启动、SmokeRunner/SmokeHook/GameHook、LAB 图形选择 API、测试游戏名匹配及自动升级特例。
- 设备测试源移到 `host_tests/fixtures/device_harness/`，不属于应用源码集；主机回归继续保留。
- 移除自动按音频测试程序名开启 trace 的逻辑，以及本轮临时诊断日志、Mesa 实例探针、上传/呈现计数器。
- 正式组装不再收录 Smoke、Vulkan 测试模块、Heaven 回放和 SPIR-V 捕获。发布过滤器阻止旧 native 缓存重新带入这些模块；实际 HAP 检查覆盖 ABC、Native 和嵌套 Wine ZIP。
- DXVK9 DLL 纳入正常组装和构建产物检查，不再依赖人工实验包。

## 运行库来源

复用用户已安装的 r17 基底，SHA-256 `017e9385e3fff8d957333131548cc701aef6c0627a61fab1b00579906406c509`（unsigned）；所选 Wine/FEX 固定库逐项保持原锁身份，不回退旧 Wine，不修改第三方 gitlink。

Gallium 从当前完整 Mesa 补丁栈增量构建，移除诊断代码后 strip。原先 r17 内外 ICD 缓存不一致：外层 ICD 与当前 Venus 构建的可加载段一致，本次将内层同步为该相同二进制，并更新实际清单、内容版本和发布锁。未跳过一致性门禁。

整理后的 Wine ZIP 删除 15,090,378 字节测试载荷。来源和转换记录位于固定任务目录 `workspace_temp/mini-virgl-performance/release-runtime-preparation.json`。本次为复用受核验组件后的正常 Hvigor 全量 UI/Native 打包，不声称所有第三方组件重新从零构建。

## 验证与交付边界

后端 104/104；客户端购买、后端继承/覆盖、串行启动、失败重启、Native 类型契约通过。Zink 上传和呈现生产函数及 ASAN/UBSAN、Mesa 完整补丁栈和增量构建、图形策略、DLL 合并通过。桌面合成 71 项无失败；Direct/Venus 会话环境、旧实验参数不能绕过能力门槛的回归通过。

正常入口为 `bash scripts/package.sh app-unsigned`。最终包须通过所选 Wine/FEX 和图形锁、产品边界、SDK stub 防护、离线 Windows Steam 组件检查，以及 APP 内嵌 HAP 校验。构建记录为 `build/upstream-release.json`；最终文件 SHA-256 见交付清单。

清理后的包尚未覆盖安装 Mini；保留已验收 r17 作为回退。发布产物是未签名 APP，供正式发布签名使用，不是 internaltesting 测试签名包。本轮不上传市场，不触发 push、标签或 CI。
