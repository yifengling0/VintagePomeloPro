# Zink shadow 传输修复与验证

基线为 main `7683a58b2f746b07979c9a56ac8e2dc240ad838f`，实施分支 `fix/zink-shadow-transfers`。沿用 Mesa pin `557fa3e781ea6e78fcff3ee759c25f60c3055064` 和 ARM64 Wine/FEX 路线；未更改 gitlink、应用版本、图形默认值或游戏配置。依据本工作区的 `main-graphics-merge-plan-20261116.zh-CN.md` 实施 A/B。

## 实现

### A：buffer 实际读取偏移

新增 `patches/mesa/0026-zink-shadow-buffer-map-offset.patch`，接在生产补丁栈之后。`WINEHUA_ZINK_SHADOW_READBACK=1` 且包含 READ 时，以 `map_offset` 而非一律以 `trans->offset` 计算 invalidate 区间，并保留 BO 子分配偏移。direct 使用 `box.x`，staging 使用自己的映射偏移。没有启用 shadow 读取时保持原路径。

同步失败时先 unmap，再释放 transfer 持有的 staging 和原资源引用；copy context 仍按原规则解锁。没有新增开关，也没有强制将 coherent 改为 false。

### B：隔离图像 staging 与局部更新

新增 `patches/mesa/0027-zink-isolate-shadow-image-staging.patch`。任一现有 shadow 图像同步开关启用时，线性图像也复用 Zink 原有 packed staging 路径。GPU image/buffer copy 负责原始图像布局，避免在 CPU 端将 rowPitch、depthPitch、arrayPitch 和 block 格式误当成连续像素。

这类 image staging 带有驱动内部资源标记，分配时使用 `ZINK_ALLOC_NO_SUBALLOC`。现有 BO 分配器据此跳过 slab 和可复用 BO 池；Venus 当前分配路径为每份内存单独建立 renderer BO。atom 扩展和 Venus unmap 自动发布至多影响该 staging 的私有 padding，不会触及目的图像或相邻子分配。普通 buffer、非 shadow 图像仍沿用既有分配方式。

READ 在 GPU copy/fence 完成后 invalidate；WRITE-only map 不预先发布旧 Guest 数据。写回时先同步隔离的 staging，再复制到图像：完整 map 更新保留单次 copy；显式局部 flush 按原 map 原点加相对 box，只复制所选 block 行和层，保留原 staging stride。未 flush 的写入不会到达目的图像。flush 失败不继续复制，并标记 device lost；map 失败清理资源引用。

未替换 `0017` staging 字节范围、`0023` 分层 PBO、`0024` private buffer 批量上传或 `0025` GPU present。新 helper 只处理带隔离标记的图像 staging，不进入 buffer 批量上传队列。

### 构建身份

注册 `0026/0027`，更新 graphics lock 的完整 Mesa 清单、生产顺序和有效 `zink_resource.c/.h` 哈希。graphics contract 增加 `--mesa-source` 校验。

`guest-gfx-build.json` 升至 schema 3，绑定 Mesa pin、生产 overlay 的顺序/哈希、有效传输源码与实际 EGL/GLES/Gallium 库哈希。构建脚本在编译安装后传入真实 source root；打包拒绝 schema 2 或旧补丁/旧源码身份。已有库需要通过正常增量 Mesa 构建重新生成配对记录，不能手动给旧库补新标签。

## 验证结果

全部在现有 Docker `winehua-dev` 内执行，镜像 ID `sha256:0a010ac5c5dbae2e9684a0d2f5227926ddc8bfc06b285f53b9a94ee81cf405ba`。源码隔离于 `/home/liufeng/src/vpp-graphics-main`；其他会话 checkout 只读提供固定 pin 的 Git 对象及已有配置/依赖，未 reset 或覆盖其源码、构建和运行时。

| 验证 | 结果与范围 |
| --- | --- |
| Buffer | 384 组 direct/staging、非零 box.x/BO offset、READ/READ_WRITE/WRITE、开关关闭、invalidate 失败及引用清理；普通与 ASan/UBSan 均通过 |
| 图像 | 648 组 RGBA8/BC1，2D/3D/2D array/1D array/cube array，非零 x/y/z、不同 row/array/depth pitch、压缩边缘块、atom 1/64/256/1024、coherent/noncoherent、局部多行多层 flush 与自动 unmap；另测分配/map/invalidate/flush 失败及开关关闭；普通与 ASan/UBSan 均通过 |
| 失败对照 | 原 main 在 buffer 区间断言、图像数据断言失败；参考 ZIP shadow 补丁也在图像数据断言失败。负例必须以指定断言的 SIGABRT 结束，不能用任意崩溃冒充预期失败 |
| 生产补丁 | 从固定 Mesa Git 对象导出后重放全部 25 份生产 overlay，并再次执行检查增量幂等；排除 diagnostic-only 0014 |
| ARM64 OHOS 编译 | 复用已有 Zink compile database 与生成头文件，完整编译修改后的 `zink_resource.c`，产出 63,664 字节 AArch64 ELF object；未链接 Mesa 库 |
| 构建身份 | 11 项测试通过，包括旧库、旧源码、生产顺序变化、旧 overlay 身份和 EGL/Gallium 不配对的拒绝 |
| 相关回归 | Zink batch upload、GPU present、graphics contract/有效 Mesa 源码、GL 失败恢复 43 项、VirGL 双向同步 34 项、renderer 生命周期 708 项、renderer scopes、VirGL FD 和 ASan/UBSan cleanup 均通过 |

固定集成结果 **15/15**。记录位于工作区 `artifacts/main-graphics-fix/results.json`、`verification.log`、`compile-result.json`；编译 object、完整临时导出和测试可执行文件验证后删除。两个原有 renderer 测试桩调整单独提交，未改变生产 EGL/Surface 生命周期。

## 尚未验收与性能影响

本轮完成源码、主机行为和单个完整编译单元验证；**没有新链接的 Mesa 库、签名 HAP、ARM64/FEX 真机、具体游戏、长时间或低内存结果**。不能据此宣称喵基因已修复或 FPS 提升了某个比例。

可减少线性局部传输发布整个图像对象的无效工作，同时避免覆盖 Host 新数据。独立 staging 会增加分配和 image copy 成本；完整更新仍为单次 GPU copy，只有显式局部更新逐 block 行/层复制。实际 FPS 和分配成本需在相同渲染尺寸、后端和场景下测量；优先验证初次上传、GPU 写后回读、局部更新和退出重入，再决定是否优化局部 copy 的提交次数。

Maleoon ShaderLayer 项继续保留：未取得实际触发 shader、SPIR-V 验证和设备管线证据，未向生产注册该 workaround。未合入 Box64、游戏识别、自动选 Zink、参考 cache 环境或其他全局默认设置。

## 复核

在具备该 pin Git 对象的 checkout 内，Docker/Makefile 执行：

```sh
make test-zink-shadow-transfers GFX_MESA_GIT=/path/to/mesa-git
python3 host_tests/guest_gfx_build_identity_test.py
python3 scripts/verify_graphics_contract.py --mesa-source /path/to/effective-mesa
```

完整固定入口为隔离 checkout 的 `workspace_temp/main-graphics-fix/run.sh`。可选负例通过 `zink_shadow_image_test.py --reference-patch` 指向原 ZIP 中的 shadow patch；只适配原补丁所需的两处上下文空行，不改变其逻辑。原 ZIP SHA-256 为 `32f522f7c7dcb93e9ba481687d06fa67675c21bd27a7aa717ba7802ace69c248`。
