import preferences from '@ohos.data.preferences';
import systemDateTime from '@ohos.systemDateTime';
import wantConstant from '@ohos.app.ability.wantConstant';
import { bundleManager, common, Want } from '@kit.AbilityKit';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { hilog } from './LogService';

const STORE_NAME = 'app_review_preferences';
const USAGE_KEY = 'foreground_usage_ms';
const SHOWN_KEY = 'review_prompt_disabled';
const MIN_USAGE_MS = 60 * 60 * 1000;

// VP AppReviewService's durable, once-only reminder and market fallback,
// with VPP foreground usage counted once across its independent windows.
export class AppReviewService {
  private static store: preferences.Preferences | null = null;
  private static owners: Set<common.UIAbilityContext> = new Set();
  private static usageMs: number = 0;
  private static lastSample: number = -1;
  private static shown: boolean = false;
  private static pending: boolean = false;
  private static prompting: boolean = false;
  private static timer: number = -1;
  private static writes: Promise<void> = Promise.resolve();

  private static supported(): boolean {
    const type = DeviceCapabilityPolicy.deviceType();
    return type === 'tablet' || type === '2in1' || type === 'pc';
  }

  private static initialize(context: common.UIAbilityContext): boolean {
    if (AppReviewService.store) return true;
    try {
      const store = preferences.getPreferencesSync(context, { name: STORE_NAME });
      const usage = store.getSync(USAGE_KEY, 0);
      AppReviewService.usageMs = typeof usage === 'number' && Number.isFinite(usage) ?
        Math.max(0, Math.min(MIN_USAGE_MS, usage)) : 0;
      AppReviewService.shown = store.getSync(SHOWN_KEY, false) === true;
      AppReviewService.store = store;
      return true;
    } catch (_) {
      hilog.warn(0, 'AppReview', 'Review preferences unavailable');
      return false;
    }
  }

  private static sample(): void {
    // Active uptime excludes deep sleep and is unaffected by wall-clock edits.
    const now = systemDateTime.getUptime(systemDateTime.TimeType.ACTIVE);
    if (AppReviewService.owners.size > 0 && AppReviewService.lastSample >= 0) {
      AppReviewService.usageMs = Math.min(MIN_USAGE_MS, AppReviewService.usageMs +
        Math.max(0, now - AppReviewService.lastSample));
    }
    AppReviewService.lastSample = now;
  }

  private static persist(): Promise<void> {
    const write = AppReviewService.writes.catch((): void => {}).then(async (): Promise<void> => {
      const store = AppReviewService.store;
      if (!store) return;
      store.putSync(USAGE_KEY, AppReviewService.usageMs);
      store.putSync(SHOWN_KEY, AppReviewService.shown);
      await store.flush();
    });
    AppReviewService.writes = write;
    return write;
  }

  private static stopTimer(): void {
    if (AppReviewService.timer !== -1) clearInterval(AppReviewService.timer);
    AppReviewService.timer = -1;
  }

  static foreground(context: common.UIAbilityContext): void {
    if (!AppReviewService.supported() || !AppReviewService.initialize(context)) return;
    AppReviewService.sample();
    AppReviewService.owners.add(context);
    if (AppReviewService.shown || AppReviewService.usageMs >= MIN_USAGE_MS || AppReviewService.timer !== -1) return;
    AppReviewService.timer = setInterval((): void => {
      AppReviewService.sample();
      void AppReviewService.persist().catch((): void => {});
      if (AppReviewService.usageMs >= MIN_USAGE_MS) AppReviewService.stopTimer();
    }, 60000);
  }

  static background(context: common.UIAbilityContext): void {
    if (!AppReviewService.owners.has(context)) return;
    AppReviewService.sample();
    AppReviewService.owners.delete(context);
    if (AppReviewService.owners.size === 0) {
      AppReviewService.stopTimer();
      AppReviewService.lastSample = -1;
    }
    void AppReviewService.persist().catch((): void => {});
  }

  static async maybePrompt(context: common.UIAbilityContext, canShow: () => boolean,
    show: () => Promise<void>): Promise<void> {
    if (!AppReviewService.supported() || AppReviewService.prompting ||
      !AppReviewService.owners.has(context) || !canShow()) return;
    AppReviewService.sample();
    if (AppReviewService.usageMs < MIN_USAGE_MS || (AppReviewService.shown && !AppReviewService.pending)) return;
    AppReviewService.prompting = true;
    try {
      if (!AppReviewService.shown) {
        AppReviewService.shown = true;
        try { await AppReviewService.persist(); }
        catch (_) { AppReviewService.shown = false; return; }
        AppReviewService.pending = true;
        AppReviewService.stopTimer();
      }
      // Persist before showing. A late navigation/background event defers the
      // claimed reminder until the library is visible again in this process.
      if (!AppReviewService.owners.has(context) || !canShow()) return;
      AppReviewService.pending = false;
      await show();
    } catch (_) {
      hilog.warn(0, 'AppReview', 'Review dialog unavailable');
    } finally { AppReviewService.prompting = false; }
  }

  static async openReviewPage(context: common.UIAbilityContext): Promise<boolean> {
    let name = 'com.vintage.pomelopro';
    try {
      const info = await bundleManager.getBundleInfoForSelf(bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
      name = info.name || name;
    } catch (_) {}
    const wants: Want[] = [{
      bundleName: 'com.huawei.appmarket', action: 'android.intent.action.VIEW',
      uri: 'market://details?id=' + encodeURIComponent(name),
      flags: wantConstant.Flags.FLAG_START_WITHOUT_TIPS
    }, {
      action: 'ohos.want.action.viewData', entities: ['entity.system.browsable'],
      uri: 'https://appgallery.huawei.com/app/detail?id=' + encodeURIComponent(name),
      flags: wantConstant.Flags.FLAG_START_WITHOUT_TIPS
    }];
    for (const want of wants) {
      try { await context.startAbility(want); return true; } catch (_) {}
    }
    return false;
  }
}
