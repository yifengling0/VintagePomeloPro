# 第三方组件与许可声明索引

基线：`e2d001416d94ec01c31d4ffd38be3720d1496b47`（2026-10-04）。本清单基于该提交的源码、子模块指针、构建和打包脚本。
“使用”指脚本中的构建/复制路径，**不是已检查最终 HAP 的声明**。修改架构、开关、缓存或输入后必须重新核对。

原版权、许可原文和免责声明保留在原文件及 [licenses/third-party/](licenses/third-party/) 中；
[manifest.json](licenses/manifest.json) 记录副本的原始来源、版本、字节数和 SHA-256。
表内短名称只是索引，不替代组件完整许可，也不覆盖逐文件例外。
分发义务和未解决项见 [LICENSING.md](LICENSING.md)。

## 1. 主源码组件

子模块 URL 见 [.gitmodules](.gitmodules)。下列 SHA 是本次基线记录的精确 gitlink；分支名不作为固定版本。

| 组件 / 源码路径 | gitlink SHA | 许可与原文副本 | 使用路径 |
|---|---|---|---|
| Wine Valve/OHOS / `thirdparty/wine-valve` | `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b` | [LGPL-2.1-or-later；内嵌组件分别许可](licenses/third-party/wine-valve/LICENSE) | [env.sh](scripts/env.sh) 默认 WINE_SRC；Wine ELF/PE、音频和字体 |
| Wine legacy OHOS / `thirdparty/wine` | `f6492f848273ae3f4220151c8049e1202669cd49` | [LGPL-2.1-or-later](licenses/third-party/wine-legacy/LICENSE)、[第三方声明](licenses/third-party/wine-legacy/NOTICES.md) | 显式 WINE_SRC 备选；部分 OHOS 适配迁移来源，不是默认 Wine 核心 |
| FEX / `thirdparty/fex` | `86ff33bbe299cd8959a6610198c169b67ec419db` | [MIT 核心](licenses/third-party/fex/LICENSE)，嵌套依赖见第 4 节 | [build_fex.sh](scripts/build_fex.sh)：ARM64EC/WoW64 DLL；含静态合入的支持库 |
| Box64 / `thirdparty/box64` | `e970ee6f21799724697fc7c5f275f5442e7d035a` | [MIT](licenses/third-party/box64/LICENSE) | [Makefile](Makefile)：arm64+x64-Wine 的 box64.so；原生 arm64 的 wowbox64.dll |
| DXVK Legacy / `thirdparty/dxvk` | `7c5cc47f772b9564af177cea920da81a40ffd47c` | [Zlib](licenses/third-party/dxvk-legacy/LICENSE) | [Makefile](Makefile)、[assemble.sh](scripts/assemble.sh)：PE 图形 DLL |
| DXVK Modern / `thirdparty/dxvk-modern` | `05a0a66d74cc41e6a90a8cb62d459c1c6a28c783` | [Zlib](licenses/third-party/dxvk-modern/LICENSE) | 同上，运行时可选择配置 |
| VKD3D-Proton / `thirdparty/vkd3d-proton` | `3e5aab6fb3e18f81a71b339be4cb5cdf55140980` | [LGPL-2.1-or-later](licenses/third-party/vkd3d-proton/COPYING)、[全文](licenses/third-party/vkd3d-proton/LICENSE)；嵌套依赖另计 | [Makefile](Makefile)：即使运行时默认关闭，仍有构建/打包路径 |
| Mesa / `thirdparty/mesa` | `330124bf18f8e135346c3f592c8f8d99121fd343` | [主要 MIT，逐文件有例外](licenses/third-party/mesa/docs/license.rst) | [build_ohos_guest_gfx.sh](scripts/build_ohos_guest_gfx.sh)：virgl/softpipe/EGL/GLES、Venus；GLX/LLVM 关闭 |
| virglrenderer / `thirdparty/virglrenderer` | `bb33e52aa03199926f662e50ff22bd00d19d7e0a` | [MIT](licenses/third-party/virglrenderer/COPYING) | [build_native.sh](scripts/build_native.sh)：本地共享库和 vtest 服务 |
| libepoxy / `thirdparty/libepoxy` | `2597254fd42e021225db02734340857cae9a2596` | [MIT 系列及 Khronos 声明](licenses/third-party/libepoxy/COPYING) | [build_native.sh](scripts/build_native.sh)：VirGL 共享依赖 |
| FreeType / `thirdparty/freetype` | `42608f77f20749dd6ddc9e0536788eaad70ea4b5` | [FTL 或 GPL-2.0-or-later](licenses/third-party/freetype/LICENSE.TXT)；本清单按 [FTL](licenses/third-party/freetype/docs/FTL.TXT) 路径 | [build_freetype.sh](scripts/build_freetype.sh)：共享库；brotli/harfbuzz/png/bzip2 关闭；内嵌文件例外保留 |
| libffi / `thirdparty/libffi` | `3d0ce1e6fcf19f853894862abcbac0ae78a7be60` | [MIT](licenses/third-party/libffi/LICENSE) | [build_deps.sh](scripts/build_deps.sh)、[assemble.sh](scripts/assemble.sh)：共享运行库 |
| Wayland / `thirdparty/wayland` | `b2649cb3ee6bd70828a17e50beb16591e6066288` | [MIT 式许可及版权](licenses/third-party/wayland/COPYING) | [build_wayland.sh](scripts/build_wayland.sh)：client/egl/server 共享库和生成代码 |
| Wayland protocols / `thirdparty/wayland-protocols` | `6bcf87d9c17a36bb56943efeb4cbb851bd3734b8` | [MIT 式许可，XML 分别署名](licenses/third-party/wayland-protocols/COPYING) | [build_ohos_guest_gfx.sh](scripts/build_ohos_guest_gfx.sh)：生成代码；允许回退 tag 源码，发布时需记录实际输入 |
| libxkbcommon / `thirdparty/libxkbcommon` | `7a31e3585edf78be281559377e26d15f8c4bc655` | [MIT/X11 系列及逐文件声明](licenses/third-party/libxkbcommon/LICENSE) | [build_xkbcommon.sh](scripts/build_xkbcommon.sh)：xkbcommon/xkbregistry |
| libxml2 / `thirdparty/libxml2` | `5e9b167dce73bd6a804ab107ae4c4b95e6849597` | [MIT](licenses/third-party/libxml2/Copyright) | [build_xkbcommon.sh](scripts/build_xkbcommon.sh)：共享库 |
| xkeyboard-config / `thirdparty/xkeyboard-config` | `4225e14a2d0fca5a4f9cb8517cf12d96ea290d84` | [多种 X11 式宽松许可](licenses/third-party/xkeyboard-config/COPYING) | [build_xkbconfig.sh](scripts/build_xkbconfig.sh)：打包键盘布局数据 |
| OpenHarmony libdrm / `thirdparty/libdrm` | `bdd6856d4a488121c120237c2b7c33b17f6a1df1` | [MIT，保留文件级声明](licenses/third-party/libdrm/LICENSE) | [build_ohos_guest_gfx.sh](scripts/build_ohos_guest_gfx.sh)：Mesa 依赖 |
| PCRE2 / `thirdparty/pcre2` | `52c08847921a324c804cabf2814549f50bce1265` | [BSD-3-Clause 及文本中的二进制分发例外](licenses/third-party/pcre2/LICENCE) | [build_gstreamer.sh](scripts/build_gstreamer.sh)：共享库，JIT 关闭 |
| GLib / `thirdparty/glib` | `3c543ef69ffab7c78e29eaf383e7fe2c7df6cd49` | [LGPL-2.1-or-later](licenses/third-party/glib/LICENSES/LGPL-2.1-or-later.txt)，其他文件可有附加条款 | [build_gstreamer.sh](scripts/build_gstreamer.sh)：glib/gobject/gmodule/gio/gthread |
| GStreamer / `thirdparty/gstreamer` | `9137f539a022ec3f5f7c5ee704198dbf35a41940` | [core 及各插件组 LGPL 文本](licenses/third-party/gstreamer/subprojects/)；按组件/文件细分 | [build_gstreamer.sh](scripts/build_gstreamer.sh)：core、base/good/bad/ugly 所选插件及 libav；bad/ugly 的 GPL 选项关闭 |

GStreamer `asfdemux` 的文件头是 LGPL-2.0-or-later，不能仅凭所属 `ugly` 目录认定为 GPL。
主 `libentry.so` 链接 Wayland/OHOS 库；Wine、图形和翻译器有各自的共享库/子进程加载路径，不能把整个 HAP 想当然视作单一静态链接程序。

FreeType 致谢：本软件的部分功能基于 FreeType 项目的工作（https://freetype.org/）。适用版权和完整条件见上述 FTL；不得把字体引擎的许可当成任意字体数据的授权。

## 2. TLS 链：实际使用发布压缩包

[build_gnutls.sh](scripts/build_gnutls.sh) 下载下列版本，使用 `--disable-static` 构建共享库，并按组件关闭部分工具/测试；最终 `stage_libs` 和 assemble 复制对应 `.so`。GnuTLS 的工具/测试明确关闭。
同名子模块虽保留在 `.gitmodules` 中，却不是此脚本当前选择的源码。
下表采用 LGPL 路径；双重许可选择仍属于各上游原授权，不在此处改写。

| 实际版本 | 许可选择与已保留原文 | 官方源码 |
|---|---|---|
| GMP 6.2.1 | [LGPL-3.0-or-later 或 GPL-2.0-or-later](licenses/third-party/gmp-6.2.1/README) | [gmp-6.2.1.tar.xz](https://ftp.gnu.org/gnu/gmp/gmp-6.2.1.tar.xz) |
| Nettle/Hogweed 3.10.2 | [LGPL-3.0-or-later 或 GPL-2.0-or-later，保留文件例外](licenses/third-party/nettle-3.10.2/README) | [nettle-3.10.2.tar.gz](https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz) |
| libtasn1 4.20.0 | [库 LGPL-2.1-or-later](licenses/third-party/libtasn1-4.20.0/README.md)，工具/测试 GPL 条款另外适用 | [libtasn1-4.20.0.tar.gz](https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.20.0.tar.gz) |
| libunistring 1.3 | [LGPL-3.0-or-later 或 GPL-2.0-or-later](licenses/third-party/libunistring-1.3/README) | [libunistring-1.3.tar.xz](https://ftp.gnu.org/gnu/libunistring/libunistring-1.3.tar.xz) |
| GnuTLS 3.8.3 | [主库 LGPL-2.1-or-later；依赖版本要求不能忽略](licenses/third-party/gnutls-3.8.3/LICENSE) | [gnutls-3.8.3.tar.xz](https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.3.tar.xz) |

GnuTLS 附带的 inih 另有 [BSD 许可](licenses/third-party/gnutls-3.8.3/lib/inih/LICENSE.txt)；本地构建会修改其 `ini.c`，对应源码须包括该步骤。
[LGPL 3 全文](licenses/third-party/gnu/LGPL-3.0.txt) 与 [GPL 3 全文](licenses/third-party/gnu/GPL-3.0.txt) 一并提供。
清单里的发布压缩包哈希仅记录本次取得的证据字节，不代表现有构建脚本已实施校验。

## 3. 直接引入和非子模块组件

| 组件 | 版本/来源与许可证据 | 边界 |
|---|---|---|
| OpenHarmonyASF `steam_core` | 2026-09 `main` 快照，原始 SHA 待确认；[Apache-2.0](licenses/third-party/openharmony-asf/LICENSE)、[现有修改 NOTICE](licenses/third-party/openharmony-asf/NOTICE.md) | 直接用于 ArkTS 应用；应补齐上游原始 NOTICE 和精确导入提交，不涉及 Steam 客户端授权 |
| Vulkan Loader | v1.3.290 / `f8616928ee19f6c7fd648c1cf1f456cba3771855`；[Apache-2.0](licenses/third-party/vulkan-loader/LICENSE.txt) | [build_ohos_guest_vulkan.sh](scripts/build_ohos_guest_vulkan.sh) 及 [本地补丁](patches/vulkan-loader-v1.3.290-ohos.patch)；共享 guest loader |
| Vulkan Headers | v1.3.290 / `b379292b2ab6df5771ba9870d53cf8b2c9295daf`；[按文件 Apache-2.0 或 MIT，部分仅 MIT](licenses/third-party/vulkan-headers/LICENSE.md) | 头文件/生成输入，不是独立运行库；不替代其他组件自己的 Vulkan-Headers pin |
| zlib | vendored 1.3.1；[原始完整许可注释](licenses/third-party/steam-zlib/NOTICE.txt) | [CMakeLists.txt](entry/src/main/cpp/CMakeLists.txt) 将 6 个 C 文件编入 `libsteamdecompress.so` |
| Zstandard | vendored 1.5.7；[BSD-3-Clause 路径](licenses/third-party/steam-zstd/LICENSE) | 同上，不改选上游其他许可路径 |
| xxHash | Zstd 内 0.8.3 [BSD-2-Clause 声明](licenses/third-party/steam-zstd-xxhash/NOTICE.txt)；独立 `hashes/` [声明](licenses/third-party/steam-xxhash/NOTICE.txt) | 两份引入均保留，不能只覆盖 Zstd 根许可证 |
| LZMA SDK | Igor Pavlov；确切版本未记录；[源文件公有领域声明](licenses/third-party/steam-lzma/NOTICE.txt) | 7-Zip 解码源文件，不把整套 SDK 或产品臆定为同一许可 |
| TinySoundFont / TinyMidiLoader | Wine pin 内 `wineohos.drv`；[MIT](licenses/third-party/wine-valve/audio/tsf.h.notice.txt) / [Zlib](licenses/third-party/wine-valve/audio/tml.h.notice.txt) | 音频代码的许可不能覆盖 SF2 音色数据 |
| Wine 内嵌库 | `wine-valve` pin 内的 [libs/ 许可原文](licenses/third-party/wine-valve/libs/) | 包括 FAudio、FluidSynth、png、jpeg、zlib、mpg123、xml2、xslt、ldap、lcms2、tomcrypt、compiler-rt、capstone、jxr、vkd3d 等；副本是该源码树的声明集合，实际编入哪些部分须检查构建/链接结果 |
| Wine 字体 | `wine-valve` pin 的 [逐字体原始 metadata](licenses/third-party/wine-valve/fonts/) 和 [Wine 许可](licenses/third-party/wine-valve/LICENSE) | assemble 复制 `fonts/*.ttf`；大部分 `.sfd` 有 LGPL-2.1-or-later 声明，`fixedsys_jp.sfd` 无完整内嵌许可文本，须连同 Wine 授权及来源保留 |
| FFmpeg Meson port | 移动分支 `meson-6.1`、本地压缩包或外部产品源码；**实际交付版本/配置未核实** | [build_gstreamer.sh](scripts/build_gstreamer.sh) 有构建并打包 libavcodec/libavformat 等共享库的路径；不能以当前分支许可证给未固定的二进制作保证 |

Wine Tahoma/Tahoma Bold 的 metadata 保留 Larry Snyder 和 Bitstream Vera 衍生/重命名说明。
[Wine legacy NOTICES.md 的 Bitstream Vera Sans 原文](licenses/third-party/wine-legacy/NOTICES.md) 同时保留字体原始许可及命名、销售、署名限制。
这不是微软字体二进制的授权；额外下载或用户导入的字体须另按其许可判断。

## 4. FEX 嵌套组件

下表依照该 FEX pin 的 Windows/ARM64EC/WoW64 构建路径整理。最终链接图仍须确认。

| 组件 | FEX 记录的精确版本 | 许可原文/证据 |
|---|---|---|
| fmt | `407c905e45ad75fc29bf0f9bb7c5c2fd3475976f` | [MIT](licenses/third-party/fex-fmt/LICENSE) |
| xxHash | `e626a72bc2321cd320e953a0ccf1584cad60f363` | [BSD-2-Clause](licenses/third-party/fex-xxhash/LICENSE) |
| rpmalloc | `1f6fb494f2a4237c35494786a3c8f1eba048b217` | [0BSD](licenses/third-party/fex-rpmalloc/LICENSE) |
| range-v3 | `ca1388fb9da8e69314dda222dc7b139ca84e092f` | [Boost Software License 1.0 及随附声明](licenses/third-party/fex-range-v3/LICENSE.txt) |
| unordered_dense | `3234af2c03549bc85656bfd3a86993bf1cd8aef1` | [MIT](licenses/third-party/fex-unordered_dense/LICENSE) |
| cpp-optparse | `9f94388a339fcbb0bc95c17768eb786c85988f6e` | [MIT](licenses/third-party/fex-cpp-optparse/LICENSE) |
| tiny-json | FEX pin 内文件 | [MIT](licenses/third-party/fex/External/tiny-json/LICENSE) |
| SoftFloat 3e | FEX pin 内文件 | [UC Regents BSD-3-Clause 完整原注释](licenses/third-party/fex/External/SoftFloat-3e/NOTICE.txt) |
| Cephes | FEX pin 内文件 | [原授权及 BSD 许可沟通记录](licenses/third-party/fex/External/cephes/LICENSE)，不改写为 FEX MIT |
| Musl/Arm 数学支持代码 | FEX pin 内 `Source/Windows/Common/CRT/Musl/` | [原 SPDX/版权头](licenses/third-party/fex/musl/)，保留 MIT 及其他逐文件声明；不能用 FEX 作者署名替换原作者 |

Windows 构建启用 rpmalloc，关闭 Linux jemalloc 和测试；VIXL disassembler/simulator、Tracy、Zydis 等可选依赖不能仅因子模块存在就认定为已交付。
改变构建选项后须补齐对应许可和嵌套 pin。FEX 静态 C++ 支持库也需要实际工具链运行库的声明/例外。

## 5. VKD3D-Proton 嵌套组件

以下 pin 来自上表 VKD3D-Proton 源码。`dxil-spirv` 的 Meson 路径构建静态库，包含下列 decoder/emitter 代码；并非只有顶层 LGPL 声明。

| 组件 | 精确版本 | 许可原文/声明 |
|---|---|---|
| SPIRV-Headers | `ae217c17809fadb232ec94b29304b4afcd417bb4` | [Khronos MIT 式宽松许可](licenses/third-party/vkd3d-spirv-headers/LICENSE) |
| Vulkan-Headers | `83e1a9ed8ce289cebb1c02c8167d663dc1befb24` | [Apache-2.0](licenses/third-party/vkd3d-vulkan-headers/LICENSE.txt) |
| dxil-spirv | `b537bbb91bccdbc695cb7e5211d608f8d1c205bd` | [LGPL-2.1-or-later 原文件授权](licenses/third-party/vkd3d-dxil-spirv/dxil_spirv_c.cpp.notice.txt)、[全文](licenses/third-party/vkd3d-dxil-spirv/LICENSE) |
| bc-decoder | 上述 dxil-spirv pin 内文件 | [Baldur Karlsson MIT 声明](licenses/third-party/vkd3d-dxil-spirv/third_party/bc-decoder/llvm_decoder.cpp.notice.txt) |
| glslang-spirv | 上述 dxil-spirv pin 内文件 | [LunarG/Google BSD 式完整声明](licenses/third-party/vkd3d-dxil-spirv/third_party/glslang-spirv/SpvBuilder.cpp.notice.txt) |

嵌套 SPIRV-Tools/SPIRV-Cross 并未列入审查到的 Meson 静态目标，不能仅因源码克隆存在就认定为运行时依赖。仍需用实际链接结果验证完整闭合。

## 6. 明确未覆盖或待核实的内容

- **Chaos SoundFont**：`entry/src/main/resources/rawfile/winehua-gm.sf2` 的 metadata 为 Chaos Bank V1.9 / Cyber Punk Yun / Chaos Club（1999）。有版权信息，没有找到明确再分发许可；现有打包会使用它。此项未关闭
- **嵌套依赖闭合**：已补入上述固定版本的主要声明，仍没有对全部上游源码逐文件扫描或对成品做完整链接审查；源码树根许可证不覆盖全部例外
- **Mesa**：[源码树许可集合](licenses/third-party/mesa/licenses/) 包含 MIT、Apache、Boost、SGI 及 GPL 文本。保留集合不表示这些许可都适用于当前启用的驱动，更不表示整个 HAP 为 GPL；需按文件和实际构建闭合
- **资源与 replay 数据**：UI 图片/音频和应用/benchmark 捕获 shader、Heaven 诊断资源未完成来源与权限清单。guest Vulkan 打包可能带入 replay 数据；生成的自有 shader 与外部捕获数据应分别核实
- **Wine Mono**：默认 `BUILD_WINE_MONO=0`；启用时 11.1.0 MSI 是多组件集合，必须另查其全部许可/源码义务
- **SDK/工具链和系统库**：OHOS、LLVM-MinGW、libc/libm/libz、C++/unwind 等的实际版本和再分发条款需从使用的 SDK/运行库核实。系统链接和把库复制到包内是不同情形。此处既不授予权限，也不认定不可分发
- **Steam/Windows/游戏和下载资源**：不因本项目 LGPL 取得任何许可；本次源码文件清单未发现已提交的 Steam 客户端二进制。以后若随包分发，须独立取得必要权利
- **开发/测试依赖**：`oh-package.json5` 的 Hypium/Hamock、构建工具和测试专用二进制不在本次运行时许可闭合范围；如果它们进入发行内容，应补充核实
- **最终发行检查**：对应源码、变更记录、许可证随包送达、用户替换/重链接和必要安装信息均未完成发行验收。详见 [分发清单](LICENSING.md#4-分发者检查清单)
