import { common, Want } from '@kit.AbilityKit';
import {
  AppDescriptor,
  GlobalSettings,
  LaunchKind,
  RuntimeSession
} from '../model/AppModels';
import { AppSessionService } from './AppSessionService';

export interface LauncherAdapter {
  supports(kind: LaunchKind): boolean;
  launch(app: AppDescriptor, settings: GlobalSettings, homeDirectory: string): Promise<RuntimeSession | null>;
}

class WineLauncherAdapter implements LauncherAdapter {
  supports(kind: LaunchKind): boolean {
    return kind === LaunchKind.WINE_EXE || kind === LaunchKind.WINE_SYSTEM || kind === LaunchKind.WINE_DESKTOP;
  }

  launch(app: AppDescriptor, settings: GlobalSettings, homeDirectory: string): Promise<RuntimeSession | null> {
    return AppSessionService.getInstance().launch(app, settings, homeDirectory);
  }
}

class SteamLauncherAdapter implements LauncherAdapter {
  supports(kind: LaunchKind): boolean {
    return kind === LaunchKind.STEAM_CLIENT || kind === LaunchKind.STEAM_GAME;
  }
  launch(app: AppDescriptor, settings: GlobalSettings, homeDirectory: string): Promise<RuntimeSession | null> {
    return AppSessionService.getInstance().launch(app, settings, homeDirectory);
  }
}

class HarmonyAbilityAdapter implements LauncherAdapter {
  private context: common.UIAbilityContext | null = null;

  initialize(context: common.UIAbilityContext): void {
    this.context = context;
  }

  supports(kind: LaunchKind): boolean {
    return kind === LaunchKind.HARMONY_ABILITY;
  }

  async launch(app: AppDescriptor, _settings: GlobalSettings, _homeDirectory: string): Promise<RuntimeSession | null> {
    if (!this.context) return null;
    const target = app.launchTarget.executable.split('/');
    if (target.length !== 2) return null;
    const want: Want = { bundleName: target[0], abilityName: target[1] };
    await this.context.startAbility(want);
    return null;
  }
}

export class LauncherAdapterRegistry {
  private static instance: LauncherAdapterRegistry | null = null;
  private adapters: LauncherAdapter[] = [];
  private harmonyAdapter: HarmonyAbilityAdapter = new HarmonyAbilityAdapter();

  private constructor() {
    this.adapters.push(new SteamLauncherAdapter());
    this.adapters.push(new WineLauncherAdapter());
    this.adapters.push(this.harmonyAdapter);
  }

  static getInstance(): LauncherAdapterRegistry {
    if (!LauncherAdapterRegistry.instance) LauncherAdapterRegistry.instance = new LauncherAdapterRegistry();
    return LauncherAdapterRegistry.instance;
  }

  initialize(context: common.UIAbilityContext): void {
    this.harmonyAdapter.initialize(context);
  }

  register(adapter: LauncherAdapter): void {
    this.adapters.unshift(adapter);
  }

  launch(app: AppDescriptor, settings: GlobalSettings, homeDirectory: string): Promise<RuntimeSession | null> {
    const adapter = this.adapters.find((item: LauncherAdapter) => item.supports(app.launchTarget.kind));
    return adapter ? adapter.launch(app, settings, homeDirectory) : Promise.resolve(null);
  }
}
