# 2.0.5 未签名测试交付

版本 2.0.5 / 2000050，分支 `feature/gamescope-port`。
产品与打包源码提交 `926e40e9480c9034882727a122eee4b3d1552a3a`。
本轮不合 main、不推送、不上架，不覆盖 2.0.4 正式交付。

## 包与构建

- 文件：`artifacts/VintagePomeloPro-2.0.5/VPP-2.0.5-arm64-unsigned.hap`。
- 大小：630868936 bytes（601.64 MiB）。
- SHA-256：`4f7148a7506a8954c4aebb9fa4b3f15bf0f9225e7a49cdd7d4c890a5d95836e8`。
- ARM64，最低 API 23，`debug=false`；使用既有 Docker `winehua-dev` / `vpp-pal4-release-build` 增量环境。
- 通过 `make gamescope-unsigned-hap` 编译实际 unsigned HAP，不从已签名包移除签名来制作交付。
- SDK `hap-sign-tool verify-app` 明确报告 `signature not found` / `No Hap Signing Block before ZIP Central Directory`，符合未签名交付要求。
- 新 unsigned 路径拒绝绑定签名的产品配置，不加入 `libgamescope_pixel_probe.so`，检查手机声明、包架构、版本及来源收据。

逐成员 SHA-256 对比 Mini 已验证的 `818201af` 调试包：
仅 `module.json`、`pack.info`、`pkgContextInfo.json` 和 `application-build.json` 改变；
移除 `.pages.info` 和测试像素探针。全部共用原生库、Wine/FEX、DXVK、Mesa、Gamescope core
及 `wine-data.zip` 字节相同，UI ABC 也相同。大小变化来自签名与测试探针等打包差异。
保留原当前及上一份 2.0.4 调试签名 HAP 作为设备测试与回退，不生成额外 2.0.5 签名包。

## 手机支持核查

1. `EntryAbility.onCreate` 先设置手机进程模式，再准备 Gamescope；没有把手机交给系统 NCP。
2. 手机 Game 会话通过同进程嵌入 core 输出；Game 页面和游戏启动不以独立 Gamescope 子进程可用为条件。
3. 手机独立窗口 IPC 输出被明确拒绝；早期 clean fork 不可用或死亡时不回退系统 NCP/晚期 fork。
4. 自动 Direct 能力探测在手机上返回 `phone_requires_venus`，保持现有 Venus 路线。
5. 设置面板按窗口宽度收窄并可滚动，沿用手机旋转与统一输入链路；包中含 `phone` 设备类型及 `READ_PASTEBOARD` 权限声明。

本轮未发现上述源码路径的新阻塞，且原生库与已验证 Mini 包一致；这不等同于手机验收。
本轮两次 HDC 查询仅见 Edge 和 Mini，M80 未在线，没有操作这些设备代替手机测试。
手机的首次初始化、已有数据升级、旋转、后台恢复、FSR/限帧实效及游戏退出重入仍待真机确认。

## 已执行验证

- `test-gamescope-process-backend`：手机无早期服务、服务死亡、平板/手机分流和 FD 所有权通过。
- `test-ncp-dispatch`：不同加载顺序及系统接口缺失场景通过。
- `test-game-presentation`、`test-game-mode-models`：几何、调度、设置继承、会话冲突、输入、剪贴板授权与手机快捷栏命中通过。
- `test-gamescope-recipe`：7 项配方测试通过；`test-gamescope-output`：输出、resize、回退及终止失败场景通过。
- `test-windows-steam`：参数、启动就绪、目录、派发所有权与恢复场景通过。
- Hvigor release HAP 构建通过；最终包 `runtime_library_guard`、`v2_product_contract`、Windows Steam helper、X11 闭包（40 ELF）、`verify_ci_release artifact` 的当前来源/运行时锁检查通过。
- 最终包无重复成员、签名材料或测试像素探针；版本/ABI/API/剪贴板声明通过。

旧 `check_runtime_components.py` 仍失败：其 `runtime-core.lock.json` 以 **1.4.9** 为基线，
首先拒绝现有 `vkd3d/limited-500k/x64/d3d12.dll`。本轮未放宽旧锁，也未替换运行时；
当前分支的完整 Wine/FEX/图形载荷锁与来源校验通过，且完整载荷与 Mini 已验证包字节相同。
此项保留为旧校验入口与当前所选运行时的基线不一致，不能将本轮称为全仓预检全部通过。

本轮没有新增设备性能测试，也没有完成手机或全部图形矩阵验收，继续保留 `renderQualified=false`。
详细逐成员证据及旧校验结果见交付目录 `delivery.json`，工作记录见 `workspace_temp/gamescope-port/unsigned-delivery.json`。
