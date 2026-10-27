import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';

interface SupervisorManifest { schemaVersion: number; architecture: string; name: string; bytes: number; sha256: string; }
const BASE = '/data/storage/el2/base/files/steamohos-native';
const NAME = 'steamohos_gnu_steam_session_hap';

// Add one HAP-owned executable beside the immutable, verified runtime files.
// It never replaces Valve files, changes release identities or touches games.
async function installSupport(context: common.Context, root: string, name: string, manifestName: string, maximum: number): Promise<void> {
  const active = ConfigurationFiles.text(ConfigurationFiles.read(`${BASE}/external-active.path`, 8192)).trim();
  const prefix = `${root}/releases/`;
  if (!root.startsWith('/') || !active.startsWith(prefix) || !/^[a-f0-9]{64}$/.test(active.substring(prefix.length))) {
    throw new Error('Steam 运行组件位置无效');
  }
  for (const directory of [root, `${root}/releases`, active, `${active}/runtime`]) {
    const stat = fs.lstatSync(directory);
    if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('Steam 运行组件目录不安全');
  }
  const raw = 'linux-steam/bootstrap';
  const manifest = JSON.parse(ConfigurationFiles.text(await context.resourceManager.getRawFileContent(`${raw}/${manifestName}`))) as SupervisorManifest;
  if (manifest.schemaVersion !== 1 || manifest.architecture !== 'aarch64-linux-gnu' || manifest.name !== name ||
      !Number.isSafeInteger(manifest.bytes) || manifest.bytes < 1024 || manifest.bytes > maximum || !/^[a-f0-9]{64}$/.test(manifest.sha256)) {
    throw new Error('Steam 会话支持清单无效');
  }
  const path = `${active}/runtime/${name}`;
  for (const file of [path, `${path}.pending`]) {
    if (fs.accessSync(file)) {
      const stat = fs.lstatSync(file);
      if (!stat.isFile() || stat.isSymbolicLink()) throw new Error('Steam 会话支持路径不安全');
    }
  }
  if (fs.accessSync(path) && fs.lstatSync(path).size === manifest.bytes &&
      await ConfigurationFiles.fileHash(path) === manifest.sha256) return;
  const data = await context.resourceManager.getRawFileContent(`${raw}/${name}`);
  if (data.byteLength !== manifest.bytes || data[0] !== 127 || data[1] !== 69 || data[2] !== 76 || data[3] !== 70 ||
      data[4] !== 2 || data[5] !== 1 || data[18] !== 183 || data[19] !== 0 ||
      await ConfigurationFiles.hash(data) !== manifest.sha256) throw new Error('Steam 会话支持校验失败，请重新安装应用');
  ConfigurationFiles.write(`${path}.pending`, data);
  fs.renameSync(`${path}.pending`, path);
}

export async function ensureLinuxSteamClientSupervisor(context: common.Context, root: string): Promise<void> {
  await installSupport(context, root, NAME, 'manifest.json', 262144);
}

export async function ensureLinuxSteamPosixSupport(context: common.Context, root: string): Promise<void> {
  await installSupport(context, root, 'libsteamohos_posix_hap.so', 'posix-manifest.json', 2097152);
  await installSupport(context, root, 'steamohos_fex', 'fex-manifest.json', 262144);
  await installSupport(context, root, 'libsteamohos_native_vulkan.so', 'native-graphics-manifest.json', 1048576);
}
