# Wine 内核语言（Windows 系统语言）

## 设置位置

`系统设定 → Wine 引擎 → Windows 系统语言`，四选一：**中文 / 繁體中文 / 日本語 / English**
（界面按两行两列排布：中文·繁體中文 / 日本語·English）。

切换后写入全局设置，提示「系统语言已保存；重启 Wine 引擎后生效」。运行中的
Windows 程序不会立即中断，下次启动引擎（或重启 App）时按新语言拉起。

## 它到底改了什么

「Wine 内核语言」= Wine 看到的 unix locale，它同时决定两件事：

1. **Windows 系统 LCID**（`GetSystemDefaultLCID` / `GetUserDefaultLCID`）；
2. **ANSI 代码页 ACP**（`GetACP`），也就是非 Unicode 程序里 `MultiByteToWideChar`
   用哪张码表 —— 中文 GBK 游戏靠 936，日语 Shift-JIS 游戏靠 932。

链路（自设置页到 Wine）：

| 层 | 位置 | 行为 |
| --- | --- | --- |
| 设置项 | `pages/SystemSettings.ets` | `updateWineLanguage()` 存 `WineLanguage`（`zh_CN` / `zh_TW` / `ja_JP` / `en_US`） |
| 存储 | `service/AppSettingsStore.ets` | `normalizeWineLanguage()` 白名单归一化，未知值回中文 |
| 引擎 | `service/WineEngineService.ets` | `launchClient(...)` 第 10 个实参传 `settings.wineLanguage` |
| NAPI | `bridge/napi_init.cpp` | 两种启动参数布局均将 `zh_CN` / `zh_TW` / `ja_JP` / `en_US` 写入 `LaunchParams::wineLang` |
| 主进程 env | `wine/wine_env.cpp` | `WineLocaleFor()` 白名单后写 `LANG` / `LC_ALL` = `<locale>.UTF-8` |
| 子进程 env | `wine_launch.cpp` / `wine_child.cpp` | wineboot 与桌面会话同样下发；子进程基线仅作兜底默认值 |
| Wine | `dlls/ntdll/unix/env.c`（patch `0008`） | unix locale → win locale → `system_lcid`；musl 的 `C.UTF-8` 走 `LC_ALL`→`LANG` 兜底 |
| Wine 字体 | `dlls/win32u/font.c` / `freetype.c` | OHOS 按 locale/ACP 选择 CJK 字体回退，并扫描 Wine prefix 的 Windows 字体目录 |

## 语言与代码页对应

| 设置 | LANG / LC_ALL | LCID | ACP | 字族取向 |
| --- | --- | --- | --- | --- |
| 中文 | `zh_CN.UTF-8` | `0x0804` | 936 (GBK) | `鸿蒙黑体` |
| 繁體中文 | `zh_TW.UTF-8` | `0x0404` | 950 (Big5) | `鸿蒙黑体`（`MingLiU` 等已映射 `HarmonyOS Sans TC`） |
| 日本語 | `ja_JP.UTF-8` | `0x0411` | 932 (Shift-JIS) | `鸿蒙黑体` |
| English | `en_US.UTF-8` | `0x0409` | 1252 | `鸿蒙黑体` |

Wine 本体界面文字（explorer / 菜单等）跟随 `LC_MESSAGES`，由构建期
`--with-gettext` 编译进 PE 资源，因此切到日语后内置程序界面也是日语。

### HarmonyOS 下的代码页和字体回退

游戏请求的字体名缺失时，Wine 在 HarmonyOS 下按有效 locale 和 ANSI 代码页选择字体回退：

- `ntdll/unix/env.c` 将 `C`、`POSIX`、`C.UTF-8` 等视为未指定 locale，并依次检查
  `LC_ALL`、`LC_MESSAGES`、`LC_CTYPE`、`LANG`。
- `win32u/font.c` 为简中、繁中和日文分别使用 ACP 936、950 和 932。中文 ACP 下跳过
  JP/KR CJK face；简中优先 SC 面，繁中优先 TC 面，并在缺少 TC 面时回退到鸿蒙简体字族。
  这样 `DEFAULT_CHARSET` 和 ANSI 文本转换会按进程语言选择代码页与字体。
- `win32u/freetype.c` 扫描 `WINEPREFIX/drive_c/windows/fonts`，把游戏或用户导入到
  Wine prefix 的 Windows 字体加入 FreeType 字体目录。

设备上的 `NotoSansCJK-Regular.ttc` 注册了 SC/HK/TC 面，没有 JP 面；中文代码页下过滤
JP/KR face 可避免字体枚举顺序令繁中/简中文本误走 Shift-JIS。产品字体替换表仍将
`MingLiU` / `PMingLiU` / `Microsoft JhengHei` 映射到 `HarmonyOS Sans TC`。

## 设备侧自查

装包后启动引擎，可在 Wine 进程 stderr / hilog 中确认：

```bash
# Win 侧看到的环境（wine_child 打印的 env override）
hdc -t <target> shell hilog -z 2000 | grep -E 'WineChild.*(LANG|LC_ALL)'

# 前缀注册表里的字族替换（重启引擎后应随语言变化）
hdc -t <target> shell "grep -i 'MSGothic\|Noto Sans CJK JP' \
  /data/app/el2/100/base/com.vintage.pomelopro/files/.wine/system.reg"
```

游戏侧验证要点：用 `GetACP()` 自证 —— GBK 文本在 950/932 下乱码、Big5 文本在
936/932 下乱码、Shift-JIS 文本在 936/950 下乱码；只有与游戏编码一致的档位才正常。

繁体档的 locale、代码页与字体路径：`locale.nls` 含 `zh-TW`，`zh_TW.UTF-8` 对应
LCID `0x0404`；Wine 的 OHOS 字体回退按 ACP 950 优先使用 HarmonyOS/Noto TC 字族。
`MASTER_FONT_SUBSTITUTES` 也将 `MingLiU` / `PMingLiU` / `Microsoft JhengHei`
映射到 `HarmonyOS Sans TC`。

## 新增第三种语言时

1. `model/AppModels.ets`：`WineLanguage` 加枚举值 + `normalizeWineLanguage` 放行；
2. `pages/SystemSettings.ets`：语言行加按钮（`id('wine-language-<tag>')`）；
3. `wine/wine_env.cpp`：`WineLocaleFor()` 白名单放行同一字符串；
4. 若目标语言确实需要不同字族取向（先确认设备注册了对应字族，见上节）：
   在 `MASTER_FONT_SUBSTITUTES` 同层加一份语言相关的替换覆盖，并让
   `prefixFontSubstitutesCurrent()` 与 `updatePrefixFontSubstitutes()` 共用它；
5. `docs/graphics/graphics-stack.lock.yaml` 与本文件表格同步更新。
