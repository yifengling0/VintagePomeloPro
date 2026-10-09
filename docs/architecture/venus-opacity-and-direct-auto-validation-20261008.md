# Venus 不透明合成修复与 Direct 自动选择验证

日期：2026-10-08。分支 `feature/main_proton`，基线 HEAD
`4cb9e7c8be2acf7545976f0699ef064e796cb2fd`，包含本地尚未提交的修改。
本文更新 `tr32-direct-precise-and-venus-status-20261008.md` 的后续状态。

## 结论

TR32 的泛白发生在宿主最后合成，不是游戏生成的 RGB 泛白。
同一帧的两个 DXVK RGBA 读回都颜色正常、alpha 全为 0，屏幕截图却泛白。
Wine 私有 Venus WSI 只支持 `VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR`，
宿主错误地使用了图像中的 alpha 进行预乘混合。

修复包在 MatePad Mini / Maleoon 910 上用明确 Venus 会话运行 TR32，
片头和主菜单恢复正常颜色。DXVK 日志是 `Virtio-GPU Venus (Maleoon 910)`，
没有用 Direct 绕过该验证。

普通启动也已实际通过 Direct 自动检测：4 帧 guest 图像导出、host 导入、
像素校验和双向 fence 交换通过，收到 Vulkan 资源析构完成标记和 IPC death。
随后无 LAB 参数的产品会话使用原生 `Maleoon 910`，显示片头、菜单和实际游戏。
手机依然保留 Venus；本轮没有手机真机验证。

## 为什么 Direct 没有泛白

原 EGL 路径把所有 NativeImage 图层标成 `opaque=false`，启用
`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`，外部纹理 shader 原样输出 alpha。
TR32 帧的 RGB 正常，但 alpha=0，于是与白色 SHM 父窗口相加，出现过亮／泛白。

Direct 桌面合成的 Vulkan 窗口层按不透明处理，shader 根据层属性强制 alpha=1，
因此相同游戏可以正常显示。这个差异不是 x86／x64 指令转译导致的。
以前只用 alpha=1 的蓝色探针无法暴露此错误；64 位游戏是否触发取决于帧内容，
不能由位数推断合成正确性。

关键证据为外部目录的 `venus-cpu-frame300-color.bin`、
`venus-cpu-frame300-source.bin`、`venus-cpu-frame300.jsonl` 和
`tr32-venus-cpu-ingame.png`。两份 800×600 RGBA 的平均值均为
`[40.1757604, 41.6531396, 42.8787375, 0.0]`。
此对照只关闭宿主 GPU upload，保留产品的 precise flush 和 ring ordering，
仍然泛白，不能将问题只归因于 GPU upload。

对应运行时 Wine 来源的 `dlls/win32u/vulkan.c` 中
`winehua_surface_capabilities()` 只广告 OPAQUE。这里的修改只适用于这个合同；
未来若支持其他 compositeAlpha，必须将实际选择传递到宿主，而不能继续无条件强制。

## 合成修复

- `graphics/egl_renderer.cpp`：Venus NativeImage 来源按 `vulkanSource` 标记不透明；
  根据图层属性控制 blend；桌面和单窗口绘制均设置 `uForceOpaque`。
- `graphics/shader_utils.cpp`：外部纹理 shader 按该属性只改输出 alpha 为 1，
  保持 RGB 原值。
- GL／CEF 的 NativeImage 不因此强制不透明；上方 ARGB SHM 菜单仍正常混合。

`host_tests/egl_multi_consumer_test.py` 执行生产绘制循环，覆盖 alpha=0、0.5、1
的 Vulkan 层盖白底，半透明 GL／CEF 层，以及游戏上方的半透明 SHM 菜单。
平台函数为替身，这不是 GPU 驱动或所有真实 CEF 窗口的真机验证。

## 自动检测修正

先前的自动检测使用 `StartNativeChildProcess`，连续在
`writer_instance` 得到 `VK_ERROR_INCOMPATIBLE_DRIVER(-9)`。
实际能运行 Direct 游戏的路径是 `CreateNativeChildProcess` + IPC。
因此先前失败不能证明平板不支持 Direct。

自动检测现在通过 `StartWineChildViaIpc()` 进入同种子进程上下文，
以白名单入口 `__winehua_direct_buffer_ipc_probe__` 加载共享图像探针。
探针不启动 Wine 游戏、不注册为游戏进程。能力只有在完整合同通过后才批准，
检测失败仍回退 Venus。IPC death 没有系统退出码，不能把它伪装为退出码 0；
另行要求 `child_cleanup_complete`，证明局部 Vulkan 对象已析构。

2026-10-08 23:06:55.807 真机日志：

```text
[DirectAuto] verified=1 stage=complete childStage=child_complete vk=0 launch=0 frames=4 device=Maleoon 910
Direct capability supported=true stage=complete device=Maleoon 910 frames=4
```

23:07:01.660，产品入口启动 `TombRaider.exe`，`graphicsPolicy=(product)`；
随后 DXVK 日志的 device 为 `Maleoon 910`，宿主 `direct=1`。
截图 `tr32-product-auto-create-launcher.png`、`tr32-product-auto-create-menu.png`、
`tr32-product-auto-create-ingame-before.png` 分别覆盖片头、主菜单和加载存档后的游戏。
帧率随场景变化；菜单约 40 FPS 不能当作实际游戏的稳态 FPS。
未做同场景、同温度的 Venus／Direct 性能 A/B，本文不宣称优化倍数。

产品策略仅对通过检测的 legacy DXVK 自动配对：

```text
WINEHUA_VULKAN_BACKEND=direct
WINEHUA_VK_PRECISE_MAP=1
DXVK_WINEHUA_PRECISE_SHADOW=1
DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1
```

未经检测、检测失败、手机、modern DXVK 和 VKD3D 继续 Venus。
明确的 Venus LAB 会话不受自动选择影响。
PAL4／RichMan8 的 WineD3D／VirGL 路径与 FEX／x87 设置不变。

## 复制和性能的边界

Direct precise-map 的改动减少 WOW64 shadow 回退在 QueueSubmit／Unmap 时
额外的整块复制，仍保留 DXVK 明确范围的 flush／invalidate。
驱动映射不能安全共享到低地址时，局部 shadow 复制仍是正确性所需。
这与展示图像是否通过 CPU 读回、上传是两个层次。

此次产品 Direct 实际游戏日志中 `gameCpuReadBytes=0`、`gameCpuUploadBytes=0`，
但这两个计数只覆盖展示路径，不能证明游戏所有资源零复制。
Venus 的泛白修复没有取消命令转发、ring 同步和资源传输；
其性能仍需要单独调查，不能因为颜色正常就宣称复制开销已消失。

## 验证、包和剩余问题

已安装包 `venus-opacity-auto-create-signed.hap`，SHA256：

```text
9926dc57acd34966dc099792e39bcbc69b769fae39227c26c39d3aba7977cfb8
```

Native、ArkTS、PackageHap 构建通过。Docker 内 SignHap 因本地签名材料认证失败，
改用既有 Windows 签名脚本成功，未改签名 profile。

已通过专项与相邻检查：

```text
make test-egl-multi-consumer test-direct-session test-venus-present-retry
make test-zc-binding-lifecycle test-gpu-followup test-broker-startup
git diff --check
```

本轮追加的 PE32 alpha=0、PE64 alpha=0.5 探针，在明确 Venus 会话中均完成
4 轮 256 KiB buffer、4 轮纹理上传／回读（全部 bad=0），120 次 Present PASS。
文件包含新 alpha 字段，已核对执行时间，未沿用旧 TSV。
采集时前台回到了 ArkUI 程序列表，因此这些截图不能当作其颜色验证；
颜色的实际真机证据来自上述 TR32 片头和主菜单。
PE32 探针结束时仍有 exit=11 记录，不能宣称完整退出生命周期已修复。

安装后首次 Wine 启动出现一次 `NCP exit callback register FAILED rc=16000050`，
Wineboot 已退出但宿主未收到权威退出通知，最终等待 180 秒失败。
force-stop 后再次启动 callback rc=0、Wineboot 正常结束；随后产品自动 Direct
启动也正常。本轮没有修改 Wineboot 等待逻辑，该偶发系统回调问题仍需独立定位。

外部证据目录：
`F:/VintagePomelo-Workspace/workspace_temp/gumu10-dx11-20261008/`。
命令台账 `device-commands.json` 记录目标、时间、超时和脱敏。
构建及测试日志位于仓库 `workspace_temp/venus-opacity-*-20261008.log`。
本轮尚未 commit／push，手机真机、长会话及现代 DXVK／VKD3D Direct 未验证。

## 用户要求切回 Venus 的性能参考

23:18:54 冷启动明确的 `observe-product-summary` Venus 会话，仍用 legacy DXVK、
FEX 和 800×600；随后通过启动器进入游戏并选择“继续”，加载现有存档。
当前保留 Venus 会话供用户操作，未再次自动切回 Direct。

| 路径 | 实际游戏参考窗口 | 帧统计 |
| --- | --- | --- |
| 产品自动 Direct | 23:16:00.329–23:16:29.430，30 个约 1 秒样本 | 平均 35.207 FPS，33.81–36.97 |
| 明确 Venus | 23:23:15.438、25.482、35.518、45.568，4 个约 10 秒样本 | 平均 17.555 FPS，17.52–17.61 |

Venus 同一 NativeImage 的消费计数从 23:23:14.075 的 2880 帧增至
23:23:48.325 的 3480 帧，600/34.250 = 17.518 FPS，与宿主帧统计一致。
截图仍显示正常颜色，DXVK 设备明确为 `Virtio-GPU Venus (Maleoon 910)`。

这不是严格 A/B：虽然是同一包、游戏、存档和分辨率，角色位置／镜头不同，
Direct 已运行更长时间；23:18:19 切换前截图还显示系统桌面，因而没有采用
那一刻的后台帧统计作基线，而采用有实际游戏截图支持的 23:16 参考窗口。
不能由表中两组数值断言固定的性能倍率，或将差距全部归为协议编解码。

Venus 稳定段 `failed_swaps=0`，EGL `upload_bytes=0`、`upload_calls_issued=0`，
测到的绘制循环活动段平均约 5.4–5.8 ms，而每个新游戏图像间隔约 57 ms。
这表明不能用最终桌面 CPU 纹理上传来解释主要差距；计时不包括所有等待，
也不是 GPU timestamp，仍需区分上游生产、GPU 执行、传输和同步。
这里的零上传只覆盖 EGL 展示图像，不能证明 Vulkan 资源上传零复制。

代码中仍有值得计时的具体点：

- `vn_device_memory.c` 的 remote-memory flush／invalidate 使用同步的
  `vn_call_vkFlushMappedMemoryRanges`／`vn_call_vkInvalidateMappedMemoryRanges`，
  映射频繁时会产生 CPU 往返。
- `vkr_device_memory.c` 在复用 in-flight shadow-upload slot 时等待 timeline／fence；
  即使关闭额外的 upload-wait 开关，也不能直接复用尚未完成的 slot。
- Guest ring 的通知、mutex、seqno／roundtrip 等待和单 ring 策略需要分别计时。
  strong publish barrier 本身是 CPU 内存屏障，不等于 GPU 全队列 idle。
- Venus 与 Direct 的设备能力广告及格式／shader 回退可能不同，需要对照日志，
  不能仅凭两边使用同一块 Maleoon GPU 就认定生成的 GPU 工作完全相同。

下一步优先利用已有 Guest ring／Host upload 分阶段统计归因，再决定批量 flush、
减少往返或改进 slot 周转；不直接删除维护跨进程数据可见性的同步。
本次没有开启这些较重诊断，表中帧率来自现有默认级别统计。

证据：`tr32-ab-venus-restart-conditions.json`、`tr32-ab-venus-runtime-route.txt`、
`tr32-ab-direct-reference-window.txt`、`tr32-ab-venus-reference-window.txt`、
`tr32-ab-venus-sample-end.txt`、`tr32-ab-venus-ingame-before.png`、
`tr32-ab-venus-ingame-after.png`，均在上述外部证据目录。
