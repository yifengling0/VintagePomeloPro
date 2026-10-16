import { timingSafeEqual } from 'node:crypto';
import { requireThat } from './errors.mjs';

/** Deploy to a separate IAM-private function with ONLY a timer trigger. */
export function createRenewalTimer(domain, secret) {
  requireThat(typeof secret === 'string' && secret.length >= 32 && secret.length <= 256, 'TIMER_NOT_CONFIGURED', 503);
  return async event => {
    const provided = event?.userData;
    requireThat(event?.triggerType === 'TIMER' && typeof provided === 'string' &&
      Buffer.byteLength(provided) === Buffer.byteLength(secret) && timingSafeEqual(Buffer.from(provided), Buffer.from(secret)),
      'TIMER_ONLY', 403);
    return domain.reminders.run();
  };
}
