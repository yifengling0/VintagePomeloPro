const COVER_PRIORITY: string[] = [
  'cover.png', 'cover.jpg', 'cover.jpeg', 'cover.webp',
  'folder.png', 'folder.jpg', 'folder.jpeg', 'folder.webp'
];

const HELPER_TOKENS: string[] = [
  'unins', 'uninstall', 'setup', 'install', 'crash', 'report',
  'crashhandler', 'werfault', 'cefsubprocess', 'vc_redist', 'vcredist',
  'redist', 'dxsetup', 'dotnet', 'update', 'updater'
];

// A game folder may use a command script or a DOS program as its public
// launcher. Keep this list in one place so automatic discovery and the
// in-app chooser always present exactly the same set of launch targets.
const WINDOWS_LAUNCH_EXTENSIONS: string[] = ['.exe', '.com', '.bat', '.cmd'];

export function catalogFileName(path: string): string {
  const normalized = path.replace(/\\/g, '/');
  const index = normalized.lastIndexOf('/');
  return index >= 0 ? normalized.substring(index + 1) : normalized;
}

// Broker registers wineboot before its @engine/ identity is applied. Keep
// infrastructure out of running-card indicators without filtering the native
// process snapshot used by shutdown and session reconciliation.
export function isWineInfrastructurePath(path: string): boolean {
  if (path.toLowerCase().startsWith('@engine/')) return true;
  const name = catalogFileName(path).toLowerCase();
  return ['wineserver', 'services.exe', 'csrss.exe', 'winedevice.exe',
    'plugplay.exe', 'svchost.exe', 'rpcss.exe', 'wineboot', 'wineboot.exe',
    'winehua_keep.exe'].includes(name);
}

export function catalogNameWithoutExtension(name: string): string {
  const index = name.lastIndexOf('.');
  return index > 0 ? name.substring(0, index) : name;
}

export function isWindowsLaunchTarget(path: string): boolean {
  const lower = catalogFileName(path).toLowerCase();
  return WINDOWS_LAUNCH_EXTENSIONS.some((extension: string) => lower.endsWith(extension));
}

function comparableName(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]/g, '');
}

export function chooseExecutablePath(directory: string, candidates: string[]): string {
  const orderedCandidates = candidates
    .filter((path: string) => isWindowsLaunchTarget(path))
    .slice()
    .sort((left: string, right: string) => left.toLowerCase().localeCompare(right.toLowerCase()));
  if (orderedCandidates.length === 0) return '';
  const directoryName = comparableName(catalogFileName(directory));
  const sameName = orderedCandidates.find((path: string) =>
    comparableName(catalogNameWithoutExtension(catalogFileName(path))) === directoryName);
  if (sameName) return sameName;
  const preferred = orderedCandidates.filter((path: string) => {
    const lower = catalogFileName(path).toLowerCase();
    return !HELPER_TOKENS.some((token: string) => lower.includes(token));
  });
  if (preferred.length === 1) return preferred[0];
  const normalizedDirectory = directory.replace(/\\/g, '/').replace(/\/+$/, '');
  const rootCandidates = preferred.filter((path: string) => {
    const normalizedPath = path.replace(/\\/g, '/');
    const separator = normalizedPath.lastIndexOf('/');
    return separator > 0 && normalizedPath.substring(0, separator) === normalizedDirectory;
  });
  if (rootCandidates.length === 1) return rootCandidates[0];
  // Versioned directories usually keep the executable's stable basename,
  // e.g. palmod2.0/palmod.exe. A unique prefix match is deterministic and
  // avoids presenting a non-launchable card merely because a launcher also
  // exists beside the actual program.
  const versionedMatches = preferred.filter((path: string) => {
    const executableName = comparableName(catalogNameWithoutExtension(catalogFileName(path)));
    if (executableName.length < 3 || directoryName.length < 3) return false;
    return directoryName.startsWith(executableName) || executableName.startsWith(directoryName);
  });
  if (versionedMatches.length === 1) return versionedMatches[0];
  return '';
}

export function chooseCoverFileName(names: string[]): string {
  for (let index = 0; index < COVER_PRIORITY.length; index++) {
    const expected = COVER_PRIORITY[index];
    const found = names.find((name: string) => name.toLowerCase() === expected);
    if (found) return found;
  }
  return '';
}

/**
 * Decide whether a cached card that was not rediscovered by the current scan
 * still represents something the user can repair. Folder cards survive a
 * missing launcher so it can be re-associated; deleted folders and deleted
 * loose files under games/ do not survive.
 */
export function shouldRetainMissingCachedCard(directoryExists: boolean,
  executableExists: boolean, directoryIsGamesRoot: boolean): boolean {
  return directoryExists && (!directoryIsGamesRoot || executableExists);
}
