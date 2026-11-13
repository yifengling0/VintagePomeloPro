# 手柄输入对齐 main 与 Winebus 进度修复

日期：2026-10-06。`feature/main_proton`，HEAD `284026ce64c631961394f2aee6fadb3a4380e579` 加本地修改。本次未提交、推送或更新远端。

## 来源与实际漏迁移

用户要求参考 main 的手柄处理。已核对本地提交：

- 宿主 `891ff62aba936e1539e7cc98076e77e021e70a57`：`fix(controller): restore canonical stick coordinates for all sources`。
- main Wine `4405de0a90e5d2d3a7b50a63f2337cdb22b31468`：`fix(winebus): consume canonical WHGP v2 controller axes`。
- `a5418033c2a` 的 opt-in 门禁已存在于 Proton，没有重复迁移。

Proton 的配对 NAPI 和物理摇杆原先拆成两次 `SetAxis`，可发送只有一轴更新的中间状态；Hub 会对没有实际变化的状态调用 listener；宿主与 Wine 仍使用 WHGP v1，Wine 对已正规化的摇杆 Y 再次翻转。这三项是代码差异，不能直接量化为用户遇到的延迟或整体帧率损失。

## 已实施

- 从 main 迁移 `ControllerHub` 的两轴原子更新、径向死区、最后活跃来源接管及释放后恢复、非有限数值归零、仅逻辑状态改变时通知。
- 物理输入在 adapter 统一成右／上为正，配对 NAPI 使用一次 `SetStick`；扳机使用 `SetTrigger`。配对 NAPI 增加 source/slot/stick/trigger 范围检查。
- 保留原来的单轴 NAPI 和 `LogicalAxis` API。兼容 `SetAxis` 在同一个 Hub 锁内修改单轴，避免先读取另一轴再写回的竞态；扳机转交专用接口。
- 宿主和 Wine 同步为 WHGP v2。摇杆 Y 不再重复反转，hat 仍按 HID 的 Y 方向转换；协议不匹配会拒绝并诊断。有效 Valve Wine 端通过 `0043-winebus-canonical-whgp-v2.patch` 实施。
- 保留 Proton 较新的多客户端生命周期和非阻塞 `sendmsg` transport，没有用 main 的旧阻塞 bridge 覆盖。

## Winebus 与诊断配套

`0041-winebus-ohos-poll-progress.patch` 修复没有消费设备却保留可读 socket、无效 fd 或 poll 错误时的空转：关闭失效连接、复位状态、50 ms 退避；正常有设备的 poll 保持 20 ms。旧生产逻辑真实 socket 主机复现可在 100 ms 内循环约 54 万／73 万次；候选降为 0／1 次 poll，并正常处理可读及断开状态。

旧 DLL 会话曾有约一个核的 Winebus CPU，但本次 11:20 的 RichMan8 冷切换中所有 winedevice CPU 很低。因此该修复消除已复现的异常空转，**不作为当前人物首次加载卡顿的共同根因**。

`0042-opengl-opt-in-wgl-lock-wait.patch` 补此前没有测到的 `wgl_lock_wait`。仅 `+winehua_perf` 时读时钟并聚合，默认关闭，不改变 GL 同步和结果。为保持连续补丁重放，保留原 0037 的文本与 mutex 调用，通过本文件包装目标 mutex。0035 显式 flush 正确性保留。

## 已完成验证

- Controller merge：59 项检查，0 失败，含保留单轴 API 的 4 项检查。
- Bridge：多 peer、按下／松开、震动、并发包、断开、背压和重启通过。
- Protocol：宿主／有效 Valve overlay 标记块逐字节相同；配对更新和两次重放一致，共 3 项通过。
- Winebus 6 类真实 socket 情况、真实 mutex 锁争用／默认关闭／锁所有权普通及 ASan/UBSan 通过；不声称 LSan 总测试通过。
- Unix perf、boundary、upload 相邻回归通过；40 个注册 overlay 连续重放两次一致。
- 本次修改范围 diff whitespace 和 build_wine shell 语法检查通过；未改既有 dirty 0037 的空白。

OHOS 宿主 `libentry.so` 使用已有 Debug CMake 缓存增量构建成功。Wine 使用全新独立源码和 `build-controller-followup-20261006`，只构建 Winebus／OpenGL Unix 模块及依赖，生产缓存身份检查通过。scratch source 用完整 tree SHA 校验，`source pin verified=False`、superproject gitlink verified=True；没有绕过检查，也不声称完成全量 Wine 构建。

## 包与平板状态

基底为正确 FEX 候选，签名包 SHA256 `87ffea5bf48f8ab21b1c03e606f246c4b5f784766427ef16f890e8fd4dee1276`。本包只替换 HAP 的三个 native 库：`libentry.so`、`winebus.so`、`opengl32.so`。strip 前后全部 SHF_ALLOC section 地址、标志、大小和字节一致；直接 native 依赖由 HAP 或 SDK sysroot 提供。payload、runtime manifest、runtime version 和 ArkTS 字节保持一致；继承的新 WOW64 FEX DLL SHA256 `5a11aa6a68d196a738fa3adff3cc745b9bd67232ee4033bd97dcde38c64a0750`。

新包：`F:/VintagePomelo-Workspace/workspace_temp/controller-followup-20261006/entry-default-controller-followup-debug-signed.hap`。

- 签名 HAP SHA256：`09c7f532f6b4ce5315a50187a70b63b5143cf165a5c6f98db129fc181592ffd6`。
- 大小：358768939 bytes。
- 版本：`1.4.5-proton.26-alpha / 1004035`。
- 官方 `hap-sign-tool verify-app` 成功；签名前所有 entries 内容一致，仅增加签名工具生成的 `.pages.info`。
- 已按用户既有授权结束目标会话并覆盖安装，保留数据；HDC 明确安装成功、bm 版本核对通过，安装后应用没有自动启动。
- 手柄真机方向、松开、触摸共用和延迟改善尚待用户操作确认，不能把主机通过等同于真机验收。

证据：同目录 `package-identity.json`、`sign-verify.log`、`device-commands.json`、build/test logs；没有提交包、库、原始采样或签名材料。

## 手动验收

用户启动旧柚 Pro 和原来的游戏／程序，分别测试方向键、左右摇杆的上下左右及斜推、松开回中、按钮／扳机，以及触摸摇杆与物理手柄交替。可在 `joy.cpl` 或实际支持手柄的游戏中核对；启动和输入继续由用户执行。检查本次连接是否出现 WHGP mismatch、反复断连或高 CPU。

性能问题继续单独跟踪人物首次准备与重复切换，使用新锁等待计时补齐证据；没有以这次手柄修复宣称解决 32 位 VirGL 帧率减半或缺图。
