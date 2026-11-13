# GNU Proton 网络接口错误处理

GE-Proton11-11 对应 Valve Wine c53678ab22a85f0fad3588dd34f0452a463bd015。
Mini 的应用权限可能拒绝网卡枚举；原 NSI 在持锁时解引用空返回值，后续查询会挂住。
迁入 HSL fix/mini-store-runtime 的通用修复：检查失败、返回真实 NTSTATUS，并在所有路径解锁。
不伪造网卡、权限或成功状态。OHOS Wine 不使用这个 GNU 模块。

源码、补丁和哈希固定在 gnu-proton-network.lock.json。重建命令：

```sh
python3 scripts/build_linux_proton_network.py --cc <GNU-AArch64-gcc> \
  --wine-source <Wine-headers> --wine-build <configured-Wine-build> \
  --widl <host-widl> --package <verified-GE-package> [--source-cache <cache>]
python3 scripts/build_linux_proton_probe.py --output build/gnu-proton-shim --cc <GNU-AArch64-gcc>
python3 scripts/prepare_linux_proton_dxvk.py --refresh-probe --shim build/gnu-proton-shim \
  --network-build build/gnu-proton-network/build.json
```

构建验证原 GE ntdll/NSI 身份、源码/补丁/编译依赖及七项 Unix-call ABI。
已验证产物随受版本管理的 dxvk-support 交付，清单同时绑定 Vulkan、NSI 与 ntdll。
客户端在任何 Wine 进程启动前校验并安装；GNU shim 只重定向该运行时的 NSI 加载。
新 clone 与 CI 无需依赖开发机的 build 目录。CI 检查源码收据、模块清单及最终 HAP 字节。

主机测试覆盖缺失模块、过期源码/补丁/头文件/二进制、错误 ABI、原 GE 配对错误和用户存档保留。
主机检查与真机游戏验证分别记录，HSL 的游戏结果不作为 VPP 已通过的证据。
