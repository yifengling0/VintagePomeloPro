# 未启用的源码参考

这些是有独立研究价值的源码补丁，不是构建缓存、已发布修复或自动应用的产品 overlay。只归档原始差异；后续移植需要单独评审和验证。GPU flight/phone replay 等一次性故障录制代码没有归档或启用。

## fex-jit-null-l1-target.patch

基线：`thirdparty/fex @ 86ff33bbe299cd8959a6610198c169b67ec419db`。

旧开发工作树保留的 L1 空目标防御候选；未登记到生产/FEX candidate 补丁链，尚未针对当前上游版本完成语义和运行验证。不要直接应用到新上游。

SHA-256：`f60ec2088b7b650c0eb2cdc479b29cd1923036b308368931ed70aac8cb237b87`。

## legacy-linux-steam-registry.patch

基线：`thirdparty/wine-valve @ cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`。

旧 Linux Steam bridge 的进程私有注册表视图。按 targets.json 的既有决定排除于 Windows Steam + OHOS Wine 产品路线，仅保留未来 Linux 分支参考。

SHA-256：`7a019cefe5dd2e91ae2b7eba201299f507db0a392592f1cffb8d95a5dcbbbc25`。
