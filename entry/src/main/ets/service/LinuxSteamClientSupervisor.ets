import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';

interface GuestCoreFile { bytes: number; sha256: string; }
interface GuestCoreManifest { schemaVersion: number; architecture: string; files: Record<string, GuestCoreFile>; }
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
  await installSupport(context, root, 'steamohos_gnu_desktop_hap', 'desktop-manifest.json', 262144);
  await installSupport(context, root, 'libsteamohos_posix_hap.so', 'posix-manifest.json', 2097152);
  await installSupport(context, root, 'steamohos_lsof_hap', 'lsof-manifest.json', 2097152);
  await installSupport(context, root, 'steamohos_fex', 'fex-manifest.json', 262144);
  await installSupport(context, root, 'libsteamohos_native_vulkan.so', 'native-graphics-manifest.json', 1048576);
  await installNativeGuestCore(context, root);
  await installNativeVulkanDriver(context, root);
  await installNativeLibraries(context, root, 'native-game-support', 'aarch64-linux-gnu', 183,
    ['FEX', 'FEXServer', 'libvulkan_virtio.so', 'libsteamohos_software_gl.so',
      'libGL-host32.so', 'libGL-host64.so', 'libGL-guest32.so', 'libGL-guest64.so']);
}

// A paired GNU x86 loader/core for FEX guest libraries. Runtime game data and
// the ARM64 Steam/Wine library namespace remain in their original locations.
async function installNativeGuestCore(context: common.Context, root: string): Promise<void> {
  await installNativeLibraries(context, root, 'native-guest-core-x86_64', 'x86_64-linux-gnu', 62,
    ['ld-linux-x86-64.so.2', 'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0',
      'librt.so.1', 'libutil.so.1', 'libresolv.so.2', 'libanl.so.1', 'libnss_dns.so.2', 'libnss_files.so.2', 'libgcc_s.so.1', 'libstdc++.so.6', 'libudev.so.1', 'libasound_module_pcm_hmosdeck.so',
      'libsteamohos_native_ipc.so']);
}
async function installNativeVulkanDriver(context: common.Context, root: string): Promise<void> {
  await installNativeLibraries(context, root, 'native-vulkan-driver', 'aarch64-linux-gnu', 183,
    ['libvulkan_virtio.so', 'libxcb-dri3.so.0', 'libxcb-present.so.0', 'libxcb-xfixes.so.0',
      'libxcb-sync.so.1', 'libxcb-randr.so.0', 'libxcb-shm.so.0', 'Xwayland', 'libGL.so.1',
      'libXxf86vm.so.1', 'libdril_dri.so', 'libgallium-25.0.1.so', 'libgbm.so.1', 'libxcb-glx.so.0'], `${BASE}/proton-probe/graphics`);
}
// Source libraries are already verified/extracted from the immutable GNU
// package by ensureReady(). Copy only this small closure into rtld's release
// scope; never broaden executable-memory permissions or touch game depots.
async function installNativeLibraries(context: common.Context, root: string, leaf: string, architecture: string,
  machine: number, names: string[], sourceRoot: string = ''): Promise<void> {
  const active = ConfigurationFiles.text(ConfigurationFiles.read(`${BASE}/external-active.path`, 8192)).trim();
  const prefix = `${root}/releases/`;
  if (!active.startsWith(prefix) || !/^[a-f0-9]{64}$/.test(active.substring(prefix.length))) throw new Error('Linux 游戏基础库位置无效');
  const raw = `linux-steam/bootstrap/${leaf}`;
  const encoded = await context.resourceManager.getRawFileContent(`${raw}/manifest.json`);
  const manifest = JSON.parse(ConfigurationFiles.text(encoded)) as GuestCoreManifest;
  if (manifest.schemaVersion !== 1 || manifest.architecture !== architecture || Object.keys(manifest.files).length !== names.length) throw new Error('Linux 游戏基础库清单无效');
  if (sourceRoot) {
    for (const parent of [BASE, `${BASE}/proton-probe`, sourceRoot]) {
      const stat = fs.lstatSync(parent);
      if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('Linux 图形库来源目录不安全');
    }
  }
  const directory = `${active}/runtime/${leaf}`;
  for (const parent of [root, `${root}/releases`, active, `${active}/runtime`]) {
    const stat = fs.lstatSync(parent);
    if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('Linux 游戏基础库路径不安全');
  }
  if (!fs.accessSync(directory)) fs.mkdirSync(directory);
  const stat = fs.lstatSync(directory);
  if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('Linux 游戏基础库目录不安全');
  for (const name of names) {
    const row = manifest.files[name];
    if (!row || !Number.isSafeInteger(row.bytes) || row.bytes < 1024 || row.bytes > (sourceRoot || leaf === 'native-game-support' ? 33554432 : 8388608) || !/^[a-f0-9]{64}$/.test(row.sha256)) throw new Error('Linux 游戏基础库记录无效');
    const path = `${directory}/${name}`;
    for (const member of [path, `${path}.pending`]) {
      if (fs.accessSync(member) && (!fs.lstatSync(member).isFile() || fs.lstatSync(member).isSymbolicLink())) throw new Error('Linux 游戏基础库文件不安全');
    }
    if (fs.accessSync(path) && fs.lstatSync(path).size === row.bytes && await ConfigurationFiles.fileHash(path) === row.sha256) continue;
    if (sourceRoot) {
      const source = fs.lstatSync(`${sourceRoot}/${name}`);
      if (!source.isFile() || source.isSymbolicLink() || source.size !== row.bytes) throw new Error('Linux 图形库来源文件无效');
    }
    const data = sourceRoot ? ConfigurationFiles.read(`${sourceRoot}/${name}`, 33554432) :
      await context.resourceManager.getRawFileContent(`${raw}/${name}`);
    const expectedMachine = leaf === 'native-game-support' && name === 'libGL-guest32.so' ? 3 :
      leaf === 'native-game-support' && name === 'libGL-guest64.so' ? 62 : machine;
    const expectedClass = expectedMachine === 3 ? 1 : 2;
    if (data.byteLength !== row.bytes || data[0] !== 127 || data[1] !== 69 || data[2] !== 76 || data[3] !== 70 || data[4] !== expectedClass || data[5] !== 1 || data[18] !== expectedMachine || data[19] !== 0 || await ConfigurationFiles.hash(data) !== row.sha256) throw new Error('Linux 游戏基础库校验失败');
    ConfigurationFiles.write(`${path}.pending`, data);
    fs.renameSync(`${path}.pending`, path);
  }
  const marker = `${directory}/manifest.json`;
  for (const member of [marker, `${marker}.pending`]) {
    if (fs.accessSync(member) && (!fs.lstatSync(member).isFile() || fs.lstatSync(member).isSymbolicLink())) throw new Error('Linux 游戏基础库标记不安全');
  }
  ConfigurationFiles.write(`${marker}.pending`, encoded);
  fs.renameSync(`${marker}.pending`, marker);
}
