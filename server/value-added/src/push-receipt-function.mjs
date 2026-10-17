import { ServiceError, requireThat } from './errors.mjs';

const response = (statusCode, body) => ({ statusCode, isBase64Encoded: false,
  headers: { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' },
  body: JSON.stringify(body) });

/** FunctionGraph APIG event contract. The gateway exposes only POST /push/receipts over HTTPS.
 * APIG may base64-encode JSON; decode exactly once before the Huawei HMAC receiver.
 * loadCallback initializes only CloudDB and callback credentials, never payment keys.
 */
export function createPushReceiptFunction(loadCallback) {
  let callback;
  return async event => {
    try {
      if (event?.httpMethod !== 'POST' || event?.path !== '/push/receipts') {
        return response(404, { code: 'NOT_FOUND' });
      }
      const headers = {};
      requireThat(event.headers && typeof event.headers === 'object' && !Array.isArray(event.headers),
        'INVALID_PUSH_RECEIPT', 403);
      for (const [key, value] of Object.entries(event.headers)) {
        const name = key.toLowerCase();
        requireThat(!(name in headers) && typeof value === 'string', 'INVALID_PUSH_RECEIPT', 403);
        headers[name] = value;
      }
      requireThat(headers['x-huawei-callback-id']?.length > 0 &&
        headers['x-huawei-callback-id'].length <= 512, 'INVALID_PUSH_RECEIPT', 403);
      requireThat(!headers['content-encoding'] || headers['content-encoding'] === 'identity',
        'UNSUPPORTED_ENCODING', 415);
      requireThat(typeof event.body === 'string' &&
        (event.isBase64Encoded === undefined || typeof event.isBase64Encoded === 'boolean'), 'INVALID_PUSH_RECEIPT');
      let body = event.body;
      if (event.isBase64Encoded === true) {
        requireThat(body.length <= 87384, 'RECEIPT_TOO_LARGE', 413);
        const bytes = Buffer.from(body, 'base64');
        requireThat(bytes.toString('base64') === body, 'INVALID_PUSH_RECEIPT');
        requireThat(bytes.length <= 65536, 'RECEIPT_TOO_LARGE', 413);
        try { body = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
        catch { throw new ServiceError('INVALID_PUSH_RECEIPT'); }
      }
      requireThat(Buffer.byteLength(body) <= 65536, 'RECEIPT_TOO_LARGE', 413);
      if (!callback) callback = Promise.resolve().then(loadCallback).catch(error => {
        callback = undefined; throw error;
      });
      return response(200, await (await callback)({ headers, body, isBase64Encoded: false }));
    } catch (error) {
      // A storage/init failure must remain retryable; never acknowledge a lost receipt.
      const status = error instanceof ServiceError && [400, 403, 413, 415].includes(error.status) ? error.status : 503;
      if (status === 503) console.error('[RenewalReminders] receipt processing unavailable');
      return response(status, { code: status === 503 ? 'RECEIPT_UNAVAILABLE' : 'RECEIPT_REJECTED' });
    }
  };
}

/** FunctionGraph APIG requires a JSON string in callback(error, result).
 * AGC handler.cjs instead uses its single-argument callback contract.
 */
export function createFunctionGraphReceiptEntry(loadCallback) {
  const run = createPushReceiptFunction(loadCallback);
  return function (event, context, callback) {
    void run(event).then(result => callback(null, JSON.stringify(result)));
  };
}
