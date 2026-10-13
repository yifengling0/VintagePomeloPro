import { createServer } from 'node:http';
import { createApiDispatcher } from './api-dispatcher.mjs';

export function createApiServer(domain) {
  const dispatch = createApiDispatcher(domain);
  return createServer({ requestTimeout: 30000, headersTimeout: 10000, maxHeaderSize: 8192 }, async (request, response) => {
    const result = await dispatch(request);
    response.setHeader('Content-Type', 'application/json; charset=utf-8');
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    if (result.status >= 400) response.setHeader('Connection', 'close');
    response.writeHead(result.status);
    response.end(result.body);
  });
}
