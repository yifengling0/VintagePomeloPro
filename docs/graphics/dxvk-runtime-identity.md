Steam 黑窗排查发现：源码和图形锁已经更新到 Modern 修复提交，但沿用的
`wine-data.zip` 仍携带旧 DLL。旧门禁只核对主图形锁和部分运行库，没有将
DXVK 源码、清单中的提交、ARM64X/x64/x86 六个实际 DLL 配对。

`dxvk-runtime.lock.json` 现在固定已核对的
源码提交、源码摘要和六个 DLL 摘要。独立门禁 `dxvk_runtime_contract.py` 同时检查当前源码与
输入 ZIP/最终 HAP；`check_runtime_components.py` 在 ARM64 runtime assemble
及 HAP 检查中校验实际 DLL、清单提交与摘要。只修改清单不能通过门禁。

更新运行库时，先完成编译与设备验证，再审阅并更新锁内的源码和产物摘要。
已有其他运行库修复需要保留时，可以执行：

```sh
python3 scripts/package_dxvk_modern_runtime.py \
  --rawfile entry/src/main/resources/rawfile \
  --profile-dir /path/to/reviewed/modern-2.6
```

工具仅替换六个锁定 DLL、DXVK 清单、外层 payload 摘要与解压版本标记。
普通 assemble 会补全 ARM64X 清单，并在复制 payload 前经过相同门禁；
旧构建输出必须重新构建或更新，不能通过重写提交号绕过验证。

2026-10-16 手机实测：同一批处理参数下旧 Modern 持续黑窗，Legacy 可显示；
换成源码逐文件一致的已验证 Modern DLL 后进入 Steam 主界面和库。
这定位了产物遗漏，不能单凭六个 DLL 的整体替换把现象归因到某一个 shader
或同步补丁。支付、Direct、游戏性能和长期稳定性不属于这项验证。

独立锁不依赖共享图形锁的迁移格式，避免提交运行库修复时包含其他在研图形修改。
