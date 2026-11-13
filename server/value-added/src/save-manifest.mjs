import { createHash } from 'node:crypto';
import { requireThat } from './errors.mjs';
export const CHUNK_BYTES = 4 * 1024 * 1024;
export const SNAPSHOT_BYTES = 256 * 1024 * 1024;
export const ACCOUNT_BYTES = 1024 * 1024 * 1024;
export const hash = value => createHash('sha256').update(value).digest('hex');
const exact = (value, keys) => requireThat(value && typeof value === 'object' && !Array.isArray(value) &&
  Object.keys(value).every(key => keys.includes(key)), 'INVALID_SAVE_MANIFEST');
export function validateSaveManifest(value) {
  exact(value, ['schemaVersion','files','chunks','totalBytes']);
  requireThat(value.schemaVersion === 1 && Array.isArray(value.files) && value.files.length > 0 && value.files.length <= 1024 &&
    Array.isArray(value.chunks) && value.chunks.length <= 64 && Number.isSafeInteger(value.totalBytes) &&
    value.totalBytes >= 0 && value.totalBytes <= SNAPSHOT_BYTES && Buffer.byteLength(JSON.stringify(value)) <= 400000,
    'INVALID_SAVE_MANIFEST');
  let offset = 0; const paths = new Set();
  for (const file of value.files) {
    exact(file,['relativePath','size','sha256','offset']);
    const path = file.relativePath;
    requireThat(typeof path === 'string' && path.length <= 240 && path === path.normalize('NFC') &&
      !/[\\:\u0000-\u001f]/.test(path) && !path.startsWith('/') && path.split('/').length <= 24 &&
      path.split('/').every(part => part && part !== '.' && part !== '..' && !/[. ]$/.test(part)), 'UNSAFE_SAVE_PATH');
    const key = path.toLowerCase();
    requireThat(!paths.has(key) && ![...paths].some(other => other.startsWith(key + '/') || key.startsWith(other + '/')), 'SAVE_PATH_COLLISION');
    paths.add(key);
    requireThat(Number.isSafeInteger(file.size) && file.size >= 0 && file.size <= 67108864 && file.offset === offset &&
      typeof file.sha256 === 'string' && /^[a-f0-9]{64}$/.test(file.sha256), 'INVALID_SAVE_MANIFEST');
    offset += file.size;
  }
  requireThat(offset === value.totalBytes && value.chunks.length === Math.ceil(offset / CHUNK_BYTES), 'INVALID_SAVE_MANIFEST');
  value.chunks.forEach((chunk,index) => {
    exact(chunk,['index','size','sha256']);
    requireThat(chunk.index === index && chunk.size === Math.min(CHUNK_BYTES, offset-index*CHUNK_BYTES) &&
      typeof chunk.sha256 === 'string' && /^[a-f0-9]{64}$/.test(chunk.sha256), 'INVALID_SAVE_MANIFEST');
  });
  return JSON.parse(JSON.stringify(value));
}
