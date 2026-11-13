# dot 接手：Proton 图形正确性与性能优化

日期：2026-10-05。发布分支为 `feature/main_proton`，本批改动基于 `0a877ea3724cf02e248d90c413aac92afbb38fd0`。本次发布源码、Wine overlays、回归测试和报告。后续性能方案尚未实施，也没有取得新的性能 A/B 结果。

## 当前结果与待验收项

- 用户已确认 texture-zero 候选中 Kingdom Rush 和 War3 可以正常启动。这是多轮窗口、焦点、OpenGL 修复后的结果，不能认定仅一个补丁解释了所有问题。
- 全屏黑边、四国语言启动链、手动启动游戏的 bitmap 字体默认已进入后续候选。生产回归通过，完整真机验收仍待确认。
- RichMan8 有声音但最初无画面的现场后来进入了菜单和地图；用户报告首次切角色卡顿、重复选择改善及人物残缺。
- 最新 0035 候选修复 WoW64 显式 buffer flush 发布旧数据；已安装，尚无明确的贴图或 FPS 验收。现有低帧采样全在这份包安装之前。

相关报告：

- [窗口、焦点、全屏及 OpenGL 调试记录](game-fullscreen-focus-followup-20261005.md)：保留各轮失败和后续修复的时间线，早期“未解决”结论只对应当时的候选。
- [main 字体修复的手动启动迁移](legacy-gdi-manual-launch-followup-20261005.md)。
- [RichMan8 缓冲刷新修复](richman8-buffer-flush-followup-20261005.md)。
- [main / Proton 实际渲染路径对照](richman8-main-proton-virgl-comparison-20261005.md)。
- [保留 FEX 的性能实施思路](proton-box-performance-alignment-20261005.md)。

## 必须保留的执行与测试约束

当前架构为原生 ARM64 Wine Unix 层 + FEX，不能通过切换默认 Box64 来“对齐性能”。RichMan8 是 32 位，其 i386 D3D9 / WineD3D DLL 仍由 FEX 执行，之后进入原生 OpenGL Unix 层、aarch64 Mesa、native vtest / virglrenderer 和 OHOS 队列合成。两边实际都是 D3D9 → WineD3D → OpenGL → VirGL，UI 的 DXVK 档位不能证明 D3D9 使用 DXVK。

用户要求自行启动应用与游戏；采集和安装不要自动拉起旧柚 Pro、Steam 或游戏。Kingdom Rush 使用本地特殊 EXE 入口，不以 Steam 启动替代。停止或安装只针对 `com.vintage.pomelopro`，保留用户数据和其他应用。

多窗口完整身份、producer 代次、有效前台记录、输入与显示共用状态、0035 显式 flush 正确性均应保留。不能通过关闭 GPU、同步或降低画质取得表面收益。

## 本批源码组成与可复现方式

Wine pin 保持 `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。Wine 的有效修改通过 `scripts/build_wine.sh` 注册的补丁管理，包含更新的 0020 和新增 0027–0035，不依赖发布新的子模块提交。本次将 32 个正常注册 overlay 从固定 Wine HEAD 重放两遍，31 个涉及文件均与本地有效源码逐字节一致，覆盖全部 24 个 tracked Wine 修改；第二遍仍一致。

新增补丁负责 ARM64EC SEH exit thunk、Windows 子进程诊断环境转发、最小化／恢复握手、短暂前台切换恢复、client-only remap、受控 readback 诊断、fshack context 能力检查、texture 0 处理以及 WoW64 显式映射刷新。宿主修改包括实际发送后提交键盘焦点、验证 owner 父链、GPU 全屏黑底、DLL override 合并及语言／字体启动链。

构建必须使用新的独立 `BUILD_DIR`，固定 `NATIVE_ARCH=arm64-v8a`、`WINE_ARCH=aarch64`、`GUEST_ARCH=aarch64` 和所选 Wine 源码。旧目录缺少身份记录或 srcdir／配置不匹配时会被守卫拒绝；不要跳过守卫或复用未记录身份的 Wine 对象。共享已验证外部依赖与复用 Wine configure／对象是不同操作。

`scripts/build_zlib_runtime.sh` 提供经源包 SHA256 校验的真实 zlib 1.3.1 OHOS runtime。若依赖缓存只含 SDK stub，按相同构建环境先运行该脚本；assemble 与 guest gfx 优先使用同一 `BUILD_DIR/zlib-runtime/<arch>/lib/libz.so`。这不是新增的默认构建阶段。原有 runtime guard 仍拒绝打包 SDK stub。

本地权威源码为 WSL Ubuntu-22.04 `/home/liufeng/src/vpp-proton`，Docker `vp-proton` 挂载为 `/data/src/winehua`。Git 操作使用 WSL checkout；构建产物、HAP、签名材料、备份和原始采样均不在本次提交中。vkd3d 嵌套依赖存在换行差异，dxvk-modern 有 `.wraplock`，均未变更源码 pin。

## 发布前验证

本次重新运行以下现成目标，均通过：

```sh
# WSL，仓库根目录
make test-dll-overrides test-gpu-followup test-egl-multi-consumer \
  test-wine-shm-state-cache test-opengl-wow64-buffer-flush \
  test-wine-font-compat test-arm64ec-seh

# Docker vp-proton，/data/src/winehua；需要可用的 Node.js
make test-font-import test-wine-language-launch test-wine-process-font-aa \
  test-broker-startup test-wine-patch-detection
```

其中 Wayland 状态缓存 13 项、minimize/restore 12 项、client remap 12 项、fshack 12 项、texture 0 5 项、activation return 15 项、WoW64 flush 6 项、字体／locale 4 项、SEH 编译／PE 检查 4 项通过。GPU 场景／输入 9 个生产场景、EGL 多 consumer 空间绘制、DLL overrides、构建身份和字体事务通过；broker 本轮在 Docker 中使用真实 Unix sockets／FD 通过。相关脚本 `bash -n`、暂存源码和文档 `git diff --check` 通过；补丁文件保留空行的单空格上下文标记，单独关闭 `blank-at-eol` 检查后其余空白检查通过。

本次没有运行整个 `make test`，也没有重新构建或安装 HAP。此前总测试曾有 LSan／Unix socket 环境阻塞，不能据这些针对性检查宣称全测试通过。平台 stubs 和编译检查不能替代 OHOS 真机行为。

## dot 建议首批工作

1. 修复已确认的 WineD3D 像素格式 ID／数组下标混用：`adapter_gl.c` 将查询成功的记录紧凑保存，`context_gl.c` 返回 WGL ID，`swapchain.c` 却按该 ID 索引数组。按 `iPixelFormat` 查找或分别保存 ID／槽位，覆盖连续、缺口、唯一、无匹配情况；不能简单减一。该缺陷尚未修复，也尚未证明是实际掉帧根因。
2. 让用户验收已安装 0035 包，分开检查人物完整性、未访问角色首次停顿、重复选择和地图稳态。
3. 添加按秒汇总的轻量计时：shadow 命中与复制字节、driver map 等待、readback／fence 等待、shader compile/link、CS 队列积压和 Present 到消费。按实际关键路径选下一项。
4. 验证 guest Mesa 现有 disk shader cache 的 OHOS 可写目录、命中和版本失效；再依据计时处理动态上传、等待范围、GL 调用批次或全屏合成快路径。

main 与当前都禁用 Mesa disk shader cache、设置 `VTEST_SYNC_GL_FINISH`、默认使用 egl-main；main 的 GLES direct 默认未启用。这些是共同优化空间，不能当作 Proton 新增回退。没有匹配的 Wine／Mesa／窗口模式／温度／内容 A/B 前，也不能断言 FEX 比 Box64 慢。

## 本地证据与最新安装包

最新包为 `F:\VintagePomelo-Workspace\workspace_temp\richman8-20261005\entry-default-richman8-buffer-flush-debug-signed.hap`，SHA256 `c04c99c764418ce3e10599e3d831db3d3e8f4f625f1a89eee0f3700689a8d8c7`。15:47 覆盖安装，版本 `1.4.5-proton.26-alpha / 1004035`。候选同版本，按哈希识别；独立 Wine 构建目录为 `build-richman8-buffer-flush-0a877ea3-20261005`。

诊断入口为 `workspace_temp/richman8-20261005/README.md`，分支对照证据为 `workspace_temp/virgl-main-compare-20261005/`。这些目录在 Windows 工作区，源码内只提交报告。后续按相同入口、分辨率、窗口模式和温度分开做 main／Proton 产品对照及 Proton 内部单变量对照，不使用旧候选的采样冒充最新包的结果。
