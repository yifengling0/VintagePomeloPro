# Wine 适配层等价重建

本阶段完成当前有效源码的父系重建，尚未升级 Wine 或 FEX 的生产 pin。

- 真实上游父系：Valve Wine `dc26e61847081a1b5cb0733dc30feba6ee575482`。
- 原 fork：`cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。
- 重建维护分支：`ohos/current-baseline`，提交 `a02acbd358e3f2d82eb4d6e0375dfbff0e5e0131`。
- 有效 Git tree：`19a4f09e7eee85b4ab2c144270250ea6bd346603`。

209 项 fork 差异按进程/内存、图形/输入、媒体、网络、启动和平台集成拆成主题提交；
随后按生产顺序处理 78 项登记补丁。7 项已在 fork 内，不重复应用；其余 71 项按序重放。
生产诊断策略和构建提供的共享头文件另行入链，合计 79 个适配提交。
独立临时索引从现用构建源码重新计算，11,227 个有效文件的完整 Git tree 与重建结果相同。
两次重建得到相同的维护提交和完整报告；第二次无需下载任何文件。

原 Valve commit 对象保存在 `wine-valve-base.commit`，其原始对象哈希与上游提交完全相同。
逆向审计清单可还原完整原始 tree；缺少的 131 个旧 blob 共约 7.8 MiB，按官方 API 提取并逐个核验
Git 对象哈希。复用现有对象数据库，未建立另一棵完整 Wine 源码或构建目录。
维护分支保留真实上游的浅历史边界，未来迁移时可继续获取更深父系。

`make_vulkan` 使用现有的 Vulkan 1.4.339 XML 与 `winevk.xml`，在稀疏临时目录重新生成。
全部 8 项输出与生产有效源码逐字节一致，包括原先待确认的 `vulkan_thunks.c`。
构建遗留的 `.orig/.rej` 只登记，不当成有效源文件；现有源码、补丁及备份均未删除或改写。

复现入口：

```sh
python3 scripts/reconstruct_wine_adaptation.py \
  --source workspace_temp/source-integrity/checkout/thirdparty/wine-valve \
  --work-dir workspace_temp/v2-development/upstream-transition \
  --report docs/upstream-sync/wine-baseline-reconstruction.json
```

输入必须是登记 fork 的实际生产有效源码。结果包含每项补丁的哈希、重放状态、主题提交、
生成输入和输出哈希。任何差异、缺文件、补丁失败或生成内容不符都会中止。
新 FEX 双架构构建、Proton Experimental 迁移和 Mini 性能验收继续按原计划推进；
本阶段的源码等价证明不替代迁移后的编译及设备回归。
