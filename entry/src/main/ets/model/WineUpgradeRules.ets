export interface WinePrefixUpgradeEvidence {
  bundledVersion: string;
  runtimeVersion: string;
  prefixVersion: string;
  hasExistingPrefix: boolean;
  resetPending: boolean;
  systemMachine: number;
}

export function wineRuntimeField(version: string, key: string): string {
  const fields = version.trim().split(';');
  const prefix = key + '=';
  for (const field of fields) {
    if (field.startsWith(prefix)) return field.substring(prefix.length);
  }
  return '';
}

export function requiresWinePrefixReset(evidence: WinePrefixUpgradeEvidence): boolean {
  if (evidence.resetPending) return true;
  if (!evidence.hasExistingPrefix || !wineRuntimeField(evidence.bundledVersion, 'wine-valve')) return false;

  // The prefix marker survives a failed runtime extraction/update. Prefer it
  // over the extracted runtime marker, which may already describe the new app.
  const previous = evidence.prefixVersion || evidence.runtimeVersion;
  if (wineRuntimeField(previous, 'wine')) return true;
  const currentArch = wineRuntimeField(evidence.bundledVersion, 'arch');
  const previousArch = wineRuntimeField(previous, 'arch');
  if (currentArch && previousArch && currentArch !== previousArch) return true;

  // 1.4.4 used AMD64 system DLLs. Detect leftovers even when an earlier
  // upgrade overwrote both markers before its desktop successfully started.
  return currentArch.endsWith(':aarch64') && evidence.systemMachine === 0x8664;
}
