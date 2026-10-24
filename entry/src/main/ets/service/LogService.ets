import { exportLogZipInBackground } from './LogExportTask';
import { hilog as hilogReal } from '@kit.PerformanceAnalysisKit';
import { bundleManager } from '@kit.AbilityKit';
import { fileIo as fs, fileUri } from '@kit.CoreFileKit';
import { taskpool } from '@kit.ArkTS';
import testNapi from 'libentry.so';

/**
 * 应用日志服务。
 *
 * 导出与真实 hilog 同形的 `hilog` 类，各业务文件仅需把
 * `import { hilog } from '@kit.PerformanceAnalysisKit'` 换成
 * `import { hilog } from './LogService'`（相对路径按目录调整），调用处不变：
 * 真实 hilog 行为保留，同时把同一行格式化后追加到
 * Download/<bundleName>/logs/app-YYYYMMDD[-n].log。
 *
 * 轮转规则：按本地日期分文件，同一天超 20MB 才滚动序号文件，
 * 保留最近 7 天（初始化/滚动时清理过期文件）。
 */

const MAX_FILE_BYTES = 20 * 1024 * 1024;
const RETENTION_DAYS = 7;

function pad2(value: number): string {
  return value < 10 ? '0' + value : '' + value;
}

function localDateKey(): string {
  const d = new Date();
  return '' + d.getFullYear() + pad2(d.getMonth() + 1) + pad2(d.getDate());
}

function localStamp(): string {
  const d = new Date();
  return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) +
    ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds()) +
    '.' + pad2(Math.floor(d.getMilliseconds() / 10));
}

function localStampCompact(): string {
  const d = new Date();
  return '' + d.getFullYear() + pad2(d.getMonth() + 1) + pad2(d.getDate()) +
    '-' + pad2(d.getHours()) + pad2(d.getMinutes()) + pad2(d.getSeconds());
}

export class hilog {
  static debug(domain: number, tag: string, format: string, ...args: (Object | undefined)[]): void {
    hilogReal.debug(domain, tag, format, ...args);
    AppLog.write('D', tag, format, args);
  }

  static info(domain: number, tag: string, format: string, ...args: (Object | undefined)[]): void {
    hilogReal.info(domain, tag, format, ...args);
    AppLog.write('I', tag, format, args);
  }

  static warn(domain: number, tag: string, format: string, ...args: (Object | undefined)[]): void {
    hilogReal.warn(domain, tag, format, ...args);
    AppLog.write('W', tag, format, args);
  }

  static error(domain: number, tag: string, format: string, ...args: (Object | undefined)[]): void {
    hilogReal.error(domain, tag, format, ...args);
    AppLog.write('E', tag, format, args);
  }

  static fatal(domain: number, tag: string, format: string, ...args: (Object | undefined)[]): void {
    hilogReal.fatal(domain, tag, format, ...args);
    AppLog.write('F', tag, format, args);
  }
}

export class AppLog {
  private static logDir: string = '';
  private static bundleName: string = '';
  private static fd: number = -1;
  private static dateKey: string = '';
  private static seq: number = 0;
  private static bytes: number = 0;
  private static inited: boolean = false;
  private static exporting: Promise<string> | null = null;

  static init(): void {
    if (AppLog.inited) return;
    AppLog.inited = true;
    try {
      const bundleInfo = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
      AppLog.bundleName = bundleInfo.name;
      const root = new fileUri.FileUri(
        'file://docs/storage/Users/currentUser/Download/' + bundleInfo.name).path;
      AppLog.logDir = root + '/logs';
      if (!AppLog.ensureDir(root)) {
        throw new Error('cannot create Download bundle dir: ' + root);
      }
      if (!AppLog.ensureDir(AppLog.logDir)) {
        throw new Error('cannot create logs dir: ' + AppLog.logDir);
      }
      testNapi.initAppLog(AppLog.logDir);
      AppLog.cleanupOld();
      AppLog.ensureFd();
      if (AppLog.fd >= 0) {
        const marker = localStamp() + ' [I] [LogSvc] === AppLog init OK dir=' +
          AppLog.logDir + ' ===\n';
        try {
          fs.writeSync(AppLog.fd, marker);
          AppLog.bytes += marker.length;
        } catch (e) {
          hilogReal.error(0x0000, 'LogSvc', 'AppLog marker write failed: %{public}s',
            JSON.stringify(e));
        }
      }
      hilogReal.info(0x0000, 'LogSvc', 'AppLog ready dir=%{public}s fd=%{public}d',
        AppLog.logDir, AppLog.fd);
    } catch (e) {
      AppLog.logDir = '';
      hilogReal.error(0x0000, 'LogSvc', 'AppLog.init failed: %{public}s', JSON.stringify(e));
    }
  }

  /**
   * docs 版 Download 的递归 mkdir 在应用专属目录不存在时会失败（与
   * AppCatalogService 一致），因此用单级 mkdir + stat 校验：
   * 先确保 Download/<bundle>，再确保 logs/。
   */
  private static ensureDir(path: string): boolean {
    try {
      const st = fs.statSync(path);
      return st.isDirectory();
    } catch (e) {
    }
    try {
      fs.mkdirSync(path);
    } catch (e) {
      try {
        const st = fs.statSync(path);
        return st.isDirectory();
      } catch (e2) {
        return false;
      }
    }
    try {
      const st = fs.statSync(path);
      return st.isDirectory();
    } catch (e) {
      return false;
    }
  }

  private static stringifyArg(value: Object | undefined): string {
    if (value === null) return 'null';
    if (value === undefined) return 'undefined';
    const t = typeof value;
    if (t === 'string' || t === 'number' || t === 'boolean') return '' + value;
    try {
      return JSON.stringify(value) ?? 'undefined';
    } catch (e) {
      return String(value);
    }
  }

  private static formatLine(tag: string, format: string, args: (Object | undefined)[]): string {
    let out = '';
    let idx = 0;
    const n = args.length;
    for (let i = 0; i < format.length; i++) {
      const ch = format.charAt(i);
      if (ch !== '%') {
        out += ch;
        continue;
      }
      const next = i + 1 < format.length ? format.charAt(i + 1) : '';
      if (next === '%') {
        out += '%';
        i++;
        continue;
      }
      if (next === '{') {
        const close = format.indexOf('}', i + 2);
        if (close >= 0) {
          const spec = format.substring(i + 2, close);
          if (spec.startsWith('private')) {
            out += '<private>';
            idx++;
          } else {
            out += idx < n ? AppLog.stringifyArg(args[idx++]) : '<missing>';
          }
          // Consume the type as well as the visibility marker, e.g. %{public}s.
          i = close + 1;
          continue;
        }
      }
      out += idx < n ? AppLog.stringifyArg(args[idx++]) : '<missing>';
      i++;
    }
    if (idx < n) {
      let extra = '';
      for (; idx < n; idx++) {
        if (extra.length > 0) extra += ', ';
        extra += AppLog.stringifyArg(args[idx]);
      }
      out += ' | extra=' + extra;
    }
    return out;
  }

  private static currentPath(): string {
    let name = 'app-' + AppLog.dateKey;
    if (AppLog.seq > 0) name += '-' + AppLog.seq;
    name += '.log';
    return AppLog.logDir + '/' + name;
  }

  private static openCurrent(): void {
    if (!AppLog.logDir) return;
    try {
      const file = fs.openSync(AppLog.currentPath(),
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.APPEND);
      AppLog.fd = file.fd;
      const st = fs.statSync(AppLog.fd);
      AppLog.bytes = st.size;
    } catch (e) {
      AppLog.fd = -1;
    }
  }

  private static ensureFd(): void {
    const date = localDateKey();
    if (AppLog.fd >= 0 && AppLog.dateKey === date) return;
    if (AppLog.fd >= 0) {
      try {
        fs.closeSync(AppLog.fd);
      } catch (e) {
      }
      AppLog.fd = -1;
    }
    AppLog.dateKey = date;
    AppLog.seq = 0;
    AppLog.bytes = 0;
    AppLog.openCurrent();
  }

  private static rotate(): void {
    if (AppLog.fd >= 0) {
      try {
        fs.closeSync(AppLog.fd);
      } catch (e) {
      }
      AppLog.fd = -1;
    }
    AppLog.seq += 1;
    AppLog.bytes = 0;
    AppLog.openCurrent();
    AppLog.cleanupOld();
  }

  static write(level: string, tag: string, format: string, args: (Object | undefined)[]): void {
    if (!AppLog.inited || !AppLog.logDir) return;
    try {
      const msg = AppLog.formatLine(tag, format, args);
      const line = localStamp() + ' [' + level + '] [' + tag + '] ' + msg + '\n';
      AppLog.ensureFd();
      if (AppLog.fd < 0) return;
      if (AppLog.bytes > 0 && AppLog.bytes + line.length > MAX_FILE_BYTES) {
        AppLog.rotate();
      }
      if (AppLog.fd >= 0) {
        fs.writeSync(AppLog.fd, line);
        AppLog.bytes += line.length;
      }
    } catch (e) {
      // 日志写入失败不影响业务
    }
  }

  private static cutoffDate(): string {
    const t = new Date(Date.now() - RETENTION_DAYS * 86400000);
    return '' + t.getFullYear() + pad2(t.getMonth() + 1) + pad2(t.getDate());
  }

  private static cleanupOld(): void {
    if (!AppLog.logDir) return;
    try {
      const cutoff = AppLog.cutoffDate();
      const names = fs.listFileSync(AppLog.logDir);
      for (const name of names) {
        const m = /^(app|native)-(\d{8})/.exec(name);
        if (m && m[2] < cutoff) {
          try {
            fs.unlinkSync(AppLog.logDir + '/' + name);
          } catch (e) {
          }
        }
      }
    } catch (e) {
    }
  }

  /**
   * 把 logs 目录里的按日日志（app 与 native 前缀）打包成 zip 放到应用缓存目录，
   * 不污染 logs 目录，随后交给系统分享面板。返回可分享的 file URI。
   */
  static async exportLogsZip(): Promise<string> {
    if (AppLog.exporting) return AppLog.exporting;
    const operation = AppLog.createExportZip();
    AppLog.exporting = operation;
    try { return await operation; } finally { if (AppLog.exporting === operation) AppLog.exporting = null; }
  }

  private static async createExportZip(): Promise<string> {
    AppLog.init();
    if (!AppLog.logDir) {
      throw new Error('log directory unavailable');
    }
    AppLog.ensureFd();
    const zipDir = '/data/storage/el2/base/cache';
    if (!AppLog.ensureDir(zipDir)) {
      throw new Error('cannot create cache dir: ' + zipDir);
    }
    const zipName = 'logs-' + localStampCompact() + '.zip';
    const zipPath = zipDir + '/' + zipName;
    const snapshotDir = zipDir + '/log-export';
    if (!AppLog.ensureDir(snapshotDir)) throw new Error('cannot create log snapshot directory');
    let identity = '';
    try {
      const bundle = bundleManager.getBundleInfoForSelfSync(0);
      identity = `Application=${bundle.name} version=${bundle.versionName} code=${bundle.versionCode}\n`;
    } catch (_) {}
    const downloadRoot = AppLog.logDir.substring(0, AppLog.logDir.lastIndexOf('/'));
    await taskpool.execute(exportLogZipInBackground, AppLog.logDir, snapshotDir, downloadRoot, identity, zipPath);
    AppLog.cleanupOldZips(zipDir);
    hilogReal.info(0x0000, 'LogSvc', 'exportLogsZip OK zip=%{public}s', zipPath);
    return fileUri.getUriFromPath(zipPath);
  }

  private static cleanupOldZips(zipDir: string): void {
    try {
      const names = fs.listFileSync(zipDir);
      const zips: string[] = [];
      for (const name of names) {
        if (name.startsWith('logs-') && name.endsWith('.zip')) zips.push(name);
      }
      zips.sort();
      while (zips.length > 3) {
        const old = zips.shift();
        if (old) {
          try {
            fs.unlinkSync(zipDir + '/' + old);
          } catch (e) {
          }
        }
      }
    } catch (e) {
    }
  }

  static clearLogs(): void {
    AppLog.init();
    if (AppLog.fd >= 0) {
      try {
        fs.closeSync(AppLog.fd);
      } catch (e) {
      }
      AppLog.fd = -1;
    }
    try {
      testNapi.clearNativeLog();
    } catch (e) {
    }
    if (AppLog.logDir) {
      try {
        const names = fs.listFileSync(AppLog.logDir);
        for (const name of names) {
          if (name.startsWith('app-') || name.startsWith('native-') || name.startsWith('export-')) {
            try {
              fs.unlinkSync(AppLog.logDir + '/' + name);
            } catch (e) {
            }
          }
        }
      } catch (e) {
      }
    }
    AppLog.dateKey = '';
    AppLog.seq = 0;
    AppLog.bytes = 0;
  }
}
