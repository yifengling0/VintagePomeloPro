# FEX 完整候选迁移与 Mini 验证

本阶段把完整 FEX 上游 `ab135df091d0` 迁移到 OHOS，未更换生产 pin。
21 项适配的最终提交为 `c3b2b2306347`，完整 tree 为 `fe8e476f8828d261dfa7f4f338a85144177b8058`。
20 项旧生产补丁和 2 项 fork 差异均已登记；上游等价项给出来源，其余按顺序重放。
保留 DEP、精确异常、SMC、内存权限和同步要求，诊断及严格乘法开关没有默认打开。

## 可还原源码与成组构建

锁文件包含完整上游、适配 tree、21 项有序补丁哈希、依赖及构建选项。
`fex-adapted-history.json` 保存对应原始 commit 对象，准备脚本逐项核验父系和源码树，
全新准备时恢复与本地相同的完整适配历史。8 项候选回归包含稀疏目录全新重放，
另有 23 项 VM 合同回归；共 31 项通过，包含 200,000 次混合 UnixCall/syscall 返回。
这些是主机语义检查，不代表 ARM JIT 性能或游戏验收。

实际使用的 13 项依赖均核验。GitHub trees 接口可能把查询 commit 回显成 sha，
不能将该值当 root tree。准备脚本从全部 leaf mode/blob ID 重建真实 root tree，
再与锁定 tree 比对。四项下载依赖均由 commit API 和重建 tree 双重确认，
各源文件逐个核验 Git blob；未变依赖通过相对链接复用，没有再复制完整依赖。

固定入口（仓库根目录，复用既有构建目录）：

```sh
python3 scripts/prepare_fex_upstream.py \
  --source workspace_temp/v2-development/upstream-transition/fex-candidate \
  --cache /mnt/f/VintagePomelo-Workspace/workspace_temp/v2-development/upstream-transition \
  --reuse workspace_temp/source-integrity/checkout/thirdparty/fex

python3 scripts/build_fex_upstream.py \
  --source workspace_temp/v2-development/upstream-transition/fex-candidate \
  --build workspace_temp/v2-development/upstream-transition/build \
  --cache /mnt/f/VintagePomelo-Workspace/workspace_temp/v2-development/upstream-transition \
  --reuse workspace_temp/source-integrity/checkout/thirdparty/fex \
  --llvm-mingw .temp/llvm-mingw-20260826-ucrt-ubuntu-22.04-x86_64 \
  --container vp-account-cloud-build-20261012

python3 host_tests/fex_upstream_candidate_test.py \
  --source workspace_temp/v2-development/upstream-transition/fex-candidate
python3 host_tests/fex_vm_contract_test.py \
  --source workspace_temp/v2-development/upstream-transition/fex-candidate
```

ARM64EC、WOW64 和两个 OHOS AArch64 Unix bridge 成组构建；PE 架构、Wine builtin 标记、
去符号前后的可装载 section 内容、桥接别名一致性均检查。
打包入口 `package_fex_upstream_candidate.py` 只产生显式 component-test-candidate，
对内外 ZIP 所有未变成员逐个比 SHA-256，并更新清单及来源记录；不放宽生产发布门禁。

## Mini 结果及限制

已安装 release/internaltesting 签名的 2.0.0 候选；签名、profile 身份及载荷逐成员校验通过。
Steam 登录保留、游戏库可打开。x86/x64 只读 Win32 探针均成功完成；
运行时、前缀的两个 DLL 和 bundle 的两个 Unix bridge 与构建产物 SHA-256 全部一致。
ARM64EC 进程对已加载模块的 MemoryWineUnixFuncs=1000 查询成功，亦取得桥接加载 trace。
探针 self-maps 输出为空，不以它证明完整进程映射；历史 loader trace 仅作补充。

正常退出曾确认 Steam/CEF 进程消失。重开能进入库；另一次重开长时间停在蓝色桌面，
随后商店显示 -118，切换库仍可使用。本轮最后通过 Steam 自身 -shutdown 正常关闭，
再次 ps 确认 Steam/CEF 消失，共享 Wine 桌面服务保留。

首次选择游戏约 3 秒看到选中态和操作按钮，6 秒时头图/动态仍未完整出现。
重启加载阶段三次约 6 秒采样的 CEF CPU 为 197.92%、145.24%、101.41%；100% 为一个核。
这是加载期间样本，不能当 idle 或性能对照；共享 RSS 不能相加为物理内存。
目前没有同一底包的受控 A/B，无法把等待归因为 FEX 或网络。
**不宣称 Steam 卡顿已解决；尚未通过性能、游戏及完整图形/媒体/输入验收。**
本轮只选择 Resident Evil 2，没有启动、串流或打开存档，遵守同账号其他设备游戏的限制。

## 签名输入与后续重校验

已签候选使用 `validation/fex-mini-input.lock.json` 和 `fex-mini-build.json` 的冻结输入。
之后修正依赖 root tree 来源核验，按当前 lock 增量重建得到 `fex-reverified-build.json`。
两次上游/适配源码 tree 相同，全部 DLL/bridge 的输出字节相同；因此设备组件仍与重校验匹配。
保留两份小型输入记录，避免重新描述已签包的历史来源。
精确包哈希、模块哈希、探针摘要和验收边界见 [机器记录](validation/fex-mini.json)。

下一步迁移锁定 Proton Experimental 的 Wine 完整源码，按主题保留 OHOS 适配。
FEX 生产 pin 继续保持 `86ff33bbe299`，不移动 `rc-2.0.0` 或其他历史标签，不触发线上 CI。
