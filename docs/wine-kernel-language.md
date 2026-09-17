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
| 设置项 | `pages/SystemSettings.ets` | `updateWineLanguage()` 存 `WineLanguage`（`zh_CN` / `ja_JP` / `en_US`） |
| 存储 | `service/AppSettingsStore.ets` | `normalizeWineLanguage()` 白名单归一化，未知值回中文 |
| 引擎 | `service/WineEngineService.ets` | `launchClient(...)` 第 10 个实参传 `settings.wineLanguage` |
| NAPI | `bridge/napi_init.cpp` | `args[9]` → `LaunchParams::wineLang` |
| 主进程 env | `wine/wine_env.cpp` | `WineLocaleFor()` 白名单后写 `LANG` / `LC_ALL` = `<locale>.UTF-8` |
| 子进程 env | `wine_launch.cpp` / `wine_child.cpp` | wineboot 与桌面会话同样下发；子进程基线仅作兜底默认值 |
| Wine | `dlls/ntdll/unix/env.c`（patch `0008`） | unix locale → win locale → `system_lcid`；musl 的 `C.UTF-8` 走 `LC_ALL`→`LANG` 兜底 |

## 语言与代码页对应

| 设置 | LANG / LC_ALL | LCID | ACP | 字族取向 |
| --- | --- | --- | --- | --- |
| 中文 | `zh_CN.UTF-8` | `0x0804` | 936 (GBK) | `鸿蒙黑体` |
| 繁體中文 | `zh_TW.UTF-8` | `0x0404` | 950 (Big5) | `鸿蒙黑体`（`MingLiU` 等已映射 `HarmonyOS Sans TC`） |
| 日本語 | `ja_JP.UTF-8` | `0x0411` | 932 (Shift-JIS) | `鸿蒙黑体` |
| English | `en_US.UTF-8` | `0x0409` | 1252 | `鸿蒙黑体` |

Wine 本体界面文字（explorer / 菜单等）跟随 `LC_MESSAGES`，由构建期
`--with-gettext` 编译进 PE 资源，因此切到日语后内置程序界面也是日语。

### 字族为什么不随语言变化（实测结论）

日语文本在这台设备上**不需要**换字族，原因是两条实测证据：

1. 设备 `/system/fonts/NotoSansCJK-Regular.ttc` 经 Wine 枚举后只注册了
   `Noto Sans CJK SC` / `HK` / `TC` 三个面，**没有 `Noto Sans CJK JP` 面**
   （见前缀 `system.reg` 的 `Fonts` 段）。把日语字族指向 JP 面会落到不存在的
   字族上，比现状更差。
2. 产品历史注释（`WineEngineService` 的 `OBSOLETE_GAME_FONT_SUBSTITUTES`
   说明）明确「鸿蒙黑体」已经能出 Hangul 与 kana，当年的问题是这些字形被按
   GBK 代码点取用 —— 也就是**代码页**问题，而不是缺字形。

所以本功能只切代码页与 LCID，字族保持 `MASTER_FONT_SUBSTITUTES`（`鸿蒙黑体`）
不变。`MS Gothic` / `MS PGothic` / `MS UI Gothic` 等日文字族请求继续映射到
`鸿蒙黑体`，由 ACP 932 保证 Shift-JIS 文本按日文代码页解析。

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

繁体档另有两点已就绪、无需额外改动：

- Wine 侧 `locale.nls` 含 `zh-TW`，`unix_to_win_locale("zh_TW.UTF-8")` 直接得到
  `0x0404`（`ntdll` 的 `__OHOS__` 强制 zh-CN 分支只在"解析失败退化成英文"时生效，
  `zh_TW` 不落进该分支）。
- 设备自带 `HarmonyOS_Sans_TC.ttf`；`MASTER_FONT_SUBSTITUTES` 里
  `MingLiU` / `PMingLiU` / `Microsoft JhengHei` 已指向 `HarmonyOS Sans TC`。

## 新增第三种语言时

1. `model/AppModels.ets`：`WineLanguage` 加枚举值 + `normalizeWineLanguage` 放行；
2. `pages/SystemSettings.ets`：语言行加按钮（`id('wine-language-<tag>')`）；
3. `wine/wine_env.cpp`：`WineLocaleFor()` 白名单放行同一字符串；
4. 若目标语言确实需要不同字族取向（先确认设备注册了对应字族，见上节）：
   在 `MASTER_FONT_SUBSTITUTES` 同层加一份语言相关的替换覆盖，并让
   `prefixFontSubstitutesCurrent()` 与 `updatePrefixFontSubstitutes()` 共用它；
5. `docs/graphics/graphics-stack.lock.yaml` 与本文件表格同步更新。
