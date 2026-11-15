// Smoke 自动化请求的公共类型：EntryAbility 解析 Want，WineEngineService
// 准备复用或隔离 prefix，再由 SmokeRunner 消费。独立文件保持协议层
// 不依赖产品 UI 和引擎实现。
export interface SmokeRequest {
  suite: string;
  runId: string;
  // 'want' = 外部脚本 aa start 协议; 'ui' = 未来产品入口
  source: string;
  // 'reuse' = 复用产品 .wine; 'clean' = 清理并启动隔离 .wine-smoke 会话
  prefix?: string;
  // dxvk-long/dxvk-modern-long 的墙钟目标 (seconds=-1 哨兵)
  longSeconds?: number;
}