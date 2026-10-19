# Linux ARM64 Steam 开发分支

后续 Linux Steam 开发统一在 `main_proton_steamarm64` 进行。此分支从已验证的
`31cf44c3ee3152f5d5bd018bffc35c71a951f4a0` 建立，完整保留当前会员、购买、续费提醒、
云同步、Wine/FEX/图形兼容修复，以及 Linux Steam 的源码、补丁、构建脚本和测试。
这些改动一起保存到新分支，不使用 stash，也不只保存为暂存区内容。

`feature/main_proton` 继续作为 Windows Steam 对应分支；本次不移动其分支指针，
也不向它合并 Linux Steam 实验。原 `feature/account-cloud-membership-mvp` 的提交指针同样保留。
两条线后续共享会员、同步等功能时应明确选择公共提交，避免直接把 Linux 实验整支合并回去。

当前 Linux 工作区仍使用原目录名 `vpp-account-cloud`，以复用已有增量构建缓存；
目录名不代表当前 Git 分支。开始构建或修改前以 `git branch --show-current` 为准。
主工程固定 Mesa 子模块为 `557fa3e781ea6e78fcff3ee759c25f60c3055064`。

## 运行边界

使用官方 Linux ARM64 Steam、外置最小 GNU 运行时和 Xwayland/Wayland 接入，
Windows 游戏由现有 OHOS Wine/FEX/DXVK 执行。不得以 Windows Steam 客户端、
QEMU 或 GNU Wine 的运行结果代替此链路验收。Steam、游戏、外置运行时及账号数据不随源码入库。

Steam 接口桥仍为显式启用的实验功能。手机已验证进程私有注册表视图、官方接口来源核验，
尚未完成新桥下真实 SteamAPI 初始化、认证、回调及游戏主菜单验收。
平板此前通过旧注册值加载的 DLL 不能作为新桥成功的证据；后续两类设备均需验证独立链路。
手机 CEF 黑屏仍需结合游戏交接日志确认，关闭 CEF GPU 后文字正常不代表所有黑屏原因均已解决。

## 构建与验证

实验候选包继续复用既有快照及增量目录。构建使用 `STEAMOHOS_PIN_ENTRY_BASELINE=1`，
不在本轮修改其他任务维护的 `EntryAbility.ets`。完整源码快照保留该文件当前修改，
实机候选包的输入仍以对应 identity 记录为准。

会员与同步使用服务端自动测试、模型及服务回归；Steam 接口桥使用固定 SDK 输入的
主机测试与手机实机探针。mock 测试通过不能替代真实购买、认证或游戏启动验收。
详细能力边界见 [STEAM_API_BRIDGE.zh-CN.md](STEAM_API_BRIDGE.zh-CN.md)。

构建产物、设备日志、签名与部署凭据留在本机，不纳入此分支；只提交可复现的源码、
配置模板、补丁、测试和说明文档。
