# Mini Heaven：精确范围发布实验

2026-11-14，Mini MLR-AL10，Heaven 4.0 Basic PE32 / Direct3D11 / `dxvk_legacy`。
固定 1280×720 全屏、Low、AA off、tessellation off、VSync off，每轮完成 26 场景。
游戏实际使用 Venus，桌面合成使用 Direct Vulkan。系统支持 Direct Vulkan 不代表该 PE32 fresh 进程已支持驱动与 producer IPC。

## 改动

`WINEHUA_FRESH_VENUS_PRECISE_MAP=1` 是默认关闭的实验开关。
仅在 fresh 进程从 Direct 兼容回退至 Venus，且同时具有以下环境时，保留最终 `WINEHUA_VK_PRECISE_MAP=1`：

- `WINEHUA_D3D_BACKEND=dxvk_legacy`
- `DXVK_WINEHUA_PRECISE_SHADOW=1`
- `DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1`

否则继续整块 shadow-map 发布的 fallback。保留既有 Venus、strong ring barrier、通知与 remote sync 策略。
启动日志打印最终 `preciseMaps` 和 `mappedFlush`，避免只观察后续会被覆盖的初始环境。
优化复用已有显式 upload/readback 发布机制，减少 QueueSubmit 前的整块重复 CPU 复制；没有新增脏页追踪，也没有证明全部映射都可以省略复制。
仅验证了 Heaven D3D11；`dxvk_legacy` 名称不能代替 D3D9 等其他 API 的写入覆盖证明。

## 同包真机结果

两种模式使用同一个可采样 HAP，但 FPS 轮次均未开启 profiler。
交错执行顺序为开启 B1、关闭 A2、开启 B2、关闭 A3。

| 模式 | FPS | 中位数 | Heaven CPU 中位数 | renderer CPU 中位数 |
| --- | --- | --- | --- | --- |
| 关闭 | 8.2、7.7 | 7.95 | 286.02% | 170.58% |
| 开启 | 11.5、10.7 | 11.10 | 229.80% | 180.42% |

中位数之比提高 39.62%；100% CPU 表示一个逻辑 CPU。CPU 数据来自 benchmark 中间 240 秒。
另有旧普通包 A1 的 9.0 FPS 参考，因包和 profileable 元数据不同，未加入同包统计。
每模式两次，未精确控温或隔离全部后台调度/通知；属于探索性结果，不能宣称统计显著、固定收益或全游戏验收。

独立的初始场景 14 秒 `hw-cpu-cycles` 四进程采样中：

- dxvk-submit 的 `memcpy` 叶子周期权重从 16.8563% 降至 0.3675%。
- 关闭时主要栈为 `thunk32_vkQueueSubmit -> win32u_vkQueueSubmit -> memcpy`。
- 开启后剩余提交线程复制主要在显式 `FlushMappedMemoryRanges`；dxvk-submit 全线程权重从 18.31% 降至 2.96%。
- renderer / vkr-ring、通信 read/write/sendmsg/recvmsg，以及 flush/present 的 ring 等待仍值得测量。

周期采样权重不能换算为帧耗时、阻塞墙钟时间或预期 FPS 收益；JIT/驱动仍存在未解析符号。

## 验证与复现资料

- `make test-direct-session`：真实环境管线、开关支持/拒绝条件及图形策略通过。
- 实际 Wine 组件 `098ece0eedff40bea426d2d57b215bb81411e239` 对应 mapping helper 验证通过，覆盖 alias、破坏性 MAP_FIXED 失败恢复、精确 upload/readback 与边界。
- arm64 release native 增量构建通过；HAP 其他 payload 与普通基线一致；内测签名和 profile/证书匹配通过。
- 四轮结果页均人工核对，抽查场景未见明显缺失纹理或大灰块。未验收 High/tess/AA、D3D9、VKD3D、64 位游戏与长时间游玩。

被测源码基线：`6094ed5eeccd64038d61ef2449bdcbf2f82db39c`，加本提交中的 3 个源码/测试文件改动。
可采样包 SHA-256：`f63417ff9c0ff12141443b6303ab59868d85f9d93c357fa53358ada86bf01adb`。
同 native payload 的普通包 SHA-256：`1c425c18ab6a0c8342727dcec5311b429c5b5c3bcb01b645667b060eb3e95d5f`，仅另做替换安装和开启优化的启动/画面 smoke。

完整原始结果、场景截图、压缩采样与 SHA-256 清单位于本机：
`F:/VintagePomelo-Workspace/artifacts/mini-heaven-optimization/`。
既有冻结基线位于相邻 `mini-heaven-performance/`，未改写。
固定复现脚本位于 `F:/VintagePomelo-Workspace/workspace_temp/heaven-performance/`：

```text
python opt-runner.py <new-label> whole
python opt-runner.py <new-label> precise
```

需要传递 `winehua.d3d_env_json` 的显式启动配置；实验不是全游戏默认值。正式 FPS 与 profiler 分开执行，并检查最终进程策略、实际 extent 和完整结果页。

下一步先测量 Venus/vtest 的消息和通知次数、ring 等待墙钟时间与 GPU 空闲区间，确定可安全批处理的同步区间；根据关键线程证据再决定调度实验。
