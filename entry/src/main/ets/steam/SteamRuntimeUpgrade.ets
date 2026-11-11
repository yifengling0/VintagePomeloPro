export interface SteamRuntimeFile { name: string; sha256: string; }
export interface SteamRuntimeIdentity {
  helper: string;
  helperSha256: string;
  previousHelperSha256: string;
  files: SteamRuntimeFile[];
}
export interface SteamRuntimeFiles {
  stopped: () => boolean;
  hash: (name: string) => Promise<string>;
  resource: () => Promise<Uint8Array>;
  hashBytes: (bytes: Uint8Array) => Promise<string>;
  replace: (name: string, bytes: Uint8Array) => Promise<void>;
}

// Only the known frozen runtime32 installation is eligible. Unknown/custom
// clients and games keep their DLLs. No account or game data is read/written.
export async function upgradeSteamRuntime(identity: SteamRuntimeIdentity, files: SteamRuntimeFiles): Promise<boolean> {
  if (!files.stopped() || identity.helper !== 'vpsteamtrust32.dll' ||
    !/^[a-f0-9]{64}$/.test(identity.helperSha256) ||
    !/^[a-f0-9]{64}$/.test(identity.previousHelperSha256) || identity.files.length !== 5) return false;
  try {
    if (await files.hash(identity.helper) !== identity.previousHelperSha256) return false;
    for (const file of identity.files) {
      if (!/^[a-zA-Z0-9_.-]+$/.test(file.name) || !/^[a-f0-9]{64}$/.test(file.sha256) ||
        await files.hash(file.name) !== file.sha256) return false;
    }
    const bytes = await files.resource();
    if (bytes.byteLength === 0 || bytes.byteLength > 1024 * 1024 ||
      await files.hashBytes(bytes) !== identity.helperSha256 || !files.stopped()) return false;
    await files.replace(identity.helper, bytes);
    return true;
  } catch (_) { return false; }
}
