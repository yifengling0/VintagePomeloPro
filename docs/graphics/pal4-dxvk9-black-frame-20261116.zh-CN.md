# PAL4 DXVK9 黑帧修复与性能差异

## 结论与适用范围

Mini 上 PAL4 的反复黑帧可由额外前缓冲轮换触发。旧 DLL 单独设置 `d3d9.noExplicitFrontBuffer = True` 后消失；回移上游按交换模式决定是否分配前缓冲的行为后，不加该配置也通过了短时真机验证。字体不再作为本次问题处理。

性能问题独立存在：同一青鸾峰屋前存档、800×600，VirGL 约 44～46 FPS，DXVK9 约 35～38 FPS，修复 DLL 截图为 36 FPS。这些是有界场景观察，尚不是热状态匹配的 3×60 秒基准；不能据此承诺整款游戏的收益。

## 实际链路和黑帧原因

- 设备：Mini MLR-AL10；安装包仍为 2.0.3 / 2000030，ARM64 调试签名。
- 路线：ARM64 Wine / PE32 FEX → DXVK 1.10.3 D3D9 → Venus → Maleoon 910。不是系统 Vulkan 直接透传。
- 新 DLL 的实际日志：`D3D9: Backbuffer policy: effect=1, windowed=1, backbuffers=1, explicitFrontBuffer=0`。
- `effect=1` 是 DISCARD。上游测试发现窗口模式、单后缓冲的 DISCARD 与 COPY 一样保留后缓冲内容。旧实现额外分配一个前缓冲并在 Present 后轮换，可把未更新的黑色缓冲换回来。
- 黑帧时 DXVK HUD 仍可显示；结合单变量对照，更支持 D3D9 缓冲内容保留问题。不能由此证明其他游戏不存在外层呈现问题。
- Vulkan 日志的 `Exclusive FS: 2` 不是 D3D9 的 Windowed 标志；本次以真实 D3D9 参数判定修复条件。

补丁 `patches/dxvk/0020-d3d9-preserve-copy-and-windowed-backbuffers.patch` 选择性回移 [上游 feebec959d3d](https://github.com/doitsujin/dxvk/commit/feebec959d3d23c3503964e89732c9d7dc28dde5)（[PR 3392](https://github.com/doitsujin/dxvk/pull/3392)）：COPY/COPY_VSYNC 和窗口单缓冲 DISCARD 不额外轮换前缓冲；保留 fullscreen/FLIP/多缓冲行为及已有 `noExplicitFrontBuffer` 显式退出选项。没有启用新的全局兼容配置、增加 GPU 等待或导入上游 GDI 回退重构。

## 单变量真机对照

| 改动 | 黑帧结果 | 性能或判定 |
| --- | --- | --- |
| 原 DXVK9，批量发布开启 | 屋前 HUD 场景 16 张中 15 张场景区域为黑 | 约 34～39 FPS；黑场景时 HUD 可见 |
| 关闭 mapped flush batching | 屋前 16 张中 13 张为黑 | 约 21～22 FPS；排除为产品修复 |
| `allowDirectBufferMapping=False` | 屋前 16 张中 14 张为黑 | 不能消除问题 |
| `maxFrameLatency=1` | 标题 16 张中 6 张为黑 | 不能消除问题 |
| 旧 DLL，单独关闭额外前缓冲 | 标题及屋前各 16 张均未采到黑帧 | 约 35～38 FPS |
| 同上，恢复 pal4.exe 原入口、关闭诊断 HUD | 屋前 16 张均未采到黑帧 | 排除必须经过诊断 CMD 才有效 |
| 新 DLL，删除 dxvk.conf，保留正常默认 | 标题及屋前各 16 张均未采到黑帧；游戏菜单和返回场景正常 | 观察约 36 FPS，实际交换链参数命中修复 |

计数使用屏幕 ROI `(500,350)-(830,620)`，排除 HUD，RGB 各通道均值均小于 3 才计黑。它是截图抽样结果，不是连续视频逐帧计数。早期整个客户区的自动分类受 HUD 干扰，不能用旧 JSON 的 false 标志否认黑帧。`dxvk9-baseline` 序列不作为以上已读档场景的负对照。

## 为什么 Heaven 有收益，PAL4 却慢

Heaven 既往约 8.9% 的收益仅为探索性结果：每项只有两次，热状态未完全匹配，历史记录还出现过实际运行 DX11 的误路由，不能推导为所有 D3D9 游戏都快。不同游戏的绘制批次、状态变化、动态数据及同步要求不同，跨进程图形协议的成本也不同。

PAL4 当前证据：

- 同场景旧样本中，VirGL 游戏进程约 172～174% CPU，DXVK9 约 129～133%，100% 表示一核。CPU 用得更少而帧率更低，提示应检查串行工作和等待；这不等于“提高线程数就能解决”。这些值不包含所有接收端进程成本。
- DXVK HUD 场景样本约 807 draws、10 render passes、5 submissions/frame；HUD 刷新周期不同，不能把单张瞬时数字与另一张逐帧对应。
- 黑帧已修复配置下，8 秒 `hiperf --offcpu` 中 dxvk-submit 根约累计 6.35 秒，其中嵌套调用链大量落在 `thunk32_vkFlushMappedMemoryRanges`；dxvk-queue 主要经过 `thunk32_vkWaitForFences`。这包含驻留/等待，不能把嵌套百分比当作全游戏 CPU 或帧时间占比。
- 另一次 6 秒、300 Hz 的 on-CPU 采样中，一个 dxvk-cs 根占全部采样 22.45%；其下一层约 48.08% 经过 `thunk32_vkAllocateDescriptorSets`，进入已核对身份的 Venus 库。部分 FEX/JIT 地址仍未符号化。
- Venus 当前 `vn_FlushMappedMemoryRanges` 在远端内存同步开启时会做同步远端调用。已有 DXVK 批量发布显著减少损失，但仍需研究发布、descriptor 分配和命令录制的跨边界成本。

因此下一步优先分析并减少远端内存发布往返，其次是 descriptor/命令录制成本，再关联 fence 等待与接收端/GPU 时间线。保留数据可见性和提交顺序，不能直接关闭 remote memory sync。只有具体改动后再做同条件 A/B。

本次没有获得可靠的硬件 GPU busy。DXVK HUD GPU% 来自其队列空闲统计，不是 Maleoon 硬件忙碌率；fence 等待也可能包含 GPU 执行成本。现在不能断言 GPU 未满，或把全部性能差距归到 CPU。

## 构建、身份和验证

- 基线分支 `fix/zink-shadow-transfers`，应用提交 `9f2fe8c5`；未移动任何 Wine/Mesa/DXVK gitlink。
- DXVK pin `5dff1d7b3dc3c2cd7f51356b99213bcc427b1af9`，既有 19 个 overlay 后追加 0020。20 补丁从干净 pin 重放、重复应用及两份锁的补丁摘要一致性通过。
- patched src SHA-256：`8760581afc626aaafe031431507ef0db945737a5ef79e4045cacd44f5dad2203`。
- `host_tests/dxvk9_backbuffer_preservation_test.py` 提取实际策略、分配语句和 Present 轮换循环；32 组行为及内容保留/轮换检查通过 ASan/UBSan，旧策略在同一组断言下失败。既有 overlay 三项测试通过。
- 在独立 `winehua-dev` 容器经 `make NATIVE_ARCH=arm64-v8a JOBS=8 dxvk-d3d9` 构建 PE32 D3D9；该目标复用标准 Meson 缓存，不触发 Wine/ARM64X 构建，也不伪造完整 DXVK stamp。
- 初次构建缺少原相对 include 需要的 bcdec.h。只补齐 71,166 字节的头文件，已与当前 Proton pin 和原 Wine 头文件逐字节比较一致；没有构建或替换旧 Wine。
- 新 DLL：5,792,432 字节，PE I386；SHA-256 `d397536ee35d6cdc7ef7a8ac5c57d1d34b15662806262b9fa449cad6d2249775`。Mini 沙箱写入后取回逐字节一致，运行日志确认新策略执行。
- 原 DLL 唯一回退副本：4,300,800 字节；SHA-256 `e7dbcc95fc326ee576039bd007751b0eafd7e6624702b8d42dcfd0ccb79c8029`。
- 两份源码锁登记新补丁，runtime 锁的 PE32 D3D9 摘要更新到新构建产物；其他 DLL 未更新。

证据固定保存在 `F:/VintagePomelo-Workspace/workspace_temp/pal4-render-review/`：`validation.json`、`roi-black-frame-review.json`、`patched-*-contact.jpg`、`patched-final-scene-detail.png`、`patched-d3d9-runtime.log`、`front-buffer-{perf,oncpu}.data` 及相应报告。新 DLL 和原 DLL 也在此目录，未复制完整运行时或游戏资源。

本次是主机行为、单 DLL 构建和 Mini PAL4 短时运行验证；未产新 HAP、未上架、未合 main、未推送。新库当前只热替换在 Mini 的调试运行时，重新安装或升级可能覆盖。fullscreen/FLIP 有主机行为覆盖，但其他游戏、其他设备和长时间运行尚未验收。不能将本结果当作 Heaven 新版基准通过。

收尾恢复 `PAL4/pal4.exe` 原入口，删除临时诊断 CMD/日志、前缓冲配置和设备端候选临时副本。PAL4 主程序、config.cfg 和既有存档/缩略图摘要与开始时相同；未清数据、重置 prefix 或覆盖存档。空间清理结果见工作区 `logs/storage-maintenance/latest.json`。
