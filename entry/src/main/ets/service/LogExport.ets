import { fileIo as fs } from '@kit.CoreFileKit';
import util from '@ohos.util';
import { ConfigurationFiles } from './ConfigurationFiles';
import { collectSteamStartupDiagnostics, diagnosticNames, readDiagnosticTail, redactSteamDiagnostic } from './SteamStartupDiagnostics';

const APP_LOG_NAME = /^(?:app|native)-\d{8}(?:-\d+)?\.log$/;
const APP_BUDGET = 16 * 1024 * 1024;
const FILE_LIMIT = 2 * 1024 * 1024;

export function cleanupLogExportSnapshots(directory: string): void {
  // Only our own fixed snapshot names. Never delete source logs or arbitrary
  // cache contents, including exports still owned by the system share sheet.
  for (const name of diagnosticNames(directory,
    /^(?:(?:app|native)-\d{8}(?:-\d+)?\.log|steam-startup-diagnostics\.log|export-manifest\.log)(?:\.tmp-[a-f0-9-]{36})?$/i, 64)) {
    try { fs.unlinkSync(`${directory}/${name}`); } catch (_) {}
  }
}

export async function createLogExportSnapshots(logDir: string, directory: string,
  downloadRoot: string, identity: string): Promise<string[]> {
  cleanupLogExportSnapshots(directory);
  const files: string[] = [];
  let manifest = redactSteamDiagnostic(identity) + '\nCollected=' + new Date().toISOString() +
    '\nExport v3: app/native newest first, max 16 files / 2 MiB per file / 16 MiB total;\n' +
    'Wine, DXVK, Windows/Linux Steam and bridge diagnostics: bounded tails, max 8 MiB.\n' +
    'offset/truncated in each source records omitted older bytes. Source logs are unchanged.\n' +
    'Account credentials and URL queries are redacted. Game data, saves, raw configs and browser profiles are excluded.\n';
  let remaining = APP_BUDGET;
  // Fetch one extra entry so omission is visible. Enumeration is shallow.
  const names = diagnosticNames(logDir, APP_LOG_NAME, 17);
  for (let i = 0; i < names.length; i++) {
    const name = names[i];
    if (i >= 16 || remaining < 4096) { manifest += `${name}: omitted (export size/file limit)\n`; continue; }
    try {
      const text = readDiagnosticTail(`${logDir}/${name}`, Math.min(FILE_LIMIT, Math.floor(remaining / 4)));
      const bytes = new util.TextEncoder().encodeInto(text);
      ConfigurationFiles.write(`${directory}/${name}`, bytes);
      remaining -= bytes.byteLength;
      files.push(`${directory}/${name}`);
      manifest += `${name}: included ${bytes.byteLength} bytes\n`;
    } catch (_) { manifest += `${name}: unavailable (missing, unreadable or unsafe file)\n`; }
    // Let the export button/progress state render between bounded reads.
    await new Promise<void>((resolve): void => { setTimeout(resolve, 0); });
  }
  manifest += `App/native snapshot bytes=${APP_BUDGET - remaining}\n`;
  try {
    const diagnostics = await collectSteamStartupDiagnostics(downloadRoot);
    if (diagnostics.length > 0) {
      const path = `${directory}/steam-startup-diagnostics.log`;
      ConfigurationFiles.json(path, redactSteamDiagnostic(identity) + diagnostics);
      files.push(path);
      manifest += 'Wine/Steam diagnostics: included\n';
    } else { manifest += 'Wine/Steam diagnostics: no existing diagnostic files\n'; }
  } catch (_) { manifest += 'Wine/Steam diagnostics: collection unavailable\n'; }
  const manifestPath = `${directory}/export-manifest.log`;
  ConfigurationFiles.json(manifestPath, manifest);
  files.push(manifestPath);
  return files;
}
