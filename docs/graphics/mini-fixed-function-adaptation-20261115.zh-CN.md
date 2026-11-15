# Mini D3D9 固定功能适配与 Heaven 后端归属（2026-11-15）

Mini 的固定功能绘制已越过两个独立阻点：超出设备上限的无效顶点输入，以及未发布到宿主的固定功能常量。修复保留为默认关闭的 opt-in 实验；VirGL/GLES 仍是稳定默认。DXVK9 / Zink 需要完整游戏正确性通过后，才能比较同 D3D9 Heaven 配置的性能。系统桌面 OpenGL 继续暂缓，未来 PC 分支处理。

## 根因与修复

FF `GetFixedFunctionIsgn()` 固定生成18项，`BindInputLayout()` 连 inactive placeholder 一并提交。Mini `maxVertexInputAttributes=16`，这次 VS `inputMask=0xff3` 只消费10项。此前 shader 已缩到1488 bytes却仍失败，原因是输入布局未变，而非已证明某条 shader 指令不受支持。

`0015-opt-in-prune-unused-vertex-attributes.patch` 在真正 vkCreateGraphicsPipelines 前按 VS mask 移除 inactive attributes，保留 binding count、顺序、compact map 与 location/format/offset。`WINEHUA_DXVK_PRUNE_VERTEX_INPUT=1` 默认关闭。当前对照从18/max16变为10/max16，host pipeline=-1变为0，CS fatal消失。location>=32不参与移位。实际消费 location16/17 的 vertex blend 等仍需单独解决，不能称全组合通过。

管线成功后图像仍黑，随后发现 FF VS/PS/shared PS 的常量直接写 CPU shadow，缺少发布。`0016-d3d9-opt-in-fixed-function-constant-upload.patch` 在三处最后一次写入之后调用 `PublishFixedFunctionConstants`，捕获 buffer 引用和物理 slice 向 CS 排队 flush；不在更早的 invalidate lambda 提前执行，不增加全局等待。开关 `WINEHUA_DXVK9_SHADOW_FIXED_FUNCTION=1` 默认关闭，尚未覆盖 vertex blend / SWVP。

## Mini 对照与验收范围

所有 label 在固定任务 `backend-pipeline-interface/` 下独立保存 launch、命令台账、host/Wine/probe日志及清理收据。没有替换 HAP 或游戏目录 DLL，没有更改 UI/图标资源。

| 证据 label | 同轮条件/检查 | 结果 |
| --- | --- | --- |
| program-vs-state | 可编程 VS + 相同 FF PS | 红纹理读回/Present PASS |
| prune-off / prune-on | 同0015 DLL/EXE/profile，仅输入过滤开关变化 | 18→10，pipeline -1→0；on仍黑 |
| ff-shadow-off / ff-shadow-on | 同最终DLL/EXE/profile，prune=1，仅FF常量开关变化 | 黑/pass=0→红 ffff0000/读回/Present PASS |
| ff-factor | FF PS TFACTOR | 绿 ff00ff00，PASS |
| ff-stage | shared PS D3DTSS_CONSTANT | 蓝 ff0000ff，PASS |
| ff-transform | 普通 FLOAT3 + 单位 World/View/Projection | 红，PASS |
| ff-bc | FF VS/PS 与 DXT1、DXT3、DXT5 | 三者均红/读回/Present PASS |

未验收非单位/动态矩阵、vertex blend、SWVP、全部 mip、复杂材质与完整老游戏。全部实验 Vulkan 路线仍是 Venus，不能称系统 Vulkan 直通。无新有效 Heaven benchmark 分数、连续 GPU busy 或30%性能收益证据。

## Heaven 归属检查

初次 `heaven-pruned` 请求 DX9、Low720p，40/90秒见到加载/场景；但没有候选 DXVK9 日志。后续 `heaven-route` 在启动前后记录 log.html 的 size/mtime，证实它仍是2026-11-13 18:49:40的旧文件，不能把该旧 D3D11 日志当作本次的直接证据。

`heaven-route` 用小型 Win32 启动器记录完整 CreateProcess 命令，跨进程 Toolhelp 模块枚举返回 error=5。启动器修改 Windows 环境不等于修改 broker 转发的 Unix environ；该实验不能证明覆盖已传到真实子进程。

修正验证配置、增加进程启动后的日志偏移与 trace+loaddll 后，`heaven-route-native/heaven-route-wine.txt` 明确记录目标 Wine tid=01b0 加载 DXGI 与 D3D11 native overlay，present 身份为 hostPid=10814 / winPid=428，窗口标题 `Unigine Heaven Benchmark 4.0 Basic (Direct3D11)`。候选 d3d9 没有加载证据。**这轮真实仍为D3D11，约15–20FPS的场景截图不属于新DXVK9性能成绩。** 已知传入 argv 请求 direct3d9，但实际引擎为何选择D3D11仍未定位，不能把 DLL 搜索修改说成已解决接管。

另做了仅用于诊断的 in-process Unigine 初始化探针，按29KiB原Heaven.exe主函数反汇编确认的 API/flags 调用引擎，主动记录 argv 和进程内模块。`heaven-engine-route` 的绝对配置路径被引擎拼成 `Z:/Z:/...`；修正相对路径与 argv 末尾空指针的 `heaven-engine-relative` 仍在 filesystem 初始化报错，90秒内未加载任何 D3D 模块。它是移动启动器位置后的资源路径失败，不是 DXVK9/GPU 失败证据，也不纳入游戏验收或帧率比较。没有改写原游戏EXE/DLL/配置。

本轮到此停止扩大游戏测试。后续先记录原目标进程实际 PEB/CRT 参数和路径解析，再用已确认模块身份的 D3D9 启动；历史只声明启动意图、未记录实际模块的 D3D9/Zink Heaven 结果也应补核实际API，不能据此形成性能排名。

## 可复现源码、构建和主机验证

- 基于根仓853fff6b，本轮只提交补丁/锁/探针/测试/文档，保留既有10个 dirty 子模块，不提交 gitlink；不 push/CI。
- 最终 PE32 d3d9.dll：4296704 bytes，SHA256 `2352b56fd087bcebf3010d1a3bf7e1355a55218cc9d4f5f5e60478a6adaafb2a`。`fixed-function-build.json` 记录收据。0015-only 与前轮DLL增量路径已被覆盖，没有额外保存二进制；不能说旧路径仍匹配旧SHA。
- FF probe：73216 bytes，SHA256 `2bc9326e1b740628cbe44902101b839f745697cf46d4a28283ce2315cb679ec3`；源码 SHA256 `c2c72b401abcf2f2daaf1914abc8a2e1018e006e3b88fbb426301837f82e42ef`，根仓 `scripts/probes/mini_backend_d3d9_fixed_function.cpp`。
- patchedSourceSha256 `52c887e727a1531451b7cd7c7d98f88300a143081d966bd4f3991820d4a370c5`；runtime lock登记0015/0016的补丁SHA与实验验收边界。
- 16 managed patches /32文件 reverse+replay 字节一致，两次幂等 verify、当前源码 verify-only通过。bool specialization、push constant、buffer publication及新vertex input/FF publication契约均通过，含ASAN/UBSAN。新测试的旧循环 mutation 确实在 out.size() assertion 失败，非编译失败代替行为验证。
- 复用固定 Ninja 增量构建，不运行非幂等 prepare-host，不复制源码树/资源/运行时，不新增HAP。

## 清理与下一步

`program-vs-state` 首次 finally 遗漏三个无前缀内部 SPIR-V，原失败收据保留；后续锁内核对三文件、取回后逐项删除并 rmdir 成功，`cleanup-after-state/recovered.json` 记录恢复。runner 已接受 SHA-only 内部shader，后续 label 清理正常。所有设备动作均持 mini.lock，最终应用停止、临时文件清除，游戏目录DLL未动。

下一步优先完成 DXVK9 的实际模块接管与 Heaven 复杂资源/材质正确性，按具体错误修复；Zink 继续解决 guest caps/Heaven 黑屏。VirGL 保留稳定路线，性能结论仍需对齐 CPU/等待/host 提交/GPU busy 时间。正确性与后端归属通过后才进行同设置 A-B-B-A 性能评价，历史不同API或不同场景截图不能混算收益。
