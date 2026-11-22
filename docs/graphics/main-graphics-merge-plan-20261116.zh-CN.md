# main 图形修复合并方针与会话交接

用户决定：配置不接入，Box64 已废弃。本会话只整理方针；产品修复由另一个会话实施。本文件可直接交给合并会话，无需重新研究参考包的全部内容。

## 任务范围

在最新 main 的 ARM64 Wine/FEX 图形链路上，按以下顺序修复通用问题：buffer 读取偏移 → 线性图像同步 → Maleoon 条件兼容。保留已有 Zink/PBO/批量上传与呈现修复，各阶段独立提交、可单独回退。

**配置明确排除**：不接入 `mewgenics_graphics.h`、游戏身份匹配、自动选 Zink、`mewgenics-zink-v6`、Box64 路径或 `BOX64_*`；不复制参考缓存和同步环境组合；不把修复变成新的全局渲染默认值。现有 shadow 开关用于约束修复生效范围，不新增该游戏的专用配置。清理残留 Box64 代码也不在这批合并范围内。

## 起点与现有修改

审计基线为 `7683a58b2f746b07979c9a56ac8e2dc240ad838f`，不是要求把后续 main 回退到该提交。合并会话先核对最新 HEAD、gitlink 和补丁注册，按新基线重新确认缺口，跳过已经修好的部分。

| 项目 | 审计时状态 |
| --- | --- |
| 主仓库 | `F:/VintagePomelo-Workspace/repos/VintagePomeloPro`，main |
| Mesa pin | `557fa3e781ea6e78fcff3ee759c25f60c3055064` |
| Wine pin | `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b` |
| VirGL pin | `bb33e52aa03199926f662e50ff22bd00d19d7e0a` |
| 已修改但未提交 | `host_tests/gl_present_failure_test.py`、`host_tests/renderer_lifecycle_test.py`，只是使测试桩适配当前接口 |
| 新增但未提交 | 本方针和 `docs/graphics/main-reference-audit-20261116.zh-CN.md` |
| 原有文件 | 根目录未跟踪 `AGENTS.md`，须保留，不自动纳入提交 |

不要覆盖当前未提交修改，不要将第三方 dirty 工作树当作干净 pin。需要隔离时使用独立 Git worktree，避免切换其他会话正在使用的分支。Linux 复用 checkout `/home/liufeng/src/vpp-account-cloud` 已被其他工作推进；不可 reset 或拿其 HEAD 冒充 main。审计时通过读取固定 pin 的 Git 对象并重放生产 overlay 来避开该问题。

## 分阶段合并

### A. 非零 buffer 读取偏移，优先处理

位置：Mesa `src/gallium/drivers/zink/zink_resource.c::zink_buffer_map()`，main 相关 overlay 为 `patches/mesa/0015-zink-opt-in-shadow-readback.patch`。

问题：返回指针用 `map_offset`，但 invalidate 起点仍用 `res->obj->offset + trans->offset`。直接 map 时二者不同，可能同步错误区间。当前模型读取 `[64,72)`，main 实际 invalidate `[0,8)`；参考补丁使用实际映射偏移后读取正确。

做法：在既有 `WINEHUA_ZINK_SHADOW_READBACK` 生效且为读取的路径，按实际 direct/staging 映射偏移计算区间，保留 BO 子分配偏移。保持非 remote 路径语义，检查失败后的 unmap/transfer 清理。不要引入参考的 `ZINK_WINEHUA_SHADOW_TRANSFERS` 开关，也不要全局把 coherent 改为 false。

验收：从生产 pin + 全套 overlay 提取实际代码做独立 Host/Guest 回归，覆盖 direct/staging、非零 box.x、非零 BO offset、READ/READ_WRITE、WRITE-only、开关关闭及同步失败。旧实现须在非零偏移用例失败，新实现只刷新期望范围；不能只断言源码出现某个变量名。

### B. 线性图像读写同步，单独提交

位置：同文件 `zink_image_map()`、`zink_transfer_flush_region()`、`transfer_unmap()`；联查 Venus flush/invalidate 及远端映射实现。

已确认的范围与方向缺口：

- coherent remote 线性图像读取没有相应 invalidate；非 coherent 旧路径在 map 时使用 flush，WRITE-only 在调用方写入前也会发布旧 Guest 数据。
- 连续 `width * height * bpp` 不能描述带 rowPitch、depthPitch/arrayPitch 的局部读取，也不能处理所有 block 格式。
- main 的直接线性图像上传发布整个对象，可能覆盖未写区域。参考补丁虽缩小区间，却漏加原始 map 原点，仍会漏传并覆盖无关数据。
- `transfer_unmap()` 把 flush box 的 x/y/z 清零，表明 flush box 相对 map；`trans->offset=srl.offset` 本身不包含 map 的 x/y/z。

做法：读取前 invalidate，写入完成后 flush；明确原始 map 原点、相对 flush box、block geometry、行/层跨度及 BO offset。不能将包含未修改行间隙的连续包围区间直接当作安全上传。优先评估独立 staging 是否能复用现有代码和所有权机制；采用直接 map 时必须证明未写区域得以保存。

同步范围还受 Vulkan `nonCoherentAtomSize` 对齐影响。逐行 flush 也可能扩大到未修改数据，不能仅把连续 flush 改成循环就宣称解决。需确认 transport 的精确复制范围、内存所有权和分配边界；必要时使用隔离 staging，并验证压缩格式、数组层及 3D 层的坐标换算。保留已有 `0017` staging 范围、`0023` 分层 PBO、`0024` 批量上传、`0025` 呈现逻辑。

验收：非零 x/y/z、非零 BO offset、行 padding、多层/3D、数组层、压缩块、显式局部 flush、自动 unmap、READ_WRITE 和开关关闭。Host/Guest 初始填不同哨兵，检查读取完整、写入完整、未写行间隙及相邻子分配保持不变，并增加非 1 的 atom size 用例。旧实现及参考补丁应能作为失败对照。

### C. Maleoon 冗余 ShaderLayer，条件候选

位置：Mesa `src/gallium/drivers/zink/nir_to_spirv/nir_to_spirv.c`，参考 `mesa-maleoon-unused-layer.patch`。

可以据参考算法准备独立补丁，但生产合入需确认实际 Maleoon 设备名和触发 shader。扫描所有指令后，只有没有变量或结构成员使用 BuiltIn Layer 时才删除冗余 ShaderLayer 能力；保留有效 Layer 和其他能力，并正确调整 `tcs_vertices_out_word`。设备条件仅约束相关 Maleoon 路径，对未知布局保守返回。

验收：闲置能力删除、变量 Layer 保留、成员 Layer 保留、无目标能力、异常长度/越界、索引调整，以及非 Maleoon 不改变输出；真实失败 shader 经 SPIR-V 验证器验证，再验证设备管线。现有 9 项合成指令断言只能证明扫描边界，不能替代这一步。

没有真实 shader/设备证据时，将此项保留为独立待验证候选，明确缺失证据；不要阻塞已验证的 A/B，也不要将它与 EGL 初始化失败混为同一根因。

## 不重复合入的项目

| 项目 | 处置 |
| --- | --- |
| 共享 FD | 参考补丁与 `patches/virglrenderer/0010-render-resource-fd-close.patch` 逐字节相同；保留，运行现有 FD 回归 |
| 设备清理 UAF | 与 `0009-device-object-cleanup.patch` 逐字节相同；保留，运行现有 ASan/UBSan 清理回归 |
| EGL/Surface | main 已有失败停止、Surface 身份保护及资源释放；不整文件替换 `egl_renderer.cpp`、`plugin_manager.cpp`，避免覆盖 Vulkan/X11/owner/暂停逻辑 |
| 实例化入口 | 生产 Wine 的 `unix_wgl.c` 已有 capability/driver 检查后的 ARB 回退；不移植旧 `wgl.c` 补丁 |
| Esc/IME | `patches/wine/0071-ime-key-scancode.patch` 已覆盖当前适用分支；不恢复已移除的旧 Wine 分支 |
| 恢复窗口重绘 | 已有请求序号和成功呈现确认；保留现有回归 |
| DirectDraw 裁剪 | `0070-ddraw-clip-surface-bounds.patch` 已注册；不重复移植 |
| 中文字体别名、旧 16 位文字掩码 | ZIP 未提供相应源码；本次证据不足，不扩大改动，另留问题记录 |

## 补丁注册、提交和验证

1. 先落实 A，再落实 B；C 保持独立。两份已有测试桩修正可作为独立测试维护提交。提交时只选择相关文件，不执行 blanket add，不夹带原有 AGENTS、第三方 dirty 状态或生成物。
2. 新增 Mesa overlay 接在当前生产补丁栈之后，编号以合并时清单为准；更新 `scripts/apply_mesa_ohos_patches.sh`、`docs/graphics/graphics-stack.lock.yaml` 及相关构建身份。若修改较早 overlay，须证明全栈仍能从干净 pin 重放，且增量应用检查可重复运行。无版本升级需求时不改 gitlink。
3. 将有价值的临时模型整理为仓库主机回归，覆盖真实抽取代码或实际实现；随后运行受影响的 Zink 上传/回读、批量上传、GPU present、graphics contract 及干净 overlay 重放。未改变相关代码时不反复运行所有无关测试。
4. 编译或设备验证前，读取仓库 `.agents/skills/winehua-build-release/SKILL.md`；复用已有 Docker/Makefile 增量产物，选择当前 ARM64 Wine/FEX 路线。检查安装包实际装入了新补丁对应的库和构建身份，不能只凭文件时间判断。
5. 验证分开记录：主机模型、完整编译、签名包检查、ARM64/FEX 实机、具体游戏、长时间/低内存。A/B 的主机修复通过不等于喵基因已修好；FD/UAF 通过也不等于低内存退出已解决。无法完成的验证写明缺项。
6. 遵守工作区磁盘规则：复用固定任务目录，优先复用已校验成果；生成调试包时登记可重建目录、只保留最新两份签名候选、清理 unsigned/解包副本，结束运行 `F:/VintagePomelo-Workspace/workspace/scripts/maintain-storage.ps1 -Apply -CompactLogs`。

## 现成证据与复核入口

- 完整审计：`F:/VintagePomelo-Workspace/repos/VintagePomeloPro/docs/graphics/main-reference-audit-20261116.zh-CN.md`。
- 原始 ZIP：`C:/Users/liufeng/Downloads/GIVE2COMBINE.zip`；SHA-256 `32f522f7c7dcb93e9ba481687d06fa67675c21bd27a7aa717ba7802ace69c248`。临时展开已清理，勿复制 ZIP 或整套运行时。
- 证据目录：`F:/VintagePomelo-Workspace/artifacts/main-graphics-audit/`。`results.json`/`verification.log` 是原 15 项检查；`reference-results.json`/`reference-verification.log` 是 ZIP 新增模型；`reference-input.json` 记录每个参考文件的大小和哈希。
- 复核说明：`F:/VintagePomelo-Workspace/workspace_temp/main-graphics-audit/REPRODUCE.md`。新增脚本检查 Windows main 的 Mesa 输入哈希；旧 15 项脚本要求原始应用基线，不能在并发变化的 checkout 上直接复跑。

既有结果：原 15/15 主机检查通过；新增模型暴露 A/B 的问题；Maleoon 9 项合成断言通过。另有 13 项参考配置断言仅属历史审计，配置已排除，不作为待合入功能。没有本批产品 HAP、设备或游戏验收结果。
