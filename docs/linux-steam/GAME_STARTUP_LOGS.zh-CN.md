# CS2 与古剑奇谭启动日志分析及修复

## 输入与结论

直接读取用户提供的 ZIP，未解包或修改游戏：

| 日志 | 大小 | SHA-256 | 游戏与结论 |
|---|---:|---|---|
| logs-20261024-171231.zip | 2558582 | fe8c26b1056466a83a47f69131f6d0b4ab5cd75e0b694461348a52b49b3f6483 | CS2（730）：Windows 版已正确启动，随后退出；现有导出没有 Wine 异常详情，根因尚未确定 |
| logs-20261024-171513.zip | 6824087 | 4cf5c663a112d68ba2fb21105d1728ff866afb99f264332bb820066e58f19403 | 古剑奇谭（570780）：接口桥不支持 SteamClient017::SetWarningMessageHook，已确认会主动中止游戏 |

下述时间统一为北京时间；Steam 自身日志原始时间需加 8 小时。两份日志来自不同游戏库，不能将其中状态混用。

## 古剑奇谭：已修复的接口缺口

17:10:13–17:10:36 配置工具 `Gujian/bin/GuJianConfigTool.exe` 正常返回 0；17:11:02–17:11:09 正式游戏 `Gujian/bin/GuJian.exe` 返回 1（waitpid）。

PE 状态明确记录 `stage=call_failed, method=1911970518, status=1, sequence=44`。依据生成器的 ABI 目录，该方法是 `SteamClient017::SetWarningMessageHook`，Windows/GNU 均为槽位 21。旧桥把它交给通用参数编码器，函数指针不属于可传输参数，因此返回 Unsupported，并通过 `Failed()` 抛出不可继续的 `0xe0484d01` 异常。

这不是已证实的账号认证拒绝。GNU 侧最后成功的方法为 `SteamClient017::GetISteamParentalSettings`。状态中的 AppTicket 是最后创建的接口，不能据此认定 AppTicket 调用失败；`authenticationVerified=false` 是诊断中的保守常量，不表示官方返回了认证失败。

修复方式：

- Windows 游戏的告警函数地址仅保留在 Wine 进程内；注册请求只传布尔值。
- GNU 侧向官方 Steam 对象注册本地回调，将警告等级和文本放入有界队列（最多 32 条，每条最多 4096 字节）。
- 在游戏 Steam 回调泵中取回文本并调用 Windows 回调；防止回调重入造成重复分发。
- 注销、拥有该钩子的用户/管道释放及会话断开时清理钩子及队列。
- 同时覆盖所有已生成版本的 ISteamClient 与 ISteamUtils，保留官方来源核验和原有认证逻辑。

PE 与 GNU 两端必须成对更新。不能只替换游戏目录中的 DLL，也不需要引入 Windows Steam、修改游戏 EXE 或伪造认证成功。

日志还存在安装辅助程序返回 1，但当前导出没有对应 Wine stderr，不能断言其原因或已随本修复消失。

## CS2：Windows 路由已生效，退出原因待补充证据

AppID 730 的独立兼容工具映射为 `hmosdeck_proton`、优先级 250。旧会话中全局映射仍为 75，不能据此声称用户已安装最新全局 100 的修正版。

Steam 已补下 Windows 版本差异（约 518 MB 下载、891 MB 暂存），17:08:39 完成。之后三次启动均使用：

```text
Z:\SteamLinuxARM64\library\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe -steam
```

三次运行分别为 17:09:26–17:09:39、17:10:21–17:10:30、17:10:48–17:10:55。原先 Steam Linux Runtime 安装符号链接失败已不是本次启动卡点。

最后桥请求 `2613366265` 是 `SteamUtils010::GetConnectedUniverse`；两端状态均为 0，GNU 已完成 9 次请求，来源核验为真。未看到可确认的桥 Unsupported 或认证拒绝。

Native 记录 `exit=-1, source=ipc-death` 仅表示 IPC 断开且真实退出码未知，不是游戏的异常代码。现有 ZIP 未包含 Wine stderr，所以还无法区分 Wine/FEX、图形初始化、游戏自身或其他兼容性失败；也不能据此认定反作弊是原因。

为下一次复现补齐诊断导出：读取固定每日 `wine_stderr_YYYYMMDD.log` 的最后 64 KiB，保留加载/异常错误，继续执行凭据脱敏、常规文件与 NOFOLLOW 检查。不额外开启高频 trace，不扫描游戏文件或存档。

## 验证和复测

已通过 32 项接口桥完整回归，并在更新旧版夹具后通过 3 项告警专项回归（包括日志中的 SteamClient017 / 1911970518）。覆盖真实 GNU 回调注册和队列读取、PE 回调分发与重入、注销与生命周期清理、异常输入和队列边界。诊断回归中的 Wine 错误尾部、读取上限、敏感字段脱敏及符号链接拒绝均已通过。24 个版本的告警钩子可编解码，212 个 Windows 虚表与固定版本的 Proton 源码核对一致；这些是静态与主机验证结果。Release HAP 已编译，包内配对及图形组件校验结果见随包 steam-warning-hook-verification.json。

本地测试不是这两款游戏的实机通过记录。安装候选 HAP 并更新配套 GNU 组件，完整退出旧 Steam 会话后重新启动：先验证古剑奇谭是否越过原来的告警钩子调用；CS2 如仍退出，立即导出包含 Wine stderr 的新日志。保留现有游戏库和存档，无需清空重下。
