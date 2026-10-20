// Legacy Steam links share the official Linux client entry on this branch.
import { LinuxSteamPanel } from '../components/hmosdeck/LinuxSteamPanel';

@Entry
@Component
struct SteamAccount {
  build() {
    Column() {
      LinuxSteamPanel()
    }.width('100%').height('100%')
  }
}
