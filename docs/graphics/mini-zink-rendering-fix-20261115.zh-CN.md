# Mini Zink 渲染修复与测试包（2026-11-15）

Zink 已从 Heaven 黑屏/灰色波纹推进到普通启动可见正确场景。必须区分初始画面正确、完整场景运行和性能结果；本文后面的运行记录才代表最终覆盖范围。Mini 仍经 Venus，不能描述成系统 Vulkan 直通。

## 按 VirGL / DXVK 的正常行为逐段定位

1. **窗口合成**：ARGB 游戏子层未被判为可直传时，旧路径错误地直传黑色父窗口，跳过子层。现在有可见子层就禁止父窗口单独直传；按提交序号缓存 alpha 分类，仅真正完全不透明的单子层可直接缩放。透明/多子层继续正常合成。生产 compositor 71/71，ASAN/UBSAN 通过。
2. **着色器阶段接口**：真实 Heaven shader 的 `distance.z` 被放入 Location 4 / Component 1，Zink 链路读取成 0。0022 让 generic float32 varying 保留完整 vec4。实际 shader 21 输出与 VirGL 逐字节一致；独立探针同包关闭 3/15 失败、开启 15/15 通过。157 份真实场景 SPIR-V 均无 Component。SPIR-V 验证使用 Vulkan 1.2 + scalar-block-layout；不是不带该布局条件的全量证明。
3. **3D 调色纹理**：真实调色前 sampler0 已有正确建筑/天空/光照；最后一个 pass 才产生灰色波纹。VirGL/Zink 的原始 PS 字节码、CPU 侧 32³ LUT 完全相同。实际上传是 PBO + BGRA/UNSIGNED_INT_8_8_8_8_REV，行距/层高均32。源131072字节正确，GPU只有第0层4096字节正确，后31层损坏。诊断拆为32次 depth=1 后两份纹理均零字节差异，最终颜色恢复。

0023 将逐层上传落实到 Mesa `st_TexSubImage`，开关 `WINEHUA_ZINK_SLICE_3D_UPLOAD=1`。只在3D、PBO、多层上传时生效，使用 Mesa 自身的 image stride，保留源offset、pixel-store skips、目标xyz及原有传输实现。没有加入每帧全局等待，也没有绕过游戏调色。开关源代码默认关闭，仅产品 Zink 分支启用。

独立 volume 探针覆盖完整32层、非零PBO offset+padding+skip，以及非零xyz的局部更新：r8三项都坏31层，r9默认三项全部字节一致。根因已限定到当前栈多层PBO传输路径；尚未定位到底层 Vulkan 驱动某条具体指令，不能扩大为所有GPU通用缺陷。

## 产品与验证

- 已打包系统及单游戏 `DXVK9（32位实验）` / `Zink（实验）`，保留“跟随系统”。自动化只覆盖当前会话，不再写用户全局后端。
- 设置实际代码模拟通过：继承、持久化、旧设置、设备回退、跨游戏重置及串行启动。
- r9 Native、Mesa 增量编译成功；完整 Mesa patch overlay reverse/replay 检查通过。
- UI资源整体与已编译UI包逐文件校验，非局部替换图标；新包仅相对r8替换 `libentry.so` 和 Gallium。
- graphics-stack 原有0011 softpipe patch SHA及此前mfplat source lock漂移仍存在；没有重写hash掩盖。r9是dirty source诊断overlay，不是完整正式release parity。
- 仙剑4、其他游戏、长时间稳定性尚未验收；不能把Heaven成功推广到所有D3D9游戏。

## 包身份

`VPP-2.0.2-mini-zink-r9-signed.hap`，629683606 bytes，SHA-256 `5a1cc06c61984ca66196c701d289b6bb0464e06242c3f2a9ed822f909d280290`。

Gallium SHA-256 `b0f8eb478dd4cbdbfc071a8a8bde530837cd88e0575e4bf1774f53f8549cf9f0`；libentry SHA-256 `d385c7c08912d312cea2f39c9611b586da87b7a325a5c964b950f5aa2f421112`。

测试只操作 Mini；原 Heaven.exe 与两份配置 SHA 保持；所有测试runner持 mini.lock并在finally清除公共临时文件。无push/CI、无手机/Edge操作、未改gitlinks。

## 证据位置

固定任务目录 `workspace_temp/mini-virgl-performance/backend-heaven-comparison/`：

- `zink-independent-varyings-off` / `zink-independent-varyings-on`
- `zink-color-pass-capture` / `virgl-color-pass-capture`（真实中间RT）
- `zink-lut-upload-direct-hook` / `zink-lut-slice-upload`（相同源数据、逐层GPU差异）
- `zink-volume-r8-regression` / `zink-volume-r9-default`（独立PBO回归）
- `zink-r9-heaven-full` 首次普通启动场景正确，但benchmark开始后OCR把Cancel读作CanceI，runner提前停止；不是有效完整跑分。

`rt-comparison-device.py` 的 ready=true只表示采集完成，不能冒认视觉验收。`virgl-gpu-lut-capture` 的替换PS因c223超过该路线限制而失败，不能作同shader对照；正常color-pass两路线对照有效。关闭buffer_storage的实验没有改善，但没有充分证明实际扩展开关生效。
