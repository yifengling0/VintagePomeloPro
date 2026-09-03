# M5 战斗爆音：根因与「宿主混音 vs 多 Renderer 系统混频」

> 日期：2026-09-01  
> 样本游戏：`M5v251017`（梦幻西游类客户端，BASS + FMOD 双引擎）  
> 设备：ARM64 平板，API 26，产品包 `com.vintage.pomelopro`  
> 对照指引：`VintagePomeloPro_Audio_Pop_Diagnosis_Architecture_Guide.md`  
> 读者：音频后端 / HarmonyOS OHAudio 评审

本文只整理**已用真机日志钉死的事实**和两条修复路径的取舍，不代替 POC。

---

## 1. 结论摘要

| 问题 | 结论 |
| --- | --- |
| 爆音主因 | **B 类：宿主 `MixStreamsS16` 把两条 Wine endpoint 的 S16 在 int32 里相加，再硬 clamp 到 ±32767。** |
| 不是什么 | 不是 A 类欠载（战斗窗口 `underrunEv=0`）。不是单流 SRC 单独能解释（单流时 peak 约 9k–30k、`clipped=0`，听感正常）。 |
| 次要因素 | **D 类**：`start/stop stream id=1` 与系统 `silent ↔ not silent` 对齐，停流后会再咔一声。 |
| 手柄 WHGP | 已缓解，与本爆音无关。 |
| 多 Renderer 是否更合适 | **对 B 类在原理上更干净**：Harmony 主通路 sink 是 **s32le**，两条 S16 交给系统混，不必在进入 OHAudio 之前先压回 S16。是否适合量产，要看 2 路 GAME Renderer 的 start/stop、静音检测、interrupt、功耗，不能只因为「能创建两个」就定案。 |
| 立刻可做的 Mixer 补丁 | 溢出才 soft-saturate（tanh），单流 bit-exact。这是「保留单 Renderer」的最小实验，**不能当成 -6 dB 永久衰减，也不能代替架构决策。** |

指引第 9 节「进入系统混频 POC」的第 1、2 条**已经成立**：爆音与 `streams=2` + `clipped>0` + `peak=32767` 同步；游戏确实开了两条独立 Wine endpoint（BASS / FMOD）。第 3 条（要不要长出完整 mixer 引擎）取决于专家是否接受 soft limiter，还是认为这已经在重复 Audio Service。

---

## 2. 当前产品链路（未改 Renderer 数量）

```text
M5.exe
  ├─ BASS_Init(44100, MONO)     → 一条 Win32 播放客户端
  └─ FSOUND_Init(44100, 32)     → 另一条 Win32 播放客户端
        ↓  mmdevapi / DirectSound
wineohos.drv
  线性 SRC → 48 kHz stereo S16
  每客户端一块 shm Ring
        ↓
Host AudioBroker
  MixStreamsS16: int32 求和 → 硬 clamp S16     ← B 类发生在这里
        ↓
单个 OH_AudioRenderer
  48 kHz / stereo / S16 / USAGE_GAME / LATENCY_NORMAL
  Broker 启动时就 Start，一直活着
        ↓
HarmonyOS Audio Service
  设备上观测到 GAME session；primary sink format=s32le, bufferSize=7680
        ↓
扬声器
```

要点：

- 游戏内部 voice（枪声、技能）**不会**变成 Renderer。Broker 只看见 **Wine IAudioClient / endpoint**，M5 稳态就是 **1～2 条**。
- 全局 Renderer 在「没有游戏流」时也在跑。空 mix 现在返回 `AUDIO_DATA_CALLBACK_RESULT_INVALID`（按华为「不要填静音」）；有 started 流但 PCM 为 0 时仍返回 VALID 全零，系统会走 `IncSilentData`。

---

## 3. 真机证据（清理日志后的干净会话）

`wine_stderr` 约 26 KB，**0 条** `WHGP connected`。下面只来自 `WL_AUDIO` + `AudioLogUtils`。

### 3.1 战斗窗口与 clip 锁死（三轮同构）

| 时间 | 事件 | streams | underrun | clipped / 秒 | peak |
| --- | --- | ---: | ---: | ---: | ---: |
| 23:05:19–39 | 仅 BGM | 1 | 0 | 0 | 9k–27k |
| **23:05:39.738** | **start stream id=1** | | | | |
| 23:05:40–48 | 战斗 | **2** | 0 | 1400–4564 | **32767** |
| 23:05:48.712 | stop stream id=1 | 1 | 0 | 0 | 0（随后系统静音） |
| 23:06:09.154 | 再 start id=1 | 2 | 0 | 815–6340 | **32767** |
| 23:06:20.502 | stop id=1 | 1 | 0 | 0 | 0 |
| 23:13:57–59 | 单流 BGM 已很响 | 1 | 0 | 0 | **29k–30k** |
| **23:13:59.994** | **start id=1（第三轮）** | 2 | 0 | **3911–6010** | **32767** |
| 23:14:11.187 | stop id=1 | 1 | 0 | 0 | 0 |

Callback 始终约 50 次/秒（960 帧 @ 48 kHz，20 ms）。`late` 只在 start/stop 附近偶发 1 次（intervalMaxUs 47–56 ms），不是稳态 xrun。

第三轮更说明问题：BGM 单流已经 30k，**再加任何一条接近满幅的 SFX，求和必然越界**。硬 clamp 把越界段切成方波，听感就是爆音/破音。

### 3.2 系统侧与 D 类叠在同一窗口

第二轮前后系统打了：

```text
23:06:07  slient 990frames change to not slient     （即将 start id=1）
23:06:09  start stream id=1
23:06:19  not slient 597frames change to slient       （即将 stop）
23:06:20  stop stream id=1
随后 IncSilentData 计数递增
```

因此专家听到的「爆音」里至少两层：

1. **B**：双流硬截幅（主因，持续整段战斗）。
2. **D/F**：Renderer 在有声/静音之间翻转（技能开始/结束的咔哒）。多 Renderer 不会自动消灭这一层；若 SFX 那路跟着 `Start/Stop Renderer`，有可能更明显。

### 3.3 单系统播放流

`hidumper -s AudioPolicyService`：应用侧 GAME 流（当时 session 100009/100010），primary sink `s32le`。没有「系统里开了很多 Renderer」；爆音发生在**进入**那一个 S16 Renderer **之前**的宿主求和。

---

## 4. 为什么会发生 B 类

```text
stream A (BGM)  S16  ≈ +30000
stream B (SFX)  S16  ≈ +20000 … +30000
        ↓ int32 累加
      ≈ +50000 … +60000
        ↓ clamp
      = +32767  （平台期，谐波极多）
```

`clipped` 计数是 **越过 ±32767 后被写成满幅的 sample 数**。48000×2=96000 sample/秒，战斗时 4000–6000/秒 ≈ **4%–6% 的采样点被方波化**，足够形成持续破音，而不只是偶发卡塔。

这与指引 5.2 的典型特征逐条对上：平静/单流正常；战斗叠加破音；`active>1`；peak 顶死；clip 与窗口同步；underrun 不涨。

---

## 5. 两条路

### 路径 A — 保留单 Renderer，只修 Mixer（局部）

| 做法 | 作用 | 限制 |
| --- | --- | --- |
| 溢出 tanh / soft saturate | 越界不再方波，单流仍 bit-exact | 仍是在 S16 域做最终混音；双流很响时会被压响度 |
| 双流时固定 headroom（如 ×3/4） | clip 次数下降 | 指引写明：**不能把任意 -6 dB 当产品修复**；BGM 会被一起打瘦 |
| float32 累加 + limiter | 接近「迷你混音引擎」 | 开始重复 Audio Service；指引认为这是转系统混频的信号 |
| start/stop 1–3 ms ramp | 针对 D 类 | 与 B 独立，不要和 SRC/Renderer 数量捆在一次提交里 |

代码现状：`MixStreamsS16` 已从硬 clamp 改成「范围内原样、越界 tanh」。这是路径 A 的最小实验，**尚未作为架构定案**。验收应看战斗时 `peak` 是否不再长期钉在 32767，以及破音是否明显下降。

路径 A 适合：专家判断「两条 endpoint + 一个 soft clip 就够，不必长成 DAW」。

### 路径 B — 一 Wine endpoint 一 `OH_AudioRenderer`，Harmony 混频

```text
Ring 1 (BASS / BGM)  → Renderer 1  S16 GAME
Ring 2 (FMOD / SFX)  → Renderer 2  S16 GAME
                         ↓
              HarmonyOS Audio Service
              primary sink 已观测为 s32le
                         ↓
                      扬声器
```

**为什么对 B 更合适（原理）：**

1. 损伤发生在「两路 S16 → 一路 S16」。系统 sink 是 **32-bit**，两路 S16 在服务里相加通常**不必先截回 16-bit**。
2. 官方模型就是多 Renderer 实例 + 系统混音（Music/Game 默认可混）。当前工程是 `LATENCY_NORMAL + USAGE_GAME`，不是低时延独占。
3. 欠载隔离：一条 Ring 空了只影响自己的 Renderer；不必在一个 callback 里对所有 Ring 统一拉齐再补零/hold。
4. M5 的粒度正确：映射的是 **endpoint（2 路）**，不是每个 FMOD voice。

**为什么不一定立刻量产：**

1. 指引要求真机跑 **1/2/4/8** 路创建/启动/长期跑；没有公开的普通 Renderer 个数上限。
2. **D 类可能搬家**：若 SFX 的 Renderer 跟着 `start/stop stream` 启停，系统 `silent ↔ not silent` 会变成**按路**翻转，技能边界的咔哒未必消失。缓解：两路都常驻，空数据返回 `INVALID`，不要 Stop。
3. 双 callback（2×50 Hz）调度、interrupt/RESUME 必须把**每一路**再 Start，否则来电后可能一路死音。
4. 两条都是 `USAGE_GAME` 时，系统是否仍按 s32 混、会不会在 sink 前再 dither/限幅，要用 hidumper + 听感验证，不能从文档直接推断「系统混就永不削波」。
5. `GetCurrentPadding` 仍是同步 IPC；每流 Renderer 后时钟/position 更不能继续用「名义 period」。这是另一层债，不要和 POC 第一刀绑死。
6. 禁止把游戏内每个音效映射成 Renderer。

路径 B 适合：专家判断「B 已证实，再在 Broker 里做 limiter/headroom/ramp 就是自建混音引擎，应把最终混频还给系统」。

---

## 6. 建议专家拍板的问题

1. **B 是否已足够定案？** 上表是否还缺 P2（入 Broker 前）PCM dump？当前没有 per-stream dump，但单流不 clip、双流立刻顶满，已经很难用 SRC 或欠载解释战斗破音。
2. **路径 A 的 tanh 算不算「简单修复」？** 若听感过关且不需要再加 per-stream de-click / 跨流 limiter，按指引应**保留单 Renderer**。
3. **路径 B 是否值得开 POC？** 条件 1、2 已满足。是否认为再留 Mixer 就必须上 float+limiter（条件 3）？
4. **SFX Renderer 生命周期：** 跟随 Wine `Start/Stop`，还是两路常驻 + `INVALID`？这直接决定 D 类会变好还是变差。
5. **两路都 `USAGE_GAME`：** 焦点、打断、蓝牙/扬声器切换是否接受；要不要一路 GAME 一路 MUSIC（可能改变混音/闪避策略，不建议为了混音去改 usage）。
6. **验收：** 同一段 M5 战斗，对比  
   - 现状硬 clamp（已采集）；  
   - 单 Renderer + soft saturate；  
   - 双 Renderer 常驻；  
   - 双 Renderer 跟随 start/stop。  
   指标：`clipped`（或系统侧 peak）、`silent` 翻转次数、听感、来电 RESUME。

---

## 7. 明确不要做的

- 同一提交里改 SRC、Ring 大小、event 模型、**再加** Renderer 数量。
- 把 FMOD/BASS 内部 voice 映射成 Renderer。
- 用全局 -6 dB 当产品修复。
- 未做 2/4 路稳定性就替换默认产品路径。
- 用 WHGP / 手柄日志解释爆音。

---

## 8. 相关代码与日志位置

| 项 | 位置 |
| --- | --- |
| 宿主混音 | `entry/src/main/cpp/audio/audio_broker.cpp` → `MixStreamsS16` |
| 单 Renderer 生命周期 | 同文件 `EnsureRendererLocked`（Broker 启动即 Start） |
| Wine 送 48k S16 | `thirdparty/wine/dlls/wineohos.drv/` |
| 遥测 | hilog tag `WL_AUDIO`，`[AudioBroker] telemetry streams=… clipped=… peak=…` |
| 系统静音 | hilog `AudioLogUtils` `ProcessVolumeData` / `IncSilentData` |
| 架构指引 | 诊断文档第 5.2、第 9–10、第 14 节决策树 |

---

## 9. 一句话给专家

M5 战斗爆音已经不是「欠载猜谜」：**两条 Windows 播放图在宿主被加回 S16 时削顶。**  
Harmony 侧 sink 是 s32，**多 Renderer 把最终求和交给系统，在位数上更对症**；但技能流的 start/stop 仍会撞静音检测，POC 必须单独设计「常驻 vs 启停」。  
若 soft saturate 就能把战斗峰从方波变成轻微压缩，按现有指引可以先留单 Renderer；若还要继续堆 mixer 能力，就应开「一 endpoint 一 Renderer」POC，而不是把 Broker 做成第二个 Audio Service。
