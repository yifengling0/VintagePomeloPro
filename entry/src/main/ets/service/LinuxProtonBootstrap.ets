import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';

interface BootstrapFile { name: string; bytes: number; sha256: string; }
interface BootstrapManifest { schemaVersion: number; architecture: string; files: BootstrapFile[]; }
const ROOT = '/data/storage/el2/base/files/steamohos-native/proton-probe/bootstrap';

// These PE files come from the same locked GE distribution as the base runtime.
// Keep the small launch/service closure independent of the 116 MB component,
// so historical offline imports and installed game libraries need no changes.
export async function ensureLinuxProtonBootstrap(context: common.Context): Promise<void> {
  const raw = 'linux-proton/bootstrap';
  const manifest = JSON.parse(ConfigurationFiles.text(await context.resourceManager.getRawFileContent(`${raw}/manifest.json`))) as BootstrapManifest;
  const required = new Set<string>(['dwmapi.dll', 'dwrite.dll', 'lsass.exe', 'mswsock.dll', 'oleacc.dll',
    'psapi.dll', 'uiautomationcore.dll', 'wevtsvc.dll', 'wsock32.dll']);
  if (manifest.schemaVersion !== 1 || manifest.architecture !== 'aarch64-windows' || manifest.files.length !== required.size) {
    throw new Error('Proton 启动支持清单无效');
  }
  for (const file of manifest.files) {
    if (!required.delete(file.name) || !Number.isSafeInteger(file.bytes) || file.bytes < 1 || file.bytes > 2097152 ||
        !/^[a-f0-9]{64}$/.test(file.sha256)) throw new Error('Proton 启动支持文件无效');
  }
  ConfigurationFiles.ensure(ROOT);
  for (const file of manifest.files) {
    const path = `${ROOT}/${file.name}`;
    if (fs.accessSync(path)) {
      const stat = fs.lstatSync(path);
      if (!stat.isFile() || stat.isSymbolicLink()) throw new Error('Proton 启动支持路径不安全');
      if (stat.size === file.bytes && await ConfigurationFiles.fileHash(path) === file.sha256) continue;
    }
    const data = await context.resourceManager.getRawFileContent(`${raw}/${file.name}`);
    if (data.byteLength !== file.bytes || await ConfigurationFiles.hash(data) !== file.sha256) {
      throw new Error('Proton 启动支持校验失败，请重新安装应用');
    }
    ConfigurationFiles.write(path, data);
  }
}
