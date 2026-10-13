import { createServer } from 'node:http';
import { CHUNK_BYTES } from './save-manifest.mjs';
import { ServiceError, requireThat } from './errors.mjs';
async function body(request) {
  requireThat(request.headers['content-type']?.split(';')[0] === 'application/json', 'INVALID_CONTENT_TYPE', 415);
  const chunks = []; let size = 0;
  for await (const chunk of request) {
    size += chunk.length; requireThat(size <= 524288, 'REQUEST_TOO_LARGE', 413); chunks.push(chunk);
  }
  try { return JSON.parse(Buffer.concat(chunks).toString('utf8')); }
  catch { throw new ServiceError('INVALID_JSON'); }
}
async function chunkBody(request,expected) {
  requireThat(request.headers['content-type']==='application/octet-stream'&&
    /^[0-9]{1,8}$/.test(request.headers['content-length']??'')&&
    Number(request.headers['content-length'])===expected&&expected<=CHUNK_BYTES,'INVALID_SAVE_LENGTH',413);
  const chunks=[];let length=0;
  for await(const chunk of request){length+=chunk.length;requireThat(length<=expected,'INVALID_SAVE_LENGTH',413);chunks.push(chunk);}
  requireThat(length===expected,'INVALID_SAVE_LENGTH',413);return Buffer.concat(chunks,length);
}
export function createApiServer(domain) {
  // Local-process safety limit. Production reverse proxy must also apply per-IP and per-account limits.
  let minute = 0, count = 0, transfers = 0;
  return createServer({ requestTimeout: 30000, headersTimeout: 10000, maxHeaderSize: 8192 }, async (request, response) => {
    response.setHeader('Content-Type', 'application/json; charset=utf-8');
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    try {
      const current = Math.floor(Date.now() / 60000);
      if (current !== minute) { minute = current; count = 0; }
      requireThat(++count <= 300, 'RATE_LIMITED', 429);
      const url = new URL(request.url, 'http://localhost');
      requireThat(!url.search && !url.hash, 'INVALID_ROUTE', 404);
      const path = url.pathname, method = request.method;
      let result;
      if (path === '/v1/capabilities' && method === 'GET') result = domain.capabilities();
      else if (path === '/v1/auth/begin' && method === 'POST') { await body(request); result = await domain.begin(); }
      else if (path === '/v1/auth/exchange' && method === 'POST') result = await domain.exchange(await body(request));
      else {
        const match = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(request.headers.authorization ?? '');
        requireThat(match, 'SIGN_IN_REQUIRED', 401);
        const token = match[1], userId = await domain.authenticate(token);
        if (path === '/v1/auth/logout' && method === 'POST') { await body(request); result = await domain.logout(token); }
        else if (path === '/v1/entitlement' && method === 'GET') result = await domain.entitlement(userId);
        else if (path === '/v1/purchases/intent' && method === 'POST') result = await domain.intent(userId, await body(request));
        else if (path === '/v1/saves' && method === 'GET') result = await domain.saves.list(userId);
        else if (path === '/v1/saves/slots' && method === 'POST') result = await domain.saves.createSlot(userId,await body(request));
        else if (path === '/v1/saves/uploads' && method === 'POST') result = await domain.saves.begin(userId,await body(request));
        else if (path === '/v1/saves/cleanup' && method === 'POST') {await body(request);result=await domain.saves.cleanup(userId);}
        else if (path.startsWith('/v1/saves/')) {
          const upload=/^\/v1\/saves\/uploads\/([a-f0-9-]{36})\/(chunks\/([0-9]{1,2})|finalize|abort)$/.exec(path);
          const ticket=/^\/v1\/saves\/slots\/([a-f0-9-]{36})\/versions\/([a-f0-9-]{36})\/chunks\/([0-9]{1,2})$/.exec(path);
          const version=/^\/v1\/saves\/slots\/([a-f0-9-]{36})\/versions\/([a-f0-9-]{36})$/.exec(path);
          if(upload?.[3]!==undefined && method==='PUT') {
            // Reject oversized declarations BEFORE reserving a transfer or consuming request bytes.
            requireThat(request.headers['content-type']==='application/octet-stream'&&
              /^[0-9]{1,8}$/.test(request.headers['content-length']??'')&&Number(request.headers['content-length'])<=CHUNK_BYTES,
              'INVALID_SAVE_LENGTH',413);
            requireThat(transfers<2,'SAVE_TRANSFER_BUSY',429);transfers++;
            try{const part=Number(upload[3]),lease=await domain.saves.acquireChunk(userId,upload[1],part);
              const bytes=await chunkBody(request,lease.expected.size);
              result=await domain.saves.upload(userId,upload[1],part,bytes,lease);
            }finally{transfers--;}
          }else if(upload?.[2]==='finalize'&&method==='POST'){await body(request);result=await domain.saves.finalize(userId,upload[1]);}
          else if(upload?.[2]==='abort'&&method==='POST'){await body(request);result=await domain.saves.abort(userId,upload[1]);}
          else if(ticket&&method==='GET')result=await domain.saves.downloadTicket(userId,ticket[1],ticket[2],Number(ticket[3]));
          else if(version&&method==='GET')result=await domain.saves.download(userId,version[1],version[2]);
          else if(version&&method==='DELETE')result=await domain.saves.deleteVersion(userId,version[1],version[2],await body(request));
          else throw new ServiceError('NOT_FOUND',404);
        }
        else if (/^\/v1\/settings\/(tablet|desktop)$/.test(path)) {
          const deviceClass = path.split('/')[3];
          if (method === 'GET') result = await domain.getPreferences(userId,deviceClass);
          else if (method === 'PUT') result = await domain.putPreferences(userId,deviceClass,await body(request));
          else throw new ServiceError('NOT_FOUND',404);
        }
        else if (path === '/v1/purchases/verify' && method === 'POST') result = await domain.verify(userId, await body(request));
        else {
          const route = /^\/v1\/profiles\/(tablet|desktop)(?:\/([a-zA-Z0-9_-]{1,128}))?$/.exec(path);
          requireThat(route, 'NOT_FOUND', 404);
          if (!route[2] && method === 'GET') result = await domain.listProfiles(userId, route[1]);
          else if (route[2] && method === 'GET') result = await domain.getProfile(userId, route[1], route[2]);
          else if (route[2] && method === 'PUT') result = await domain.putProfile(userId, route[1], route[2], await body(request));
          else throw new ServiceError('NOT_FOUND', 404);
        }
      }
      response.writeHead(200); response.end(JSON.stringify(result));
    } catch (error) {
      // Never log token, authorization code, receipt, request body, provider exception, or credentials.
      response.setHeader('Connection','close');
      response.writeHead(error instanceof ServiceError ? error.status : 500);
      response.end(JSON.stringify({ code: error instanceof ServiceError ? error.code : 'SERVICE_ERROR' }));
    }
  });
}
