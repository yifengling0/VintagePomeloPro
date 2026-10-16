import { constants, createPrivateKey, sign } from 'node:crypto';
import { requireThat } from './errors.mjs';

export const MEMBERSHIP_ACTION = 'com.vintagepomelo.action.MEMBERSHIP';
const encode = value => Buffer.from(JSON.stringify(value)).toString('base64url');
/** HarmonyOS NEXT Push v3 uses a service-account JWT, never the Android OAuth API. */
export class HuaweiPushSender {
  constructor({ credential = {}, projectId = '', receiptId = '', verified = false, fetcher = fetch, clock = Date.now } = {}) {
    this.credential = credential; this.projectId = projectId; this.fetcher = fetcher; this.clock = clock;
    this.receiptId = receiptId;
    this.ready = verified && /^[0-9]{5,32}$/.test(projectId) && credential.project_id === projectId &&
      typeof credential.key_id === 'string' && typeof credential.sub_account === 'string' &&
      typeof credential.private_key === 'string';
    if (this.ready) {
      this.key = createPrivateKey(credential.private_key);
      requireThat(this.key.asymmetricKeyType === 'rsa', 'INVALID_PUSH_CREDENTIAL', 503);
    }
  }
  jwt() {
    const now = Math.floor(this.clock() / 1000);
    if (this.cached && this.expires > now + 60) return this.cached;
    const content = encode({ alg: 'PS256', typ: 'JWT', kid: this.credential.key_id }) + '.' +
      encode({ iss: this.credential.sub_account, aud: 'https://oauth-login.cloud.huawei.com/oauth2/v3/token', iat: now, exp: now + 3600 });
    this.cached = content + '.' + sign('sha256', Buffer.from(content), {
      key: this.key, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32
    }).toString('base64url');
    this.expires = now + 3600; return this.cached;
  }
  async send(token, { deliveryId, renewalAt, amount, currency, userId }) {
    requireThat(this.ready, 'REMINDERS_UNAVAILABLE', 503);
    const date = new Intl.DateTimeFormat('zh-CN', { timeZone: 'Asia/Shanghai', year: 'numeric', month: '2-digit',
      day: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false }).format(renewalAt);
    const price = new Intl.NumberFormat('zh-CN', { style: 'currency', currency }).format(amount / 100);
    const response = await this.fetcher('https://push-api.cloud.huawei.com/v3/' + this.projectId + '/messages:send', {
      method: 'POST', signal: AbortSignal.timeout(10000), headers: { 'Content-Type': 'application/json',
        Authorization: 'Bearer ' + this.jwt(), 'push-type': '0' },
      body: JSON.stringify({ payload: { notification: { category: 'SUBSCRIPTION', title: '旧柚 VIP 续费提醒',
        body: '连续包月预计于 ' + date + '（北京时间）续费 ' + price + '，延长一个月。华为可能提前24小时扣款；点击管理或取消自动续费。',
        clickAction: { actionType: 1, action: MEMBERSHIP_ACTION, data: { deliveryId } },
        profileId: userId, style: 0, notifyId: parseInt(deliveryId.slice(0, 7), 16), appMessageId: deliveryId } },
        target: { token: [token] }, pushOptions: { biTag: deliveryId, ...(this.receiptId ? { receiptId: this.receiptId } : {}),
          ttl: Math.min(86400, Math.max(1, Math.floor((renewalAt - this.clock()) / 1000))) } })
    });
    // HTTP 200 alone is not acceptance. Do not log the response (it may contain tokens).
    let result; try { result = await response.json(); } catch { throw new Error('PUSH_UNAVAILABLE'); }
    requireThat(response.ok && result.code === '80000000' && typeof result.requestId === 'string', 'PUSH_UNAVAILABLE', 503);
    return { requestId: result.requestId.slice(0, 256) };
  }
}
