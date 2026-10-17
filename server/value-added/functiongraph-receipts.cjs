// This entry is only for FunctionGraph + an HTTPS APIG trigger; never AGC's SDK trigger.
let entry;
exports.handler = function (event, context, callback) {
  if (!entry) entry = import('./src/push-receipt-function.mjs').then(adapter =>
    adapter.createFunctionGraphReceiptEntry(async () =>
      (await import('./src/push-receipts.mjs')).loadPushReceiptCallback()));
  void entry.then(run => run(event, context, callback)).catch(() => {
    entry = undefined;
    callback(null, JSON.stringify({ statusCode: 503, isBase64Encoded: false,
      headers: { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' },
      body: JSON.stringify({ code: 'RECEIPT_UNAVAILABLE' }) }));
  });
};
