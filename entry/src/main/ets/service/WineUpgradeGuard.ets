import { fileIo as fs } from '@kit.CoreFileKit';
import { requiresWinePrefixReset } from '../model/WineUpgradeRules';

const FILES_BASE = '/data/storage/el2/base/files';
const PREFIX_DIR = FILES_BASE + '/.wine';
// Keep this outside both trees: runtime extraction and an interrupted prefix
// reset must not silently dismiss the upgrade requirement.
const RESET_REQUIRED_MARKER = FILES_BASE + '/.winehua-upgrade-reset-required';
const RESET_REQUIREMENT = 'legacy-wine-to-proton-v1';

export const WINE_UPGRADE_RESET_MESSAGE =
  '检测到 1.4.4 等旧版本遗留的 Wine 环境，与当前 Proton/Wine 运行环境不兼容。' +
  '请先进入「系统设置 → Wine 引擎」，执行「重置 Wine prefix」，完成后再启动桌面。' +
  '\n\n如果重置后仍卡住，请先备份重要数据，再卸载本应用并重新安装。卸载会清除应用内的数据。';

function exists(path: string): boolean {
  return fs.accessSync(path);
}

function readMarker(path: string): string {
  return exists(path) ? fs.readTextSync(path).trim() : '';
}

function runtimeText(bytes: Uint8Array): string {
  let text = '';
  for (let index = 0; index < bytes.byteLength; index++) text += String.fromCharCode(bytes[index]);
  return text.trim();
}

function prefixSystemMachine(): number {
  const path = PREFIX_DIR + '/drive_c/windows/system32/ntdll.dll';
  if (!exists(path)) return 0;
  let input: fs.File | null = null;
  try {
    input = fs.openSync(path, fs.OpenMode.READ_ONLY);
    const header = new ArrayBuffer(64);
    if (fs.readSync(input.fd, header, { length: 64 }) !== 64) return 0;
    const dos = new DataView(header);
    if (dos.getUint16(0, true) !== 0x5a4d) return 0;
    const offset = dos.getUint32(60, true);
    if (offset < 64 || offset > fs.statSync(path).size - 6) return 0;
    const peHeader = new ArrayBuffer(6);
    if (fs.readSync(input.fd, peHeader, { offset: offset, length: 6 }) !== 6) return 0;
    const pe = new DataView(peHeader);
    return pe.getUint32(0, true) === 0x00004550 ? pe.getUint16(4, true) : 0;
  } finally {
    if (input) fs.closeSync(input);
  }
}

export class WineUpgradeGuard {
  needsReset(bundledVersion: Uint8Array): boolean {
    const pending = exists(RESET_REQUIRED_MARKER);
    const required = requiresWinePrefixReset({
      bundledVersion: runtimeText(bundledVersion),
      runtimeVersion: readMarker(FILES_BASE + '/wine/.winehua-runtime-version'),
      prefixVersion: readMarker(PREFIX_DIR + '/.winehua-prefix-runtime-version'),
      hasExistingPrefix: exists(PREFIX_DIR + '/system.reg') ||
        exists(PREFIX_DIR + '/drive_c/windows/system32/kernel32.dll'),
      resetPending: pending,
      systemMachine: prefixSystemMachine()
    });
    if (required && !pending) {
      const temporary = RESET_REQUIRED_MARKER + '.new';
      const output = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      try {
        if (fs.writeSync(output.fd, RESET_REQUIREMENT) !== RESET_REQUIREMENT.length) {
          throw new Error('Unable to persist Wine upgrade reset requirement');
        }
      } finally {
        fs.closeSync(output);
      }
      fs.renameSync(temporary, RESET_REQUIRED_MARKER);
    }
    return required;
  }

  completeReset(): void {
    if (exists(RESET_REQUIRED_MARKER)) fs.unlinkSync(RESET_REQUIRED_MARKER);
  }
}
