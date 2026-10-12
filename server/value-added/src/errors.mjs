export class ServiceError extends Error {
  constructor(code, status = 400) { super(code); this.code = code; this.status = status; }
}
export function requireThat(condition, code, status = 400) {
  if (!condition) throw new ServiceError(code, status);
}
export const hashKey = value => /^[a-f0-9]{64}$/.test(value);
