FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Asia/Shanghai \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# Aliyun mirrors (中国大陆加速)
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources \
 && sed -i 's|http://security.ubuntu.com/ubuntu/|http://mirrors.aliyun.com/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources

# libltdl-dev 必须显式列出：Ubuntu 官方 docker 镜像默认关闭 Recommends，
# 不装则 aclocal 找不到 ltdl.m4，libffi autogen.sh 会失败。
RUN apt-get update && apt-get install -y \
    # 编译工具链
    build-essential cmake ninja-build meson \
    bison flex autoconf automake libtool libltdl-dev \
    pkgconf zip git file lsof patch python3 python3-pip glslang-tools \
    # Wine 翻译资源与构建期下载工具（恢复 main 已验证的构建依赖）
    gettext curl wget \
    spirv-tools \
    # wayland-scanner 原生构建 (生成 Wayland 协议代码)
    libexpat1-dev libxml2-dev libffi-dev \
    # sfnt2fon 字体工具 (Wine .fon 生成)
    libfreetype-dev \
    # Wine OHOS 交叉 PE 编译 (i386 + x86_64 mingw, C++17 for icu.dll)
    gcc-mingw-w64-i686 g++-mingw-w64-i686 \
    gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
    mingw-w64-tools \
    # HAP 签名
    default-jdk \
 && apt-get clean && rm -rf /var/lib/apt/lists/*

# Python 包 (virglrenderer + Mesa guest_gfx 构建)
RUN pip3 install --break-system-packages pyyaml mako markupsafe \
 && rm -rf /root/.cache/pip

# libxml2.so.2 兼容性修复
# OHOS SDK 的 ld.lld 链接器依赖 libxml2.so.2，Ubuntu 26.04 提供的是 libxml2.so.16
RUN ln -sf /usr/lib/x86_64-linux-gnu/libxml2.so.16 /usr/lib/x86_64-linux-gnu/libxml2.so.2 \
 && ldconfig

WORKDIR /data/src/winehua

# 使用时挂载:
#   -v /path/to/wineohos:/data/src/winehua                  (项目源码)
#   -v /path/to/harmony-sdk:/apps/harmony                   (OHOS SDK)
# 可选: 从 Windows/DevEco Studio 直接导入签名, 免去复制到 WSL
#   -v /mnt/c/path/to/deveco_project:/mnt/user-profile      (含 build-profile.json5 的工程目录)
#   -v /mnt/c/path/to/signature_dir:/mnt/user-signature     (含 .cer / .p7b / .p12 的目录)
# 两者需同时挂载才生效; 缺失任一则回退到项目内置 build-profile.json5.
#
# 构建:
#   docker run --rm -v $(pwd):/data/src/winehua -v ~/huawei/command-line-tools:/apps/harmony \
#     wineohos-build make NATIVE_ARCH=arm64-v8a DEVICE_TYPE=pad
