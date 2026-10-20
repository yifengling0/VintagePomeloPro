import { http } from '@kit.NetworkKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { LinuxSteamDownloadArchive } from '../model/LinuxSteamDownloadModels';
import { ConfigurationFiles } from './ConfigurationFiles';

export class LinuxSteamDownloadError extends Error {
  code: string;
  constructor(code: string, message: string) { super(message); this.code = code; }
}
export type LinuxSteamByteProgress = (received: number) => void;

// One stream at a time. Range data is appended only after validating its range;
// a server that ignores Range restarts just that archive, never appends a 200.
export class LinuxSteamComponentDownload {
  private request: http.HttpRequest | null = null;
  private stopped: boolean = false;
  private cancelWait: (() => void) | null = null;
  pause(): void { this.stopped = true; this.request?.destroy(); this.cancelWait?.(); }
  private check(): void { if (this.stopped) throw new LinuxSteamDownloadError('PAUSED', '下载已暂停，可继续'); }
  async fetch(archive: LinuxSteamDownloadArchive, destination: string, progress: LinuxSteamByteProgress,
    allowNetwork: boolean = true): Promise<void> {
    this.check();
    if (fs.accessSync(destination) && fs.statSync(destination).size === archive.bytes &&
      await ConfigurationFiles.fileHash(destination) === archive.sha256) { progress(archive.bytes); return; }
    if (!allowNetwork) throw new LinuxSteamDownloadError('LOCAL_CACHE', '本地包中的组件不可用，请重新导入');
    if (fs.accessSync(destination)) fs.unlinkSync(destination);
    const partial = destination + '.part';
    const layout = partial + '.append-v1';
    let compatible = false;
    try { compatible = ConfigurationFiles.text(ConfigurationFiles.read(layout, 128)) === archive.sha256; } catch (_) {}
    if (!compatible) {
      if (fs.accessSync(partial)) fs.unlinkSync(partial);
      ConfigurationFiles.json(layout, archive.sha256);
    }
    for (let attempt = 0; attempt < 3; attempt++) {
      this.check();
      try {
        let offset = fs.accessSync(partial) ? fs.statSync(partial).size : 0;
        if (offset > archive.bytes) { fs.unlinkSync(partial); offset = 0; }
        while (offset < archive.bytes) {
          await this.stream(archive, partial, offset, progress, attempt);
          this.check();
          const next = fs.statSync(partial).size;
          if (next <= offset) throw new LinuxSteamDownloadError('INCOMPLETE', '下载未取得新内容，请继续');
          offset = next;
        }
        this.check();
        if (fs.statSync(partial).size !== archive.bytes) throw new LinuxSteamDownloadError('INCOMPLETE', '网络中断，组件未下载完整');
        if (await ConfigurationFiles.fileHash(partial) !== archive.sha256) {
          fs.unlinkSync(partial);
          throw new LinuxSteamDownloadError('CHECKSUM', '组件校验失败，将重新下载该组件');
        }
        fs.renameSync(partial, destination);
        if (fs.accessSync(layout)) fs.unlinkSync(layout);
        progress(archive.bytes); return;
      } catch (error) {
        this.check();
        const failure = error as LinuxSteamDownloadError;
        if (failure.code === 'HTTP_416' && fs.accessSync(partial)) fs.unlinkSync(partial);
        if (attempt === 2 || ['DISK', 'HTTP_403', 'HTTP_404', 'HTTP_410'].includes(failure.code)) throw error as Error;
        await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 500 * (attempt + 1)); });
      }
    }
  }
  private async stream(archive: LinuxSteamDownloadArchive, path: string, offset: number,
    progress: LinuxSteamByteProgress, attempt: number): Promise<void> {
    const request = http.createHttp(); this.request = request;
    const output = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.APPEND | fs.OpenMode.NOFOLLOW);
    let position = offset; let headerReady = false; let reset = false; let failure: LinuxSteamDownloadError | null = null;
    let expectedEnd = archive.bytes;
    const rangeEnd = Math.min(offset + 4194304, archive.bytes) - 1;
    let slowTimer = -1;
    let writeMillis = 0; let chunks = 0; let diagnosticAt = Date.now();
    const started = Date.now();
    const diagnostic = (): void => {
      try { ConfigurationFiles.json('/data/storage/el2/base/files/steamohos-native/component-transfer.json',
        JSON.stringify({ archive: archive.id, source: attempt, offset, received: position - offset,
          elapsed: Date.now() - started, writeMillis, chunks })); } catch (_) {}
    };
    let ended = false;
    let finish: (() => void) | null = null;
    const completion = new Promise<void>((resolve: () => void): void => { finish = resolve; });
    let interrupt: ((value: number) => void) | null = null;
    const interruption = new Promise<number>((resolve: (value: number) => void): void => { interrupt = resolve; });
    const end = (): void => { ended = true; finish?.(); };
    this.cancelWait = (): void => { finish?.(); interrupt?.(-1); };
    const fail = (code: string, message: string): void => {
      if (!failure) failure = new LinuxSteamDownloadError(code, message);
      request.destroy();
      finish?.();
      interrupt?.(-1);
    };
    const headers = (value: Object): void => {
      const record = value as Record<string, string>;
      let range = ''; let line = ''; let length = '';
      Object.keys(record).forEach((key: string): void => {
        if (key.toLowerCase() === 'content-range') range = String(record[key]);
        if (key.toLowerCase() === 'status-line') line = String(record[key]);
        if (key.toLowerCase() === 'content-length') length = String(record[key]);
      });
      const status = /\s([0-9]{3})(?:\s|$)/.exec(line);
      // Some OHOS releases omit status-line from headersReceive. For this
      // pinned CDN, only a matching Content-Range/full Content-Length may
      // authorize writing before requestInStream supplies the real status.
      // An error body with unknown status is discarded, preserving .part.
      const code = status ? Number(status[1]) : range ? 206 : Number(length) === archive.bytes ? 200 : 0;
      if (code === 0) { headerReady = false; return; }
      // Redirect headers may be delivered too; their body is discarded.
      if (code >= 300 && code < 400) { headerReady = false; return; }
      if (code !== 200 && code !== 206) { fail(`HTTP_${code}`, `下载服务返回 HTTP ${code}`); return; }
      if (code === 206) {
        const match = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(range);
        if (!match || Number(match[1]) !== offset || Number(match[3]) !== archive.bytes ||
          Number(match[2]) < offset || Number(match[2]) >= archive.bytes) {
          fail('RANGE', '下载服务器续传响应不正确，请重试'); return;
        }
        expectedEnd = Number(match[2]) + 1;
      } else if (offset > 0) {
        fs.truncateSync(output.fd, 0); position = 0; reset = true;
      }
      headerReady = true;
      if (attempt < 2 && slowTimer < 0) slowTimer = setTimeout((): void => {
        if (position - (reset ? 0 : offset) < 131072 && !ended) fail('SLOW', '当前官方线路较慢，正在尝试另一条官方线路');
      }, 8000);
    };
    const data = (bytes: ArrayBuffer): void => {
      if (failure || this.stopped) return;
      if (!headerReady) return;
      if (position + bytes.byteLength > archive.bytes) { fail('SIZE', '组件大小与清单不符'); return; }
      try {
        const before = Date.now();
        let sent = 0;
        while (sent < bytes.byteLength) {
          // APPEND keeps OHOS's relative WriteOptions.offset semantics out of
          // resumable writes. Truncating on an ignored Range also resets EOF.
          const written = fs.writeSync(output.fd, bytes.slice(sent));
          if (written <= 0) throw new Error('short write');
          sent += written; position += written;
        }
        writeMillis += Date.now() - before; chunks++;
        if (Date.now() - diagnosticAt >= 5000) { diagnosticAt = Date.now(); diagnostic(); }
        progress(position);
      } catch (_) { fail('DISK', '无法写入下载文件，请检查剩余存储空间和目录权限'); }
    };
    try {
      request.on('headersReceive', headers); request.on('dataReceive', data); request.on('dataEnd', end);
      const header: Record<string, string> = { 'accept-encoding': 'identity' };
      if (archive.bytes > 4194304) header['range'] = `bytes=${offset}-${rangeEnd}`;
      else if (offset > 0) header['range'] = `bytes=${offset}-`;
      const bases = ['https://client-update.steamstatic.com/', 'https://media.steampowered.com/client/', 'https://cdn.akamai.steamstatic.com/client/'];
      const url = bases[attempt] + archive.url.substring(archive.url.lastIndexOf('/') + 1);
      const code = await Promise.race([interruption, request.requestInStream(url, { method: http.RequestMethod.GET, header,
        connectTimeout: 20000, readTimeout: 120000, usingCache: false })]);
      this.check();
      if (failure) throw failure as LinuxSteamDownloadError;
      if (code !== 200 && code !== 206) throw new LinuxSteamDownloadError(`HTTP_${code}`, `下载服务返回 HTTP ${code}`);
      if (!ended) {
        let timeout = -1;
        try {
          await Promise.race([completion, new Promise<void>((resolve: () => void): void => {
            timeout = setTimeout(resolve, 120000);
          })]);
        } finally { if (timeout >= 0) clearTimeout(timeout); }
      }
      this.check(); if (failure) throw failure as LinuxSteamDownloadError;
      if (!ended || !headerReady) throw new LinuxSteamDownloadError('INCOMPLETE', '组件传输未结束，请继续下载');
      if (offset > 0 && code === 200 && !reset) throw new LinuxSteamDownloadError('RANGE', '下载服务器不支持续传，请重试');
      if (position !== expectedEnd) throw new LinuxSteamDownloadError('INCOMPLETE', '组件未下载完整，重试将继续下载');
      fs.fsyncSync(output.fd);
    } catch (error) {
      if (failure) throw failure as LinuxSteamDownloadError;
      this.check();
      const problem = error as LinuxSteamDownloadError;
      if ((error as Error) instanceof LinuxSteamDownloadError) throw error as LinuxSteamDownloadError;
      const code = String(problem.code || 'NETWORK');
      throw new LinuxSteamDownloadError(code, `网络连接失败（${code}），请检查网络后继续`);
    } finally {
      diagnostic();
      if (slowTimer >= 0) clearTimeout(slowTimer);
      this.cancelWait = null;
      request.off('headersReceive', headers); request.off('dataReceive', data); request.off('dataEnd', end); request.destroy();
      fs.closeSync(output); if (this.request === request) this.request = null;
    }
  }
}
