import { createApiDispatcher } from './api-dispatcher.mjs';
import { requireThat, ServiceError, unexpectedDiagnostic } from './errors.mjs';

// This transport carries JSON only. 4 MiB save chunks must not be base64-expanded
// into an unverified gateway limit; keep saves closed until binary transport is qualified.
export const FUNCTION_REQUEST_BYTES = 768 * 1024;
export const FUNCTION_RESPONSE_BYTES = 1024 * 1024;
export function createCloudFunction(domain) {
  const dispatch = createApiDispatcher(domain);
  return async event => {
    try {
      // CloudFoundation's HTTP trigger wraps the SDK data in event.body. Console
      // tests can supply the protocol directly. Transport headers never supply identity.
      if (event && typeof event === 'object' && event.protocol === undefined && Object.hasOwn(event, 'body')) {
        requireThat(event.isBase64Encoded !== true &&
          (typeof event.body === 'string' || event.body && typeof event.body === 'object'), 'INVALID_REQUEST');
        requireThat(Buffer.byteLength(typeof event.body === 'string' ? event.body : JSON.stringify(event.body)) <=
          FUNCTION_REQUEST_BYTES, 'REQUEST_TOO_LARGE', 413);
        if (typeof event.body === 'string') {
          try { event = JSON.parse(event.body); }
          catch { throw new ServiceError('INVALID_REQUEST', 400); }
        } else event = event.body;
      }
      requireThat(event && typeof event === 'object' && !Array.isArray(event), 'INVALID_REQUEST');
      requireThat(Object.keys(event).every(key => ['protocol', 'method', 'path', 'authorization', 'body'].includes(key)) &&
        event.protocol === 1 && ['GET', 'POST', 'PUT', 'DELETE'].includes(event.method) &&
        typeof event.path === 'string' && /^\/v1\/[a-zA-Z0-9/_-]{1,240}$/.test(event.path) &&
        typeof event.authorization === 'string' && event.authorization.length <= 64 &&
        typeof event.body === 'string', 'INVALID_REQUEST');
      requireThat(Buffer.byteLength(JSON.stringify(event)) <= FUNCTION_REQUEST_BYTES &&
        Buffer.byteLength(event.body) <= 524288, 'REQUEST_TOO_LARGE', 413);
      requireThat(event.method !== 'GET' || event.body === '', 'INVALID_REQUEST');
      requireThat(!event.path.startsWith('/v1/saves'), 'CLOUD_SAVE_TRANSPORT_UNAVAILABLE', 503);
      const request = { url: event.path, method: event.method,
        headers: { authorization: event.authorization, 'content-type': 'application/json' },
        async *[Symbol.asyncIterator]() { if (event.body) yield Buffer.from(event.body); } };
      const result = await dispatch(request);
      if (event.path === '/v1/capabilities' && result.status === 200) {
        const capabilities = JSON.parse(result.body);
        capabilities.cloudSaves = false;
        result.body = JSON.stringify(capabilities);
      }
      requireThat(Buffer.byteLength(result.body) <= FUNCTION_RESPONSE_BYTES, 'RESPONSE_TOO_LARGE', 503);
      return { protocol: 1, ...result };
    } catch (error) {
      return { protocol: 1, status: error instanceof ServiceError ? error.status : 500,
        body: JSON.stringify(error instanceof ServiceError ? { code: error.code } :
          { code: 'SERVICE_ERROR', diagnosticCode: unexpectedDiagnostic(error) }) };
    }
  };
}
