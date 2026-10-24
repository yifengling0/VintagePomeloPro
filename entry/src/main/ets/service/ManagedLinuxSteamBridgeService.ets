import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';

interface BridgeModule { path: string; bytes: number; sha256: string; }
interface BridgeManifest { schemaVersion: number; files: BridgeModule[]; }
interface CompatibilityModule { name: string; bytes: number; sha256: string; }
interface CompatibilityManifest { schemaVersion: number; files: CompatibilityModule[]; }

// Called only while preparing Wine, before any server/worker is started.
// Wineboot does not copy ordinary MinGW lsteamclient DLLs into an existing prefix.
export class ManagedLinuxSteamBridgeService {
  static async synchronize(runtime: string, prefix: string): Promise<void> {
    const marker = `${runtime}/linux-steam-bridge.json`;
    if (!fs.accessSync(marker)) return; // Windows-only/older runtimes.
    const data = JSON.parse(ConfigurationFiles.text(ConfigurationFiles.read(marker, 4096))) as BridgeManifest;
    const paths: string[] = ['bin/i386-windows/kernelbase.dll', 'bin/i386-windows/lsteamclient.dll',
      'bin/aarch64-windows/kernelbase.dll', 'bin/aarch64-windows/lsteamclient.dll'];
    if (data.schemaVersion !== 1 || !Array.isArray(data.files) || data.files.length !== paths.length) {
      throw new Error('Linux Steam 接口桥清单不完整，请更新组件');
    }
    // Verify the complete runtime pair before modifying the prefix.
    const modules: BridgeModule[] = [];
    for (const path of paths) {
      const matches = data.files.filter((item: BridgeModule): boolean => item.path === path);
      if (matches.length !== 1) throw new Error('Linux Steam 接口桥清单路径无效');
      const item = matches[0];
      if (!Number.isSafeInteger(item.bytes) || item.bytes < 1024 || item.bytes > 16777216 ||
        !/^[0-9a-f]{64}$/.test(item.sha256) || !fs.accessSync(`${runtime}/${path}`) ||
        fs.statSync(`${runtime}/${path}`).size !== item.bytes ||
        await ConfigurationFiles.fileHash(`${runtime}/${path}`) !== item.sha256) {
        throw new Error('Linux Steam 接口桥组件校验失败，请重新安装组件');
      }
      modules.push(item);
    }
    const pending: string[] = [];
    const targets: string[] = [];
    try {
      for (const item of modules) {
        const bits = item.path.includes('/i386-windows/') ? 'syswow64' : 'system32';
        const name = item.path.substring(item.path.lastIndexOf('/') + 1);
        const directory = `${prefix}/drive_c/windows/${bits}`;
        const target = `${directory}/${name}`;
        if (fs.accessSync(target) && fs.statSync(target).size === item.bytes &&
          await ConfigurationFiles.fileHash(target) === item.sha256) continue;
        if (name === 'kernelbase.dll' && fs.accessSync(target) && !ManagedLinuxSteamBridgeService.wineBuiltin(target)) {
          throw new Error('检测到自定义 kernelbase.dll，无法安全更新 Steam 接口桥');
        }
        if (!fs.accessSync(directory)) fs.mkdirSync(directory, true);
        const temporary = `${target}.winehua-steam-update`;
        pending.push(temporary); targets.push(target);
        fs.copyFileSync(`${runtime}/${item.path}`, temporary);
        if (await ConfigurationFiles.fileHash(temporary) !== item.sha256) {
          throw new Error('Linux Steam 接口桥复制不完整');
        }
      }
      // An interrupted rename is repaired on the next preparation; Wine is
      // never started with only part of this validated set installed.
      for (let i = 0; i < pending.length; ++i) fs.renameSync(pending[i], targets[i]);
    } finally {
      for (const path of pending) if (fs.accessSync(path)) fs.unlinkSync(path);
    }
  }

  // Proton's native PE module layout is separate from the actual Linux
  // backend. Only pinned modules shipped by the Linux client enter this view.
  static async synchronizeCompatibility(context: common.Context, client: string, prefix: string): Promise<void> {
    const raw = await context.resourceManager.getRawFileContent('linux-steam/client-compat.json');
    const manifest = JSON.parse(ConfigurationFiles.text(raw)) as CompatibilityManifest;
    const allowed: string[] = ['steamclient.dll', 'steamclient64.dll', 'Steam.dll'];
    if (manifest.schemaVersion !== 1 || manifest.files.length !== allowed.length ||
      allowed.some((name: string): boolean => manifest.files.filter((item: CompatibilityModule): boolean => item.name === name).length !== 1)) {
      throw new Error('Linux Steam 兼容模块清单无效');
    }
    const directory = `${prefix}/drive_c/windows/hmosdeck-steam`;
    if (!fs.accessSync(directory)) fs.mkdirSync(directory, true);
    if (!fs.lstatSync(directory).isDirectory() || fs.lstatSync(directory).isSymbolicLink()) throw new Error('Steam 兼容目录无效');
    const pending: string[] = []; const targets: string[] = [];
    try {
      for (const item of manifest.files) {
        const source = `${client}/steamohos/compat/${item.name}`;
        if (!Number.isSafeInteger(item.bytes) || item.bytes < 1024 || item.bytes > 33554432 ||
          !/^[0-9a-f]{64}$/.test(item.sha256) || !fs.accessSync(source) ||
          !fs.lstatSync(source).isFile() || fs.lstatSync(source).isSymbolicLink() ||
          fs.statSync(source).size !== item.bytes || await ConfigurationFiles.fileHash(source) !== item.sha256) {
          throw new Error('Linux Steam 兼容模块校验失败，请修复组件');
        }
        const target = `${directory}/${item.name}`;
        if (fs.accessSync(target)) {
          if (!fs.lstatSync(target).isFile() || fs.lstatSync(target).isSymbolicLink()) throw new Error('Steam 兼容模块目标无效');
          if (fs.statSync(target).size === item.bytes && await ConfigurationFiles.fileHash(target) === item.sha256) continue;
        }
        const temporary = `${target}.winehua-steam-update`;
        // A stale temporary file is reusable; a link must never be followed.
        try {
          const stat = fs.lstatSync(temporary);
          if (!stat.isFile() || stat.isSymbolicLink()) throw new Error('Steam 兼容模块临时文件无效');
        } catch (error) {
          if (fs.accessSync(temporary) || (error as Error).message === 'Steam 兼容模块临时文件无效') {
            throw new Error((error as Error).message);
          }
        }
        pending.push(temporary); targets.push(target);
        fs.copyFileSync(source, temporary);
        if (await ConfigurationFiles.fileHash(temporary) !== item.sha256) throw new Error('Steam 兼容模块复制不完整');
      }
      for (let i = 0; i < pending.length; ++i) fs.renameSync(pending[i], targets[i]);
    } finally {
      for (const path of pending) if (fs.accessSync(path)) fs.unlinkSync(path);
    }
  }

  private static wineBuiltin(path: string): boolean {
    const input = fs.openSync(path, fs.OpenMode.READ_ONLY);
    try {
      const header = new ArrayBuffer(80);
      if (fs.readSync(input.fd, header, { offset: 0, length: 80 }) !== 80) return false;
      const bytes = new Uint8Array(header); const signature = 'Wine builtin DLL';
      for (let i = 0; i < signature.length; ++i) if (bytes[64 + i] !== signature.charCodeAt(i)) return false;
      return true;
    } finally { fs.closeSync(input); }
  }
}
