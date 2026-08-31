import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from './LogService';

// Payload preparation only. Tests and games launch through the normal catalog
// and WineEngineService; this module never starts or changes a Wine session.
const DOMAIN = 0x0000;
const FILES_BASE = '/data/storage/el2/base/files';
const WINE_ROOT = `${FILES_BASE}/wine`;

function pathExists(path: string): boolean {
  try {
    return fs.accessSync(path);
  } catch (_) {
    return false;
  }
}

function ensureDirectory(path: string): void {
  if (!pathExists(path)) fs.mkdirSync(path, true);
}

function removeRecursive(path: string): void {
  if (!pathExists(path)) return;
  const stat = fs.lstatSync(path);
  if (stat.isSymbolicLink() || !stat.isDirectory()) {
    fs.unlinkSync(path);
    return;
  }
  for (const name of fs.listFileSync(path)) removeRecursive(`${path}/${name}`);
  fs.rmdirSync(path);
}

function copyRecursive(source: string, target: string): void {
  const stat = fs.statSync(source);
  if (!stat.isDirectory()) {
    const slash = target.lastIndexOf('/');
    if (slash > 0) ensureDirectory(target.substring(0, slash));
    fs.copyFileSync(source, target);
    return;
  }
  ensureDirectory(target);
  for (const name of fs.listFileSync(source)) copyRecursive(`${source}/${name}`, `${target}/${name}`);
}

function readText(path: string): string {
  try {
    return fs.readTextSync(path, { encoding: 'utf-8' }).trim();
  } catch (_) {
    return '';
  }
}

function prefixRoot(prefixMode: string): string {
  return prefixMode === 'clean' ? `${FILES_BASE}/.wine-smoke` : `${FILES_BASE}/.wine`;
}

export function syncManagedSmoke(prefixMode: string): boolean {
  const source = `${WINE_ROOT}/smoke`;
  const target = `${prefixRoot(prefixMode)}/drive_c/smoke`;
  const sourceManifest = readText(`${source}/manifest.json`);
  if (!sourceManifest) {
    hilog.error(DOMAIN, 'ManagedSmokePayload', 'managed smoke manifest missing: %{public}s', source);
    return false;
  }
  /* A previous interrupted extraction can leave a matching manifest but a
   * partially populated C:\smoke tree.  Validate the managed entry points
   * before taking the fast path; DXVK x64 otherwise fails as a misleading
   * "d3d11.dll not found" while the x86 slot happens to exist. */
  const required = [
    'x64/winehua_audio_smoke.exe', 'x64/winehua_graphics_smoke.exe',
    'x64/winehua_vulkan_smoke.exe', 'x64/winehua_d3d11_smoke.exe',
    'x64/winehua_d3d8_smoke.exe', 'x64/winehua_d3d_switch_cube.exe',
    'x64/winehua_media_smoke.exe',
    'x64/winehua_win32_driver.exe',
    'x64/winehua_network_probe.exe',
    'x86/winehua_audio_smoke.exe', 'x86/winehua_graphics_smoke.exe',
    'x86/winehua_vulkan_smoke.exe', 'x86/winehua_d3d11_smoke.exe',
    'x86/winehua_d3d8_smoke.exe', 'x86/winehua_d3d_switch_cube.exe',
    'x86/winehua_media_smoke.exe',
    'x86/winehua_win32_driver.exe',
    'assets/venus_storage_write.spv', 'assets/venus_storage_read.spv',
    'assets/venus_image_fetch.spv', 'assets/venus_combined_sample.spv',
    'assets/venus_separated_sample.spv',
  ];
  /* VKD3D D3D12 demos are packaged with wine-data when vkd3d-proton built.
   * Keep them optional so an older payload without these files still syncs. */
  const optional = [
    'x64/triangle.exe',
    'x64/gears.exe',
  ];
  /* DXVK is a WineHua runtime overlay, not part of C:\\smoke. Remove files
   * leaked there by older payloads, but only inside this managed directory. */
  for (const file of ['x64/d3d11.dll', 'x64/dxgi.dll', 'x86/d3d11.dll', 'x86/dxgi.dll']) {
    try {
      const stale = `${target}/${file}`;
      if (pathExists(stale)) fs.unlinkSync(stale);
    } catch (error) {
      hilog.warn(DOMAIN, 'ManagedSmokePayload', 'stale managed DXVK cleanup failed path=%{public}s error=%{public}s',
        `${target}/${file}`, JSON.stringify(error));
    }
  }
  /* Remove the old payload layout from prefixes upgraded before DXVK became a
   * runtime component.  This never touches user-installed game files. */
  const staleDxvk = target + '/dxvk';
  if (pathExists(staleDxvk)) removeRecursive(staleDxvk);
  let complete = sourceManifest === readText(`${target}/manifest.json`);
  for (const file of required) {
    if (!pathExists(`${target}/${file}`)) {
      complete = false;
      break;
    }
  }
  if (complete) {
    for (const file of optional) {
      if (pathExists(`${source}/${file}`) && !pathExists(`${target}/${file}`)) {
        complete = false;
        break;
      }
    }
  }
  if (complete) return true;

  try {
    removeRecursive(target);
    ensureDirectory(target);
    /* Keep the runtime-owned dxvk/ directory out of the prefix. */
    copyRecursive(`${source}/manifest.json`, `${target}/manifest.json`);
    for (const file of required) copyRecursive(`${source}/${file}`, `${target}/${file}`);
    for (const file of optional) {
      if (pathExists(`${source}/${file}`)) copyRecursive(`${source}/${file}`, `${target}/${file}`);
    }
    if (pathExists(`${source}/assets`)) copyRecursive(`${source}/assets`, `${target}/assets`);
    ensureDirectory(`${target}/results`);
    hilog.info(DOMAIN, 'ManagedSmokePayload', 'managed smoke synchronized prefix=%{public}s', prefixMode);
    return sourceManifest === readText(`${target}/manifest.json`);
  } catch (error) {
    hilog.error(DOMAIN, 'ManagedSmokePayload', 'managed smoke sync failed: %{public}s', JSON.stringify(error));
    return false;
  }
}
