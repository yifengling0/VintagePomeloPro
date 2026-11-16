import { common, wantConstant } from '@kit.AbilityKit';
import { fileIo as fs, fileUri } from '@kit.CoreFileKit';
import { promptAction } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { AppSettingsStore } from './AppSettingsStore';
import { parseSystemOpenTarget } from '../model/SystemOpenTarget';

export class SystemOpenService {
  private static context: common.UIAbilityContext | null = null;
  private static prefix: string = '';
  private static active: boolean = false;
  private static subscribed: boolean = false;
  private static readonly settingsChanged: () => void = (): void => {
    testNapi.setSystemOpenEnabled(SystemOpenService.active &&
      AppSettingsStore.getInstance().getGlobalSettings().systemOpenEnabled === true);
  };
  static initialize(context: common.UIAbilityContext): void {
    SystemOpenService.context = context;
    if (!SystemOpenService.subscribed) {
      AppSettingsStore.getInstance().subscribeGlobalChanges(SystemOpenService.settingsChanged);
      SystemOpenService.subscribed = true;
    }
  }
  static start(prefix: string): void {
    SystemOpenService.stop();
    SystemOpenService.prefix = prefix;
    SystemOpenService.active = true;
    testNapi.setOpenTargetCallback((target: string): boolean => SystemOpenService.open(target));
    SystemOpenService.settingsChanged();
  }
  static stop(): void {
    SystemOpenService.active = false;
    testNapi.setSystemOpenEnabled(false);
    testNapi.setOpenTargetCallback(null);
    SystemOpenService.prefix = '';
  }
  private static open(target: string): boolean {
    const context = SystemOpenService.context;
    if (!context || !SystemOpenService.active ||
      AppSettingsStore.getInstance().getGlobalSettings().systemOpenEnabled !== true) return false;
    const parsed = parseSystemOpenTarget(target, SystemOpenService.prefix);
    if (!parsed) return false;
    let uri = parsed.uri;
    let flags: number = 0;
    if (parsed.path) {
      try {
        // Resolve the existing Wine dosdevices mapping, then use a shareable
        // system URI. Never pass a sandbox Unix file:// path to another app.
        const file = fs.openSync(parsed.path, fs.OpenMode.READ_ONLY);
        try {
          if (!fs.statSync(file.fd).isFile()) return false;
          uri = fileUri.getUriFromPath(file.path);
        } finally { fs.closeSync(file); }
        if (!uri.startsWith('file://')) return false;
        flags = wantConstant.Flags.FLAG_AUTH_READ_URI_PERMISSION;
      } catch (_) { return false; }
    }
    try {
      void context.startAbility({ action: 'ohos.want.action.viewData', uri, flags })
        .catch((): void => { promptAction.showToast({ message: '系统应用未能打开链接或文件，请检查是否安装了对应应用' }); });
      return true; // Accepted; asynchronous launch failure is reported above.
    } catch (_) {
      promptAction.showToast({ message: '系统应用启动失败，请稍后重试' });
      return true;
    }
  }
}
