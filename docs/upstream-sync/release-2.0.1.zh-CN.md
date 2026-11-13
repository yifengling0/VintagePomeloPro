# 2.0.1 发布组件选择

2.0.1 使用 Mini 已实测的完整上游候选：Wine `098ece0eedff`（Valve Wine `e8ca5806eec4`）
与 FEX `c3b2b2306347`（上游 `ab135df091d0`）。这不是旧 Wine/FEX 的版本号改名，
也不表示 DXVK、VKD3D 和媒体栈已升级为 Proton 最新整套依赖。

`release-runtime.lock.json` 是本分支 ARM64 发布选择；原子模块 pin 仍是构建独立组件和历史对照的基线。
常规 `scripts/package.sh hap[-unsigned] / app[-unsigned]` 现已自动路由到所选组件打包入口，
先检查暂存组件，再检查最终 HAP，以及 APP 内嵌 HAP；旧运行时、混合组件或仅改来源标签均不得交付。

已有合格组件包时，从同一个增量检出构建：

```sh
export NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64 GUEST_ARCH=aarch64
export VPP_UPSTREAM_RUNTIME_HAP=/absolute/path/to/verified-component-package.hap
bash scripts/package.sh app-unsigned
```

输入 HAP 只提供已构建的运行库，应用源码、ArkTS、原生宿主和 2.0.1 元数据由 Hvigor 正常构建。
37 个受管成员包含完整 Wine 运行时 ZIP、Wine Unix 库、匹配的 FEX 桥和来源记录；
按已提交的源码锁与 SHA-256 验证输入，不能拿任意同名 HAP 代替。
运行时 ZIP 的完整校验也覆盖 Wine PE、FEX 双架构、Gecko 双架构、ICU、图形依赖和字体。
没有输入包时只允许复用完全匹配的暂存内容；不匹配立即失败，绝不静默回退旧组件。

从源码还原运行库时，沿用已提交的 `prepare_fex_upstream.py`、`build_fex_upstream.py`、
`package_fex_upstream_candidate.py` 及对应 Wine 三个入口，按各自候选锁顺序重放和整组构建。
可复用已有构建输出，但必须通过对应构建输入、输出和组件配对检查；不得修改共享编译输出来匹配版本号。
这些入口产出的完整组件包须通过本发布锁后才能进入上述应用构建。
若工具链或产物身份变化，需重新核验并显式更新发布锁，不能放宽哈希检查。

Mini 实测内容和未结异常仍见 [游戏与 Steam 短测](validation/mini-games-and-steam.zh-CN.md)。
旧报告中的“未提升生产 pin”描述当时的候选阶段，不能解释成 Mini 测的是旧运行时。
此次发布选择在用户认可本轮短测后确定；历史报告和历史标签保持原样。
