import { common } from '@kit.AbilityKit';
import { fileIo as fs, fileUri, picker } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { ConfigurationFiles } from './ConfigurationFiles';
import { ensureSmokeDirectory, readSmokeText, writeSmokeText } from './ManagedSmokePayloadService';
import { reclaimLinuxSteamReleases } from './LinuxSteamReleaseRetention';
import { prepareSingleLinuxSteamLibrary } from './LinuxSteamLibraryLayout';
import { SteamVdfNode, parseSteamVdf, serializeSteamVdf } from '../steam/SteamLocalRules';

const BASE = '/data/storage/el2/base/files/steamohos-native';
const IMPORT = `${BASE}/external-import`;

interface ExternalFile {
  path: string;
  sha256: string;
  bytes: number;
}
interface ExternalManifest {
  schemaVersion: number;
  layout: string;
  executionModel: string;
  steamSha256: string;
  files: ExternalFile[];
}
interface RuntimeData { sha256: string; }
interface RuntimeProbe { data: RuntimeData; }
interface RuntimeManifest { steamClientProbe: RuntimeProbe; xwaylandProbe: RuntimeProbe; }

// DOWNLOAD resolves the same app-owned system directory used by Wine's Z:.
export async function prepareLinuxSteamExternalStorage(context: common.Context): Promise<string> {
  const documentPicker = new picker.DocumentViewPicker(context);
  const choices = await documentPicker.save({ pickerMode: picker.DocumentPickerMode.DOWNLOAD });
  if (!choices || choices.length === 0) throw new Error('Cannot resolve Steam Z drive directory');
  const home = new fileUri.FileUri(choices[0]).path.replace(/\/$/, '');
  const root = `${home}/SteamLinuxARM64`;
  if (!ensureSmokeDirectory(root)) throw new Error('Cannot create Steam Z drive directory');
  if (!ensureSmokeDirectory(BASE)) throw new Error('Cannot create Steam diagnostic state directory');
  writeSmokeText(`${BASE}/external-root.path`, root);
  return root;
}

function validExternalPath(path: string): boolean {
  return /^runtime\/[A-Za-z0-9_.+\/\-]+$/.test(path) && !path.split('/').includes('..') &&
    !path.split('/').includes('.') && !path.endsWith('/');
}

function copyExternalFile(source: string, destination: string): void {
  if (!ensureSmokeDirectory(destination.substring(0, destination.lastIndexOf('/')))) {
    throw new Error('Cannot create external runtime directory');
  }
  const input = fs.openSync(source, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
  try {
    const output = fs.openSync(destination, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try {
      const buffer = new ArrayBuffer(4 * 1024 * 1024);
      while (true) {
        const read = fs.readSync(input.fd, buffer);
        if (read === 0) break;
        if (read < 0) throw new Error('External runtime read failed');
        let written = 0;
        while (written < read) {
          const count = fs.writeSync(output.fd, buffer.slice(written, read));
          if (count <= 0) throw new Error('External runtime write failed');
          written += count;
        }
      }
    } finally { fs.closeSync(output); }
  } finally { fs.closeSync(input); }
}

// Only the explicit diagnostic suite consumes a local HDC-staged directory.
// A completed release remains on Z: across HAP updates; there is no bundled
// Steam fallback and no automatic third-party download or redistribution.
export async function prepareLinuxSteamClientData(context: common.Context): Promise<void> {
  const root = await prepareLinuxSteamExternalStorage(context);
  const sourceManifest = `${IMPORT}/manifest.json`;
  const previousVersion = readSmokeText(`${root}/active-version.txt`).trim();
  let version = previousVersion;
  if (fs.accessSync(sourceManifest)) {
    const text = ConfigurationFiles.text(ConfigurationFiles.read(sourceManifest, 1048576));
    const manifest = JSON.parse(text) as ExternalManifest;
    if (manifest.schemaVersion !== 1 || manifest.layout !== 'SteamLinuxARM64' ||
        manifest.executionModel !== 'native_aarch64' || !/^[a-f0-9]{64}$/.test(manifest.steamSha256) ||
        manifest.files.length < 1 || manifest.files.length > 512) throw new Error('Invalid external Steam manifest');
    version = await ConfigurationFiles.hash(ConfigurationFiles.bytes(text));
    const cleanup = await reclaimLinuxSteamReleases(root, previousVersion, version, true);
    writeSmokeText(`${BASE}/release-cleanup.json`, JSON.stringify(cleanup));
    const release = `${root}/releases/${version}`;
    const complete = `${release}/.verified`;
    if (readSmokeText(complete).trim() !== version) {
      const seen: Set<string> = new Set<string>();
      let importedFiles = 0;
      for (const file of manifest.files) {
        if (!validExternalPath(file.path) || seen.has(file.path) ||
            !/^[a-f0-9]{64}$/.test(file.sha256) || !Number.isInteger(file.bytes) || file.bytes < 1 ||
            file.bytes > 536870912) throw new Error('Invalid external Steam file');
        seen.add(file.path);
        if (importedFiles % 16 === 0) writeSmokeText(`${BASE}/import-progress.json`, JSON.stringify({ phase: 'copy', completed: importedFiles, total: manifest.files.length, file: file.path }));
        importedFiles++;
        const staged = `${IMPORT}/${file.path}`;
        const source = fs.accessSync(staged) ? staged : `${root}/releases/${previousVersion}/${file.path}`;
        if (!fs.accessSync(staged) && !/^[a-f0-9]{64}$/.test(previousVersion)) throw new Error('External Steam base release is unavailable');
        if (fs.statSync(source).size !== file.bytes || await ConfigurationFiles.fileHash(source) !== file.sha256) {
          throw new Error(`External Steam source checksum failed: ${file.path}`);
        }
        const destination = `${release}/${file.path}`;
        copyExternalFile(source, destination);
        if (await ConfigurationFiles.fileHash(destination) !== file.sha256) {
          throw new Error(`External Steam copy checksum failed: ${file.path}`);
        }
      }
      if (!seen.has('runtime/steamrtarm64/steam') || !seen.has('runtime/ld-linux-aarch64.so.1') ||
          !seen.has('runtime/manifest.json') || !seen.has('runtime/steam-client-data.zip') ||
          !seen.has('runtime/xkb-data.zip')) throw new Error('External Steam runtime is incomplete');
      writeSmokeText(`${release}/external-manifest.json`, text);
      writeSmokeText(complete, version);
    }
  }
  if (!/^[a-f0-9]{64}$/.test(version)) throw new Error('请先将 Steam ARM64 资源导入 Z:\\SteamLinuxARM64');
  const release = `${root}/releases/${version}`;
  if (readSmokeText(`${release}/.verified`).trim() !== version) throw new Error('External Steam release is incomplete');
  const runtime = `${release}/runtime`;
  const releaseManifestBytes = ConfigurationFiles.read(`${release}/external-manifest.json`, 1048576);
  if (await ConfigurationFiles.hash(releaseManifestBytes) !== version) throw new Error('External release manifest changed');
  const releaseManifest = JSON.parse(ConfigurationFiles.text(releaseManifestBytes)) as ExternalManifest;
  for (const file of releaseManifest.files) {
    if (!validExternalPath(file.path)) throw new Error('Invalid cached external Steam path');
  }
  let expectedSteam = '';
  for (const file of releaseManifest.files) {
    if (file.path === 'runtime/steamrtarm64/steam') expectedSteam = file.sha256;
  }
  if (!/^[a-f0-9]{64}$/.test(expectedSteam) || expectedSteam !== releaseManifest.steamSha256 ||
      await ConfigurationFiles.fileHash(`${runtime}/steamrtarm64/steam`) !== expectedSteam) {
    throw new Error('Original Steam ARM64 identity changed');
  }
  const runtimeManifest = JSON.parse(readSmokeText(`${runtime}/manifest.json`)) as RuntimeManifest;
  const home = `${root}/client/.steam/steam`;
  if (readSmokeText(`${home}/.packaged-data-version`).trim() !== runtimeManifest.steamClientProbe.data.sha256) {
    if (!ensureSmokeDirectory(home)) throw new Error('Cannot create external Steam data directory');
    const archive = `${runtime}/steam-client-data.zip`;
    if (await ConfigurationFiles.fileHash(archive) !== runtimeManifest.steamClientProbe.data.sha256) throw new Error('Steam data checksum failed');
    await zlib.decompressFile(archive, home, {});
    if (!fs.accessSync(`${home}/steamui/index.html`) || !fs.accessSync(`${home}/steamrtarm64/icudtl.dat`)) {
      throw new Error('External Steam resources are incomplete');
    }
    writeSmokeText(`${home}/.packaged-data-version`, runtimeManifest.steamClientProbe.data.sha256);
  }
  const xkb = `${root}/xkb-data`;
  const xlocale = `${root}/x11-locale`;
  if (readSmokeText(`${xkb}/.data-version`).trim() !== runtimeManifest.xwaylandProbe.data.sha256 ||
    !fs.accessSync(`${xkb}/rules/evdev`) || !fs.accessSync(`${xlocale}/C/XLC_LOCALE`)) {
    if (!ensureSmokeDirectory(xkb)) throw new Error('Cannot create external XKB data directory');
    const archive = `${runtime}/xkb-data.zip`;
    if (await ConfigurationFiles.fileHash(archive) !== runtimeManifest.xwaylandProbe.data.sha256) throw new Error('XKB checksum failed');
    // The historical archive contains xkb-data/ and the standard x11-locale/.
    await zlib.decompressFile(archive, root, {});
    if (!fs.accessSync(`${xkb}/rules/evdev`)) throw new Error('External XKB resources are incomplete');
    if (!fs.accessSync(`${xlocale}/locale.dir`) || !fs.accessSync(`${xlocale}/C/XLC_LOCALE`)) {
      throw new Error('External Xlib locale resources are incomplete');
    }
    writeSmokeText(`${xkb}/.data-version`, runtimeManifest.xwaylandProbe.data.sha256);
  }
  try {
    await prepareHmosDeckLibrary(root, runtime);
  } catch (error) {
    writeSmokeText(`${BASE}/single-library-error.json`, JSON.stringify({ message: (error as Error).message }));
    throw new Error((error as Error).message);
  }
  writeSmokeText(`${root}/active-version.txt`, version);
  writeSmokeText(`${BASE}/external-active.path`, release);
  // Retain the active release and the immediately previous one for rollback.
  if (previousVersion !== version) {
    await reclaimLinuxSteamReleases(root, version, previousVersion, false);
  }
  // After activation reclaim only the known HDC staging files.
  const activated = releaseManifest;
  let stagedFilesRemaining = 0;
  for (const file of activated.files) {
    const staged = `${IMPORT}/${file.path}`;
    try { if (fs.accessSync(staged)) fs.unlinkSync(staged); } catch (_) { stagedFilesRemaining++; }
  }
  if (stagedFilesRemaining === 0) { try { if (fs.accessSync(sourceManifest)) fs.unlinkSync(sourceManifest); } catch (_) {} }
  writeSmokeText(`${BASE}/import-progress.json`, JSON.stringify({ phase: 'ready', version, root, stagedFilesRemaining }));
}

// The current ARM64 client initializes Proton Cloud known-folder suffixes only
// when the registered tool ID contains "proton" (not just its layer metadata).
export function migrateHmosDeckToolReferences(text: string): string {
  return text.replace(/("name"\s+)"hmosdeck"/g, '$1"hmosdeck_proton"');
}

function migrateHmosDeckToolSelections(steam: string): void {
  const paths = [`${steam}/config/config.vdf`];
  try {
    const accounts = fs.listFileSync(`${steam}/userdata`);
    for (const account of accounts.slice(0, 64)) {
      if (/^[0-9]{1,12}$/.test(account)) paths.push(`${steam}/userdata/${account}/config/localconfig.vdf`);
    }
  } catch (_) {}
  for (const path of paths) {
    const before = readSmokeText(path);
    const after = migrateHmosDeckToolReferences(before);
    if (before === after) continue;
    const backup = `${path}.before-hmosdeck-proton-v1`;
    if (!fs.accessSync(backup)) {
      writeSmokeText(backup, before);
      if (readSmokeText(backup) !== before) throw new Error('Cannot preserve Linux Steam tool configuration');
    }
    writeSmokeText(path, after);
    if (readSmokeText(path) !== after) throw new Error('Cannot migrate HmosDeck tool selection');
  }
}

// The GNU client cannot use Valve's separate GNU Wine runtime as the OHOS
// compatibility tool. Seed its default and migrate only installed games that
// still select a built-in Proton; preserve explicit third-party tool choices.
export function normalizeHmosDeckToolMappings(text: string, installedApps: string[]): string {
  const document = parseSteamVdf(text);
  let node = document;
  for (const key of ['installconfigstore', 'software', 'valve', 'steam', 'compattoolmapping']) {
    let child = node.child(key);
    if (!child) {
      child = new SteamVdfNode();
      node.children.set(key, child);
    } else if (child.value.length > 0 && child.children.size === 0) {
      throw new Error('Linux Steam 兼容工具配置无效');
    }
    node = child;
  }
  let changed = false;
  const apps = ['0', ...installedApps.filter((id: string): boolean => /^[1-9][0-9]{0,9}$/.test(id))];
  for (const id of apps) {
    let mapping = node.child(id);
    const name = mapping?.text('name') ?? '';
    if (name === 'hmosdeck_proton') continue;
    if (name.length > 0 && name !== 'hmosdeck' && !/^proton(?:_|$)/.test(name)) continue;
    // Non-default empty selections keep their ordinary inherited behavior.
    if (id !== '0' && name.length === 0) continue;
    if (!mapping) {
      mapping = new SteamVdfNode();
      node.children.set(id, mapping);
    }
    mapping.set('name', 'hmosdeck_proton');
    mapping.set('config', '');
    mapping.set('priority', id === '0' ? '75' : '250');
    changed = true;
  }
  return changed ? serializeSteamVdf(document) : text;
}

function prepareHmosDeckToolDefaults(steam: string, apps: string[]): void {
  const path = `${steam}/config/config.vdf`;
  if (!ensureSmokeDirectory(`${steam}/config`)) throw new Error('Cannot prepare Linux Steam configuration');
  const before = fs.accessSync(path) ? fs.readTextSync(path, { encoding: 'utf-8' }) : '';
  const after = normalizeHmosDeckToolMappings(before, apps);
  if (before === after) return;
  const backup = `${path}.before-hmosdeck-default-v1`;
  if (before.length > 0 && !fs.accessSync(backup)) {
    writeSmokeText(backup, before);
    if (fs.readTextSync(backup, { encoding: 'utf-8' }) !== before) {
      throw new Error('Cannot preserve Linux Steam default-tool configuration');
    }
  }
  const temporary = `${path}.hmosdeck-default-write`;
  writeSmokeText(temporary, after);
  if (fs.readTextSync(temporary, { encoding: 'utf-8' }) !== after) {
    throw new Error('Cannot verify Linux Steam default-tool configuration');
  }
  fs.renameSync(temporary, path);
}

// Dedicated to native Linux Steam. Never imports the Windows Steam directory.
async function prepareHmosDeckLibrary(root: string, runtime: string): Promise<void> {
  const steam = `${root}/client/.steam/steam`;
  const library = `${root}/library`;
  const common = `${library}/steamapps/common`;
  const tool = `${steam}/compatibilitytools.d/hmosdeck`;
  for (const directory of [common, tool, `${steam}/steamapps`]) {
    if (!ensureSmokeDirectory(directory)) throw new Error('Cannot prepare isolated Linux Steam library');
  }
  copyExternalFile(`${runtime}/compatibilitytools.d/hmosdeck/hmosdeck`, `${tool}/hmosdeck`);
  writeSmokeText(`${tool}/compatibilitytool.vdf`,
    '"compatibilitytools" { "compat_tools" { "hmosdeck_proton" { "install_path" "." ' +
    '"display_name" "HmosDeck (OHOS Wine)" "from_oslist" "windows" "to_oslist" "linux" } } }');
  writeSmokeText(`${tool}/toolmanifest.vdf`,
    '"manifest" { "version" "2" "commandline" "/hmosdeck %verb%" "compatmanager_layer_name" "proton" }');
  migrateHmosDeckToolSelections(steam);
  const migration = await prepareSingleLinuxSteamLibrary(root);
  prepareHmosDeckToolDefaults(steam, migration.installedApps);
  writeSmokeText(`${BASE}/single-library-migration.json`, JSON.stringify(migration));
  writeSmokeText(`${BASE}/game-library.json`, JSON.stringify({
    source: 'native_aarch64_linux_steam', library, windowsLibraryImported: false,
    tool: 'hmosdeck_proton', steamApiAuthenticationVerified: false
  }));
}
