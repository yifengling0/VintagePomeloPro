# GNU Proton 媒体闭包

GE-Proton11-11 ARM64 使用 `winedmo.so` 和 FFmpeg 8；其原始归档不包含 GStreamer。原 OHOS Wine 仍使用原生 GStreamer 1.24.4，两套 ABI 不混用。

审计发现 GNU 组件含 WineDMO，却漏了它的五个 FFmpeg SONAME、其他传递依赖及 64 位 MF/DirectShow DLL。仅检查图形 DT_NEEDED 不能发现游戏稍后动态加载的这些模块。

## 交付与兼容

`proton-media.lock.json` 锁定同一个 GE 原包、媒体 DLL/ELF 来源、逐文件哈希及三份 Ubuntu ARM64 支持包。新增支持层包含 38 个文件、约 42.8 MiB 展开数据；依赖已交付 GNU runtime 的 25 个库，不复制整份运行时。VA/VDPAU 库仅满足上游链接依赖，不表示已经接通鸿蒙硬件视频解码。

`LinuxProtonMedia.ets` 在选定上游运行包就绪、游戏启动之前安装媒体支持。校验 WineDMO ABI、文件大小/哈希，拒绝链接/路径逃逸，以临时文件及 rename 提交；失败不启动游戏，下次重试。重复启动复用文件戳，仅重新校验变化文件。媒体 ELF 安装到已有 GNU 加载器搜索的私有根目录，不额外复制一份，也不依赖父进程的 LD_LIBRARY_PATH 侥幸传递。补充 DLL 在独立 media 目录，按清单进入私有前缀。

原 GNU Proton ZIP、运行版本目录及已发布离线包内容身份不变；没有用同一下载 URL 指向新字节。用户所选兼容工具、注册表、账号、游戏库和存档不在此层替换范围。未来更换 WineDMO ABI 时需更新媒体支持；不匹配时明确报错，禁止静默混用。

## 重建与检查

先复用已下载且 SHA-512 匹配的 GE 归档，以及 SHA-256 匹配的 Ubuntu 支持包，不完整解包 rootfs：

```sh
python3 scripts/prepare_linux_proton_media.py --archive <GE归档> \
  --runtime <已校验共享GNU运行库> --support-cache <三份deb的缓存目录>
python3 scripts/prepare_linux_proton_media.py --verify-only
python3 host_tests/linux_proton_media_test.py
node scripts/run_linux_proton_media_tests.cjs <typescript目录>
```

Ubuntu包名称、来源及哈希在构建脚本和锁内；许可证随媒体目录交付。相同输入直接复用；修改选择规则后显式 `--refresh`。所有运行输入必须随代码提交，干净 checkout 和最终 HAP 的组件门禁检查这份支持层。

已完成：实际媒体文件的哈希/架构/ELF依赖检查，逐个删除/损坏编解码库和64位媒体DLL的拒绝测试，以及真实ArkTS服务冷安装、热复用、损坏修复、空间不足、中断重试、路径/链接/ABI拒绝和用户数据保留测试；Native及Release编译通过。

未完成：设备播放、真实视频帧/声音验收，以及原OHOS Wine“白色恋人”OP反馈的复现。此次补齐GNU缺失不应标记为原Wine OP已修复。
