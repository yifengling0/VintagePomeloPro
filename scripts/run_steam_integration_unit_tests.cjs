#!/usr/bin/env node
// Host-side regression tests for the actual ArkTS services, with HTTP/storage adapters mocked.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '../entry/src/main/ets');
const log = { hilog: { info() {}, warn() {}, error() {} } };

function loader(mocks = {}) {
  const cache = new Map();
  function load(file) {
    const absolute = path.resolve(file);
    if (cache.has(absolute)) return cache.get(absolute);
    const exports = {};
    cache.set(absolute, exports);
    const compiled = ts.transpileModule(fs.readFileSync(absolute, 'utf8'), {
      compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2021 }
    }).outputText;
    vm.runInNewContext(compiled, {
      exports, Date, ArrayBuffer, Uint8Array, Map, Set,
      require(name) {
        if (Object.hasOwn(mocks, name)) return mocks[name];
        assert.ok(name.startsWith('.'), `Unexpected platform dependency: ${name}`);
        return load(path.resolve(path.dirname(absolute), name + '.ets'));
      }
    }, { filename: absolute });
    return exports;
  }
  return relative => load(path.join(root, relative));
}

async function main() {
  const protocolLoad = loader({
    './SteamCryptoAdapter': { SteamCryptoAdapter: class {
      async aesEcbDecryptBlock(key, block) {
        const cipher = crypto.createDecipheriv('aes-256-ecb', key, null);
        cipher.setAutoPadding(false);
        return new Uint8Array(Buffer.concat([cipher.update(block), cipher.final()]));
      }
      async aesCbcDecrypt(key, iv, bytes) {
        const cipher = crypto.createDecipheriv('aes-256-cbc', key, iv);
        return new Uint8Array(Buffer.concat([cipher.update(bytes), cipher.final()]));
      }
    } }
  });
  const { PicsAppInfo, PicsProductInfoResponse, DepotManifest, ManifestFile } =
    protocolLoad('steam/SteamContentProtocol.ets');
  const depots = '"depots" { "235901" { "config" { "oslist" "windows" } ' +
    '"manifests" { "public" { "gid" "18446744073709551614" } } } ' +
    '"235902" { "config" { "oslist" "linux" } ' +
    '"manifests" { "public" { "gid" "20" } } } }';
  for (const vdf of [depots, '"235900" { ' + depots + ' }',
    '"appinfo" { "appid" "235900" ' + depots + ' }']) {
    const info = new PicsAppInfo();
    info.appId = 235900;
    PicsProductInfoResponse.parseAppBuffer(info, new Uint8Array(Buffer.from(vdf + '\0')));
    assert.equal(info.depots.length, 1, 'all supported PICS wrappers must expose Windows depots');
    assert.equal(info.depots[0].manifestGid, '18446744073709551614', 'manifest IDs must retain uint64 precision');
  }
  const wrongInfo = new PicsAppInfo();
  wrongInfo.appId = 235900;
  assert.throws(() => PicsProductInfoResponse.parseAppBuffer(wrongInfo,
    new Uint8Array(Buffer.from('"appinfo" { "appid" "1" ' + depots + ' }'))), /不一致/);

  // Independent Node crypto fixtures exercise Steam's wire format, including
  // the wrapped Base64 names that caused the real device download failure.
  const fixtureKey = new Uint8Array(32).fill(0x5a);
  const encryptedFile = name => {
    const iv = Buffer.alloc(16, 0x23);
    const ecb = crypto.createCipheriv('aes-256-ecb', fixtureKey, null);
    ecb.setAutoPadding(false);
    const cbc = crypto.createCipheriv('aes-256-cbc', fixtureKey, iv);
    const encrypted = Buffer.concat([ecb.update(iv), ecb.final(),
      cbc.update(Buffer.from(name + '\0')), cbc.final()]).toString('base64');
    const file = new ManifestFile();
    file.path = encrypted.slice(0, 20) + '\r\n\t ' + encrypted.slice(20) + '\n';
    return file;
  };
  const manifest = new DepotManifest();
  manifest.filenamesEncrypted = true;
  manifest.files = [encryptedFile('bin/RPGXP.exe'), encryptedFile('繁體/ゲーム.exe')];
  await manifest.decryptFilenames(fixtureKey);
  assert.equal(manifest.files[0].path, 'bin/RPGXP.exe');
  assert.equal(manifest.files[1].path, '繁體/ゲーム.exe');
  assert.equal(manifest.filenamesEncrypted, false);
  const partial = new DepotManifest();
  partial.filenamesEncrypted = true;
  partial.files = [encryptedFile('bin/RPGXP.exe'), new ManifestFile()];
  partial.files[1].path = 'invalid!';
  const originalPath = partial.files[0].path;
  await assert.rejects(partial.decryptFilenames(fixtureKey), /encoding/);
  assert.equal(partial.files[0].path, originalPath, 'failed name decryption must leave the manifest intact');
  assert.equal(partial.filenamesEncrypted, true);

  let rawCache = '';
  let requestCount = 0;
  let respond = async () => { throw new Error('offline'); };
  const store = {
    loadLibraryCache: async () => rawCache,
    saveLibraryCache: async raw => { rawCache = raw; }
  };
  const http = {
    RequestMethod: { GET: 'GET' }, HttpDataType: { ARRAY_BUFFER: 1 },
    createHttp: () => ({
      request: (...args) => { requestCount++; return respond(...args); }, destroy() {}
    })
  };
  const load = loader({
    '@kit.NetworkKit': { http }, '../service/LogService': log,
    './SteamAccountStore': { SteamAccountStore: { getInstance: () => store } }
  });
  const { SteamLibraryService } = load('steam/SteamLibrary.ets');
  const { SteamLibraryGame } = load('steam/vendor/models/SteamModels.ets');
  const oldTime = Date.now() - 31 * 60 * 1000;
  rawCache = JSON.stringify({ savedAt: oldTime, steamId64: '100', games: [
    { appId: '1', name: 'Portal', playtimeForeverSeconds: 60 }
  ] });
  const library = new SteamLibraryService();
  assert.equal((await library.fetchOwnedGames('100', 'fake-token')).length, 1);
  assert.equal(library.cacheSavedAt, oldTime, 'offline load must preserve expiry');
  assert.equal(library.hasFreshCache('100'), false);
  assert.equal((await library.fetchOwnedGames('100', '')).length, 1, 'offline session can read its cache');
  await assert.rejects(library.fetchOwnedGames('200', ''), 'never show a different account cache');
  const freshTime = Date.now() - 15 * 60 * 1000;
  rawCache = JSON.stringify({ savedAt: freshTime, steamId64: '100', games: [] });
  const emptyLibrary = new SteamLibraryService();
  const before = requestCount;
  await emptyLibrary.fetchOwnedGames('100', 'fake-token');
  await emptyLibrary.fetchOwnedGames('100', 'fake-token');
  assert.equal(requestCount, before, 'fresh empty library must not repeatedly fetch');
  assert.equal(emptyLibrary.cacheSavedAt, freshTime, 'cache read must not renew its TTL');
  const pending = new Map();
  respond = url => new Promise(resolve => pending.set(new URL(url).searchParams.get('steamid'), resolve));
  rawCache = '';
  const concurrent = new SteamLibraryService();
  const first = concurrent.fetchOwnedGames('100', 'fake-token');
  const second = concurrent.fetchOwnedGames('200', 'fake-token');
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(pending.size, 2, 'in-flight requests must be scoped to account');
  for (const [id, resolve] of pending) {
    resolve({ responseCode: 200, result: JSON.stringify({ response: {
      game_count: 1, games: [{ appid: Number(id), name: id }]
    } }) });
  }
  assert.equal((await first)[0].name, '100');
  assert.equal((await second)[0].name, '200');

  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'pomelo-steam-test-'));
  try {
    const fileIo = {
      OpenMode: { WRITE_ONLY: 1, CREATE: 2, TRUNC: 4 },
      listFileSync: dir => fs.readdirSync(dir),
      openSync: file => ({ fd: fs.openSync(file, 'w') }),
      writeSync: (fd, bytes) => fs.writeSync(fd, Buffer.from(bytes)),
      closeSync: file => fs.closeSync(file.fd),
      renameSync: fs.renameSync, unlinkSync: fs.unlinkSync
    };
    const { SteamMatch } = loader({
      '@kit.NetworkKit': { http }, '@kit.CoreFileKit': { fileIo }, '../service/LogService': log
    })('steam/SteamMatch.ets');
    const game = (id, name) => new SteamLibraryGame(id, name, '', '', 0, 0, 0, false);
    assert.equal(SteamMatch.bestMatch('Portal', [game('2', 'Portal 2'), game('1', 'Portal')]).appId, '1');
    assert.equal(SteamMatch.bestMatch('Portal', [game('2', 'Portal 2')]), null);
    assert.equal(SteamMatch.bestMatch('Portal', [game('1', 'Portal'), game('2', 'Portal')]), null);
    assert.equal(SteamMatch.titlesMatch('Ｐｏｒｔａｌ ２', 'Portal 2'), true);
    assert.equal(SteamMatch.titlesMatch('Demo (2020)', 'Demo (2021)'), false);
    assert.equal(SteamMatch.titlesMatch('Demo', 'Demo Remastered'), false);
    const png = new Uint8Array(32);
    png.set([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);
    respond = async () => ({ responseCode: 200, result: png.buffer });
    let manual = false;
    respond = async () => { manual = true; return { responseCode: 200, result: png.buffer }; };
    assert.equal(await SteamMatch.downloadCoverTo('1', '', directory, () => !manual), false);
    assert.deepEqual(fs.readdirSync(directory), [], 'manual choice during download must block commit');
    respond = async () => ({ responseCode: 200, result: new ArrayBuffer(32) });
    assert.equal(await SteamMatch.downloadCoverTo('1', '', directory, () => true), false);
    respond = async () => ({ responseCode: 200, result: png.buffer });
    assert.equal(await SteamMatch.downloadCoverTo('1', '', directory, () => true), true);
    assert.deepEqual(fs.readdirSync(directory), ['cover.png']);
    const requestsBeforeExisting = requestCount;
    assert.equal(await SteamMatch.downloadCoverTo('1', '', directory, () => true), false);
    assert.equal(requestCount, requestsBeforeExisting, 'existing cover must bypass download');
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }

  const authLoad = loader();
  const { SteamAuthService } = authLoad('steam/vendor/auth/SteamAuthService.ets');
  const { SteamAuthHttpResponse } = authLoad('steam/vendor/auth/SteamAuthProtocol.ets');
  let refreshFields = [];
  let refreshEndpoint;
  const auth = new SteamAuthService({ postForm: async (endpoint, fields) => {
    refreshEndpoint = endpoint;
    refreshFields = fields;
    return new SteamAuthHttpResponse(200, JSON.stringify({ response: { access_token: 'fake-access' } }));
  } }, {}, { nowEpochSeconds: () => 100 });
  const fakeJwt = 'header.' + Buffer.from('{"sub":"76561198000000001"}').toString('base64url') + '.signature';
  await auth.refresh('fixture', fakeJwt);
  assert.equal(refreshEndpoint,
    'https://api.steampowered.com/IAuthenticationService/GenerateAccessTokenForApp/v1/');
  assert.equal(refreshFields.find(field => field.name === 'steamid').value, '76561198000000001');
  const { SteamAuthRequest } = authLoad('steam/vendor/contracts/SteamPlatformContracts.ets');
  const nativeFailure = new SteamAuthService({ postForm: async () => {
    throw { code: 2300006, message: 'fixture native network error' };
  } }, {}, { nowEpochSeconds: () => 100 });
  await assert.rejects(nativeFailure.signIn(new SteamAuthRequest('fixture',
    new (authLoad('steam/vendor/models/SteamModels.ets').SteamCredentialSet)('fixture', 'fixture-password'))),
    error => error.code === 2300006 && error.stage === 'rsa_public_key');
  const { SteamHttpTransport } = loader({ '@kit.NetworkKit': { http } })('steam/SteamHttpTransport.ets');
  const { SteamAuthEndpoint, SteamFormField } = authLoad('steam/vendor/auth/SteamAuthProtocol.ets');
  const calls = [];
  respond = async (url, options) => {
    calls.push({ url, options });
    return { responseCode: 200, result: '{}', header: {} };
  };
  await new SteamHttpTransport().postForm(SteamAuthEndpoint.GET_PASSWORD_RSA_PUBLIC_KEY,
    [new SteamFormField('account_name', 'fixture')]);
  assert.equal(calls.length, 1, 'RSA lookup must send GET directly');
  assert.equal(calls[0].options.method, 'GET');
  assert.equal(calls[0].url.includes('account_name=fixture'), true);
  calls.length = 0;
  http.RequestMethod.POST = 'POST';
  respond = async (url, options) => {
    calls.push({ url, options });
    return { responseCode: 405, result: '{}', header: {} };
  };
  await new SteamHttpTransport().postForm(SteamAuthEndpoint.BEGIN_AUTH_SESSION,
    [new SteamFormField('encrypted_password', 'fixture')]);
  assert.equal(calls.length, 1, 'credential POST must not be retried with secrets in a GET URL');
  assert.equal(calls[0].options.method, 'POST');
  assert.equal(calls[0].url.includes('encrypted_password'), false);

  let challenge;
  let qrResolve;
  const models = authLoad('steam/vendor/models/SteamModels.ets');
  const authAdapter = { completeChallenge: async request => {
    challenge = request.challengeKind;
    throw new Error('fixture rejection');
  } };
  const serviceLoad = loader({
    '@kit.NetworkKit': { http }, '../service/LogService': log,
    './SteamDownloadService': { SteamDownloadService: class {} },
    './SteamCmRuntime': { SteamCmScheduler: class {}, SteamCmMultiDecompressor: class {} },
    './SteamAccountStore': { SteamAccountStore: { getInstance: () => ({}) } },
    './vendor/auth/SteamAuthService': { SteamAuthService: class {} },
    './SteamCryptoAdapter': { SteamCryptoAdapter: class {} },
    './SteamHttpTransport': { SteamHttpTransport: class {
      postForm() { return new Promise(resolve => { qrResolve = resolve; }); }
    } }
  });
  const { SteamService, SteamUiState } = serviceLoad('steam/SteamService.ets');
  const service = new SteamService();
  service.authService = authAdapter;
  service.setState(SteamUiState.NEEDS_EMAIL_CODE);
  await service.submitCode('ABCDE');
  assert.equal(challenge, models.SteamAuthChallengeKind.EMAIL_CODE);
  assert.equal(service.state, SteamUiState.NEEDS_EMAIL_CODE, 'rejection must retain challenge type');
  service.setState(SteamUiState.NEEDS_GUARD_CODE);
  await service.submitCode('ABCDE');
  assert.equal(challenge, models.SteamAuthChallengeKind.TWO_FACTOR);
  const qr = service.beginQrLogin();
  service.cancelQrLogin();
  qrResolve(new SteamAuthHttpResponse(200, JSON.stringify({ response: {
    client_id: '1', request_id: '2', challenge_url: 'https://example.invalid/fixture'
  } })));
  await assert.rejects(qr, /取消/);
  assert.equal(service.qrClientId, '', 'cancelled begin must not recreate a QR session');
  const rejectedQr = service.beginQrLogin();
  qrResolve(new SteamAuthHttpResponse(200, '{}', new Map([['x-error', 'rejected']])));
  await assert.rejects(rejectedQr, /拒绝/);

  let initializeCount = 0;
  let initializeResolve;
  const initializing = new SteamService();
  initializing.store = {
    initialize: () => { initializeCount++; return new Promise(resolve => { initializeResolve = resolve; }); },
    loadTimeOffset: async () => 0,
    loadProfile: async () => ({ accountName: '', steamId64: '', avatarUrl: '', personaName: '' })
  };
  const initFirst = initializing.initialize({});
  const initSecond = initializing.initialize({});
  assert.equal(initializeCount, 1);
  initializeResolve();
  await Promise.all([initFirst, initSecond]);
  let refreshCount = 0;
  let refreshResolve;
  service.store = { loadSecret: async () => 'fake-refresh' };
  service.steamId64 = '100';
  service.authService = { refresh: () => {
    refreshCount++;
    return new Promise(resolve => { refreshResolve = resolve; });
  } };
  const restoreFirst = service.restoreSession();
  const restoreSecond = service.restoreSession();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(refreshCount, 1, 'entry and account page must share a pending restore');
  refreshResolve({ session: { state: models.SteamSessionState.IDLE } });
  await Promise.all([restoreFirst, restoreSecond]);
  console.log('Steam integration regression tests passed (PICS wrappers, encrypted filenames, cache, account isolation, matching, cover race, refresh, challenges, QR cancellation).');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
