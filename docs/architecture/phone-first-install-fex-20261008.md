# 手机首次初始化缺少 FEX 的修复（2026-10-08）

## 结论与证据边界

在 VYG-AL00 手机上卸载后安装 1.4.5.28，复现 wineboot 退出但初始化不通过。新 prefix 的 system32 缺少两颗 FEX DLL，32 位 rundll32 注册程序退出码为 53；system.reg 只有原生视图的 ExplorerBrowser / ShellWindows 注册，WOW6432Node 视图缺失。wineboot 自身完成令牌已经写成 `wineboot-init-ok`，因此真正不满足的是原有完整性检查，不能将它描述成 wineserver 没启动或完成令牌未写。

原包完整退出后再次启动，已有 system32 使原来的升级同步方法补齐两颗 FEX；同一运行时约 58 秒完成修复，32 位注册项出现，并进入桌面启动阶段。这是部署缺口的对照证据，没有修改 FEX、Wine 或图形二进制。

本轮连接前，手机原有包为 1.4.5.26，旧 prefix 中已有两颗 FEX。用户旧截图的最初失败原因没有同期日志，不能把所有旧版本或所有用户的初始化失败归因于本次 28 的新首启缺口。ERROR 后根据任意残留进程继续等待则是代码确认的独立问题。

## 修改

- `WineEngineService.syncManagedFexModules()` 在尚无 prefix 时创建 system32，并在启动 wineserver / wineboot 前部署 32 位 WOW64 与 64 位 ARM64EC 的受管理 CPU DLL。
- 部署前校验成对 DLL 非空且为普通文件；保留逐块比较、临时复制、完整性核对和 rename。历史无 FEX 的 x64 runtime 不创建额外目录，用户其他 DLL 保留。
- ERROR 重试复用现有串行 restart，先停止并等待旧进程 drain，再重新执行运行时修复与启动；不再把残留服务当作仍然有效的启动协调者。clean / reuse 请求在该路径保留。
- 未缩短 Wineboot 时限或绕过注册表完整性检查。FEX 默认、x87 设定、Wine/图形载荷均与 28 相同。

## 回归与构建

执行生产 ArkTS 方法的真实文件系统回归通过：新 prefix 双 DLL 部署、不完整 / 空 DLL 拒绝、已有文件升级、不变文件、用户 DLL 保留、短读、部分复制、rename 失败与重试。

执行生产 ERROR/restart 方法的回归通过：残留进程停止等待、并发 restart 共用操作、clean/reuse 转发、失败正常返回。初始化进度回归仍通过。

复现命令：

```sh
node scripts/run_wine_managed_cpu_tests.cjs <SDK Typescript 路径>
node scripts/run_wine_engine_retry_tests.cjs <SDK Typescript 路径>
python3 host_tests/wine_engine_progress_test.py --node <SDK Node> --typescript <SDK Typescript 路径>
```

`proRelease / release` 构建成功，版本 `1.4.5.29 / 1004038`，名称仅数字与点。官方 APP/HAP 签名、profile、未签名 HAP 无签名检查通过，APP 内嵌 HAP 与未签名 HAP 载荷匹配。Wine runtime ZIP 与 28 完全一致，SHA256 `4194066d9bed5313cd03aefc1040c2d2d5cbc9ce6e6a2d963f8f731aef61af50`。

| 发布文件 | SHA256 |
| --- | --- |
| VintagePomeloPro-1.4.5.29-1004038-unsigned.hap | 1836681121ed95b9eeee8d3633f54add2f791386d7f7e113f416432feadc888d |
| VintagePomeloPro-1.4.5.29-1004038-appgallery-release.app | e83a15f85f8c9b872923812fc17730a95706684b9d7ff3678fce9832accc0900 |

交付目录：`F:\VintagePomelo-Workspace\artifacts\VintagePomeloPro-1.4.5.29-20261008`。设备使用同载荷、本地设备材料签名的 HAP，哈希 `0ba68d855ceb43fe52ded686002e3712f61e2176f31cf252897843c2ab8604e5`。没有上传商店或提交构建产物。

## 真机证据

证据目录：`workspace_temp/phone-first-install-20261008`（Windows 工作区），含命令审计、版本核对、卸载后空 files 目录、截图、日志和私有旧 prefix 备份。旧 prefix 备份 1,284,758,528 字节 / 6,056 个成员已逐一检查普通文件可读；仅原来无法读取的 IPC socket 与两份临时 FPS 文件未保存，未还原到干净测试 prefix。

1.4.5.29 卸载后全新安装通过：安装前 files 目录为空；首轮 Wineboot 在 07:32:16.400 启动，07:33:42.974 完成（日志计时 86 秒），直接进入 DesktopAbility。两颗 CPU DLL 的实际哈希分别匹配 `5df18a0d55b1aea696d9b49413897598b63d9600d845e00c7ddca21438434bcf` 与 `618b3311f484e4831ce6c6a6b4ffaa255f604490996d414e9b5cb4252626e273`。完整性检查要求的原生和 WOW6432Node 四项 Shell COM 注册全部存在，首启标记清除，prefix 版本标记写入。

第一次进入桌面后直接点开始菜单成功，未先打开文件管理器来补焦点。完整停止应用再启动通过：07:36:24.030 执行 wineboot --init，07:36:27.031 完成（约 3 秒），走 prefix-ready 复用路径而非再次强制更新，桌面正常显示。失败重试路径已做生产方法回归，未在真机注入失败故障。没有 Steam 或游戏性能结论；其他用户设备的旧截图仍需各自日志判断。
