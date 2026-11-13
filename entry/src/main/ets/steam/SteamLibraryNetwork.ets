export interface SteamLibraryNetworkIdentity {
  originalSha256: string;
  previousPatchedSha256: string;
  patchedSha256: string;
  scriptSha256: string;
}
export interface SteamLibraryNetworkFiles {
  stopped: () => boolean;
  read: () => Promise<string>;
  resource: () => Promise<string>;
  hash: (text: string) => Promise<string>;
  replace: (text: string) => Promise<void>;
}

// Only the exact frozen SharedJSContext bootstrap is managed. Inline the small
// policy before deferred library code; no extra local resource request is needed.
// The single atomic replacement also makes interrupted preparation retryable.
export async function prepareSteamLibraryNetwork(identity: SteamLibraryNetworkIdentity,
  files: SteamLibraryNetworkFiles): Promise<boolean> {
  if (!files.stopped() || !/^[a-f0-9]{64}$/.test(identity.originalSha256) ||
    !/^[a-f0-9]{64}$/.test(identity.previousPatchedSha256) ||
    !/^[a-f0-9]{64}$/.test(identity.patchedSha256) || !/^[a-f0-9]{64}$/.test(identity.scriptSha256)) return false;
  try {
    const original = await files.read();
    if (original.length > 65536) return false;
    const originalHash = await files.hash(original);
    let bootstrap = original;
    if (originalHash === identity.previousPatchedSha256) {
      const start = original.indexOf('<script>');
      const end = original.indexOf('</script>', start);
      if (start < 0 || end < start) return false;
      bootstrap = original.slice(0, start) + original.slice(end + '</script>'.length);
    }
    if (await files.hash(bootstrap) !== identity.originalSha256) return false;
    const script = await files.resource();
    if (!script || script.length > 16384 || /<\/script/i.test(script) ||
      await files.hash(script) !== identity.scriptSha256) return false;
    const anchor = '<script defer="defer"';
    if (!bootstrap.includes(anchor)) return false;
    const updated = bootstrap.replace(anchor, '<script>' + script + '</script>' + anchor);
    if (await files.hash(updated) !== identity.patchedSha256 || !files.stopped() ||
      await files.hash(await files.read()) !== originalHash) return false;
    await files.replace(updated);
    return true;
  } catch (_) { return false; } // Custom clients must remain usable and untouched.
}
