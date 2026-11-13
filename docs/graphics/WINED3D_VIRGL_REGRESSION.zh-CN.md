# 1.5.1 WineD3D / VirGL 图形补丁回退

本文记录首轮 WineD3D 定位。后续发现的 DXVK、Venus、vtest 和前缀升级遗漏，以及最终交付结果，见 [完整运行库审计](RUNTIME_COMPAT_AUDIT.zh-CN.md)。

## 核验结果

对照正式保留的 1.4.9 星露谷修正版 HAP 与 1.5.1 Steamworks extended 候选包，VirGL renderer、Mesa OpenGL 驱动、EGL 和 winewayland 的文件校验值一致，但两套 WineD3D DLL 的实际代码不同。不能仅以主工程 Git 提交或补丁文件仍然存在判断运行库没有回退。

最近用于队列修复的 Wine PE 构建源缺少以下既有 WineD3D 补丁，定向替换 DLL 时只检查了队列后端：

- 0036：用 WGL 像素格式 ID 查询，避免对紧凑数组错误索引。
- 0038：将 D3D DISCARD 语义传给 OpenGL 缓冲映射。
- 0039：保留 WOW64 动态缓冲 sysmem 暂存选项，默认不启用该实验。
- 0049：缺少 framebuffer sRGB 写控制时，分开线性与 sRGB 纹理存储。
- 0053：恢复兼容 GL 上下文的 Alpha Test 启用／禁用状态。

其中 0049、0053 在 1.4.9 的两套实际 DLL 中均存在，而回退包中均缺失。这确认了图形运行库回退；是否完全解释《大富翁8》此次模型缺失，仍需实机复测。

## 修复与防回退

重用现有 Wine PE 增量构建目录，补齐上述五个补丁，保留 0068 的 auto-reset event 队列唤醒修复，编译 i386 和 ARM64X WineD3D。五个图形源文件与之前已验证的有效图形源码逐字节一致；cs.c 保留新队列实现。没有直接回装旧 DLL，以免重新引入队列等待问题。

`scripts/wined3d_runtime_contract.py` 核验全部已注册 WineD3D overlay 的实际应用、PE 架构、图形与队列标记以及最终 DLL 哈希。实际文件检查使用 GNU patch，不依赖嵌套源码快照的 Git 路径过滤。`stage_wined3d_queue_runtime.py`、标准 `assemble.sh` 和 `check_runtime_components.py` 共用此检查。运行库中保存 `wined3d-graphics.json`，HAP 外层清单保存同一记录。

升级运行库版本标记，使客户端更新已解压的运行库。没有清空用户 Wine prefix、Steam 游戏库或存档。

## 验证范围

- 实际 1.4.9 DLL 被新检查拒绝：保留旧 thread-ID alert 队列，不适合直接回滚。
- 实际回退的 1.5.1 DLL 被拒绝：缺少图形修复。
- 修复后的两套 DLL 通过；过期哈希、遗漏架构、遗漏 overlay、缺少实际 Alpha Test 补丁的源码均被拒绝。
- Alpha Test 的启用、禁用、比较方式、引用值更新、blit 后恢复及 core context 保护通过行为测试；旧问题可被测试复现。
- 像素格式、DISCARD/NOOVERWRITE、动态 sysmem 及 VirGL sRGB 行为测试通过。
- 1.5.1 Release 未签名 HAP 编译、运行库闭包和最终包校验通过。
- 新旧 1.5.1 包逐成员对照：运行库仅替换两套 wined3d.dll 并增加检查记录；Native 库、ArkTS 字节码、GNU 适配器和 Steam 接口桥一致。
- 配套 `hmosdeck-steam-arm64-interfaces.zip` 已逐档案核验，并与新 HAP 的离线元数据大小、ID 和 SHA-256 匹配。输入没有变化，复用同一离线包。
- 本轮未在设备运行《大富翁8》，不将源码／包级验证视作模型恢复的实机结论。
