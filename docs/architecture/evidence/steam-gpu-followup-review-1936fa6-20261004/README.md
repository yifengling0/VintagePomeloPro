# 1936fa6 独立复核与构建交付证据

完整报告：[steam-gpu-followup-independent-review-20261004.md](../../steam-gpu-followup-independent-review-20261004.md)。

评审源码：`1936fa68864dbb0ecda8ed6d571c167e14991842`。逻辑回归、完整主机测试、全新 Wine/OHOS/HAP 构建与签名通过；平板升级成功，但锁屏拒绝启动，因此游戏/Steam 切换、菜单、实际输入仍待真机验收。

## 证据入口

- `focused-tests.log`、`full-host-tests.log`、`host-test-result.json`：专项与完整测试，完整退出码 0。
- `baselines/`：指定旧提交出现预期断言，排除因编译/平台错误而偶然失败。
- `wine-build-identity.json`、`wine-build-excerpts.log`、`pre-assemble-identity.log`：新 BUILD_DIR 的实际有效源码和构建后检查；完整 Wine 编译日志留本地。
- `build-fresh.sh`：本轮实际执行的构建入口记录，含复用依赖的明确清单；其固定路径是本机证据，不是通用构建脚本。
- `old-cache-rejection.log`、`old-cache-preservation.json`：旧目录拒绝与 configure/Makefile/config.status 字节保全。
- `assemble.log`、`hap-build.log`、`sign.log`、`runtime-component-check.log`、`package-identity.json`：已安装签名包的构建、依赖闭包与库/对象身份链。
- `device-install/`：脱敏的升级/启动操作记录、升级成功输出、版本字段和系统锁屏拒绝。
- `unsigned-hap/`：重新构建的未签名包身份、SHA256、构建日志、runtime closure 和 SDK 无签名块验证。
- `source-artifacts.json`：原始本地材料相对路径与原始 SHA256；与 LF/ANSI 规范化后的仓库文件哈希分开记录。

## 回归复核

在仓库根目录执行（Python 3 / g++）：

```bash
make test-gpu-followup test-egl-multi-consumer test-zc-binding-lifecycle \
     test-steam-gpu-contracts test-frame-loop-diagnostics test-cef-render-switches
python3 host_tests/gpu_owner_contract_test.py --baseline
python3 host_tests/gpu_scene_input_test.py --baseline
python3 host_tests/steam_gpu_contract_test.py --baseline
make -k test
```

主机测试使用生产逻辑和平台桩，不能代替 OHOS/Steam 真机显示与输入验证。需要新会话的 candidate、currentHit、draw-issued、swap 和用户画面反馈确认原 SHM 游戏遮挡是否修复。

本目录的 `SHA256SUMS.txt` 覆盖本目录证据以及 `../../steam-gpu-followup-independent-review-20261004.md`，可以在本目录执行 `sha256sum -c SHA256SUMS.txt`。日志已脱敏；HAP、原始截图、完整大日志、构建目录和签名材料不进入本提交。
