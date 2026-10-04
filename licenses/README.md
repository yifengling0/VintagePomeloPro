# 上游许可与声明副本

本目录保留在 `e2d001416d94ec01c31d4ffd38be3720d1496b47` 基线审查时已核实的主要上游许可、版权与声明。
使用范围见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)，分发要求和未关闭问题见 [LICENSING.md](../LICENSING.md)。

## 来源与完整性

[manifest.json](manifest.json) 对每份副本记录原 URL、SHA-256、字节数、可用的源码修订和复制方式：

- `verbatim file`：上游文件逐字节副本
- `verbatim archive member`：指定发布压缩包中的文件；另记压缩包 URL、SHA-256 和成员路径
- `complete opening comment`：完整保留源文件开头的许可/出处注释，并添加结尾换行；同时记录原文件哈希和字节范围
- `metadata` / `leading SPDX`：原字体 metadata 或源文件 SPDX/版权行摘录，**不是声称该片段单独含有完整许可证**；按原组件全文和逐文件声明一起使用

通用 LGPL 3/GPL 3 文本另放在 `third-party/gnu/`，直接复制自 GMP 6.2.1 发布包。
原始字节可能含 CRLF 或行尾空格；为了不改动上游文本而原样保留。
发布包哈希记录本次取得的材料，不代表构建脚本已校验它，也不证明缓存二进制由它构建。

## 范围限制

- 这是所选源码声明集合，不是完整二进制 SBOM、对应源码包或可分发性证明
- Wine `libs/` 和 Mesa `licenses/` 包含所在源码树的多种声明；目录中有某个文本不代表它适用于每个编译产物
- 同一种许可保留各组件自己的副本是为了不丢失版权和适用范围，不能任意用通用模板替代
- 不同名称的 Wine 路径对应不同 pin；legacy `NOTICES.md` 的完整第三方声明仅按其来源保留，不能据此断言 Valve pin 里每个内嵌文件都与之相同
- 原来源仍是权威依据。本目录不覆盖 SoundFont 等尚未找到授权的资源、未固定 FFmpeg 构建、未审查的嵌套依赖或外部 SDK
- 这些文件目前没有由本次更改自动加入 HAP；分发者仍须完成随包声明和源码/重链接等检查

可使用 Python 标准库读取 `manifest.json`，对每个 `file`（相对仓库根目录）的字节计算 SHA-256，并与 `sha256`/`bytes` 字段核对。
