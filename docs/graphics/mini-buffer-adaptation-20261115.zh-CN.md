# Mini 通用适配：普通 VB/IB 上传修复与固定功能 VS 定位

2026-11-15，父提交 c8ab83a695472e9f18d7b2c6a312c5741df62ac8。方针不变：Mini 保留 VirGL/GLES 稳定默认；DXVK9、Zink 先正确性，再用相同 D3D9 配置比较 Heaven；系统桌面 GL 留给未来 PC 分支。本轮仅独立探针临时加载，没有替换 Heaven DLL、生成或安装 HAP。

## 已解决的问题

普通 D3D9 VB/IB 的 CPU 写入在 shadow 路径缺少宿主发布。新0014补丁通过 WINEHUA_DXVK9_SHADOW_BUFFERS=1（默认关闭）补齐两条路径：

- backing-copy：在 FlushBuffer 的 CS copy 前解析并发布对应源 slice，遵守前序 DISCARD rename。
- DIRECT：最后一次 Unlock、CPU 写入完成后，捕获 buffer 引用和物理 slice，限定 dirty range，再按 CS 顺序发布。后续 DISCARD 不会把已排队操作重定向到新 slice。

复用 DX11 使用的 flushMappedBuffer，不把发布提前放进 invalidate lambda，不增加全局等待；flush 失败明确传播。这是上传可见性修复，尚未量化传输成本或游戏性能，不能解释为 CPU 利用率提高或30%帧率收益。

## Mini 验证

设备 MLR-AL10，实际 Vulkan device 仍是 Venus。每轮持 mini.lock，最多36秒观察，提前 PASS 或明确错误即停止；单文件采集上限约2 MiB。两组负/正对照各自使用同 DLL、EXE、profile，仅 buffer 开关不同。

| 证据 label | 路径与检查 | 结果 |
|---|---|---|
| managed-off / managed-on | MANAGED 普通 VB | 关时黑色；开时初始红、非零 offset 部分更新绿，读回/Present PASS |
| dynamic-off / dynamic-on | 默认动态路径 | 关时黑色；开时初始、部分更新、DISCARD/NOOVERWRITE PASS |
| staged-on | DEFAULT usage0 backing-copy | 初始红、部分更新绿 PASS |
| indexed-on | 动态 VB/IB，非零 firstIndex | 初始、部分更新、双区域追加、索引追加蓝色 PASS |
| indexed-up | DrawIndexedPrimitiveUP | 红色、读回/Present PASS |
| dynamic-backed-config | allowDirectBufferMapping=false | 日志确认配置生效；三个阶段 PASS |
| dynamic-direct-confirm | allowDirectBufferMapping=true | 日志确认配置生效；三个阶段 PASS |

初轮 dynamic-backed-on 的 DXVK_CONFIG_FILE 路径不符合 probe 的 Z 盘映射，虽绘制 PASS，不能作为强制 backing 证据。runner 改为 Z:\backend-draw-dxvk.conf 后，用两个新 label 分别确认 false/true。默认 dynamic 负/正对照两边配置条件相同。旧证据未覆写。

设备验收 DLL：4288512 bytes，SHA256 acbc1051c788b09279281f461fffd8a770c0522b3f39124af3d604b6b1bd0702。
buffer probe EXE：70144 bytes，SHA256 08705afe0b86a0340f5a91575bb8fee1abd973a688de2d0bc3f5e49f8aa4772a。
源码：scripts/probes/mini_backend_d3d9_buffers.cpp；主机回归：scripts/test_d3d9_buffer_publication.py。

## 固定功能 VS：排除项与剩余定位

临时诊断只替换 debugName 精确匹配的失败 FF VS 模块，vertex input、pipeline layout、PS 与渲染状态保留原调用条件。每轮以 host codeSize/FNV 核对实际替换命中。

| 模块 | bytes | host FNV1a32 | 管线结果 |
|---|---:|---|---|
| 原始精确重放 | 8068 | 0xdb324e82 | -1 / CS fatal |
| aggressive DCE | 6848 | 0x08d55d13 | -1 / CS fatal |
| spirv-opt -O | 6660 | 0x070ae137 | -1 / CS fatal |
| PointSize 常数，随后 DCE | 5468 | 0x19d220d8 | -1 / CS fatal |
| 缩小主函数，保留声明/输入输出 | 6072 | 0x72dd1ae0 | -1 / CS fatal |
| 再去掉未使用变量和代码 | 1488 | 0x993e8220 | -1 / CS fatal |

模块均通过 spirv-val --target-env vulkan1.1。最后一项没有原来的 uniform/push constant 读取和点大小计算，仍失败。仅做 DCE、算术简化或去掉点大小计算不足以修复；不能据此认定唯一根因是 GPU 编译器、接口数量或某条 Vulkan 状态。SPIR-V 校验通过不等于完整 pipeline 合法。前轮全 scalar 固化也失败，本轮未重复。

下一步应补全失败与成功调用的 vertex attribute/binding、阶段 location/type、layout 和设备 limits，建立可精确重放的最小 pipeline，再逐项收缩接口。目前日志缺少足够的顶点输入字段，继续调游戏画质或通用编译优化等级不能补上这一证据缺口。

临时 shader hook 已撤出生产源码。恢复编译 DLL：4288512 bytes，SHA256 f9dd00ec4c9f47b645cd7931f87270f3faf5597a80b806218177eadbb43a3713。与设备验收版全文件比较，只有 COFF 和 IMAGE_DEBUG_DIRECTORY 的 TimeDateStamp 字段不同；这两个字段归零后所有字节一致（SHA256 d7a969a4d8c599226fff1084aaed21fa6ef5ed21e00de105e96f4163f0fc3e8a）。代码/数据均一致，比较收据保存偏移和原值，代替重复 DLL。

## 验证与边界

- 主机执行生产 UnlockBuffer/helper，检查默认关闭、嵌套锁、延迟 CS、捕获物理 slice 和引用、部分范围、READONLY、backing 路由、越界及 flush 错误传播；ASAN/UBSAN、Wall/Wextra/Werror 通过。
- mock 不执行真实 FlushBuffer 的临时 staging fallback，不是并发压力测试。真机覆盖普通 backing 与配置明确生效的动态 backing，不能推广为所有 buffer 更新通过。
- 14个 managed 补丁、31个文件 reverse/replay 字节一致，两次幂等 verify、当前 verify-only、既有 bool 特化/push constant 契约与 git diff 检查通过。源码 SHA 锁包含0014；十个既有 dirty 子模块与 gitlink 保留。
- 固定功能/共享/SWVP 常量未验收，FF VS 管线仍阻塞。Zink 仍是此前 GL2.1/Heaven 黑屏条纹状态，本轮没有新的 Zink 验收。
- 没有新增 Heaven FPS、连续 GPU busy 或 GPU timestamp。CPU/GPU 主导关系仍需关联时间线。历史 Low720p 中位16.3 FPS，+30%门槛21.19 FPS，尚未证明达到。

## 收尾与证据

所有轮次应用停止，临时 EXE/DLL/SPV/config 清除，leftovers=[]。只操作 Mini，未清数据、未 push/CI，未改默认后端、UI资源或系统要求。继续保留两个已有签名 HAP、固定增量构建和一份必要旧基线 DLL。

原始证据：F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/backend-buffer-adaptation/。
matrix-summary.json、ff-diagnostic-summary.json、final-build.json、source-verification.json 提供逐项收据。前轮 backend-draw-analysis/backend-general-adaptation 不覆写；其引用的增量 DLL/源码是可变路径，旧源码从对应 commit 恢复。旧 d642… DLL 本轮没有另保留，不声称原路径仍匹配旧哈希。

本地提交、最终设备状态和存储维护结果见同交付目录 final-state.json；manifest 引用现有证据，不复制日志、资源或构建树。
