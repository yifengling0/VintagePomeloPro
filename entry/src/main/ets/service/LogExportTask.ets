import { fileIo as fs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { cleanupLogExportSnapshots, createLogExportSnapshots } from './LogExport';

// Only strings cross the task boundary. Files, redaction buffers and ZIP data
// stay on a taskpool worker; no UI/Ability context or logger singleton is sent.
@Concurrent
export async function exportLogZipInBackground(logDir: string, snapshotDir: string,
  downloadRoot: string, identity: string, zipPath: string): Promise<number> {
  const started = Date.now();
  hilog.info(0x0000, 'LogExportTask', 'Background log export started');
  try {
    const files = await createLogExportSnapshots(logDir, snapshotDir, downloadRoot,
      identity + 'Collector=TaskPool background worker\n');
    const options: zlib.Options = {};
    await zlib.compressFiles(files, zipPath, options);
    hilog.info(0x0000, 'LogExportTask', 'Background log export complete files=%{public}d ms=%{public}d',
      files.length, Date.now() - started);
    return files.length;
  } catch (error) {
    try { fs.unlinkSync(zipPath); } catch (_) {}
    throw new Error((error as Error).message || 'log export failed');
  } finally { cleanupLogExportSnapshots(snapshotDir); }
}
