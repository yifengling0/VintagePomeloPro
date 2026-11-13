// Saved 1.x routes use the same Windows manager; no retired runtime is initialized.
import { SteamComponentsPanel } from '../components/SteamComponentsPanel';

@Entry
@Component
struct LinuxSteam {
  build() {
    Column() {
      SteamComponentsPanel({ legacyLinux: true })
    }.width('100%').height('100%')
  }
}
