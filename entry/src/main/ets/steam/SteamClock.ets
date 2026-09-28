import { IClock } from './vendor/contracts/SteamPlatformContracts';

/** 宿主时钟（v1 直接用设备时间，不做 NTP 校准）。 */
export class SteamClock implements IClock {
  nowEpochSeconds(): number {
    return Math.floor(Date.now() / 1000);
  }
}
