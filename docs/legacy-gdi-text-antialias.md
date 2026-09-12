# Legacy GDI text antialiasing (游戏内汉字乱码修复)

## 现象

梦幻群侠传5（GGE/XYQ 私服客户端，`启动器.exe`）在 Old Pomelo Pro 里跑起来后：

- 窗口标题、任务栏、聊天频道标签（`系统`）、表情图标显示正常；
- 但**游戏内所有文字**（对话气泡、聊天栏、系统提示、任务提示）显示为
  零散笔画碎片，看起来像假名/乱码。

## 根因

不是编码问题（GBK 一直按 cp936 正确解码成 Unicode），不是缺字/cmap/
FontSubstitutes，也不是字体缺失。真正原因是引擎与 Wine 对"GDI 文字是否抗锯齿"
的假设不一致：

1. 引擎把文字画进 offscreen bitmap，再用 `GetDIBits` 读回，然后**二值化成字形遮罩**。
   `启动器.exe` 的文字绘制函数（VA `0x4a5b40`）里判据是

   ```
   movzx ebx, WORD [eax+ecx*2]   ; 16bpp 像素
   cmp   bx, 0x7ffe
   jbe   -> alpha = 0            ; <= 0x7ffe（约半亮）一律当"没有笔画"
   否则  -> alpha = 0x20         ; 只有接近纯白的像素才算笔画
   ```

   即引擎要求 GDI 输出**双值（无抗锯齿）**字形。
2. Wine 侧 `font_SelectFont ... aa 5` = `GGO_GRAY4_BITMAP`：OHOS 前缀
   `Control Panel\Desktop\FontSmoothing=2`，另外 patch 0001 在没有 fontconfig 的
   OHOS 上把 `default_aa_flags` 也强制成灰度 AA。
3. 12px 汉字的笔画开 AA 后大部分是半覆盖的灰，被 2 的阈值判成透明，
   屏幕上只剩笔画核心的碎片。

对照实验可以直接证明第 3 点：同一套 Wine 里 `notepad.exe` 打开 GBK 文本中文显示
完全正常（说明字体/字形管线没问题）。

## 修复

按进程关闭 GDI 文字抗锯齿，桌面 UI 保持原有灰度 AA：

| 位置 | 改动 |
| --- | --- |
| `patches/wine/0010-ohos-font-aa-override.patch` | `dlls/win32u/font.c` 新增 `ohos_font_aa_override()`：`WINEHUA_FONT_AA=bitmap` → `GGO_BITMAP`，`=gray` → `GGO_GRAY4_BITMAP` |
| `entry/src/main/ets/service/WineEngineService.ets` | `launchExecutable()` 对非 explorer 的启动注入 `WINEHUA_FONT_AA=bitmap`；shell/explorer 不注入，显式设置过该变量的按调用方设置走 |

适用范围：走 GDI（`TextOutA/DrawTextA/ExtTextOut`）渲染文字、并且把渲染结果
当作遮罩/纯色位图使用的老 Windows 游戏。这类 2000 年代中日韩 2D 引擎
（XYQ/GGE/HGE 系、大量私服客户端）非常常见，所以这是一个通用修复。

- 不受影响：DirectWrite、GDI+（`Graphics::DrawString`）、自带光栅器的引擎、
  已经用点阵字库出图的游戏 —— 它们不经过 win32u 的 GDI 字体 AA 路径。
- 代价：这些游戏进程里的 GDI 文字变成双值（老 Windows 观感），
  如果需要恢复灰度 AA，可在单游戏环境变量里显式设置 `WINEHUA_FONT_AA=gray`。

## 验证

```bash
bash scripts/vpbuild.sh make wine      # 改了 Wine 补丁
bash scripts/vpbuild.sh make hap       # NATIVE_ARCH=arm64-v8a, GUEST_ARCH=x86_64
```

1. 包内 `resources/rawfile/wine-data.zip` 的 `bin/x86_64-unix/win32u.so`
   必须包含字符串 `WINEHUA_FONT_AA`（补丁生效的唯一判据）。
2. 装机后进游戏看对话/聊天是否为正常简体中文。
3. 需要日志时用 `winehua.d3d_env_json` 传 `WINEDEBUG=+font`，确认
   `font_SelectFont ... aa 1`（`GGO_BITMAP`）而不再是 `aa 5`。

## 附：本标题的其它已发现缺陷（与乱码无关）

游戏用**相对路径** `Media/font\*.ttf` 调 `AddFontResourceExA` 注册自家字体，
而字体文件实际位于游戏根目录 `font/`，因此真机日志里
`AddFontResourceExW ... res 00000000`（全部失败），游戏退化为用系统字体渲染。
修复乱码不依赖这些字体（问题是 AA 而非字形文件），若要还原作者设计观感，
把 `font/*.ttf` 复制成 `Media/font/*.ttf` 即可。

## 补丁序列现状

排查期间 `patches/wine/` 加过一批"GBK 被当 Shift-JIS 解"假设下的补丁，
问题定位后已整理：

| 补丁 | 状态 |
| --- | --- |
| `0005-ohos-scan-prefix-windows-fonts.patch` | 保留（扫描 prefix 的 `windows/fonts`，用户导入的字体才可见） |
| `0006-ohos-cjk-missing-font-gbk-fallback.patch` | 保留，但只留"缺字体名→本地 CJK 面、不落到 Noto CJK JP/KR"的部分 |
| `0008-ohos-musl-c-utf8-zh-cn-locale.patch` | 保留（musl 的 `C.UTF-8` 按 LANG 处理，中文环境 ACP=936） |
| `0010-ohos-font-aa-override.patch` | 本次修复的核心 |
| ~~`0007` / `0009` / `0011`~~ | 已删除：按字体字符集强制改写 codepage、`MultiByteToWideChar(932)`→936 重映射、GBK 反查假名码点。它们对显式请求 cp932/cp949 的日韩游戏是回归隐患，且对 GBK 游戏也已无必要 |

自检：`git -C thirdparty/wine status` 只应有 4 个改动文件
（`mfplat/main.c`、`ntdll/unix/env.c`、`win32u/font.c`、`win32u/freetype.c`）；
`kernelbase/locale.c` 必须与 submodule HEAD 一致（0009 已撤除）。
