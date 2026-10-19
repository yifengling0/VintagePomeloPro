import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { withRuntimeLogger, runtimeInfo, runtimeError } from '../src/runtime-log.mjs';

test('concurrent timer invocations keep their own AGC logger across asynchronous work', async () => {
  const first = [], second = [];
  const logger = target => ({ info: message => target.push(message), error: message => target.push(message) });
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const pending = withRuntimeLogger(logger(first), async () => {
    runtimeInfo('first-start'); await gate; runtimeError('first-error');
  });
  await withRuntimeLogger(logger(second), async () => {
    await Promise.resolve(); runtimeInfo('second-summary'); release();
  });
  await pending;
  assert.deepEqual(first, ['first-start', 'first-error']);
  assert.deepEqual(second, ['second-summary']);
});

test('timer entry emits a bounded summary and sanitized failure through its invocation logger', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'vp-timer-logging-'));
  const oldSecret = process.env.VP_RENEWAL_TIMER_SECRET;
  const secret = 'test-only-timer-secret'.repeat(3);
  process.env.VP_RENEWAL_TIMER_SECRET = secret;
  try {
    await mkdir(join(directory, 'src'));
    await copyFile(new URL('../handler.cjs', import.meta.url), join(directory, 'handler.cjs'));
    for (const file of ['renewal-timer.mjs', 'runtime-log.mjs', 'errors.mjs']) {
      await copyFile(new URL('../src/' + file, import.meta.url), join(directory, 'src', file));
    }
    await writeFile(join(directory, 'src/main.mjs'), `
      export async function loadService() {
        return { reminders: { run: async () => ({ processed: 3, failed: 1, complete: false, privateData: 'never-log-this' }) } };
      }
    `);
    const { renewalHandler } = await import(pathToFileURL(join(directory, 'handler.cjs')));
    const logs = [];
    const logger = { info: message => logs.push(['info', message]), error: message => logs.push(['error', message]) };
    const invoke = event => new Promise(resolve => {
      assert.equal(renewalHandler(event, {}, resolve, logger), undefined);
    });
    assert.equal((await invoke({ triggerType: 'TIMER', userData: secret })).processed, 3);
    assert.deepEqual(logs.shift(), ['info', 'VP_RENEWAL_TIMER {"processed":3,"failed":1,"complete":false}']);
    assert.equal((await invoke({ triggerType: 'TIMER', userData: 'private-input' })).diagnosticCode, 'TIMER_ONLY');
    assert.deepEqual(logs, [['error', '[RenewalReminders] timer failed: TIMER_ONLY']]);
    assert.ok(!JSON.stringify(logs).includes('private-input'));
  } finally {
    if (oldSecret === undefined) delete process.env.VP_RENEWAL_TIMER_SECRET;
    else process.env.VP_RENEWAL_TIMER_SECRET = oldSecret;
    await rm(directory, { recursive: true, force: true });
  }
});
