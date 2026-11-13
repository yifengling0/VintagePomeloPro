import { fileIo as fs } from '@kit.CoreFileKit';
import util from '@ohos.util';
import cryptoFramework from '@ohos.security.cryptoFramework';

export class ConfigurationFiles {
  static ensure(path: string): void {
    if (!fs.accessSync(path)) fs.mkdirSync(path, true);
    if (!fs.lstatSync(path).isDirectory() || fs.lstatSync(path).isSymbolicLink()) throw new Error('CONFIG_UNSAFE_PATH');
  }
  static read(path: string, limit: number = 8388608): Uint8Array {
    if (!fs.lstatSync(path).isFile() || fs.lstatSync(path).isSymbolicLink()) throw new Error('CONFIG_UNSAFE_PATH');
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      const size = fs.statSync(file.fd).size;
      if (size < 0 || size > limit) throw new Error('CONFIG_QUOTA');
      const bytes = new Uint8Array(size); let read = 0;
      while (read < size) { const part = new ArrayBuffer(size - read); const n = fs.readSync(file.fd, part);
        if (n <= 0) throw new Error('CONFIG_READ_FAILED'); bytes.set(new Uint8Array(part, 0, n), read); read += n; }
      return bytes;
    } finally { fs.closeSync(file); }
  }
  static text(bytes: Uint8Array): string { return util.TextDecoder.create('utf-8', { fatal: true }).decodeWithStream(bytes, { stream: false }); }
  static bytes(text: string): Uint8Array { return new util.TextEncoder().encodeInto(text); }
  static write(path: string, bytes: Uint8Array): void {
    const temporary = path + '.tmp-' + util.generateRandomUUID();
    const file = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      let written = 0;
      while (written < bytes.length) { const n = fs.writeSync(file.fd, bytes.buffer.slice(bytes.byteOffset + written, bytes.byteOffset + bytes.length));
        if (n <= 0) throw new Error('CONFIG_WRITE_FAILED'); written += n; }
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
    try { fs.renameSync(temporary, path); } catch (error) { try { fs.unlinkSync(temporary); } catch (_) {} throw new Error((error as Error).message); }
  }
  static json(path: string, text: string): void { ConfigurationFiles.write(path, ConfigurationFiles.bytes(text)); }
  static async hash(bytes: Uint8Array): Promise<string> {
    const md = cryptoFramework.createMd('SHA256'); await md.update({ data: bytes }); const result = await md.digest();
    return Array.from(result.data).map((b: number): string => b.toString(16).padStart(2, '0')).join('');
  }
  static async fileHash(path: string): Promise<string> {
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      const before = fs.statSync(file.fd); if (!before.isFile() || before.size > 536870912) throw new Error('CONFIG_QUOTA');
      const md = cryptoFramework.createMd('SHA256'); const bytes = new ArrayBuffer(65536); let count = 0;
      while (true) { const n = fs.readSync(file.fd, bytes); if (!n) break; count += n; if (count > 536870912) throw new Error('CONFIG_QUOTA');
        await md.update({ data: new Uint8Array(bytes.slice(0, n)) }); }
      const after = fs.statSync(file.fd); if (count !== before.size || before.size !== after.size || before.mtime !== after.mtime) throw new Error('CONFIG_CHANGED');
      const result = await md.digest(); return Array.from(result.data).map((b: number): string => b.toString(16).padStart(2, '0')).join('');
    } finally { fs.closeSync(file); }
  }
}
