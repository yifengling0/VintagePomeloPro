# Steam 下载与 OHOS Wine 退出调查（2026-10-08）

## 结论

新附件来自手机，应用日志明确 `NCP backend=forks deviceType=phone`，宿主启动记录 `directNcp=0`。系统 NCP 未参与这一轮运行；应用通过手机 fork/broker 兼容路径启动了 Steam 和 CEF，完成目标 depot 鉴权、获取 manifest 并接收下载数据。因此手机不支持系统 NCP 是实际架构差异，但目前不能把它直接认定为下载解包失败的根因。

实际发现两个独立方向：历史 Steam 的实际补丁身份仍需核对；平板 Wine 退出链路存在已确认的 OHOS `exit()` 拦截问题，已制作并验证局部修复。FEX 多线程程序仍有另一条退出异常，不宣称本次已经全部解决。

## 新下载日志

`content_log (1).txt` 中有 356 次解包失败、196 个不同块。其中 depot 1942281 为 319 次／159 个不同块，depot 2436941 为 37 次／37 个不同块。各失败记录的压缩长度非零，产出与输出缓冲统计均为 0。

两个 depot 都有鉴权成功及 manifest HTTP 200；1942281 的待处理块由 249 降为 159 后停止。与此前失败的 159 个块特征一致，但尚未取得真实失败块的解密后格式，不能仅据此证明每个失败都是 Zstd。CDN 被标坏是解包失败之后的处理结果，不足以证明 DNS/CDN 为首要原因。

附件没有实际 steamclient/helper 的哈希或加载模块清单。更新旧柚 Pro 的 HAP 不会替换用户外部游戏目录的 Steam 文件。本轮只读核查电脑 `Downloads/Steam/Steam` 为完整 v2；平板 `Z:/games/steam-legacy/Steam` 六个 DLL 为完整 v3 候选。这两个目录的身份均不能代替附件对应手机的实际身份。

增加 `scripts/steam_legacy_zstd/audit_install.py`，对实际目录只读计算六个 DLL 的 SHA256，区分原版、v2、v3 和混用／缺失／未知文件。只读文件身份不证明运行中模块，须完整退出 Wine 后冷启动。没有更改账号、下载鉴权、CRC、最终文件校验或冻结客户端版本。

## 平板退出故障：已确认的一个根因

设备实际为 MatePad Mini / MLR-AL10，HarmonyOS 7.0.0.109，旧柚 Pro 1.4.5.29 / 1004038，系统 NCP 路径。

通过 `hidumper -s 1201 -a "-p Faultlogger ..."` 读取系统崩溃日志，绕过 shell 无法读取原始 faultlog 目录的限制。07:49:00、07:53:45、07:56:12 三份记录均给出：

```text
Reason: SIGABRT(SI_TKILL)
LastFatalMessage: [appspawn_server.c:69] Unexpected call: exit(0)
raise -> abort -> libappspawn_helper.z.so(exit+144) -> ntdll.so
```

这证明部分所谓退出失败来自 OHOS appspawn 的 `exit()` 拦截。不能将正常程序写出结果、甚至 NCP 偶尔报告退出 0，直接当成所有退出链路正常。

新增 `0046-ntdll-ohos-process-exit.patch`：仅 OHOS 的 `process_exit_wrapper()` 在关闭 server socket 后使用 `_exit(status)`，避开宿主 appspawn CRT。Windows DLL 退出／CRT 清理在进入这个 Unix 最终出口之前执行；其他平台保留原 `exit()`。没有修改 FEX 默认、x87 设置或图形路由。

构建使用全新 `build-ncp-exit-20261008` 与独立源码，不复制旧 Wine 目标对象。只编译所需 Unix ntdll.so；宿主工具和依赖来自已验证构建。候选在 1.4.5.29 未签名 HAP 基础上只替换 `libs/arm64-v8a/ntdll.so`，Wine 数据 ZIP 与 CPU DLL 不变。ELF 代码签名及 HAP 官方 verify-app 通过。设备覆盖安装，保留 prefix、游戏和存档。

| 验证 | 结果 |
| --- | --- |
| 主机实际生产函数回放，原版与 OHOS／其他平台路径，状态 0／7／255 | 通过；原版触发模拟 appspawn 拦截，OHOS 保留退出码且关闭 socket |
| 候选原生 system32 CMD，写出全新标记文件 | pid 51260，NCP exit=0；此前同类 CMD pid 45481 退出异常并有 DFX SIGABRT |
| 64 位 Steam AES→解包→校验测试 | pid 52306，180 checks / 0 failures，时间戳 2026-10-08T00:53:18.330Z；随后 NCP SIGSEGV，不能算完整进程成功 |
| FEX 32 位 syswow64 CMD | pid 52226，写出全新标记；随后 NCP SIGSEGV，未解决 |
| 初始桌面的附加 Explorer | 候选仍有退出异常；不宣称稳定性已恢复 |

干净局部修复设备候选 SHA256：`353a4a040b1003019dac134c82d4fa839e09fa9578265eda717df2fdfe18f6c7`。仅供调试，不是新正式发行版本。另有仅本地的线程关闭阶段诊断包，未加入发布补丁。

## 剩余问题与下一步

32 位 CMD 和真正 x64/FEX 解包测试仍正常执行后在退出阶段失败，与已修复的原生 `exit()` 拦截不是同一个结论。默认 stderr 是多个子进程共用文件，未带 PID 的退出标记不能可靠归属。局部线程阶段诊断也不能替代最终故障栈。下一步需要分进程 stderr／带 PID/TID 的退出阶段记录，结合最终 native PC，定位 FEX 线程清理、Wine syscall 栈切换和宿主 pthread 收尾。

Steam 下载优先核对附件对应手机的六个 DLL 和实际加载路径，再以同一客户端、同一公开加密块做手机 fork／平板 NCP 的运行对照。如正确补丁仍失败，加入默认关闭的解包分支诊断，取得格式、返回码、期望长度与缓冲状态，不记录账号密钥、原始数据或 CDN 授权 URL。随后恢复目标游戏下载并确认最终安装完成。当前没有真实下载完成结论，也没有证明切换 NCP 可以解决解包错误。

原始附件、崩溃详情和账号现场只保留在本地 `workspace_temp/steam-exit-investigation-20261008/`。可分享证据仅为脱敏统计与必要的验证摘要。
