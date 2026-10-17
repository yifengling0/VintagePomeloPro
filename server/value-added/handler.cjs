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

let timer;
exports.renewalHandler = function (event, context, callback) {
  if (!timer) timer = Promise.all([import('./src/main.mjs'), import('./src/renewal-timer.mjs')])
    .then(async ([main, adapter]) => adapter.createRenewalTimer(await main.loadService(), process.env.VP_RENEWAL_TIMER_SECRET))
    .catch(error => { timer = undefined; throw error; });
  timer.then(run => run(event)).then(result => callback(result)).catch(() => {
    console.error('[RenewalReminders] timer failed'); callback({ code: 'RENEWAL_TIMER_FAILED' });
  });
};

let receipts;
exports.pushReceiptHandler = function (event, context, callback) {
  if (!receipts) receipts = import('./src/push-receipts.mjs')
    .then(adapter => adapter.loadPushReceiptCallback())
    .catch(error => { receipts = undefined; throw error; });
  receipts.then(run => run(event)).then(body => callback({ statusCode: 200, headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body) })).catch(error => {
      const statusCode = error?.code === 'INVALID_PUSH_RECEIPT' ? (error.status === 403 ? 403 : 400) : 503;
      callback({ statusCode, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({
        code: statusCode === 503 ? 'RECEIPT_UNAVAILABLE' : 'RECEIPT_REJECTED' }) });
    });
};
