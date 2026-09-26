#!/usr/bin/env node
// The guest runtime is generated outside Hvigor and ignored by Git. Refuse to
// package a stale archive after the Wine or another runtime gitlink changes.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');

const root = path.resolve(__dirname, '..');
const rawfiles = path.join(root, 'entry', 'src', 'main', 'resources', 'rawfile');
const versionPath = path.join(rawfiles, 'wine_runtime.version');
const manifestPath = path.join(rawfiles, 'wine-runtime-manifest.json');
const payloadPath = path.join(rawfiles, 'wine-data.zip');
const runtimeModules = [
  'wine', 'box64', 'mesa', 'virglrenderer', 'gstreamer',
  'gnutls', 'glib', 'pcre2', 'dxvk'
];

function fail(message) {
  throw new Error(`Wine runtime preflight: ${message}. Rebuild with make hap or copy a verified runtime from the same source revision`);
}

function gitlink(moduleName) {
  const output = execFileSync('git', ['-C', root, 'ls-tree', 'HEAD', `thirdparty/${moduleName}`],
    { encoding: 'utf8' }).trim();
  const match = /^160000 commit ([0-9a-f]{40})\t/.exec(output);
  if (!match) fail(`missing gitlink for thirdparty/${moduleName}`);
  return match[1];
}

function payloadHash() {
  const digest = crypto.createHash('sha256');
  const buffer = Buffer.allocUnsafe(1024 * 1024);
  const fd = fs.openSync(payloadPath, 'r');
  try {
    let count = 0;
    while ((count = fs.readSync(fd, buffer, 0, buffer.length, null)) > 0) {
      digest.update(buffer.subarray(0, count));
    }
  } finally {
    fs.closeSync(fd);
  }
  return digest.digest('hex');
}

function verifyWineRuntime() {
  for (const file of [versionPath, manifestPath, payloadPath]) {
    if (!fs.existsSync(file) || fs.statSync(file).size === 0) fail(`missing ${path.basename(file)}`);
  }
  const version = fs.readFileSync(versionPath, 'utf8').trim();
  const fields = new Map(version.split(';').map((field) => field.split('=', 2)));
  if (!/^[0-9a-f]{16}$/.test(fields.get('content') || '')) fail('invalid content identity');
  for (const moduleName of runtimeModules) {
    const recorded = fields.get(moduleName);
    const pinned = gitlink(moduleName);
    if (!recorded || recorded.length < 7 || !pinned.startsWith(recorded)) {
      fail(`${moduleName} archive was built from ${recorded || 'unknown'}, but source pins ${pinned.slice(0, 10)}`);
    }
  }
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  if (manifest.schemaVersion !== 1 || manifest.payload !== 'wine-data.zip' ||
      !/^[0-9a-f]{64}$/.test(manifest.payloadSha256 || '')) {
    fail('invalid runtime manifest');
  }
  const actual = payloadHash();
  if (actual !== manifest.payloadSha256) fail(`archive SHA-256 ${actual} differs from manifest`);
  process.stdout.write(`Wine runtime verified: ${fields.get('wine')} ${actual}\n`);
}

if (require.main === module) {
  try { verifyWineRuntime(); } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}

module.exports = { verifyWineRuntime };
