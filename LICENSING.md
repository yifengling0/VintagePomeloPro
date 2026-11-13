# 许可范围与分发要求

审查基线：`feature/main_proton` 的 `e2d001416d94ec01c31d4ffd38be3720d1496b47`，2026-10-04。
本次是源码、构建和打包脚本的许可梳理，没有构建或验收任何发布 HAP，也没有更改代码、资源或打包行为。

## 1. 保留已有授权

本工程派生自 [winehua/WineHua](https://github.com/winehua/WineHua)。上游在
[1dc0283e01b2efc6bc766c78ef6f3f34dc7f3119](https://github.com/yifengling0/VintagePomeloPro/commit/1dc0283e01b2efc6bc766c78ef6f3f34dc7f3119)
加入的根目录 `LICENSE` 已含 GNU LGPL 2.1 全文及以下项目声明：

> WineHua — Wine compatibility layer for HarmonyOS
> Copyright (C) 2026 winehua

原声明允许 LGPL 2.1 或任何后续版本，即 **LGPL-2.1-or-later**。本次保留原文件全文和原权利人声明，仅在许可证正文之外增加范围说明；不新增版权归属、不撤回原有授权，也不把整个项目改为一个新许可证。

- 根目录 [LICENSE](LICENSE) 说明已有 WineHua 项目授权
- 文件自己的声明，以及其所属第三方组件的许可，继续适用于该文件；不得用根目录许可证覆盖
- 对第三方代码的修改应保留原声明，并按其许可标记变更。提交记录和补丁有助于追溯，但不能替代许可证要求的声明
- 自动生成文件中的注释、SDK 绑定生成信息或接口名称，不足以证明整个文件由该注释中提到的机构授权
- 此处没有为未查明来源的资源补授许可；没有授权使用第三方商标，也没有授予 Steam、Windows、游戏或 SDK 的使用/再分发权

## 2. 依赖不能只按子模块名称判断

[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 区分实际构建输入、可选路径和待核实项。
子模块 SHA 可用于定位源码，但不自动证明该源码进入某个 HAP。发布者必须保存实际构建记录并核对最终产物。

- [scripts/env.sh](scripts/env.sh) 默认选择 `thirdparty/wine-valve`；`thirdparty/wine` 是另一条源码路径
- [scripts/build_gnutls.sh](scripts/build_gnutls.sh) 使用指定版本的发布压缩包，**并不使用同名 GMP/Nettle/libtasn1/libunistring/GnuTLS 子模块作为构建输入**
- [scripts/build_gstreamer.sh](scripts/build_gstreamer.sh) 包含所选 GStreamer 插件及 FFmpeg 构建。插件集合、配置、下载回退和本地缓存都会影响产物
- [scripts/build_fex.sh](scripts/build_fex.sh) 可接受另一个 `FEX_SRC` 或既有产品源码；[scripts/assemble.sh](scripts/assemble.sh) 也会选择已有库。仅记录顶层提交不够
- 原生 SDK/系统库和外部下载文件须另行核实，不能把工具链名称或脚本里生成的版本字段当成真实二进制来源

## 3. LGPL 版本与组合边界

当前 TLS 链包含 GMP 6.2.1、Nettle 3.10.2、libunistring 1.3，其库提供 LGPL-3.0-or-later 或 GPL-2.0-or-later 的选择；GnuTLS 3.8.3 的主库声明也明确提醒这一依赖影响。
这些版本不能概括成“全是 LGPL 2.1”。

本清单对这些库按 **LGPL-3.0-or-later 路径**整理义务，不声称使用 GPL 选择，也不因此改变根目录的 LGPL-2.1-or-later 授权。
在构成组合/派生作品的范围内，必须满足所选 LGPL 3 条款及其引用的 GPL 3 条款；已有 `or-later` 授权可用于需要的版本兼容处理。
哪些模块构成组合作品、哪些只是独立汇集，应按实际链接、代码复制和交互方式判断。
仅把文件放在同一 HAP/压缩包内，不足以把所有独立作品自动改为 GPL；反过来，动态加载、IPC 或分进程也不自动豁免实际适用的许可义务。

OpenHarmonyASF 的 Apache-2.0 ArkTS 代码由应用直接导入，不只是同包附带文件。
不能仅凭根目录 LGPL-2.1-or-later 声明，就认定该组合可只按 LGPL 2.1 分发；须按实际合并/链接边界确认兼容路径，必要时对允许升级的部分采用 LGPL 3，并继续履行 Apache 条件。
根目录的 `or-later` 不能替其他权利人升级许可；例如 Wine 内 mpg123 的版本声明须按其文件原文单独核对，不能一概改标为 LGPL-2.1-or-later。
[FFmpeg 的兼容说明](https://ffmpeg.org/doxygen/trunk/md_LICENSE.html) 明确讨论 Apache-2.0 与 LGPL 版本的这一差别。

Apache-2.0、MIT、BSD、zlib、FreeType 等组件仍保留各自的许可、版权及 NOTICE 要求。
特别是 Apache-2.0 与 GPL 2.0-only 不能仅因“都是开源”就视为兼容；如改变上述许可选择或合并方式，须重新审查。
[GNU LGPL 3](https://www.gnu.org/licenses/lgpl-3.0.html)、[GNU GPL 3](https://www.gnu.org/licenses/gpl-3.0.html) 和
[Apache 的 GPL 兼容说明](https://www.apache.org/licenses/GPL-compatibility.html) 可作为条款说明入口，最终以各组件随附文本为准。

## 4. 分发者检查清单

以下是分发前要完成的工作，不是本项目已履行的声明，也不是本项目发出的书面源码要约。

### A. 冻结真正交付的内容

- 记录 HAP/ZIP、全部 ELF/PE 库、插件、字体、音色库及其他资源的版本和哈希
- 记录顶层 SHA、所有实际使用的子模块 SHA、下载源码/二进制的 URL 与哈希，以及缓存/回退源码来源
- 对照静态链接输入、动态依赖、`dlopen` 名称、PE 导入、复制步骤和架构分支，生成该产物自己的依赖清单；不要把这份源码级清单当作完整 SBOM
- 若交付工具链运行库、SDK 库、额外插件或下载包，增加它们自己的许可审查

### B. 随二进制提供许可证和声明

- 随发行包提供适用的许可全文、版权、免责声明、上游 NOTICE 和变更说明；GitHub 仓库存在这些文件，不等于接收二进制的用户已经收到
- LGPL 3 路径须同时包含 LGPL 3 和 GPL 3 全文。其他组件仍须携带自己的许可，不能用一份通用 MIT/BSD 文本替代具体版权声明
- 保留 [Steam vendor 原有 NOTICE](entry/src/main/ets/steam/vendor/NOTICE.md) 和 Apache-2.0 许可；如原始上游还有适用 NOTICE，应取得并随附，当前快照来源仍待锁定
- FreeType 使用 FTL 路径时，分发文档应包含其要求的 FreeType 致谢；字体和数据资源还需独立核查
- 给用户可访问的许可材料入口；发布验收时检查最终 HAP/发行附件中的内容

### C. 提供对应源码与修改材料

- 为适用的 LGPL/GPL 组件提供与交付二进制匹配的完整对应源码，包含本地下游改动、补丁、构建/安装脚本和必要配置；一个可能移动或缺少补丁的上游链接不够
- 保存 [scripts/patches/](scripts/patches/)、[patches/](patches/)、实际补丁应用顺序和开关；同时提供嵌套依赖及自动生成所需材料
- 按实际适用条款选择并实施源码交付方式，确认接收者能取得内容。本次文档不自动作出“三年有效”或其他有约束力的源码供应承诺
- 修改库时满足相应标记、日期和许可义务；二进制来自外部缓存时，先取得对应源码和许可证明

### D. 满足修改、替换与重链接要求

- LGPL 2.1/3 对与库结合的应用有各自要求。动态链接方式须实际允许用户使用兼容的修改版库；只写“使用动态库”不能证明做到
- 静态链接或不能由用户替换库的布局，须按适用条款提供必要的应用对象文件/源代码、链接材料及操作方法，使用户能够修改库并重新组合
- 不得对为调试这类修改所需的逆向工程施加不相容限制
- 如适用 GPL 3/LGPL 3 的“用户产品”安装信息要求，须评估 HAP 签名、安装和设备限制，提供许可证要求的安装信息及可行安装路径。不要把生产签名私钥直接提交源码仓库；保密或平台限制本身不豁免上述义务，无法满足时应先解决分发方案
- 完成一次实际的替换/重链接验证，再记录可复现步骤

## 5. 本次尚未关闭的分发问题

1. **音色库授权未确认。** `entry/src/main/resources/rawfile/winehua-gm.sf2` 的内嵌信息指向 Chaos Bank V1.9、Cyber Punk Yun、Chaos Club（1999）。文件已被打包脚本使用，但目前没有找到明确的再分发许可。文件名、仓库可下载或内嵌版权信息本身都不是许可。应取得来源和明确授权，或在另行授权的代码/资源变更中替换或排除它；本次没有删除资源，也没有断言其侵权
2. **实际 FFmpeg 修订未固定。** 构建脚本允许已有源码，回退使用移动的 `meson-6.1` 分支。必须记录交付构建真正使用的提交和配置，才可确认 LGPL/GPL 及其他特性限制；不能拿当前分支文本证明历史二进制合规
3. **Steam vendor 只有月份/分支快照记录。** 现有 Apache-2.0 文本和修改 NOTICE 已保留；应补齐精确来源提交并检查原始 NOTICE/嵌套代码声明
4. **随包材料尚未验证。** 本次只增加文档，没有把这些许可复制进 HAP，也没有验收对应源码、用户替换/重链接能力或安装信息要求
5. **嵌套与外部依赖须按产物补全。** Wine、Mesa、FEX、GStreamer、FreeType 等包含多种文件级许可。此处保留已核实的主要声明，并不保证覆盖每个启用选项、宿主工具、字体衍生来源或 SDK 运行库

在这些问题关闭之前，不应以本次文档更新宣称当前 HAP 已获得完整开源合规放行。
