# VPP 2.0 实施记录

## 2026-11-12：P0 CI、版本和 Windows 基线

- 从 `feature/v2-ohos-wine-winsteam@f990b176` 继续，不移动 `rc-1.5.8-final`。
- 新分支 push 和面向该分支的 PR 纳入现有 HAP workflow，保留原会员与 GNU 过渡检查。
- versionName `2.0.0`，versionCode `2000000`；bundle/签名身份不变。当前为过渡开发版，Linux UI/后端还未移除。
- `verify_ci_release.py` 只额外接受精确 `rc-<应用版本>-final`，保留 commit、版本和来源校验；错误版本、额外后缀和错误 commit 仍拒绝。
- 新入口 `make test-windows-steam` 校验生产 helper 来源/PE/HAP资源、客户端参数、事务导入恢复与实际 Steam 服务。`make test-game-runtime` 在开头调用同一入口，后续删除 GNU 测试时也保留 Windows 回归。
- 定向验证：`host_tests/ci_release_test.py` 17 项 PASS；Windows runtime 身份3项、C++参数、VDF/path/argv与14种事务恢复、实际服务的中文多库/缓存/安装状态/进程归属/冷热启动等 PASS。主机回归不代替真机下载、游戏和后台恢复。
- 完整回归、2.0 HAP 编译和 Mini Windows Steam 实测：待执行，不先标记 P0 全部完成。

历史 tag 的工作流文件仍是 67 的版本。修改新分支验证器不会自动改变旧标签工作流；重新运行旧 tag 原 job 仍会拒绝 final 后缀。修复适用于新提交上的标签；冻结源码重发布仍须有独立、可追溯的发布验证流程，禁止移动标签或跳过校验。

存储：核对任务现存两份较新 HAP、读取 module 身份并验证 SHA 后，清理12份旧 `app.hackeris.winehua / 1.0.13 / debug=true` 安装候选，回收4164315504 bytes。保留 VPP 当前67/c09测试包、旧任务较新两包、所有用户数据/备份、正式发布包、源码和共享缓存。构建复用 E 盘既有 WSL增量目录，未新建完整源码或运行库副本。

恢复执行核对：67ea17f7 的分支 CI 43158316892 已成功；final 标签 CI 43165968197 仅在源版本验证阶段失败。当前 Wayland close 生命周期回归通过，focus fixture ABI 已包含在历史提交 efb8ae2c。开发树的 Docker 子模块绝对 Git 路径和 VirGL 实验 overlay 影响全量主机回归，因此后续使用既有 source-integrity 独立检出及增量缓存验收；保留开发子模块，不重置实验文件。P0 定向回归与完整产品验收分开记录。
