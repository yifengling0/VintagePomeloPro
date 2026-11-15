# Mini 通用图形适配：D3D9 常量上传与 push constant 修复

2026-11-15。父提交510966c1。用户明确把系统桌面 OpenGL 暂时保留，未来 PC 单开分支适配；当前推进通用适配。没有新增 PC 分支、系统 GL 接入或 HAP。

## 本轮结论

1. 修正固定功能和 DXSO 编译器把 pushConstSize 误写成 offset 的共同错误。主机契约回归通过，旧代码 mutation 被拒绝；它没有解决 Mini 固定功能 VS 管线创建失败，不能作为该失败的根因结论。
2. 确认并修复硬件 VS/PS 常量缺少 shadow 发布：同一 EXE/DLL/profile 的顶点常量负对照黑色，开启后红色、读回、Present PASS。像素常量也通过。默认关闭，尚未推广到游戏。
3. 雾效、alpha test 的两个分支通过。旧 DLL 雾效本身也通过，所以不能宣称雾效画面是0012修复带来的收益。
4. 没有新的 Heaven FPS、GPU busy 或30%性能提升结论。当前仍是资源正确性阶段。

## 0012：D3D9 push-constant 元数据

`d3d9_fixed_function.cpp` 与 `dxso_compiler.cpp` 都使用了 `info.pushConstSize = m_pushConstOffset`。应当写入 `m_pushConstSize`。

| 编译路径 | 需要的 offset / size（bytes） | 旧 offset-as-size 的问题 |
|---|---:|---|
| 固定功能 VS | 28 / 24 | size误报28 |
| 固定功能 PS | 0 / 28 | size为0，漏报fragment stage |
| 可编程 VS | 28 / 12 | size误报28 |
| PS2 | 0 / 28 | size为0，漏报fragment stage |
| PS3 | 24 / 4 | size误报24 |

共享 `DxvkDescriptorSlotMapping::definePushConstRange` 实际把范围并到 offset=0，并取各阶段结束位置的最大值。因此初始怀疑“VS offset=28 导致雾参数字节不覆盖”不成立；本轮未扩大 VS range，生产补丁只有两处 size 赋值修正。

`scripts/test_d3d9_push_constant_contract.py` 编译生产 setup 函数、元数据赋值和 mapper，按真实 `D3D9RenderStateInfo` 字段检查五类 shader、六种阶段组合。ASAN/UBSAN、Wall/Wextra/Werror 通过；恢复旧赋值时同一契约检查必须失败。这验证元数据与布局，不代表完整 Vulkan validation layer 或所有游戏验收。

修正后的 `ff-push-range` 仍在 host vkCreateGraphicsPipelines=-1 后发生 CS error，故没有据此继续盲跑 Heaven。固定功能 VS/vertex interface/pipeline 仍需精确重放定位。

## 0013：硬件 shader 常量的可见性

`UploadConstantSet` 原先分配 slice 并提前入队 invalidate，然后 CPU memcpy、覆盖 shader-defined constants。shadow 内存路径没有对应的宿主发布，简单的 `mul oPos, v0, c0`（c0由游戏设为全1）就能复现黑色。

新增 `WINEHUA_DXVK9_SHADOW_CONSTANTS=1`，默认关闭。发布入队点放在所有 CPU 写入、shader-defined overrides 完成之后，捕获 buffer与物理slice，复用 `ctx->flushMappedBuffer`。不把 flush 塞入先前可能已执行的 invalidate lambda，保留slice轮转与命令顺序；发布会带来必要的传输开销，本轮尚未量化。flush失败明确抛错。

本轮只覆盖硬件 VS/PS 的 `UploadConstantSet`。SWVP、固定功能 VS/PS/共享常量、普通VB/IB、动态更新与各种 converter 仍待适配；这不是完整D3D9同步修复。当前同时使用前轮纹理/UP发布、读回、BC实验开关。

## 真机有界验证

Mini MLR-AL10；实际 Vulkan 路线仍为 Venus。每轮最多36秒观察，提前PASS/失败结束；设备操作持mini.lock。只临时部署独立探针与DLL，没有修改Heaven或用户默认后端。

最终 DLL：4280320 bytes，SHA256 `d6421820e0a8b291cefe86ffe8d093657b71b2665b64075d77bf7257ac41f029`。
最终探针 SHA256：`78595359dd53b162c1605b16e1b53eeba6bddd27bdcb583c745a568921f89729`。

| label | 检查 | 实际像素 | 结果 |
|---|---|---|---|
| vs-constant-off | 同DLL，常量发布=0 | ff000000 | 负对照，预期红色却黑色 |
| vs-constant-on | 同DLL，常量发布=1 | ffff0000 | 像素/读回/Present PASS |
| ps-constant-on | 像素着色器读取c0 | ffff0000 | 像素/读回/Present PASS |
| fog-on | 线性雾完全覆盖为绿色 | ff00ff00 | 像素/读回/Present PASS |
| alpha-reject | alpha128，GREATER ref192 | ff000000 | 正确丢弃，Present PASS |
| alpha-accept | alpha128，GREATER ref64 | 80ff0000 | 正确保留，Present PASS |

前两轮只有常量开关不同，EXE、DLL、profile及其他环境完全相同。PS常量仅有正向检查，尚未单独做负对照；alpha检查比较RGB与丢弃行为，不构成全部混合模式验收。没有重跑无关画质/线程数矩阵。

首次fog-baseline因锁屏导致aa error10106102，应用没有启动，不能计为shader失败。用户解锁后fog-retry-baseline通过。runner已补充aa文本错误检查，后续不再把该启动错误当作36秒无响应。

## 三路线与后续范围

- VirGL/GLES保留稳定默认。guest GL3.1与host GLES3.2分层；后续对齐guest CS、socket等待、host draw和GPU时间，再选择能减少每帧成本的优化。
- DXVK9保留实验路线。当前优先固定功能VS管线，以及普通buffer、固定功能/SWVP常量的发布契约；验证correctness后才准入Heaven。
- Zink当前guest GL2.1、此前Heaven黑屏条纹，按实际cap门槛与资源同步逐项定位。
- 系统桌面GL登记为未来PC独立分支项目，详见 `docs/graphics/pc-desktop-opengl-backlog.zh-CN.md`。当前不为此提高通用路线的系统要求。

## 代码与收尾

13个managed补丁、31个文件完整reverse/replay字节一致，两次幂等verify、当前verify-only通过。新push-constant契约回归及既有bool特化回归通过；增量编译成功。只提交根仓补丁、探针、测试、SHA锁和文档，保留十个既有dirty子模块。

最终设备收据：应用停止，临时EXE/DLL/输出文件清理，leftovers=[]。最终DLL未打入HAP，不安装新包、不卸载、不清数据、不触碰其他设备。

原 `backend-draw-analysis` 证据不覆写。它的manifest引用了可变源码/增量build路径；旧源码可从510966c1恢复，旧DLL只保留一份于本轮 `d3d9-baseline.dll`（SHA2d369eff…），用于本轮必要的baseline对照。新报告的manifest记录当前hash与这个引用迁移，不伪称旧可变路径仍匹配旧hash。

本轮本地commit、存储维护和文件清单见交付目录 `final-state.json` / `manifest.json`。小型收据引用现有日志和DLL，不重复复制大包。
