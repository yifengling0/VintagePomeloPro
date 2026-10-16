import { createHash, createHmac, timingSafeEqual } from 'node:crypto';
import { requireThat } from './errors.mjs';
const hash = value => createHash('sha256').update(value).digest('hex');
/** Separate HTTPS callback. Huawei HMAC authentication is required; no account session is accepted. */
export function createPushReceiptCallback(domain, { username, secret, packageName }) {
  requireThat(typeof username === 'string' && username.length > 0 && typeof secret === 'string' && secret.length >= 24 &&
    typeof packageName === 'string' && packageName.length > 0, 'PUSH_RECEIPT_NOT_CONFIGURED', 503);
  const hmacKey = Buffer.from(secret, 'base64');
  requireThat(hmacKey.length >= 16 && hmacKey.toString('base64') === secret, 'INVALID_PUSH_RECEIPT_KEY', 503);
  return async event => {
    const headers = event?.headers ?? {};
    const header = headers['x-huawei-callback-id'] ?? headers['X-HUAWEI-CALLBACK-ID'];
    requireThat(typeof header === 'string' && header.length <= 512, 'INVALID_PUSH_RECEIPT', 403);
    const match = /^timestamp=(\d{13});\s*nonce=([a-zA-Z0-9-]{16,64});\s*value=([A-Za-z0-9+/]{43}=)$/.exec(header);
    requireThat(match && Math.abs(domain.clock() - Number(match[1])) <= 300000, 'INVALID_PUSH_RECEIPT', 403);
    const expected = createHmac('sha256', hmacKey).update(match[1] + match[2] + username).digest('base64');
    requireThat(timingSafeEqual(Buffer.from(match[3]), Buffer.from(expected)), 'INVALID_PUSH_RECEIPT', 403);
    requireThat(event.isBase64Encoded !== true && typeof event.body === 'string' && Buffer.byteLength(event.body) <= 65536,
      'INVALID_PUSH_RECEIPT');
    let payload; try { payload = JSON.parse(event.body); } catch { throw new Error('INVALID_PUSH_RECEIPT'); }
    requireThat(Array.isArray(payload.statuses) && payload.statuses.length <= 100, 'INVALID_PUSH_RECEIPT');
    for (const item of payload.statuses) requireThat(item.appPackageName === packageName && item.pushType === 0 &&
      /^[a-f0-9]{64}$/.test(item.biTag ?? '') && typeof item.token === 'string' && item.token.length <= 4096 &&
      typeof item.requestId === 'string' && item.requestId.length <= 256 && Number.isInteger(item.deliveryStatus?.result) &&
      Number.isSafeInteger(item.deliveryStatus?.timestamp) && item.deliveryStatus.timestamp <= domain.clock() + 60000,
      'INVALID_PUSH_RECEIPT');
    await domain.store.transaction(async tx => {
      const nonce = 'push-receipt-nonce:' + hash(match[2]);
      if (await tx.get(nonce)) return;
      for (const item of payload.statuses) {
        const deviceId = 'notification-device:' + hash(item.token);
        const suffix = hash(item.biTag + '\n' + deviceId), key = 'renewal-delivery:' + suffix;
        const report = { requestId: item.requestId, result: item.deliveryStatus.result, at: item.deliveryStatus.timestamp };
        tx.put('renewal-receipt:' + suffix, report, 'renewal-receipts');
        const receipt = await tx.get(key);
        if (!receipt || receipt.providerRequestId !== report.requestId) continue;
        tx.put(key, { ...receipt, deviceReportedAt: report.at, providerDeliveryResult: report.result,
          deliveredAt: report.result === 0 ? report.at : receipt.deliveredAt ?? 0 }, 'renewal-deliveries:' + receipt.userId);
        if ([2, 5, 6, 10].includes(report.result)) {
          const device = await tx.get(deviceId);
          if (device?.userId === receipt.userId) tx.put(deviceId, { ...device, enabled: false, updatedAt: domain.clock() },
            'notification-devices-retired');
        }
        if (report.result === 14) {
          if (receipt.jobId) {
            const current = await tx.get(receipt.jobId);
            if (current?.cycle === item.biTag) tx.put(receipt.jobId, { ...current, sentCycle: '', retryAt: domain.clock() + 900000 }, 'renewal-jobs');
          }
          tx.put(key, { ...receipt, acceptedAt: 0, deviceReportedAt: report.at, providerDeliveryResult: report.result },
            'renewal-deliveries:' + receipt.userId);
        }
      }
      tx.put(nonce, { at: domain.clock() }, 'push-receipt-nonces');
    });
    if (payload.statuses.some(item => item.deliveryStatus.result !== 0)) console.error('[RenewalReminders] device delivery failed');
    return { code: '0', message: 'success' };
  };
}
