# 2026-10-09 OpenGL / Gal 修复证据

- 原有 WGL / Vulkan 能力矩阵保留，用于说明 GL 3.3 / 4.6 与 Zink 的实际限制。
- `chaos-matched-story-ab-20261009.json`：GPU → SHM → GPU 三轮，同一 New Game 首张剧情卡，中央中文区域逐像素一致。原始 PNG 留在本地，JSON 保存其 SHA256；JPG 仅用于视觉评审。
- `chaos-ab-*-fps-window.txt`：同期新图像呈现统计与实际上传 / import / reuse / fence 计数；温度快照另附。硬编码的旧 gameCpuReadBytes=0 不是 GL 无回读证明。
- `d3d9-srgb-gpu-final{32,64}-20261009.tsv`：最终候选两架构各 10 项颜色检查。
- `gpu-present-probe{32,64}-gpu-final-20261009.tsv` 及四张 PNG：双窗口、resize、销毁重建；32 个角区域结果在像素 JSON 中。
- `rottr64-gpu-final-*`：原 DXVK Direct 64 位游戏启动 / 菜单回归。没有新增游戏内性能结论。
- 名为 `chaos-final-matched-title-summary.json` 的早期样本实际为 OP，已显式标注场景不匹配，不用于同标题 A/B。

本目录不含 HAP、完整 Wine stderr 或原始大型采样。设备序列号、SteamID 由采集 wrapper 脱敏；截图为离线游戏内容。
完整说明在上级架构报告。SHA256SUMS 覆盖除其自身外的每个文件。
