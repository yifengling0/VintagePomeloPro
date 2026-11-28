import { SteamClientAction } from '../model/SteamClientModels';
import { SteamLocalService } from '../steam/SteamLocalService';
import { AppCatalogService } from './AppCatalogService';
import { AppSettingsStore } from './AppSettingsStore';
import { AppSessionService } from './AppSessionService';
import { HostWindowTasks } from './HostWindowTasks';
import { DeckLaunchFeedbackService } from './DeckLaunchFeedbackService';

const STARTUP_HINT = '首次启动或更新可能较慢，请耐心等待。';
const SLOW_STARTUP_HINT = '首次启动或更新可能需要更久。若长时间卡住，请检查网络或尝试更稳定的连接。';

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
  async open(action: SteamClientAction = 'library', restoreWindow: boolean = false,
    classicFeedback: boolean = true): Promise<void> {
    // Recheck at dispatch time, including callers resumed after an import dialog.
    if (!['library', 'downloads', 'store', 'account', 'desktop', 'bigPicture'].includes(action)) {
      throw new Error('Steam 客户端操作无效，请从游戏或会话入口启动和结束游戏');
    }
    this.assertCanOpen();
    this.opening = true;
    const tasks = HostWindowTasks.getInstance();
    const feedback = DeckLaunchFeedbackService.getInstance();
    let navigation = tasks.navigationSerial();
    let ticket = 0;
    let slowTimer = -1;
    try {
      const app = SteamLocalService.getInstance().clientCard();
      if (app.steam) app.steam.clientAction = action;
      const sessions = AppSessionService.getInstance();
      if (restoreWindow && action === 'library') {
        const key = sessions.steamClientWindowKey(app);
        if (key) {
          const activation = tasks.activate(key);
          navigation = tasks.navigationSerial();
          if (await activation || tasks.navigationSerial() !== navigation) return;
        }
      }
      let status = '正在准备 Steam…';
      let hint = STARTUP_HINT;
      ticket = feedback.begin({ title: 'Steam', cover: '', message: status, hint, classic: classicFeedback });
      slowTimer = setTimeout((): void => {
        hint = SLOW_STARTUP_HINT;
        feedback.update(ticket, status, hint);
      }, 35000);
      // Give the launch surface a frame before starting the native engine.
      await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 48); });
      const result = await sessions.launch(app,
        AppSettingsStore.getInstance().getGlobalSettings(), AppCatalogService.getInstance().getWineHomeDirectory());
      if (!result) throw new Error('Windows Steam 启动失败，请重试或查看运行日志');
      // A command-sender PID or the Wine desktop is not a ready Steam client.
      await sessions.waitForSteamClientWindow(app, result, navigation, undefined, undefined,
        (message: string): void => {
          status = message;
          feedback.update(ticket, status, hint);
        });
    } catch (error) {
      if (tasks.navigationSerial() === navigation) {
        try { await tasks.showLauncher(); } catch (_) {}
      }
      throw new Error((error as Error).message);
    } finally {
      if (slowTimer >= 0) clearTimeout(slowTimer);
      feedback.finish(ticket);
      this.opening = false;
    }
  }
}
