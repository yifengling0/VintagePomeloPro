# Venus 与 Direct 性能差距：宿主呈现实验和实际映射回退

日期：2026-10-09。权威仓库 `/home/liufeng/src/vpp-proton`，分支
`feature/main_proton`，基线 `4cb9e7c8`，包含前序未提交修改。本轮未提交或推送。

## 当前结论

1. 已恢复旧 main 风格的 Venus 宿主 NativeBuffer/release-fence 实验，并在平板
   确认实际命中。游戏仍使用 Venus；这不等于 guest Vulkan Direct。
2. 这项呈现调整没有证明明显帧率收益，不能继续把 Direct/Venus 的全部差距
   归因于最后一次 WSI 呈现或末尾 CPU fence wait。
3. 新诊断确认当前 PE32 Tomb Raider 的 Venus 映射实际进入 Wine WOW64 CPU
   shadow 回退。此前“代码保留了 fd，所以游戏应当没有额外复制”的推断不成立。
4. Direct 曾修复的范围发布机制有直接参考价值，但仍未量出本游戏每次提交的
   实际复制字节和耗时，也未证明复制解释了约 10 FPS 的全部差距。

## 安装与候选身份

平板：MatePad Mini / Maleoon 910，bundle `com.vintage.pomelopro`，
已安装 `1.4.5.29 / 1004038`。

候选 `venus-native-present-signed-20261009.hap`：

```
SHA256 515f4a30f2ff7596466b0ea94b070e174a9cbadd4c53bafae9726396016cc8ce
libentry.so      7212298c8978a79775bfabf4c817e3061573b207787de2f5757c0a536ae55e4c
libvirgl_child.so 91bfa062879c4475af42cead110e153d5431c0ac398fec9e58936cf86d7a51a9
```

Wine `win32u.so` / `winevulkan.so` 和 Mesa `libvulkan_virtio.so` 的包内 `.text`
均与用于源码、反汇编核查的本地符号文件一致。完整文件哈希不同是打包 strip
导致，不能用完整文件不同误判旧运行时；结果在
`venus-map-timeline-analysis-20261009.json`。

## 宿主 NativeBuffer 实验

LAB `isolate-venus-native-buffer` 仅用于 legacy DXVK。
保留 FEX、Venus、精确 dirty-range 上传策略及游戏分辨率 800×600。

成功路径为 request/acquire → GPU copy → OHOS signal release → NativeWindow
flush；末尾不由 CPU 等待当前复制。循环 frame slot 重用前仍检查完成。
失败锁定 WSI 回退，重建或切换前等待 outstanding GPU 工作。

游戏日志确认 `Virtio-GPU Venus (Maleoon 910)`；宿主确认：

```
transport=direct-native-buffer post_present_cpu_wait=0 slots=2
```

两次游戏启动成功，本轮未见该实验的 Native fallback、failed swaps 或泛白。
修复了设备 release 等待失败时提前销毁对象的路径，以及重复 reset fence。
等待失败时 Abandon，只清理本地状态，不把未完成 buffer 当作可安全回收。

通过 `make test-venus-native-present test-direct-session`，相邻呈现回归及
`git diff --check`；OHOS 原生库与 Hvigor PackageHap 成功。Docker SignHap 仍受
已有加密材料错误限制，使用 Windows 既有签名流程生成测试包并安装成功。
测试提取执行实际 NativePresentLocked / EndFrame / FinishDeviceRelease，
覆盖 acquire、record、reset、submit、release、flush 和失败所有权。

整个 Ensure lifecycle 的故障注入仍不完整，Native rebuild QueueWaitIdle
也可能无界等待。实验保持默认关闭，不能以目前验证范围晋升产品默认。

### 帧率记录及限制

| 窗口 | 新图像统计 FPS | 场景和限制 |
| --- | ---: | --- |
| 旧包 WSI `venus-native-old-cave-20261009` | 18.355 | 洞穴站立，较早且温度较低 |
| 候选 Native `venus-native-candidate-cave1-20261009` | 16.594 | 镜头变化，表面约 43.51°C，电池 38°C |
| 同候选 WSI `venus-native-candidate-wsi-cave1-20261009` | 17.619 | 洞穴另一方向，表面约 43.4°C，电池 38°C |
| 第二次 Native `venus-native-candidate-native-cave2-20261009` | 18.623 | 实际为开场动态画面；文件名含 cave2，不能列为洞穴对照 |

Native 的源提交统计与显示统计接近，未见显著丢弃。上述场景、温度和交互
不严格匹配，而且 WSI observation 开启 guest summary，Native LAB 未开启。
不能算精确退化比例；这组记录只是不支持呈现调整带来明显约 10 FPS 收益。

Native 累计 present_us_avg 约 0.8–0.9ms，仅覆盖 NativePresentLocked，
不含外层循环 frame fence 等待，也不是 GPU 执行时间。

## Direct 当时的哪些修复值得参考

配对源码是 `workspace_temp/wine-direct32-clean-v3-source-20261008`，
不是未应用完整补丁的原始 thirdparty Wine 树。

`0047-win32u-wow64-vulkan-map-safety.patch` 的核心作用：

- 修复固定低地址 mmap 失败后向失效 VMA memcpy 的崩溃；
- 正确处理实际 MapMemory offset/size；
- 配对 DXVK 显式 flush/invalidate，在 precise 模式取消 QueueSubmit/Unmap
  对整个存活 shadow 映射的额外上传；
- 保留无法建立共享别名时的正确范围复制和 GPU 回读。

Direct 使用 `WINEHUA_VK_PRECISE_MAP=1`，同时启用 legacy DXVK 的
`DXVK_WINEHUA_PRECISE_SHADOW=1` / `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`。
同源 PE32 资源测试两侧数据均 PASS，120 次提交耗时 1992.737ms 对
1201.126ms。它是合成负载提交耗时，不能用作真实游戏帧率倍率。
后来游戏约 38 FPS 有记录，但与之前低帧率窗口的场景不同，不能把全部改善
归因于这一个修改。

此前 wineserver O2 修复在 PE64 ROTTR 的实际对照约 14.916 / 14.871 FPS，
没有帧率收益，不应当作当时明显变快的原因。

## 新真机证据：Venus 确实命中 CPU fallback

本轮单独启动 `observe-frame-timeline`，设置
`WINEHUA_WINEDEBUG=-all,err+vulkan` 和 guest summary，仅用于诊断。
没有全局修改 precise-map 或产品路由。启动前备份了 Tomb Raider 203160 存档，
没有通过测试脚本主动选择 New Game 或覆盖存档。

新会话明确输出：

```
WOW64 could not open backing fd for map 0x7002c60000 vma=1
WOW64 copying vkMapMemory 0x7002c60000 -> 0x5810000 (CPU fallback)
```

最终过滤尾段包含 12 次 fallback map 事件、8 个不同的 host 指针；这些不是
存活 map 数量，也不是每次 submit 的复制次数。尾段未见成功 alias。
日志和启动条件在 `venus-map-timeline-final-evidence-20261009.txt` /
`venus-map-timeline-20261009-conditions.json`。

静态代码确认 Venus 产品配置仍为 `WINEHUA_VK_PRECISE_MAP=0`。
`winehua_wow64_flush_copies()` 对每个仍存活的 non-precise copy map，在提交时
memcpy 整个有效 map_size。它与 Direct 的范围发布是实质差异。
“Venus host shadow_bytes=0”并不能排除此层 Wine 复制：计数属于不同层。

本地 Mesa vtest_bo_map 保留 fd 的修改已进入当前包：包内 `.text` 核对通过，
对应反汇编成功分支没有关闭 fd。因此不能简单认为此次 fallback 只是忘记
部署 fd 保留补丁。

Wine fd 查找依赖 VMA inode、map_files 访问及 fstat 的设备号/inode 匹配。
当前错误文案不能区分 inode 缺失、fd 不匹配和权限拒绝。已有 OHOS proxy
源码专门处理 sealed anonymous file 的 fstat 被沙箱拒绝而 mmap/fcntl 可用，
这是值得验证的适配疑点，但**本轮没有取得该游戏 fd 的 errno，尚未证实它
就是本次 fd 查找失败的直接原因**。外部 shell 读取游戏 /proc/maps 被拒绝。
不能放松身份检查后猜测任意 fd；优先考虑让映射提供者传递准确身份或直接
创建低地址共享视图。

## 上传和等待的其他实际线索

最终 25 个 host 采样帧：每帧 7 次 QueueSubmit，GPU upload 约
2.11–2.29MB / 587–888 个范围，prepare_us 7.394–13.165ms，host submit
总 CPU 阶段约 10.375–20.434ms。这些是诊断帧，不能代表关诊断后的净成本。
upload_us 接近 0 表示采用 inline GPU 记录路径，并非 GPU 上传耗时为 0。

guest summary 的 ring seqno、fence status 和同步回复时间持续增长。
CPU 采样的返回地址指向 QueueSubmit 调用，以及 Mesa ring_wait_seqno。
这支持继续拆分上传、同步 RPC 和 ring 等待，但不能把堆栈返回地址的占比
直接称为 memcpy 的 CPU 占比。不同线程的累计等待可能重叠，不能相加得
“每帧损耗”；也没有测 GPU timestamp。

generation lock 的现有 contended=0、累计等待很小，暂不支持把互斥锁视作
主要帧率差距。

## 后续最有价值的修复路线

1. 在默认关闭诊断中补齐 fd 查找阶段及 errno、alias/copy 命中数、实际 submit
   复制字节与时间。先证明回退的直接原因和具体成本。
2. 优先让 Venus 与 Wine 可靠共享同一低地址 backing；绕过不适用于 OHOS
   的 proc/fstat 猜测，保留生命周期和准确身份。这可以删除 Wine→guest
   这一段复制，但不会自动删除 guest→GPU 的上传。
3. 同时可用 legacy DXVK 的显式发布合同做隔离的 Venus precise-map 对照，
   先验上传/回读、动态常量/顶点、纹理、加载和重建正确性，再关诊断测 FPS。
   原生 coherent Vulkan 应用未必主动 flush，不能全局直接取消复制。
4. 如果 Wine 复制已消失但差距仍在，再减少 host 范围扫描和 GPU upload
   prepare 成本，合并同步发布、核对已有 DXVK batching，并改善 fence status
   的轮询 RPC；保留真正的完成约束和回读时序。

手机继续保留 Venus 回退；本轮没有手机性能或正确性结论。
FEX/x87、PAL4/RichMan8 的 WineD3D/VirGL、Direct 自动能力选择均未改变。
