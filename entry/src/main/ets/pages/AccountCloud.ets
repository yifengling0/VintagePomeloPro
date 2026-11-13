import router from '@ohos.router';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';

/** Preserve the registered route; all visible content uses the settings shell. */
@Entry
@Component
struct AccountCloud {
  aboutToAppear(): void {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures()) { router.back(); return; }
    void router.replaceUrl({ url: 'pages/SystemSettings', params: { section: 'sync' } });
  }
  build() { Column() { }.width('100%').height('100%') }
}
