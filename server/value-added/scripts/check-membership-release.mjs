import { readFile } from 'node:fs/promises';
const root = new URL('../../../', import.meta.url);
const read = async path => JSON.parse(await readFile(path, 'utf8'));
const config = await read(new URL('entry/src/main/resources/rawfile/value_added_config.json', root));
const products = await read(new URL('../config/product-catalog.json', import.meta.url));
const failures = [];
const check = (ok, message) => { if (!ok) failures.push(message); };
const loadEvidence = async (path, label) => {
  if (!path) { failures.push(label + ' missing'); return {}; }
  try { return await read(path); } catch { failures.push(label + ' unreadable'); return {}; }
};
const [release, reminder, storage] = await Promise.all([
  loadEvidence(process.argv[2], 'NORMAL release evidence'), loadEvidence(process.argv[3], 'Push/timer/device evidence'),
  loadEvidence(process.argv[4], 'CloudDB pagination evidence')
]);
check(!!config.membershipOperatorName?.trim(), 'Registered operator name missing from bundled config');
check(release.operatorName === config.membershipOperatorName && !!release.operatorName, 'Operator differs from accepted release record');
check(release.environment === 'NORMAL' && release.appId === config.accountClientId, 'NORMAL application identity not verified');
for (const field of ['normalPaymentVerified', 'backupAccepted', 'supportVerified', 'cancelInAppVerified', 'agreementsReadVerified']) {
  check(release[field] === true, 'Release evidence incomplete: ' + field);
}
const single = products.find(p => p.id === config.membershipOneTimeProductId && p.type === 3);
const automatic = products.find(p => p.id === config.membershipProductId && p.type === 2);
if (config.membershipOneTimePurchases === true) {
  check(single?.purchasesEnabled !== false && single?.policyVersion === 'calendar-month-v1' && single.prices?.CNY?.includes(500), 'Independent CNY 5 calendar-month product missing');
  check(release.oneTimeProductId === single?.id, 'NORMAL single-month SKU not verified');
} else {
  check(single?.purchasesEnabled === false, 'Historical single-month SKU must be closed to new purchases');
}
check(automatic?.subscriptionGroupId === config.membershipSubscriptionGroupId && automatic?.prices?.CNY?.includes(500), 'Monthly subscription group/price mismatch');
check(automatic?.purchasesEnabled !== false && release.membershipProductId === automatic?.id, 'NORMAL subscription SKU not verified');
check(reminder.appId === config.accountClientId && typeof reminder.projectId === 'string' && !!reminder.projectId, 'Push application/project mismatch');
check(reminder.notificationCategory === 'ACCOUNT', 'Membership renewal notices require the ACCOUNT category');
for (const field of ['timerActive', 'deviceDisplayed', 'deepLinkOpened', 'accountCategoryEligible', 'receiptVerified', 'paginationVerified',
  'advanceNoticeVerified', 'cancelStopsVerified', 'permissionClosedVerified', 'dedupVerified', 'retryAlarmVerified']) {
  check(reminder[field] === true, 'Reminder evidence incomplete: ' + field);
}
check(storage.paginationVerified === true && storage.rows >= 300 && typeof storage.fingerprint === 'string', 'CloudDB ordered pagination not verified');
for (const [label, evidence] of [['release', release], ['reminder', reminder], ['storage', storage]]) {
  check(Number.isSafeInteger(evidence.testedAt) && evidence.testedAt > 0 && evidence.testedAt <= Date.now(), label + ' evidence timestamp missing/invalid');
}
if (failures.length) { console.error(failures.join('\n')); process.exitCode = 1; }
else console.log('Membership release evidence/configuration checks PASS. Verify the final signed package against these records before submission.');
