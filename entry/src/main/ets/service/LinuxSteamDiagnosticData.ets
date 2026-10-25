import { LinuxGnuProtonComponent } from './LinuxGnuProtonComponent';
import { LINUX_STEAM_PRIVATE_ROOT, LinuxSteamStorageLocation } from './LinuxSteamPaths';
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
interface RuntimeData { sha256: string; kind?: string; essential?: ClientHelperFile[]; }
interface ClientHelperFile { path: string; bytes: number; sha256: string; }
interface ClientHelperData { file: string; sha256: string; files: ClientHelperFile[]; }
interface RuntimeProbe { data: RuntimeData; helpers?: ClientHelperData; }
interface RuntimeManifest { steamClientProbe: RuntimeProbe; xwaylandProbe: RuntimeProbe; }

// DOWNLOAD resolves the app-owned system directory without a selection dialog.
async function resolveLinuxSteamDownloadRoot(context: common.Context): Promise<string> {
  const documentPicker = new picker.DocumentViewPicker(context);
  const choices = await documentPicker.save({ pickerMode: picker.DocumentPickerMode.DOWNLOAD });
  if (!choices || choices.length === 0) throw new Error('Cannot resolve Steam Z drive directory');
  const home = new fileUri.FileUri(choices[0]).path.replace(/\/$/, '');
  return `${home}/SteamLinuxARM64`;
}

function existingExternalRoot(root: string): boolean {
  if (!root.startsWith('/') || !root.endsWith('/SteamLinuxARM64') || root.split('/').includes('..') ||
    /[\x00-\x1f]/.test(root)) return false;
  try {
    const stat = fs.lstatSync(root);
    return stat.isDirectory() && !stat.isSymbolicLink();
  } catch (_) { return false; }
}

export function linuxSteamStorageLocation(): LinuxSteamStorageLocation {
  const root = readSmokeText(`${BASE}/external-root.path`).trim();
  if (root) return root === LINUX_STEAM_PRIVATE_ROOT ? 'private' : 'download';
  return readSmokeText(`${BASE}/storage-location.txt`).trim() === 'download' ? 'download' : 'private';
}

// Discover without installing, moving or resetting data. Legacy public installs
// keep their location; a new install defaults to app-private storage.
export async function recoverLinuxSteamExternalStorage(context: common.Context): Promise<string> {
  const cached = readSmokeText(`${BASE}/external-root.path`).trim();
  if (existingExternalRoot(cached)) return cached;
  const selected = readSmokeText(`${BASE}/storage-location.txt`).trim();
  let root = selected !== 'download' && existingExternalRoot(LINUX_STEAM_PRIVATE_ROOT) ? LINUX_STEAM_PRIVATE_ROOT : '';
  if (!root && selected !== 'private') {
    try { const download = await resolveLinuxSteamDownloadRoot(context); if (existingExternalRoot(download)) root = download; }
    catch (_) {}
  }
  if (root && ensureSmokeDirectory(BASE)) {
    ConfigurationFiles.json(`${BASE}/external-root.path`, root);
    ConfigurationFiles.json(`${BASE}/storage-location.txt`, (root === LINUX_STEAM_PRIVATE_ROOT ? 'private' : 'download') + '\n');
  }
  return root;
}

export async function selectLinuxSteamStorage(context: common.Context, location: LinuxSteamStorageLocation): Promise<void> {
  const root = location === 'private' ? LINUX_STEAM_PRIVATE_ROOT : await resolveLinuxSteamDownloadRoot(context);
  if (!ensureSmokeDirectory(BASE)) throw new Error('无法保存 Steam 安装位置');
  ConfigurationFiles.json(`${BASE}/storage-location.txt`, location + '\n');
  ConfigurationFiles.json(`${BASE}/external-root.path`, root);
}

export async function linuxSteamDownloadHome(context: common.Context): Promise<string> {
  // Keep the ordinary Wine Z: mapping even when native Steam lives on C:.
  // DOWNLOAD is an OS picker service and may return no URI while a desktop
  // transition is in progress. Reuse a previously resolved, accessible home.
  const cached = readSmokeText(`${BASE}/wine-z-root.path`).trim();
  const external = readSmokeText(`${BASE}/external-root.path`).trim();
  const candidates: string[] = [cached];
  if (external !== LINUX_STEAM_PRIVATE_ROOT && existingExternalRoot(external)) {
    candidates.push(external.substring(0, external.length - '/SteamLinuxARM64'.length));
  }
  for (const home of candidates) {
    if (!home.startsWith('/') || home.split('/').includes('..') || /[\x00-\x1f]/.test(home)) continue;
    try {
      const stat = fs.lstatSync(home);
      if (stat.isDirectory() && !stat.isSymbolicLink()) return home;
    } catch (_) {}
  }
  const root = await resolveLinuxSteamDownloadRoot(context);
  const home = root.substring(0, root.length - '/SteamLinuxARM64'.length);
  ConfigurationFiles.json(`${BASE}/wine-z-root.path`, home + '\n');
  return home;
}

export async function prepareLinuxSteamExternalStorage(context: common.Context): Promise<string> {
  let root = readSmokeText(`${BASE}/external-root.path`).trim();
  if (existingExternalRoot(root)) return root;
  if (!root) {
    await selectLinuxSteamStorage(context, linuxSteamStorageLocation());
    root = readSmokeText(`${BASE}/external-root.path`).trim();
  }
  if (!root.startsWith('/') || !root.endsWith('/SteamLinuxARM64') || root.split('/').includes('..')) throw new Error('Steam 安装位置无效');
  if (!ensureSmokeDirectory(root) || !existingExternalRoot(root)) throw new Error('无法创建 Steam 安装目录');
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
export async function prepareLinuxSteamClientData(context: common.Context, diagnosticImport: boolean = true,
  candidateVersion: string = ''): Promise<void> {
  const root = await prepareLinuxSteamExternalStorage(context);
  const sourceManifest = `${IMPORT}/manifest.json`;
  const previousVersion = readSmokeText(`${root}/active-version.txt`).trim();
  let version = previousVersion;
  if (candidateVersion) {
    if (diagnosticImport || !/^[a-f0-9]{64}$/.test(candidateVersion)) throw new Error('Invalid download deployment');
    version = candidateVersion;
  }
  if (diagnosticImport && fs.accessSync(sourceManifest)) {
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
  if (!/^[a-f0-9]{64}$/.test(version)) throw new Error('请先在 Steam 组件页安装或导入运行组件');
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
  const clientData = runtimeManifest.steamClientProbe.data;
  if (clientData.kind === 'installed-resources') {
    if (readSmokeText(`${home}/.packaged-data-version`).trim() !== clientData.sha256 ||
      !clientData.essential || clientData.essential.length < 4 || clientData.essential.length > 16) {
      throw new Error('Steam 界面资源尚未安装完成，请继续安装运行组件');
    }
    for (const file of clientData.essential) {
      if (!file.path || file.path.startsWith('/') || file.path.includes('\\') || file.path.split('/').includes('..') ||
        !/^[a-f0-9]{64}$/.test(file.sha256) || fs.statSync(`${home}/${file.path}`).size !== file.bytes ||
        await ConfigurationFiles.fileHash(`${home}/${file.path}`) !== file.sha256) {
        throw new Error('Steam 关键资源校验失败，请修复运行组件');
      }
    }
  } else if (readSmokeText(`${home}/.packaged-data-version`).trim() !== clientData.sha256) {
    if (!ensureSmokeDirectory(home)) throw new Error('Cannot create external Steam data directory');
    const archive = `${runtime}/steam-client-data.zip`;
    if (await ConfigurationFiles.fileHash(archive) !== runtimeManifest.steamClientProbe.data.sha256) throw new Error('Steam data checksum failed');
    await zlib.decompressFile(archive, home, {});
    if (!fs.accessSync(`${home}/steamui/index.html`) || !fs.accessSync(`${home}/steamrtarm64/icudtl.dat`)) {
      throw new Error('External Steam resources are incomplete');
    }
    writeSmokeText(`${home}/.packaged-data-version`, runtimeManifest.steamClientProbe.data.sha256);
  }
  const helpers = runtimeManifest.steamClientProbe.helpers;
  if (helpers) {
    if (helpers.file !== 'steam-client-helpers.zip' || !/^[a-f0-9]{64}$/.test(helpers.sha256) ||
        helpers.files.length !== 2) throw new Error('Invalid official Linux helper archive');
    const allowed: Set<string> = new Set<string>(['legacycompat/iscriptevaluator.exe', 'bin/d3ddriverquery64.exe']);
    for (const file of helpers.files) {
      if (!allowed.delete(file.path) || !/^[a-f0-9]{64}$/.test(file.sha256) ||
          file.bytes < 1 || file.bytes > 1048576) throw new Error('Invalid official Linux helper');
    }
    if (readSmokeText(`${home}/.packaged-helpers-version`).trim() !== helpers.sha256) {
      const archive = `${runtime}/${helpers.file}`;
      if (await ConfigurationFiles.fileHash(archive) !== helpers.sha256) throw new Error('Linux helper archive checksum failed');
      // This separate archive contains exactly two locked helpers, so adding
      // first-install support does not re-extract account/configuration data.
      await zlib.decompressFile(archive, home, {});
    }
    for (const file of helpers.files) {
      if (fs.statSync(`${home}/${file.path}`).size !== file.bytes ||
          await ConfigurationFiles.fileHash(`${home}/${file.path}`) !== file.sha256) {
        throw new Error(`Official Linux helper checksum failed: ${file.path}`);
      }
    }
    writeSmokeText(`${home}/.packaged-helpers-version`, helpers.sha256);
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
    await LinuxGnuProtonComponent.ensureReady(context);
    await LinuxGnuProtonComponent.ensureReady(context);
    await prepareHmosDeckLibrary(root, runtime);
  } catch (error) {
    writeSmokeText(`${BASE}/single-library-error.json`, JSON.stringify({ message: (error as Error).message }));
    throw new Error((error as Error).message);
  }
  ConfigurationFiles.json(`${root}/active-version.txt`, version);
  ConfigurationFiles.json(`${BASE}/external-active.path`, release);
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

// App manifests include shared runtimes as well as games. Only game entries
// may receive a Windows compatibility override; a tool must stay a Linux tool.
export function isHmosDeckGameManifest(text: string, appId: string): boolean {
  try {
    const app = parseSteamVdf(text).child('appstate');
    if (!app || app.text('appid') !== appId) return false;
    const name = app.text('name').trim();
    const directory = app.text('installdir');
    if (!name || !directory || /[\\/\x00-\x1f]/.test(directory) || directory === '.' || directory === '..') return false;
    const type = app.text('type').toLowerCase();
    if (type && type !== 'game' && type !== 'application') return false;
    return !/^(?:Steam Linux Runtime(?:\b|_)|Steamworks (?:Common Redistributables|Shared)$|Proton(?:\b|_)|FEX(?:\b|-)|Steam Controller Configs\b)/i.test(name) &&
      !/^(?:SteamLinuxRuntime(?:$|_)|Steamworks Shared$|Proton(?:\b|_)|FEX(?:\b|-))/i.test(directory);
  } catch (_) { return false; }
}

// The global default cannot reliably beat per-app Linux mappings at priority
// 100/101. Pin installed games at user-selection priority, while preserving
// explicit native/custom choices. This runs before the GNU client starts.
export function normalizeHmosDeckToolMappings(text: string, installedApps: string[], defaultTool: string = 'hmosdeck_proton', includeDefault: boolean = true): string {
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
  const apps = [...(includeDefault ? ['0'] : []), ...installedApps.filter((id: string): boolean => /^[1-9][0-9]{0,9}$/.test(id))];
  for (const id of apps) {
    let mapping = node.child(id);
    const name = mapping?.text('name') ?? '';
    const minimumPriority = id === '0' ? 100 : 250;
    const priority = Number(mapping?.text('priority') ?? '');
    if (name === 'hmosdeck_proton' || name === 'hmosdeck_gnu_proton' || name === 'hmosdeck_gnu_proton_gl') {
      if (!Number.isFinite(priority) || priority < minimumPriority) {
        mapping?.set('priority', minimumPriority.toString());
        changed = true;
      }
      continue;
    }
    if (name.length > 0 && priority >= 250) continue;
    const automaticNative = id !== '0' && priority < 250 &&
      /^(?:native|SteamLinuxRuntime(?:_.*)?)$/i.test(name);
    if (name.length > 0 && name !== 'hmosdeck' && !/^proton(?:_|$)/.test(name) && !automaticNative) continue;
    if (!mapping) {
      mapping = new SteamVdfNode();
      node.children.set(id, mapping);
    }
    mapping.set('name', defaultTool);
    mapping.set('config', '');
    mapping.set('priority', minimumPriority.toString());
    changed = true;
  }
  return changed ? serializeSteamVdf(document) : text;
}

interface HmosDeckToolInitialization {
  configuration: string;
  initialized: string;
}

// Record only which defaults have been initialized, never the user's choice.
// A cleared checkbox or changed global default remains Steam-owned on restart.
export function initializeHmosDeckToolMappings(text: string, apps: string[], defaultTool: string,
  initialized: string, legacyGnuMigrated: boolean = false): HmosDeckToolInitialization {
  const state = parseSteamVdf(initialized);
  const fresh = apps.filter((id: string): boolean => /^[1-9][0-9]{0,9}$/.test(id) && state.text(`app_${id}`) !== '1');
  const globalReady = state.text('global') === '1';
  let configuration = text;
  if (defaultTool === 'hmosdeck_gnu_proton' && state.text('gnu_default') !== '1') {
    if (!legacyGnuMigrated) {
      const document = parseSteamVdf(configuration);
      const mapping = document.child('installconfigstore')?.child('software')?.child('valve')?.child('steam')?.child('compattoolmapping')?.child('0');
      // Only replace an old product default, never an explicit Steam selection.
      if (mapping && ['hmosdeck', 'hmosdeck_proton'].includes(mapping.text('name')) &&
        Number(mapping.text('priority')) < 250) {
        mapping.set('name', defaultTool);
        configuration = serializeSteamVdf(document);
      }
    }
    state.set('gnu_default', '1');
  }
  configuration = normalizeHmosDeckToolMappings(configuration, fresh, defaultTool, !globalReady);
  state.set('global', '1');
  for (const id of fresh) state.set(`app_${id}`, '1');
  return { configuration, initialized: serializeSteamVdf(state) };
}

function prepareHmosDeckToolDefaults(steam: string, apps: string[], defaultTool: string = 'hmosdeck_proton'): void {
  const path = `${steam}/config/config.vdf`;
  if (!ensureSmokeDirectory(`${steam}/config`)) throw new Error('Cannot prepare Linux Steam configuration');
  const before = fs.accessSync(path) ? fs.readTextSync(path, { encoding: 'utf-8' }) : '';
  const statePath = `${steam}/config/hmosdeck-defaults-initialized.vdf`;
  const stateBefore = fs.accessSync(statePath) ? fs.readTextSync(statePath, { encoding: 'utf-8' }) : '';
  const result = initializeHmosDeckToolMappings(before, apps, defaultTool, stateBefore,
    readSmokeText(`${BASE}/gnu-proton-default-v1`).trim() === '1');
  const after = result.configuration;
  const saveState = (): void => {
    if (stateBefore === result.initialized) return;
    const temporary = `${statePath}.write`;
    writeSmokeText(temporary, result.initialized);
    if (fs.readTextSync(temporary, { encoding: 'utf-8' }) !== result.initialized) throw new Error('Cannot verify Steam default initialization');
    fs.renameSync(temporary, statePath);
  };
  if (before === after) { saveState(); return; }
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
  saveState();
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
  const gnu = `${BASE}/proton-probe`;
  const gnuReady = LinuxGnuProtonComponent.ready();
  if (gnuReady) {
    // Small local native launcher only. No duplicate Steam client or library.
    copyExternalFile(`${gnu}/hmosdeck`, `${tool}/hmosdeck`);
    for (const variant of ['gnu_proton', 'gnu_proton_gl']) {
      const directory = `${steam}/compatibilitytools.d/hmosdeck_${variant}`;
      if (!ensureSmokeDirectory(directory)) throw new Error('Cannot register GNU Proton');
      copyExternalFile(`${gnu}/hmosdeck`, `${directory}/hmosdeck`);
      // GNU exec resolves client helpers into the existing verified code root.
      // Install the matching native helper there as well; client metadata alone
      // cannot make a new executable appear in that runtime namespace.
      const code = `${runtime}/compatibilitytools.d/hmosdeck_${variant}`;
      if (!ensureSmokeDirectory(code)) throw new Error('Cannot prepare GNU Proton launcher');
      copyExternalFile(`${gnu}/hmosdeck`, `${code}/hmosdeck`);
      const flag = variant === 'gnu_proton' ? '--gnu-proton' : '--gnu-proton-gl';
      const label = variant === 'gnu_proton' ? 'HmosDeck (GNU Proton / DXVK)' : 'HmosDeck (GNU Proton / WineD3D)';
      writeSmokeText(`${directory}/compatibilitytool.vdf`,
        `"compatibilitytools" { "compat_tools" { "hmosdeck_${variant}" { "install_path" "." "display_name" "${label}" "from_oslist" "windows" "to_oslist" "linux" } } }`);
      writeSmokeText(`${directory}/toolmanifest.vdf`,
        `"manifest" { "version" "2" "commandline" "/hmosdeck ${flag} %verb%" "compatmanager_layer_name" "proton" }`);
    }

  }
  migrateHmosDeckToolSelections(steam);
  const migration = await prepareSingleLinuxSteamLibrary(root);
  const games = migration.installedApps.filter((id: string): boolean => {
    const path = `${library}/steamapps/appmanifest_${id}.acf`;
    try {
      const info = fs.lstatSync(path);
      return info.isFile() && !info.isSymbolicLink() && info.size <= 1048576 &&
        isHmosDeckGameManifest(fs.readTextSync(path, { encoding: 'utf-8' }), id);
    } catch (_) { return false; }
  });
  prepareHmosDeckToolDefaults(steam, games, gnuReady ? 'hmosdeck_gnu_proton' : 'hmosdeck_proton');
  writeSmokeText(`${BASE}/single-library-migration.json`, JSON.stringify(migration));
  writeSmokeText(`${BASE}/game-library.json`, JSON.stringify({
    source: 'native_aarch64_linux_steam', library, windowsLibraryImported: false,
    tool: gnuReady ? 'hmosdeck_gnu_proton' : 'hmosdeck_proton', steamApiAuthenticationVerified: false
  }));
}
