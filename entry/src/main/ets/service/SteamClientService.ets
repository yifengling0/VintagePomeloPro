import { SteamClientAction } from '../model/SteamClientModels';
import { SteamLocalService } from '../steam/SteamLocalService';
import { AppCatalogService } from './AppCatalogService';
import { AppSettingsStore } from './AppSettingsStore';
import { AppSessionService } from './AppSessionService';

/** Both launcher skins share the Windows client and its existing session owner. */
export class SteamClientService {
  private static instance: SteamClientService | null = null;
  private opening: boolean = false;
  static getInstance(): SteamClientService {
    if (!SteamClientService.instance) SteamClientService.instance = new SteamClientService();
    return SteamClientService.instance;
  }
  installed(): boolean { return SteamLocalService.getInstance().isInstalled(); }
  isRunning(): boolean { return SteamLocalService.getInstance().hasRunningProcesses(); }
  assertCanOpen(): void {
    const windows = SteamLocalService.getInstance();
    if (windows.isImporting()) throw new Error('Steam 正在导入或更新，请完成后再启动');
    if (!windows.isInstalled()) throw new Error('Windows Steam 尚未安装，请先导入 Windows Steam ZIP');
    if (this.opening) throw new Error('Steam 正在启动，请稍候');
  }
  async open(action: SteamClientAction = 'library'): Promise<void> {
    // Recheck at dispatch time, including callers resumed after an import dialog.
    if (!['library', 'downloads', 'store', 'account', 'desktop', 'bigPicture'].includes(action)) {
      throw new Error('Steam 客户端操作无效，请从游戏或会话入口启动和结束游戏');
    }
    this.assertCanOpen();
    this.opening = true;
    try {
      const app = SteamLocalService.getInstance().clientCard();
      if (app.steam) app.steam.clientAction = action;
      const result = await AppSessionService.getInstance().launch(app,
        AppSettingsStore.getInstance().getGlobalSettings(), AppCatalogService.getInstance().getWineHomeDirectory());
      if (!result) throw new Error('Windows Steam 启动失败，请重试或查看运行日志');
    } finally { this.opening = false; }
  }
}
