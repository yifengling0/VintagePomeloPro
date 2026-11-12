// Compatibility notice for saved 1.x routes. Never initialize the retired runtime.
import { SteamComponentsPanel } from '../components/SteamComponentsPanel';

@Entry
@Component
struct LinuxSteam {
  build() { SteamComponentsPanel({ legacyLinux: true }) }
}
