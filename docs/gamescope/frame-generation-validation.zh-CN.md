# Mini 免费插帧评价（2026-11-18）

Mini 已完成交接、保留数据覆盖安装，并取得 PAL4/VirGL 的两轮性能结果。**默认关闭、初步收益明确；完整画质、其他后端和三端验收尚未完成，保持测试分支。** 基线 `main@d8128446`，分支 `feature/gamescope-framegen`，版本 2.0.5；未合 main、未推送、未新增 tag、未上架。

## 当前交付与构建

- HAP 源码：`99030044138bb832a5dbb9acadcae8c0be8ad9b2`；后续仅报告文档提交不代表重新编译。
- 调试签名包：`workspace_temp/gamescope-port/VPP-2.0.5-gamescope-debug-signed.hap`，634,086,555 bytes，SHA-256 `1c81c0eb28d8bdc42286daa836445478e9bd64e27631b53af5d31f91a0a80cb8`。
- 回退：同目录 `VPP-2.0.5-gamescope-debug-previous-signed.hap`，源码 `aad0fa63`，634,078,730 bytes，SHA-256 `3fc59fe4e9fb77637871a806d294cdd78bd1cbffa8a807314f6e5b5209afd8b5`。覆盖回退，不卸载、不清数据。
- core SHA-256 `c3f2ce19823d5ecaf3f73c620fb3c424174b7e682ba9e5de1a5a7d8a9d9fed62`；本次调度修复只重编 app-native，复用同输入 core/ArkTS。原生编译、签名/Profile、最终包身份审计通过。
- 与回退包比较只更新 `.pages.info`、`libentry.so`、`application-build.json`；Wine/FEX/DXVK/Mesa 及其他运行时载荷未变。包内 core/entry/child 的 ELF 已分配节与构建产物一致，许可及 READ_PASTEBOARD 声明通过检查。
- 保留 WSL 原有 dirty `thirdparty/dxvk`，源码收据为 local-effective-source；不能宣称干净 CI 构建。正式交付包未替换。

## 本轮定位与修复

| 提交 | 原因及处理 |
| --- | --- |
| `94fc2622` / `aad0fa63` | 初次开启黑底花屏：使用了 Gamescope 采样解码用、无 STORAGE usage 的 SRGB view。改为 UNORM 编码值/storage view，经同一生产模板和 GPU 行为测试校验；GL/Venus 去掉消费端重复限帧，修正 30→60 计算预算。 |
| `99030044` | 生成帧之后，消费端先接收新源会替换待显示的真实端点。优先提交有效的待显示真实帧，再接收 Direct/GL/Venus；旧期限、失去节拍、后台和历史重置放行新源。 |

Win-FG 仍为锁定 MIT model 3 / Balanced，不改所选图形后端。CPU 全局运动、训练与采集关闭；生产链无 CPU 截屏或整帧读回。

主机已通过真实 Vulkan 计算图、编码色阶/正确 storage view、实际运动中间位置、场景突变、130×96 非块对齐、五轮引擎生命周期、descriptor 失败及独立实例。新增生产 renderer 行为用例连同原生命周期共 **727 个检查**通过，覆盖待显示真实帧保护、GL 独立源限帧、过期/失去回调/重置/暂停/关闭放行，以及更新失败。调度、game presentation 和 embedded ABI 回归通过；主机引擎五轮不能替代设备五轮游戏会话。

## 性能条件与计数口径

Mini MLR-AL10 / Maleoon 910，PAL4 存档 2（青鸾峰、序章、0:01:52），原始固定镜头、人物待机及树叶动画；VirGL/GLES，源 800×600，Native 输出 2400×1600，截图 2560×1600 含黑边；线性缩放。NativeVSync 约 8.34 ms / 120 Hz。开关后预热 10 秒，关开各三次、每次超过 60 秒；采样期间无 UI 轮询或输入。

真实 FPS 是**接受的唯一 GPU 游戏源图像**，不是引擎逻辑/输入采样帧率；生成 FPS 排除场景切换回退，呈现 FPS 统计唯一真实图像输出与有效生成完成，重复显示不计。它们不是物理扫描率。GPU ns 来自 Vulkan timestamp，不等于 GPU 利用率。

## 第一轮：30 FPS 档（`aad0fa63`）

| 模式 | 源 FPS 中位数 | 生成 FPS 中位数 | 呈现 FPS 中位数 |
| --- | ---: | ---: | ---: |
| off | 26.18 | 0 | 26.18 |
| x2 | 28.60 | 28.60 | 57.20 |

关组呈现 25.96 / 26.18 / 26.37；开组 57.03 / 57.20 / 57.39。观察呈现增益 **118.55%**；若以理想 30 FPS 作更保守基准，增益 **90.68%**。源接收率增加 9.27% 是调度差异，不是游戏逻辑加速。平均计数对应间隔 38.20→17.48 ms，并非逐帧 p95。VPP 及子进程 CPU 占全 12 核约 19.35%→18.13%；GPU 计算采样中位数约 5.86 ms。

此轮完整六组数据仅对应 `aad0fa63`，不能改标为新包测试。新包的完整复测为下一节。

## 第二轮：修复后 60 FPS 上限档（`99030044`）

游戏实际源只有约 36.9～38.0 FPS，**不能称为 60→120 通过**；没有自动降低渲染分辨率或真实帧率。

| 组 | 秒数 | 源 FPS | 生成 FPS | 呈现 FPS |
| --- | ---: | ---: | ---: | ---: |
| off 1 | 62.94 | 37.05 | 0.00 | 37.05 |
| off 2 | 63.11 | 36.93 | 0.00 | 36.93 |
| off 3 | 63.10 | 36.15 | 0.00 | 36.15 |
| x2 1 | 62.47 | 37.99 | 37.57 | 75.54 |
| x2 2 | 62.50 | 37.84 | 37.13 | 74.97 |
| x2 3 | 63.49 | 38.00 | 37.52 | 75.54 |

| 三组中位数 | off | x2 |
| --- | ---: | ---: |
| 源 FPS | 36.93 | 37.99 |
| 生成 FPS | 0.00 | 37.52 |
| 呈现 FPS | 36.93 | 75.54 |
| VPP及子进程 CPU / 全12核 | 19.27% | 19.12% |
| CPU 核等效 | 2.31 | 2.29 |

呈现增益 **104.51%**，唯一源接收率变化 **+2.85%**；插帧 GPU 一秒采样的组中位数约 **4.95 ms**。实际逐帧 p95 与触控端到端延迟未测。

修复前 60 档的有界诊断约源 36.96、生成 23.60、呈现 50.22 FPS，真实端点被新源替换；这不是三组匹配 A/B，不能把跨包差异作为严格性能百分比。新包三组 off/x2 才是同包同场景比较。

GPU 利用率 sysfs 读取被拒，**不知道 GPU 是否满载**。进程 RSS 采样不替代长时间/五轮设备资源验证。尚未与原稳定正式包做关闭态 >5% 回归对照。

## 画质与生命周期边界

- `aad0fa63` 的有界镜头/人物移动截图中未再看到初始黑底花屏，中文存档和 HUD 可读；屋顶黑色孔洞在 off 也存在，不归因于插帧。新包 `99030044` 完成固定镜头的六组性能复测及关闭态恢复截图；没有重做运动逐帧画质验收。
- 一张转镜头截图曾被怀疑为大片绿色拖影。补拍 off 后同样存在前景绿色树叶遮挡；近似相同方向的开关运动截图未证明是 FG 独有绿色损坏。**该次归因已撤回**，保留原图和对照。截图不是逐生成帧身份捕获，细边缘、HUD 重影及快速运动完整画质仍未验收，不能宣称所有拖影已修复。
- 已覆盖升级、加载原存档、开关及正常关闭返回 Game 游戏库。没有卸载、清数据、保存覆盖或更改 HSL 数据；五轮完整设备会话、三轮 resize 和持续资源增长仍待验收。

## 剩余验收与证据

Heaven、一个 DX11 游戏、DXVK/Zink、M80 同进程/旋转/后台和 PC 窗口场景仍未验收；PAL4 既有 Zink 白地面独立追踪。真正 60→120、逐生成帧画质与旧稳定包关闭态对照仍需完成。保持默认 off，不将本次 PAL4/VirGL 收益外推为三端所有游戏通过。

固定任务目录保存 `package.json`、`previous-package.json`、`framegen-package-audit.json`、`framegen-admission-host.json`、`framegen-mini-summary.json`、`framegen-mini-admission-summary.json`，以及 `device/fg-pal4-{off,x2}-{1,2,3}.json`、`device/fg-pal4-admission60-{off,x2}-{1,2,3}.json`、`device/fg-pal4-60-short.log`、`device/fg-motion-review.json` 及配套小日志/截图。

结束恢复 FSR 1.0 / sharpness 10 / 跟随屏幕 / 插帧 off / HUD 保持开，关闭临时诊断、正常退出并释放 Mini；具体执行收据见 `framegen-device-restoration.json`。维护只保留两代调试包，删除已核验 Windows 留存的 Linux 签名重复和无用 probe；磁盘变化见 `framegen-storage.json`。
