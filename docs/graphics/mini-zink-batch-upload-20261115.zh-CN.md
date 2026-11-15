# Zink 按 batch 发布私有上传（2026-11-15）

0024将shadow映射下的小块上传按zink_batch_state合并，在录制线程交给submit worker之前发布；持有捕获resource object及BO mapping，不跨未写间隙合并。仅非exportable的内部单线程buffer且WRITE|UNSYNCHRONIZED|FLUSH_EXPLICIT进入，其他路径即时发布。READ、PERSISTENT、ONCE、THREAD_SAFE及TC异步映射排除。

256 ranges/call、4096项pending上限；CPU READ前drain，reset/destroy释放。早期flush失败保留batch lost，后续空drain仍失败，不允许丢失错误后继续提交。Mesa层默认关闭；wine_env仅Zink产品分支启用WINEHUA_ZINK_BATCH_UPLOAD=1。每游戏设0可回退；STATS默认关闭。

增量Mesa/Native构建、完整patch stack reverse/replay、host_tests/test_zink_batch_upload.py普通与ASAN/UBSAN通过。测试使用真实生产函数及筛选表达式，覆盖范围、引用/映射寿命、失败路径、lost持续、筛选排除与提交顺序。

Mini r10同包开关：flush约7927/s→344/s（下降95.7%），自由镜头Heaven短采样2–3FPS→5–9FPS；镜头/温度未固定，不能冒认完整平均提帧。固定500draw上传微负载off/on/off预热后均值42.776ms→16.635ms（约61.1%耗时下降），仅一个小负载序列。

r11已安装，正常Zink入口默认命中。500次独立可见uniform更新一次读回全部正确（500 inputs→1 call/1 range）；显式关闭同样正确且无batch日志。15项varying检查通过；r10开启的15项GL阶段及3D PBO三项回归通过。r11默认微负载26.579/28.785ms；Heaven截图6FPS及游戏9FPS/外层10FPS，均属冒烟，未配平温度/频率。

完整报告：F:/VintagePomelo-Workspace/artifacts/mini-heaven-backend-comparison/zink-upload-optimization.md。
原始证据：F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/backend-heaven-comparison/；复算summarize-zink-upload.py、zink-upload-results.json。

r11 signed SHA256=59c0fc049ed4616c5278e508f5f1241b9892f6b58d546c7c29cd9ecad64a63e7，629689839 bytes。仅相对r10更换libentry/Gallium，UI逐文件一致。基底56cfd994+dirty overlay，本轮未提交/push/CI；正式release parity、其他游戏及长时验收尚待完成。当前仍Venus且每帧CPU readback；无GPU busy证据。下一步优先GPU呈现桥接，再测量deferred-host-copy整队列等待，不能删除已有正确性workaround。
