# Mini 插帧运动、后端与 Heaven 短测（2026-11-19）

本轮按“简单跑一下”执行，有效结论是：**PAL4 三个后端均能出图并生成插帧，但运动画质未通过；DXVK9/Zink 有预算旁路后不自动恢复的问题；Heaven DX11 自动巡游出图，但未通过插帧验收。** 没有修改业务或渲染源码，没有重新构建、合 main、推送或上架。

## 包与条件

- 分支 `feature/gamescope-framegen`，测试前 HEAD `448e7089`。
- Mini MLR-AL10 已安装 2.0.5 调试包，原生行为基线 `99030044`，SHA-256 `1c81c0eb28d8bdc42286daa836445478e9bd64e27631b53af5d31f91a0a80cb8`。本轮没有安装 2.0.6。
- 现有 2.0.6 未签名交付包未改动；已有 `release206-delivery.json` 证明其 ArkTS、Gamescope、Wine/FEX/DXVK/Mesa 载荷与该调试包一致，版本/打包/探针及签名差异不能作为 2.0.6 安装验收。
- PAL4 加载存档 2（青鸾峰、序章、0:01:52），源 800×600、Native 输出 2400×1600、线性缩放、60 FPS 上限；运动段左右转镜头，未写存档。
- 下表是约 10～14 秒的有界采样，含截图和操作开销，**不是同条件三次 60 秒基准，不能计算优化百分比或可靠的后端排名**。真实 FPS 统计接受的唯一 GPU 源图像；呈现计数不代表物理 scanout；GPU 时间不等于 GPU 利用率。

## PAL4 短测

| 后端／动作 | 秒数 | 真实 FPS | 生成 FPS | 呈现 FPS | 结果 |
| --- | ---: | ---: | ---: | ---: | --- |
| VirGL / off / 转镜头 | 11.25 | 34.04 | 0.00 | 34.04 | 关闭态运动对照 |
| VirGL / x2 / 转镜头 | 10.11 | 37.39 | 37.09 | 74.48 | 生成正常，屋檐、人物、小地图出现额外重影 |
| DXVK9 / x2 / 转镜头 | 11.19 | 35.12 | 0.00 | 35.12 | state 9，预算旁路 |
| DXVK9 / off→x2 / 待机 | 14.13 | 37.51 | 37.02 | 74.53 | state 2，重新开启后生成恢复 |
| Zink / x2 / 转镜头 | 10.13 | 28.52 | 0.00 | 28.52 | state 9，预算旁路；天空黑块 |
| Zink / off→x2 / 待机 | 14.21 | 28.23 | 28.09 | 56.31 | state 2，重新开启后生成恢复 |

运动图像对照：`smoke-pal4-virgl-x2-motion-6.jpeg` 的屋檐、人物及小地图可见多轮廓，`smoke-pal4-virgl-off-motion-6.jpeg` 的近似视角对照没有同类多轮廓。视角并非逐像素配对，截图没有标定真实帧/生成帧身份；因此确认当前运动画质存在明显问题，同时不宣称完成全部逐生成帧验收。此前绿色前景树叶的争议归因不作为本次证据。

DXVK9 短测未复现旧的黑帧交替，中文存档文字可读；短测不能证明长期稳定性。Zink 的远景天空黑块在生成 FPS=0 时存在，VirGL 近似方向天空明亮，应独立追踪底层渲染；本存档视角未见旧的大片白地面，不代表其全面修复。

静态复核 `native/gamescope/ohos/OhosBackend.cpp`：连续三次预算失败后 `m_fgBudgetFailures>=3` 禁止新生成，计数主要依赖新计算完成、配置切换或图层身份变化重置。停止新生成后缺少定时重试路径，与“关开恢复”的现象一致。仍需用行为测试和真机复验确认修复，不能把静态定位称为已修复。

## Heaven / DX11

1. 首轮卡片启动参数错误，相关 FPS **全部作废**。生产函数 `AppModels.ets::mergeLaunchArguments` 对单个 token 忽略大小写去重，吞掉重复的数值 `0`；旧 descriptor 参数与新 settings 合并还保留旧 API 值。主机提取执行当前函数及设备实际 argv 均复现：

   ```text
   -video_multisample 0 -video_vsync 0
   → -video_multisample 0 -video_vsync

   旧 -video_app direct3d11 + 新 -video_app direct3d9
   → -video_app direct3d11 direct3d9
   ```

   约 5 FPS 的早期结果不属于可靠的 Low 档性能结论。所谓 DX9/VirGL 的 feature-level 弹窗实际混入 DX11 参数，也不能算 DX9 兼容性失败。

2. 临时 392 字节 CMD 绕开参数合并，直接传入完整 DX11 参数：800×600、Low、AA 0、VSync 0、tessellation disabled，后端 DXVK 1.10 / Direct。观察到城堡和飞船等自动巡游画面；运行窗口标题为 Direct3D11。
3. 巡游继续时，VPP 计数停在 real 67 / gen 55 / present 122，后续状态为 7“等待可插帧的 GPU 游戏画面”，浮层显示零。**仅确认 DX11 出图和自动巡游，未确认持续插帧或有效性能收益**。临时脚本子进程/窗口图层关联及 GPU 呈现路由需后续定位，当前不能确定根因。
4. 另一个独立 DX11 游戏、有效 Heaven DX9 对照、手机和 PC、本轮均未完成验收；不以 PAL4 收益外推。

## 后续修复优先级

1. 修复通用启动参数合并，保留顺序、重复参数值和用户意图，避免旧 descriptor 参数污染新配置。先加真实 option/value 行为回归，再重测 Heaven。
2. 将预算旁路改为有界冷却与重新探测，并保留失败/超时的真实帧路径；避免一次加载尖峰使整个会话一直不能插帧。
3. 核对光流方向、历史帧时序、遮挡/场景突变及 HUD 处理，使用运动 off/x2 对照；帧数增长不能替代画质验收。
4. 单独修复 Heaven 子进程/窗口 GPU 识别，以及 Zink 天空渲染。继续保持插帧默认关闭，当前不能宣布全后端通过。

## 收尾及证据

证据固定存于 `F:/VintagePomelo-Workspace/workspace_temp/gamescope-port/device/`：

- `smoke-pal4-{virgl-off-motion,virgl-x2-motion,dxvk9-x2-motion,dxvk9-x2-retry,zink-x2-motion,zink-x2-retry}.json` 及同名日志/截图。
- `smoke-launch-argument-audit.json`、`smoke-heaven-invalid-arguments.log`；早期两个 Heaven FPS JSON 在汇总中明确标为无效，保留原始数据供审计。
- `smoke-heaven-script-dx11.jpeg`、`smoke-heaven-final.jpeg`、`smoke-heaven-backend-identity.log`；不引用早于本轮的引擎 HTML 日志。
- `smoke-original-pal4-game.json` 与 `smoke-pal4-restored-game.json` 设置字段比较无差异；三个 PAL4 进程均正常退出。
- `smoke-restoration.json`：Heaven 自带 Quit 正常退出（exit 0），两个 CMD 子进程结束，运行中任务为空；删除设备临时 CMD、关闭非持久诊断，返回 Game 游戏库并释放 Mini。

Heaven 最初没有关联主程序且无 sidecar；本轮新增的临时脚本入口已移除，保留自带 `Heaven Benchmark 4.0/heaven.bat` 的正常关联、参数为空、图形后端跟随默认、Game 性能设置跟随全局。**这不是完全回滚最初“未关联”的元数据状态**。游戏资源和用户存档未删除。

本轮不生成新 HAP，保留原有当前/上一份调试签名包及 2.0.6 正式交付目录。日志单文件小于 32 MiB，维护空间结果记录在固定任务目录 `smoke-storage.json`。
