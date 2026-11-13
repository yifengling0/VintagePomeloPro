import { SaveManifestEntry } from './ValueAddedModels';

export interface SaveFolderChoice { name: string; relativePath: string; }
export interface SaveLocation {
  appId: string; selectedPath: string; updatedAt: number;
  gameTitle?: string; gameIdentity?: string; enabled?: boolean; lastStatus?: string;
}
export interface GameBackupState {
  supported: boolean; enabled: boolean; selectedPath: string; reason: string;
}
export interface BackupGameSummary {
  appId: string; title: string; installed: boolean; enabled: boolean; selectedPath: string;
  count: number; latestAt: number; totalBytes: number; status: string;
}
export interface SaveLocationIndex { schemaVersion: number; locations: SaveLocation[]; }
export interface SavePreview {
  selectedPath: string;
  files: SaveManifestEntry[];
  totalBytes: number;
  added: number;
  changed: number;
  removed: number;
  destinationMissing: boolean;
}
/** Compare content, not modification times: copying an unchanged save must not create another version. */
export function sameSaveContent(left: SaveManifestEntry[], right: SaveManifestEntry[]): boolean {
  return left.length === right.length && left.every((file: SaveManifestEntry): boolean =>
    right.some((other: SaveManifestEntry): boolean => other.relativePath === file.relativePath &&
      other.size === file.size && other.sha256 === file.sha256));
}
export function savePreview(selectedPath: string, current: SaveManifestEntry[], target: SaveManifestEntry[],
  destinationMissing: boolean = false): SavePreview {
  return { selectedPath: selectedPath, files: current,
    totalBytes: target.reduce((sum: number, file: SaveManifestEntry): number => sum + file.size, 0),
    added: target.filter((file: SaveManifestEntry): boolean => !current.some((other: SaveManifestEntry): boolean =>
      other.relativePath === file.relativePath)).length,
    changed: target.filter((file: SaveManifestEntry): boolean => current.some((other: SaveManifestEntry): boolean =>
      other.relativePath === file.relativePath && (other.sha256 !== file.sha256 || other.size !== file.size))).length,
    removed: current.filter((file: SaveManifestEntry): boolean => !target.some((other: SaveManifestEntry): boolean =>
      other.relativePath === file.relativePath)).length, destinationMissing: destinationMissing };
}
