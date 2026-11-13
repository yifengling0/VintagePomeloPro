# 1.5.1 运行库兼容性审计

## 源码与补丁的关系

应用的 ArkTS、C++、Steam GNU 适配器和接口桥直接修改源码。Wine、Mesa、VirGL 以固定上游版本加受管理补丁构建；补丁实际修改参与编译的 C/C++ 文件，并非运行时修改游戏或给二进制打补丁。DXVK 使用固定定制分支，并核对源码指纹和交付 DLL。

源码和补丁都在，不代表最终包一定使用了对应产物。本次问题出现在定向构建、缓存产物、运行库清单和最终打包之间的配对不足。旧产物缺少完整逐模块构建记录，不能仅凭校验值断言历史上是哪一条命令引入了所有错配。

## 基准与发现

基准为保留的 `VintagePomeloPro-1.4.9-1004090-unsigned.hap`，SHA-256 为 `93f4975182744f9cb474fc269ee1deeb0c689fb9b510fce01c890cd749b028a4`。核对实际 HAP 内容及已验证的有效源码，不能只对比旧版主工程提交。

| 组件 | 发现 | 本次处理 |
| --- | --- | --- |
| WineD3D | 定向 PE 构建源遗漏 0036、0038、0039、0049、0053 | 补齐后编译 i386、ARM64X，同时保留 0068 event-only 队列；不直接回滚整套旧 DLL |
| DXVK 1.10 | 九个 DLL 与已核验纹理兼容版本不同 | 恢复与纹理兼容锁逐字节相同的已验证产物 |
| DXVK 2.6 | 六个主 DLL 与基准和锁一致 | 保留；实际源码的线程、拷贝、实例化和浮点控制测试通过 |
| Venus | 顶层和内层 ICD 不一致，清单记录过时；Release 再次 strip 也会改变顶层文件哈希 | 配对到锁定版本，纠正清单，保留顶层已验证 ICD，成包后再次核对两处内容 |
| VirGL vtest | 实际 present 处理缺少选回生产者 GL 上下文的调用 | 恢复验证过的修正版；修正补丁脚本在嵌套 Git 源码导出中可能假成功的问题 |
| Wine 前缀升级 | 替换内建 DLL 后 `wine.inf` 内容标识仍旧 | 按实际内建 PE 文件重新生成标识；两个定向 DLL 更新工具同步执行，并在最终校验中拒绝旧标识 |

## 其余差异

- 最终包有 119 个原生库、2210 个运行库成员与 1.4.9 基准完全相同。包含保留的图形、输入、网络、FEX 和媒体基础模块。
- 76 个 GStreamer 插件从运行库内迁至 HAP 原生库目录，逐个校验内容完全相同；新位置用于系统允许执行的原生链接器命名空间。插件库存、依赖闭包及加载器配对通过。
- `kernelbase/registry.c` 反向移除 0069、0065 后与 1.4.9 有效源码完全相同，再正向重放可精确恢复当前源码。改动受 Linux Steam 会话标记控制。
- `winegstreamer/unixlib.c` 反向移除 0066 后与 1.4.9 有效源码完全相同，正向重放精确恢复新加载器入口。
- WineD3D 的五个图形源文件与验证过的有效源码一致；队列文件保留新实现。保留应用 C++ 中的双向共享纹理栅栏、异常呈现退避及后续 Steam 管理改动。
- `wineboot` 保留带身份记录的 0067 单次托管初始化版本，接口桥与 GNU 适配器继续使用当前配对版本。
- 检查变化模块的导出表，没有移除旧 Wine 导出。应用内部 C++ 签名有变化，但未发现包内其他原生库仍导入被移除的旧签名。

## 防止再次遗漏

`runtime-core.lock.json` 固定经审查保留的 119 个原生库和 1362 个 Wine PE 模块；刻意升级这些组件时，必须同时审查锁文件与回归证据。DXVK、Venus 和 WineD3D 分别使用更具体的源码／补丁／二进制契约。

标准 assemble 检查运行库载荷；标准 package 对未签名和签名 HAP 均检查最终文件，覆盖 Release strip 后的内容。定向 WineD3D、Steam DLL 更新同时刷新内建模块标识，升级时保留用户前缀、游戏库和存档。直接使用 DevEco/Hvigor 的其他入口也应执行 `scripts/check_runtime_components.py --hap <最终HAP>`。

## 验证与交付

- 实际错配的旧 DXVK、两处 Venus、vtest 和内建模块标识均能被新校验拒绝；修复后的 HAP 通过。
- VirGL 实际 present 上下文及错误回复 204 项、共享纹理双向同步 34 项检查通过。
- WineD3D Alpha Test、像素格式、DISCARD、动态 sysmem 和 sRGB 行为测试通过；DXVK 线程故障、资源清理、CPU 栅栏、拷贝、实例化、缓存策略和 384 组浮点策略组合通过。
- Release 编译、ZIP 完整性、运行库／媒体依赖闭包、GNU／PE 桥配对及官方工具确认未签名通过。
- 相对前一 WineD3D-only 候选，本次只改变两处原生库和十三个运行库成员；ArkTS、GNU 适配器、Steam 接口桥保持一致。
- 最终 HAP：`VintagePomeloPro-1.5.1-1005010-runtime-compat-fix-unsigned.hap`，SHA-256 `3619e0ccec9ed50d937ea5b2cfa5a9bdc6a9864e04cdafa373a6fe1792bb9a5e`。
- 离线 Steam 包输入没有变化，继续使用 `hmosdeck-steam-arm64-interfaces.zip`，SHA-256 `fe5d2cfde32bc63c3c9ab456aeabb22dfe34d87af442efdca251db0c5d1b8a95`，已重新核对与最终 HAP 的安装计划配对。

本轮没有设备运行《大富翁8》，不能将包级审计结论当作模型恢复或所有游戏兼容的实机证明。最终完整差异和测试记录保存在交付目录的 `runtime-full-audit.json`、`source-overlay-audit.json`、`runtime-compat-verification.json`。
