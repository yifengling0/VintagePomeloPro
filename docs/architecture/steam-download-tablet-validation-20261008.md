# Steam 下载：平板真实验证与反馈手机的判断边界（2026-10-08）

本轮保留冻结 Steam、FEX 默认及游戏图形策略，没有升级 Steam、清空 prefix 或跳过账号／下载块／最终文件校验。手机离线后按用户要求使用平板继续调查。

## 已确认结果

平板为 MatePad Mini / MLR-AL10，HarmonyOS 7.0.0.109，旧柚 Pro 1.4.5.29 / 1004038，系统 NCP 路径。外部 Steam 位于 `Z:\games\steam-legacy\Steam`。六 DLL 的只读哈希与完整 v3 候选一致；原 content 日志无新下载任务，不能据此推断下载正常。

部署独立解包诊断包后冷启动，进程中的诊断记录确认调用者为该目录下的 32 位 `steamclient.dll`。初始 gzip 请求成功。Steam 的商店页面一度显示 CEF 错误 `-118`，游戏库仍可操作；网页错误没有阻止本次 CDN 下载。恢复运行中应用的卡片后可显示 Steam，不将辅助程序结束后返回程序列表误判成主窗口退出。

附件失败的 Brotato / AppID 1942280 在此平板账号上没有许可，Steam 返回“无许可”。因此没有重测附件中的同一 depot，也没有改动授权检查。改用账号游戏库中的 Purrgatory / AppID 1713610 完成真实下载。

| 项目 | 实测 |
| --- | --- |
| 下载时段 | 2026-10-08 09:51:12–09:51:29（设备时间） |
| Depot / manifest / build | 1713611 / 1130161071657144734 / 22144708 |
| 下载／写出计划 | 126,782,064 / 192,924,256 bytes |
| 下载块 | 210 |
| 游戏块格式 | VSZa 56、VZa 154 |
| 32 位进程诊断总调用 | 250：gzip 38、PKZIP 2、VZa 154、VSZa 56；返回值全部 1 |
| 真实网络 | CDN 鉴权成功、manifest HTTP 200；LAN peer 连接返回 File Not Found，没有转成本地传输成功证据 |
| 最终状态 | 提交到 common/Purrgatory，Fully Installed，scheduler result No Error |
| 本轮新增 Failed unpacking chunk | 0 |

诊断总调用包含网络消息和 manifest，不能把 250 当作下载块数。210 个 VZa/VSZa 调用与 content 日志的块数对齐；CRC、输出尺寸以及 Steam 原最终校验保留。此结果证明修补后的真实下载路径可用，不只是公开样本调用成功。

## 修改和验证

`vpsteamzstd.c` 增加编译期结果宏，普通构建仍展开原有返回值；`decoder_diagnostic.c` 是独立可选构建，只在进程环境 `VPP_STEAM_DECODE_DIAG=1` 时记录。诊断 thunk 在 32／64 位均调用包装器；非 VSZa 回调原函数，VSZa 使用原 v3 解码逻辑。保持原 ABI、重定位、Windows x64 shadow space 和栈对齐。日志不包含块内容、密钥、账号或 URL。

诊断构建根据实际生成的 PE32 client/helper 哈希编译启动守卫，没有放宽到未知二进制。普通解码 DLL 重新构建与原 v3 完全相同：

```text
vpsteamzstd32.dll 57c1b0503d131853aa899a896d3a0077b9f62a44ec5545dd6ce58e0b798e0faf
vpsteamzstd64.dll e339ce8195a6fdffdd627a1c528fbacc0058fb6d4db7f35e2a2215fecf5c135b
```

Windows 两架构各完成诊断关闭 180、开启 180、强制重定位 181、上限测试 780 项，全部 0 failures；关闭时 0 行、开启及重定位各 337 行、上限 512 行。精确启动守卫 14 项通过，包括异目录、损坏／缺失文件拒绝。真机实际下载使用首版诊断 helper；随后仅将日志打开／写入失败的重试也纳入 512 次上限，并重跑上述主机测试。该后续日志限额调整未另做真机下载，解码逻辑不变。

下载后恢复原 v3 六 DLL并冷启动；从活动目录再次复制并计算六个文件的 SHA256，全部与原 v3 一致。控制器和相对触控板恢复原开启状态。恢复后的最后一次冷启动仍停留在 Steam 加载动画，没有取得第二次常规包游戏下载结果；不以此宣称启动稳定性已修好。已结束该加载中的 Wine 会话，保留下载游戏及原数据。诊断只作为问题定位工具，不替换常规运行包。

## 能判断与不能判断的部分

旧冻结 Steam 不支持 VSZa/Zstd 已在此前原版公开样本测试复现。现在又确认真实 CDN 提供混合 VZa/VSZa，补丁能够完整下载这款游戏。因而“原版客户端缺少新解包格式”确实能解释一类 `c>0,u=0,b=0` 的失败。

这次真正完成下载块解包的是 32 位 Steam 主进程中的 `steamclient.dll`，诊断 `bits=32`，所有 56 个 VSZa 成功记录都来自该模块。64 位系统和 64 位 Wine 不代表冻结 Steam 的每个进程也是 64 位；只替换 `steamclient64.dll`／64 位 helper 无法覆盖本次实际下载路径。分发和升级应使用完整六 DLL 组。

但 `content_log (1).txt` 的失败手机没有六 DLL 哈希，也没有解密后的格式／解包分支记录。本轮不把平板成功当成该手机已修好，不把附件全部 196 个不同失败块都认定为 VSZa。更新 HAP 不会替换外部 Steam 目录；应先核对并完整部署 v3 六文件，再完全退出 Wine 冷启动、重试同一 depot。

附件本身走 phone fork/broker，已成功鉴权、获取 manifest 和接收数据；平板此次走 NCP 并成功下载。这支持优先排查实际文件及本地解包分支，尚不足以证明所有手机 fork 与 NCP 行为等价。不能仅凭手机不支持系统 NCP 就认定下载必然失败。

若正确 v3 的手机仍失败，用独立诊断覆盖包抓首次失败：format、stage、result、input、max_output、before/after put、allocation、error、client 路径。若是 VSZa，再按 frame／CRC／buffer／allocator 阶段修复；若根本没有解包调用，则向上查 AES／下载块长度与调用链，不能继续盲目改 Zstd。同步查看 content 新增段，避免诊断上限漏掉后续记录。

本地原始与脱敏证据位于 `workspace_temp/steam-download-next-20261008/`，不提交账号现场、完整日志、HAP、二进制或签名材料。
