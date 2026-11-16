# Wine 多手柄独立接入（2026-11-16）

用户报告所有设备都存在只能使用一个手柄的问题；当前没有多只实体手柄可供验收。本轮完成通用代码修复、自动回归及调试包交付，实体手柄兼容性仍待验证。

## 根因与行为

原 Game Controller Kit 回调没有读取设备 ID，Physical 适配器把全部输入写入 slot 0。宿主新连接也只发送 slot 0 快照；Winebus 固定创建一个虚拟设备，只接受 slot 0，震动回包同样固定为 slot 0。这是两端共同的单设备限制。

现在通过 Kit 的 Button/Axis/DeviceInfo 设备 ID 区分实体手柄，首次枚举和后续热插拔分配最多四个槽位。已连接设备保持原槽位，空位可复用；第五只不会覆盖前四只。断开只清除对应槽位的 Physical 状态，晚到的离线事件不会重新抢槽位。slot 0 继续和触屏控制合并，维持现有单手柄用法。

Winebus 为连接的额外槽位按需创建 HID 游戏控制器，使用不同的 UID 和序列号，使 DirectInput/XInput 能区分设备。没有额外手柄时不会凭空创建三个空设备。连接建立前已经按下的输入保留到 HID 设备启动，拔插期间的延迟 start/stop 不会移除新设备。传输断开时释放所有手柄状态。

震动数据携带原槽位，宿主回调把它转换为对应的设备 ID，再按该 ID 选择外接马达；去重和停止震动也按设备隔离。固件返回的 Kit ID 与 vibrator ID 的对应关系尚无实体设备证据，不将编译或模拟结果表述为已实测震动兼容。

## 协议与发布完整性

WHGP 仍为 v2，header 16 bytes，state 20 bytes。`reserved[0]` 的 bit 0 表示拓扑信息有效，bit 1 表示已连接。旧宿主的零保留字段保持兼容；旧客户端继续处理 slot 0。完整多手柄支持要求新的宿主和 Winebus 同时生效。

`patches/wine/0083-winebus-multiple-gamepads.patch` 已登记到正常 Wine overlay 链，并应用到当前实际发布的 upstream Wine 工作树。原有 ntdll loader 未提交修改保持不变。复用现有增量目录只重编宿主输入对象和 winebus，没有复制完整源码或构建树。

staging 使用原子替换打断旧 winebus 文件的硬链接，保留其他共享基线。release-runtime.lock 和运行时 manifest 登记新 Winebus、四槽位协议、overlay SHA-256 及 `hardwareValidated=false`。打包会核对 controller metadata、协议块与 overlay，不以旧 Winebus 搭配新宿主。Mesa 配对修复继续保留。

## 验证与交付

- Controller Hub 原合并与坐标测试：59 checks / 0 failures。
- 四手柄生产适配器、Hub 和真实 Unix socket 联合测试：通过；覆盖独立按键/摇杆/扳机、首次快照、溢出、拔插隔离、离线回调、槽位复用、触屏合并、按槽位震动。
- 完整生产 Winebus C 源配合录制 HID 边界的测试：通过；覆盖四个不同身份、缓存启动状态、热插拔、延迟 stop、反馈槽位、断连归零和旧 v2 输入。
- 多 peer socket/backpressure 回归通过；协议 overlay 完整重放与幂等检查 3 项通过；发布边界测试 7 项通过。
- HarmonyOS arm64 宿主与真实 upstream Winebus 编译、ArkTS 编译和标准 HAP 打包通过。
- 调试签名、JIT ACL、profile 有效期及签名后 payload 一致性检查通过。对比上个 Mesa 修复调试包，322 个有效 payload 中仅 4 个改变：modules.abc、libentry.so、winebus.so、wine-runtime-manifest.json；其余 318 个逐 SHA-256 相同。

调试包：`F:/VintagePomelo-Workspace/workspace_temp/multi-gamepad/VPP-2.0.3-multiple-gamepads-debug-signed.hap`。
版本 2.0.3 / 2000030；627466419 bytes；SHA-256 `3495247a91016426e27a958b001404a211c57cdd6090dcb615b163568ff00bd2`。
小型收据与日志在同一任务目录。构建时源码基线为 `574483e0`，包含本轮未提交修改；本次提交同步纳入源码、overlay 和运行库锁，构建收据保持原始事实。本轮未安装设备、未上传市场；此前正式签名 APP 保留，其内容不含本轮多手柄修复。

以后有多只实体手柄时，在“输入与手柄”中使用“Wine 手柄”模式，重启 Wine 会话，打开 Wine 控制面板的游戏控制器（joy.cpl）。验证两只手柄分别响应，再检查拔掉其中一只、重新连接，以及游戏内 1P/2P 的 DirectInput/XInput 分配和震动。软件回归不替代这部分验收。
