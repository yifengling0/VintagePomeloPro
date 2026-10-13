import { readFile, chmod, access } from 'node:fs/promises';
import { constants } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { requireThat } from './errors.mjs';

// The function runtime includes Node's OpenSSL library, but no OpenSSL CLI. A bundled
// musl loader avoids the cloud host's glibc version while preserving full PKIX checks.
export const PKIX_FILES = ['openssl-verify', 'openssl', 'ld-musl-x86_64.so.1', 'libssl.so.3', 'libcrypto.so.3'];
export async function loadPkixRuntime(directory = new URL('../runtime/', import.meta.url)) {
  let json;
  try { json = await readFile(new URL('manifest.json', directory), 'utf8'); }
  catch (error) { if (error.code === 'ENOENT') return {}; throw error; }
  requireThat(process.platform === 'linux' && process.arch === 'x64', 'PKIX_NOT_CONFIGURED', 503);
  const manifest = JSON.parse(json);
  requireThat(manifest.protocol === 1 && manifest.opensslVersion === '3.5.9' &&
    Object.keys(manifest.files ?? {}).sort().join('|') === [...PKIX_FILES].sort().join('|'), 'PKIX_NOT_CONFIGURED', 503);
  for (const name of PKIX_FILES) {
    const file = new URL(name, directory);
    requireThat(createHash('sha256').update(await readFile(file)).digest('hex') === manifest.files[name], 'PKIX_NOT_CONFIGURED', 503);
    // Some providers reset ZIP modes. Only package-verified executable files gain execute permission.
    if (['openssl-verify', 'openssl', 'ld-musl-x86_64.so.1'].includes(name)) {
      try { await access(file, constants.X_OK); } catch { await chmod(file, 0o700); }
    }
  }
  const opensslPath = fileURLToPath(new URL('openssl-verify', directory));
  let version;
  try { version = execFileSync(opensslPath, ['version'], { encoding: 'utf8', timeout: 5000, stdio: ['ignore', 'pipe', 'pipe'] }); }
  catch { requireThat(false, 'PKIX_NOT_CONFIGURED', 503); }
  requireThat(/^OpenSSL 3\.5\.9 /.test(version), 'PKIX_NOT_CONFIGURED', 503);
  return { opensslPath,
    verifierDigest: createHash('sha256').update(json).digest('hex') };
}
