// Huawei calls callback(result), not the Node callback(error, result) convention.
let service;
async function run(event) {
  if (!service) {
    service = Promise.all([import('./src/main.mjs'), import('./src/cloud-function.mjs')])
      .then(async ([main, adapter]) => adapter.createCloudFunction(await main.loadService()))
      .catch(error => { service = undefined; throw error; });
  }
  return (await service)(event);
}
exports.myHandler = function (event, context, callback) {
  // Do not return the Promise: Huawei may select its resolved value instead of
  // the callback result. This entry uses only the single-argument callback contract.
  run(event).catch(error => {
    const codes = ['ENOENT', 'EACCES', 'MODULE_NOT_FOUND', 'ERR_MODULE_NOT_FOUND', 'NOT_CONFIGURED', 'NODE_RUNTIME_NOT_SUPPORTED',
      'CLOUDDB_NOT_VERIFIED', 'CLOUDDB_LOCK_MISSING', 'STORAGE_TIMEOUT'];
    let diagnosticCode = codes.includes(error?.code) ? error.code :
      error?.name === 'TypeError' ? 'TYPE_ERROR' : error?.name === 'SyntaxError' ? 'SYNTAX_ERROR' : 'INITIALIZATION_FAILED';
    try {
      const sdkCode = error?.getCode?.() ?? error?.errorCode ?? error?.code;
      if (/^\d{1,12}$/.test(String(sdkCode))) diagnosticCode = String(sdkCode);
    } catch { /* Preserve only the fixed fallback. */ }
    return { protocol: 1, status: 503, body: JSON.stringify({ code: 'SERVICE_UNAVAILABLE', diagnosticCode }) };
  })
    .then(result => { callback(result); });
};
