import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { requireThat, ServiceError } from './errors.mjs';

export const SDK_VERSION = '1.0.7';
export const LOCK_KEY = 'internal:transaction-lock';
/** Match this object type in AGC. All client roles must have no access; server SDK only. */
export class ValueAddedRecord {
  constructor(key = '', scope = '', json = '') { this.key = key; this.scope = scope; this.json = json; }
  getClassName() { return 'ValueAddedRecord'; }
  getPrimaryKeyList() { return ['key']; }
  getFieldTypeMap() { return new Map([['key','String'], ['scope','String'], ['json','Text']]); }
  getIndexList() { return new Map([['scopeIndex', ['scope']]]); }
  getEncryptedFieldList() { return []; }
}
const bounded = promise => Promise.race([promise, new Promise((_, reject) => {
  const timer = setTimeout(() => reject(new ServiceError('STORAGE_TIMEOUT', 503)), 30000); timer.unref();
})]);
export class CloudDbStore {
  constructor(zone, Query, fingerprint) { this.zone = zone; this.Query = Query; this.fingerprint = fingerprint; this.ready = false; }
  query(key) { return this.Query.where(ValueAddedRecord).equalTo('key', key); }
  async get(key) {
    const result = await bounded(this.zone.executeQuery(this.query(key)));
    const rows = result.getSnapshotObjects();
    requireThat(rows.length <= 1, 'STORAGE_INVARIANT', 503);
    return rows.length ? JSON.parse(rows[0].json) : null;
  }
  async list(scope) {
    const result = await bounded(this.zone.executeQuery(this.Query.where(ValueAddedRecord).equalTo('scope', scope).limit(257)));
    const rows = result.getSnapshotObjects();
    requireThat(rows.length <= 256, 'PROFILE_LIMIT', 409);
    return rows.map(row => JSON.parse(row.json));
  }
  async transaction(action) {
    let result;
    const committed = await bounded(this.zone.runTransaction({ apply: async raw => {
      // Empty query results do not carry a CloudDB version. Every transaction must read AND
      // update this pre-existing row so first insert/owner binding is also conflict protected.
      const locks = await raw.executeQuery(this.query(LOCK_KEY));
      requireThat(locks.length === 1, 'CLOUDDB_LOCK_MISSING', 503);
      const lock = JSON.parse(locks[0].json);
      requireThat(Number.isSafeInteger(lock.revision) && lock.revision < Number.MAX_SAFE_INTEGER, 'STORAGE_INVARIANT', 503);
      const writes = new Map();
      result = await action({
        get: async key => {
          requireThat(key !== LOCK_KEY, 'RESERVED_KEY');
          if (writes.has(key)) return JSON.parse(writes.get(key).json);
          const rows = await raw.executeQuery(this.query(key));
          requireThat(rows.length <= 1, 'STORAGE_INVARIANT', 503);
          return rows.length ? JSON.parse(rows[0].json) : null;
        },
        put: (key, value, scope) => {
          requireThat(key !== LOCK_KEY && key.length <= 200 && scope.length <= 200, 'INVALID_STORAGE_KEY');
          const json = JSON.stringify(value);
          requireThat(Buffer.byteLength(json) <= 524288, 'STORAGE_QUOTA');
          writes.set(key, new ValueAddedRecord(key, scope, json));
        }
      });
      // SDK requires all reads before all writes. Callback has no external side effects and may rerun.
      raw.executeUpsert([new ValueAddedRecord(LOCK_KEY, 'internal', JSON.stringify({ revision: lock.revision + 1 })),
        ...writes.values()]);
      return true;
    } }));
    requireThat(committed === true, 'STORAGE_CONFLICT_RETRY', 503);
    return result;
  }
  async loadVerification(file) {
    const evidence = JSON.parse(await readFile(file, 'utf8'));
    requireThat(evidence.fingerprint === this.fingerprint && evidence.sdkVersion === SDK_VERSION &&
      evidence.sameBaseConflict === true && evidence.duplicateOrder === true && evidence.callbackAbort === true && evidence.serverRollback === true &&
      evidence.ownerImmutable === true && evidence.disconnectedFailsClosed === true, 'CLOUDDB_NOT_VERIFIED', 503);
    requireThat(await this.get(LOCK_KEY), 'CLOUDDB_LOCK_MISSING', 503);
    this.ready = true;
  }
}
export async function connectCloudDb({ credentialFile, zoneName }) {
  const { AGCClient, CredentialParser } = await import('@agconnect/common-server');
  const { AGConnectCloudDB, CloudDBZoneConfig, CloudDBZoneQuery } = await import('@agconnect/database-server');
  requireThat(credentialFile && /^[a-zA-Z0-9_-]{1,64}$/.test(zoneName), 'NOT_CONFIGURED', 503);
  const credential = CredentialParser.toCredential(credentialFile);
  AGCClient.initialize(credential);
  const client = AGCClient.getInstance();
  AGConnectCloudDB.initialize(client);
  const zone = AGConnectCloudDB.getInstance(client).openCloudDBZone(new CloudDBZoneConfig(zoneName));
  const lockDigest = createHash('sha256').update(await readFile(new URL('../package-lock.json', import.meta.url))).digest('hex');
  const fingerprint = createHash('sha256').update(credential.getProjectId() + '\n' + credential.getRegion() +
    '\n' + zoneName + '\n' + SDK_VERSION + '\n' + lockDigest).digest('hex');
  return new CloudDbStore(zone, CloudDBZoneQuery, fingerprint);
}
