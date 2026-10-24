import { common } from '@kit.AbilityKit';
import { UIContext } from '@kit.ArkUI';
import { LinuxSteamAction } from '../model/LinuxSteamModels';
import { SteamLocalService } from '../steam/SteamLocalService';
import { AppCatalogService } from './AppCatalogService';
import { AppSettingsStore } from './AppSettingsStore';
import { AppSessionService } from './AppSessionService';
import { LinuxSteamInstallService } from './LinuxSteamInstallService';
import { LinuxSteamSessionService } from './LinuxSteamSessionService';
import { linuxSteamInstallBusy } from '../model/LinuxSteamDownloadModels';

export type SteamClientBackend = 'windows' | 'linux';

/** Client selection affects client navigation only. Game cards keep their own backend. */
export class SteamClientService {
  private static instance: SteamClientService | null = null;
  private choice: Promise<SteamClientBackend | ''> | null = null;
  private opening: SteamClientBackend | '' = '';
  static getInstance(): SteamClientService {
    if (!SteamClientService.instance) SteamClientService.instance = new SteamClientService();
    return SteamClientService.instance;
  }
  installed(): SteamClientBackend[] {
    const result: SteamClientBackend[] = [];
    if (SteamLocalService.getInstance().isInstalled()) result.push('windows');
    if (LinuxSteamInstallService.getInstance().installed()) result.push('linux');
    return result;
  }
  async choose(ui: UIContext): Promise<SteamClientBackend | ''> {
    if (this.choice) return this.choice;
    const installed = this.installed();
    if (installed.length < 2) return installed[0] ?? '';
    const choice = new Promise<SteamClientBackend | ''>((resolve): void => {
      ui.showAlertDialog({ title: '选择 Steam 版本', message: '两个版本的客户端和游戏库独立保存。',
        buttons: [
          { value: 'Windows Steam', action: (): void => resolve('windows') },
          { value: 'Linux ARM64 Steam', action: (): void => resolve('linux') },
          { value: '取消', action: (): void => resolve('') }
        ], cancel: (): void => resolve('') });
    });
    this.choice = choice;
    try { return await choice; } finally { if (this.choice === choice) this.choice = null; }
  }
  assertCanOpen(backend: SteamClientBackend): void {
    if (!this.installed().includes(backend)) throw new Error('所选 Steam 尚未安装，请先导入或安装组件');
    if (this.opening && this.opening !== backend) throw new Error('另一个 Steam 正在启动，请稍后重试');
    const windows = SteamLocalService.getInstance();
    if (windows.isImporting() || linuxSteamInstallBusy(LinuxSteamInstallService.getInstance().snapshot().phase)) {
      throw new Error('Steam 组件正在安装或更新，请完成后再启动');
    }
    if (backend === 'windows' && LinuxSteamSessionService.getInstance().isActive()) {
      throw new Error('请先正常退出 Linux Steam 及其游戏，再启动 Windows Steam');
    }
    if (backend === 'linux' && (windows.hasRunningProcesses() || windows.hasForeignClient())) {
      throw new Error('请先正常退出 Windows Steam 及其游戏，再启动 Linux Steam');
    }
  }
  async open(context: common.UIAbilityContext, backend: SteamClientBackend,
    action: LinuxSteamAction = 'library'): Promise<void> {
    // Launch and shutdown remain owned by the originating game/session service.
    if (action === 'launch' || action === 'shutdown') throw new Error('请从对应的游戏或会话入口执行此操作');
    this.assertCanOpen(backend);
    if (this.opening) throw new Error('Steam 正在启动，请稍候');
    this.opening = backend;
    try {
      if (backend === 'linux') {
        const session = LinuxSteamSessionService.getInstance(); session.initialize(context);
        await session.open(action);
      } else {
        const app = SteamLocalService.getInstance().clientCard();
        if (app.steam) app.steam.clientAction = action;
        const result = await AppSessionService.getInstance().launch(app,
          AppSettingsStore.getInstance().getGlobalSettings(), AppCatalogService.getInstance().getWineHomeDirectory());
        if (!result) throw new Error('Windows Steam 启动失败，请重试或查看运行日志');
      }
    } finally { this.opening = ''; }
  }
}
