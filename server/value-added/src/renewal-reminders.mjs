import { createHash, randomUUID } from 'node:crypto';
import { requireThat } from './errors.mjs';
import { openReceipt } from './subscription.mjs';
import { runtimeError } from './runtime-log.mjs';

const DAY = 86400000, LEASE = 10 * 60000;
// One daily scan, with twelve hours of tolerance for a delayed scheduler.
export const RENEWAL_TIMER_MAX_AGE_MS = 36 * 3600000;
const hash = value => createHash('sha256').update(value).digest('hex');
const installation = value => typeof value === 'string' && /^[a-f0-9-]{36}$/.test(value);
const cycle = value => hash([value.subscriptionKey, value.nextRenewalAt].join('\n'));

/** Durable jobs survive app exit and session expiry. Only the private timer entry runs jobs. */
export class RenewalReminders {
  constructor(domain, sender = { ready: false }) { this.domain = domain; this.sender = sender; this.timerReady = false; }
  get ready() { return this.sender.ready === true && this.domain.store.ready === true; }
  async register(userId, { installationId, token, enabled }) {
    requireThat(installation(installationId) && typeof token === 'string' && token.length > 0 && token.length <= 4096 &&
      !/[\s\x00-\x1f]/.test(token) && typeof enabled === 'boolean', 'INVALID_NOTIFICATION_DEVICE');
    requireThat(this.ready, 'REMINDERS_UNAVAILABLE', 503);
    const id = 'notification-device:' + hash(token), now = this.domain.clock();
    await this.domain.store.transaction(async tx => {
      // A token has one owner. Account switching replaces the binding rather than adding another one.
      const bindingKey = 'notification-installation:' + installationId;
      const binding = await tx.get(bindingKey);
      if (binding?.deviceId && binding.deviceId !== id) {
        const old = await tx.get(binding.deviceId);
        if (old?.installationId === installationId) tx.put(old.id, { ...old, enabled: false, token: '', updatedAt: now },
          'notification-devices-retired');
      }
      tx.put(bindingKey, { deviceId: id, userId }, 'notification-installations');
      tx.put(id, { id, userId, installationId, token, enabled, updatedAt: now }, 'notification-devices:' + userId);
    });
    return { ready: enabled && await this.timerHealthy() };
  }
  async revoke(userId, { installationId }) {
    requireThat(installation(installationId), 'INVALID_NOTIFICATION_DEVICE');
    const devices = await this.domain.store.list('notification-devices:' + userId);
    for (const device of devices.filter(item => item.installationId === installationId)) {
      await this.domain.store.transaction(async tx => {
        const current = await tx.get(device.id);
        if (current?.userId === userId && current.installationId === installationId) {
          tx.put(device.id, { ...current, enabled: false, token: '', updatedAt: this.domain.clock() }, 'notification-devices-retired');
        }
      });
    }
    return {};
  }
  async available(userId) {
    if (!await this.timerHealthy()) return false;
    return (await this.domain.store.list('notification-devices:' + userId)).some(device => device.userId === userId && device.enabled && device.token);
  }
  async timerHealthy() {
    if (!this.ready) return false;
    const heartbeat = await this.domain.store.get('renewal-timer-checkpoint');
    this.timerReady = !!heartbeat && heartbeat.lastRunAt >= this.domain.clock() - RENEWAL_TIMER_MAX_AGE_MS;
    return this.timerReady;
  }
  async record(tx, state, subscriptionKey) {
    const id = 'renewal-job:' + hash(subscriptionKey), previous = await tx.get(id);
    const next = { ...previous, id, userId: state.userId, subscriptionKey,
      nextRenewalAt: state.nextRenewalAt ?? 0, nextRenewalPrice: state.nextRenewalPrice ?? 0,
      renewalCurrency: state.renewalCurrency ?? '',
      enabled: state.status === 'purchased' && state.autoRenew && !state.billingRetry,
      updatedAt: this.domain.clock() };
    next.cycle = cycle(next);
    if (previous?.cycle !== next.cycle) { next.attempts = 0; next.retryAt = 0; next.sentCycle = ''; }
    // Do not invent a future price/date from the last receipt or the expiry date.
    next.dueAt = next.nextRenewalAt ? next.nextRenewalAt - 7 * DAY : 0;
    tx.put(id, next, 'renewal-jobs');
  }
  async run() {
    requireThat(this.ready, 'REMINDERS_UNAVAILABLE', 503);
    const store = this.domain.store, started = Date.now();
    const checkpointKey = 'renewal-timer-checkpoint';
    let cursor = (await store.get(checkpointKey))?.cursor ?? '', processed = 0, failed = 0;
    // Paginated scans retain a cursor when the runtime budget is reached; no 256-row truncation.
    do {
      const page = await store.page('renewal-jobs', cursor, 25);
      let consumed = 0;
      for (const row of page) {
        cursor = row.key;
        consumed++;
        try { if (await this.process(row.value.id)) processed++; }
        catch { failed++; runtimeError('[RenewalReminders] delivery failed'); }
        if (Date.now() - started > 45000) break;
      }
      if (consumed === page.length && page.length < 25) { cursor = ''; break; }
    } while (Date.now() - started <= 45000);
    await store.transaction(async tx => {
      const previous = await tx.get(checkpointKey);
      tx.put(checkpointKey, { ...previous, cursor, lastRunAt: this.domain.clock(), processed, failed }, 'internal');
    });
    this.timerReady = true;
    return { processed, failed, complete: cursor === '' };
  }
  async process(id) {
    const { store } = this.domain;
    let job = await store.get(id), now = this.domain.clock();
    if (!job?.enabled || job.retryAt > now || job.dueAt > now ||
      job.sentCycle === job.cycle && job.nextRenewalAt > now || job.checkAt > now &&
      (job.sentCycle === job.cycle || job.dueAt > now)) return false;
    const lease = randomUUID();
    const acquired = await store.transaction(async tx => {
      const current = await tx.get(id);
      if (!current?.enabled || current.leaseUntil > now) return false;
      tx.put(id, { ...current, lease, leaseUntil: now + LEASE }, 'renewal-jobs'); return true;
    });
    if (!acquired) return false;
    try {
      const stored = await store.get(job.subscriptionKey);
      requireThat(stored?.userId === job.userId, 'ORDER_OWNER_IMMUTABLE', 403);
      const receipt = openReceipt(stored.receiptCipher, this.domain.config.purchaseBindingSecret, job.userId + '\n' + job.subscriptionKey);
      // Always requery and verify official signatures immediately before sending.
      await this.domain.verify(job.userId, { purchaseData: receipt, productType: 2 });
      job = await store.get(id); now = this.domain.clock();
      if (!job.enabled || job.sentCycle === job.cycle || job.dueAt > now) return false;
      requireThat(job.nextRenewalAt > now && job.nextRenewalPrice > 0 && /^[A-Z]{3}$/.test(job.renewalCurrency),
        'RENEWAL_PLAN_UNAVAILABLE', 503);
      const devices = (await store.list('notification-devices:' + job.userId)).filter(device => device.userId === job.userId && device.enabled && device.token);
      requireThat(devices.length > 0, 'NOTIFICATION_DEVICE_UNAVAILABLE', 503);
      const deliveryId = job.cycle;
      let accepted = 0;
      for (const device of devices) {
        const deliveryKey = 'renewal-delivery:' + hash(deliveryId + '\n' + device.id);
        const old = await store.get(deliveryKey);
        if (old?.acceptedAt) { accepted++; continue; }
        // Check current ownership after the provider request; a token may have changed accounts.
        const current = await store.get(device.id);
        if (!current?.enabled || current.userId !== job.userId || current.token !== device.token) continue;
        const currentJob = await store.get(id);
        if (!currentJob?.enabled || currentJob.cycle !== job.cycle || currentJob.lease !== lease || currentJob.leaseUntil <= this.domain.clock()) return false;
        const result = await this.sender.send(device.token, { deliveryId, renewalAt: job.nextRenewalAt,
          amount: job.nextRenewalPrice, currency: job.renewalCurrency, userId: job.userId });
        let earlyFailure = false;
        await store.transaction(async tx => {
          const prior = await tx.get(deliveryKey);
          const report = await tx.get('renewal-receipt:' + hash(deliveryId + '\n' + device.id));
          earlyFailure = report?.requestId === result.requestId && report.result === 14;
          tx.put(deliveryKey, { ...prior, id: deliveryKey, userId: job.userId, deviceId: device.id,
            deliveryId, acceptedAt: earlyFailure ? 0 : this.domain.clock(), providerRequestId: result.requestId,
            jobId: job.id, displayStatus: prior?.displayStatus ?? 'UNKNOWN', openedAt: prior?.openedAt ?? 0,
            ...(report?.requestId === result.requestId ? { deviceReportedAt: report.at, providerDeliveryResult: report.result,
              deliveredAt: report.result === 0 ? report.at : 0 } : {}) }, 'renewal-deliveries:' + job.userId);
        });
        requireThat(!earlyFailure, 'PUSH_DEVICE_RETRY', 503);
        accepted++;
      }
      requireThat(accepted > 0, 'NOTIFICATION_DEVICE_UNAVAILABLE', 503);
      await store.transaction(async tx => {
        const current = await tx.get(id);
        if (current?.cycle === job.cycle && current.lease === lease) {
          tx.put(id, { ...current, sentCycle: job.cycle, acceptedAt: this.domain.clock() }, 'renewal-jobs');
        }
      });
      if (job.nextRenewalAt - now < 6 * DAY) runtimeError('[RenewalReminders] late renewal notice');
      return true;
    } catch (error) {
      await store.transaction(async tx => {
        const current = await tx.get(id);
        if (current?.lease === lease) tx.put(id, { ...current, attempts: (current.attempts ?? 0) + 1,
          retryAt: this.domain.clock() + 15 * 60000, lastFailureAt: this.domain.clock() }, 'renewal-jobs');
      });
      throw error;
    } finally {
      await store.transaction(async tx => {
        const current = await tx.get(id);
        if (current?.lease === lease) tx.put(id, { ...current, leaseUntil: 0, checkAt: this.domain.clock() + 6 * 3600000 }, 'renewal-jobs');
      });
    }
  }
  async opened(userId, { deliveryId, installationId }) {
    requireThat(/^[a-f0-9]{64}$/.test(deliveryId ?? '') && installation(installationId), 'INVALID_NOTIFICATION_RECEIPT');
    for (const device of await this.domain.store.list('notification-devices:' + userId)) {
      if (!device.enabled || device.installationId !== installationId) continue;
      const key = 'renewal-delivery:' + hash(deliveryId + '\n' + device.id);
      await this.domain.store.transaction(async tx => {
        const receipt = await tx.get(key);
        if (receipt?.userId === userId && receipt.acceptedAt) tx.put(key,
          { ...receipt, openedAt: this.domain.clock(), displayStatus: 'OPENED' }, 'renewal-deliveries:' + userId);
      });
    }
    return {};
  }
}
