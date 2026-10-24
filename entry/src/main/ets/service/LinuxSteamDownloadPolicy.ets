import { fileIo as fs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';
import { ensureSmokeDirectory, readSmokeText } from './ManagedSmokePayloadService';

const BASE = '/data/storage/el2/base/files/steamohos-native';
const SETTING = `${BASE}/download-connection.txt`;
const BEGIN = '// BEGIN HMOSDECK DOWNLOAD CONNECTION\n';
const END = '// END HMOSDECK DOWNLOAD CONNECTION\n';
export function linuxSteamDownloadHttp1(): boolean { return readSmokeText(SETTING).trim() === 'http1'; }
export function setLinuxSteamDownloadHttp1(enabled: boolean): void {
  if (!ensureSmokeDirectory(BASE)) throw new Error('无法保存下载连接设置');
  ConfigurationFiles.json(SETTING, enabled ? 'http1' : 'default');
}
export function linuxSteamDownloadConfig(text: string, http1: boolean): string {
  const begin = text.indexOf(BEGIN);
  if (begin >= 0) {
    const end = text.indexOf(END, begin + BEGIN.length);
    if (end < 0 || text.indexOf(BEGIN, begin + BEGIN.length) >= 0) throw new Error('Steam 下载配置标记不完整，未修改原配置');
    text = text.substring(0, begin) + text.substring(end + END.length);
  }
  if (!http1) return text;
  return text + (text.length > 0 && !text.endsWith('\n') ? '\n' : '') + BEGIN +
    '@nClientDownloadEnableHTTP2PlatformLinux 0\n' + END;
}
export function applyLinuxSteamDownloadPolicy(root: string): void {
  const file = `${root}/client/.steam/steam/steam_dev.cfg`;
  let before = '';
  if (fs.accessSync(file)) {
    const stat = fs.lstatSync(file);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 65536) throw new Error('Steam 下载配置不可安全更新');
    before = ConfigurationFiles.text(ConfigurationFiles.read(file, 65536));
  }
  const after = linuxSteamDownloadConfig(before, linuxSteamDownloadHttp1());
  if (before === after) return;
  const pending = file + '.hmosdeck-pending';
  ConfigurationFiles.json(pending, after || '\n');
  fs.renameSync(pending, file);
}
