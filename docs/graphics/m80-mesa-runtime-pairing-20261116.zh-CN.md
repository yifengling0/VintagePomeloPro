# M80 Wine 桌面启动与 Mesa 配对修复

2.0.2 的手机 NCP 修复没有丢失。调试签名冷初始化中，所有进程创建返回 0，WOW64 745/745 文件正常，wineboot 返回 0 并完成持久化核验。后续桌面退出是另一处图形运行库配对错误，现已在 M80 修复验证。

## 根因与修改

手机 VYG-AL00，OpenHarmony 7.0.0.111。失败现场为 EGL 空函数调用，随后 explorer 报 `failed to set persistent desktop lifetime error 6`，最终桌面就绪超时。

临时定位库将 LR 解析到包内 `libEGL.so+0x3682c`，对应 `dri2_setup_screen()` 的 `pipe_screen::is_format_supported` 调用。旧 EGL 从结构偏移 728 读取函数；当前 Gallium 在偏移 736 保存该函数。Mesa 0025 补丁在 `pipe_screen` 开头增加了一个 8 字节指针，发布整理时只重建/替换 Gallium，EGL 与 DRI 别名仍为旧批次。手机现场确认加载了 el1 副本；Mini 先前验收不能代表这组实际混用组合通过。

修复复用同一 Mesa 增量构建，成套生成 EGL、GLES1、GLES2、Gallium，将 bundle 与嵌套 `guest_gfx` 中的库及 DRI 别名统一。保留桌面持久化语义和原有进程创建实现。当前 Gallium 本体 SHA 未变；修正其消费者及旧别名。

防回归措施：

- 运行库校验拒绝与 Gallium 不同构建的 DRI 别名。
- 新运行库清单记录完整 Mesa 集合；校验 bundle、数据区副本及其哈希。
- Mesa 构建身份升级为 schema 2，包含 EGL/GLES，拒绝旧的仅 Gallium 身份及后续替换的 EGL。
- 发布运行库锁更新到修复后的实际字节。

临时 fault 定位仅在任务目录编译，未修改产品 `wine_child.cpp`。最终包恢复原始产品 `libwine_child.so`，UI、Wine/FEX、win32u、winewayland 等保持原有产品字节。

## 验证

- 真机始终使用真正 debug profile，debug=true、JIT ACL、设备覆盖及 SDK 验签通过。本轮未使用正式签名。
- 覆盖安装后，01:06:43 引擎进入 ready，实际截图显示 Wine 桌面及开始栏。
- 正确执行 `aa force-stop com.vintage.pomelopro` 后重新启动，01:08:22 再次 ready；wineboot 返回 0。
- 两次修复后启动的 stderr 均未再出现该空 PC 调用或持久桌面错误。未清理原 prefix/用户数据。
- runtime_library_guard 11 项、guest_gfx_build_identity 8 项、upstream_release_runtime 5 项通过；NCP 真实动态链接三种加载顺序通过。
- 新校验实际拒绝原 2.0.3 混用包，接受修复包；52 项发布运行库锁核验通过。
- 标准 `package.sh app-unsigned` 完成。其 HAP 与真机已测调试包逐成员核对，只有 module.json 的 app.debug 不同；APP 内 HAP 与独立 HAP 只有 pack.info 不同。

这些结果覆盖初始化、桌面和强停恢复，不代表游戏性能或三种图形后端的完整回归。后续设备被用户切回远程桌面，未将远程桌面画面当作 Wine 交互验收。

## 交付

版本 2.0.3 / 2000030，arm64-v8a。基于源码提交 `574483e045214cebeaba0b668b9ee123c93086e2`，本次提交纳入修复的校验代码及锁；构建时这些修改尚未提交，原有十个 dirty 子模块保持原样。

正式交付目录：`F:/VintagePomelo-Workspace/artifacts/v2.0.3-mesa-fix/`。

| 文件 | bytes | SHA-256 |
| --- | ---: | --- |
| VintagePomeloPro-2.0.3-2000030-unsigned.hap | 624535831 | dea5e3a5ca27bc253040b553a376418b70a0edcc06964aea78a2fd06771c619f |
| VintagePomeloPro-2.0.3-2000030-unsigned.app | 519320207 | 5c95d4d3895cbb574322cb75e999461f67a77fe88265fed9cf1244bacbe9812f |
| VPP-2.0.3-M80-Mesa-fixed-debug-signed.hap | 627456910 | 7fae8146bde61b80cc4f4a1beb3b3d850b6bd3a00f89df3c131384a19009350f |

调试包只在 `F:/VintagePomelo-Workspace/workspace_temp/mini-virgl-performance/phone-init-regression/` 保留一份，M80 已安装。必要的构建、签名、设备日志与核验收据同目录。原正式签名 APP 的 SHA `7d7759186331b68b133cdbea5465104619fee2fc9d0b448e3057abc6e86bc9c9` 未包含此修复，不应提交市场；旧正式文件保留并标注过期，本轮未重签、未上传。

后续正式签名交付已完成，当前可上架 APP 为 `F:/VintagePomelo-Workspace/artifacts/v2.0.3-mesa-fix/VintagePomeloPro-2.0.3-2000030-release-signed.app`，519335008 bytes，SHA-256 `97e819d0e0aa96d4f82f755ae0d0e8c520e2f62017d34f803911d250bab42eb1`。SDK APP/profile 验签通过，尚未上传市场；此正式包不含随后新增的多手柄修复。
