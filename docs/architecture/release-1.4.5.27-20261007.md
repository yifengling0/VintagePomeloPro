# 旧柚 Pro 1.4.5.27 构建交付

日期：2026-10-07。版本名称 `1.4.5.27`，版本码 `1004036`，包名 `com.vintage.pomelopro`。

采用当前 `feature/main_proton` 的本地代码（HEAD `62ca61faa87351307daa18b59c7d6fd26cbc0db6`，包含尚未提交的修复）重新完成 `proRelease / release` 构建。Hvigor 构建成功。

包含当前手机进程存活／wineserver 启动判断、初始化阶段进度和横竖屏修复；保留桌面输入修复、FEX 默认路径、`FEX_X87REDUCEDPRECISION=1` 和既有图形策略。Steam Zstd 下载补丁是独立的历史 Steam 客户端包，此 HAP 没有内置 Steam 客户端或该补丁。

交付两种文件：

- `VintagePomeloPro-1.4.5.27-1004036-unsigned.hap`：350,691,710 bytes，SHA256 `2955c5fd21bad226c9fef95cb364c1e32351480412a570112198ead64e2e72cb`。release 构建、`debug=false`，官方验证工具确认没有签名；安装前须另行签名。
- `VintagePomeloPro-1.4.5.27-1004036-appgallery-release.app`：272,100,511 bytes，SHA256 `64e04ed554cc6b45f12b7fd4beee1e6740a6842190ae00057f8121a51bd0a497`。使用已有正式发布证书和 `release / app_gallery` profile，由官方 Hvigor SignApp 生成，用于上架上传。

两包均为 ARM64，版本名称只含数字和点。APP 容器、内嵌发布 HAP 与 profile 的官方验证均通过，证书链指纹及包名匹配，profile 在有效期内。APP 内嵌 HAP 与独立未签名 HAP 的内容一致；pack.info 按语义比较相同。

核实当前 release 原生库已装入 HAP，包含手机进程和初始化进度修复标记；Wine 数据 ZIP 和 FEX DLL 保持已有版本和哈希。release 处理移除部分 ELF 非加载元数据，Wayland／OpenGL 库所有加载区段的地址、尺寸、类型、标志和内容 SHA256 与构建输入逐项一致。运行库打包 guard 通过，包内没有私钥、keystore 或 signing profile 文件。临时签名配置已恢复为原 unsigned 源配置。

本次未安装这两个最终发布文件，未向 AppGallery 上传，未提交或推送源码。手机功能的此前真机结果与这轮构建／包验证分开记录。完整身份与校验依据见 `manifest.json`、`SHA256SUMS.txt` 及同目录验证日志。
