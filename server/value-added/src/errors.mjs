export class ServiceError extends Error {
  constructor(code, status = 400) { super(code); this.code = code; this.status = status; }
}
export function requireThat(condition, code, status = 400) {
  if (!condition) throw new ServiceError(code, status);
}
export const hashKey = value => /^[a-f0-9]{64}$/.test(value);

// Return only fixed categories or a bounded numeric SDK code. Provider messages,
// stack traces, identifiers and request data must never reach logs or clients.
export function unexpectedDiagnostic(error) {
  try {
    const code = error?.getCode?.() ?? error?.errorCode ?? error?.code;
    if ((typeof code === 'number' && Number.isSafeInteger(code) || typeof code === 'string') &&
      /^[0-9]{1,12}$/.test(String(code))) return String(code);
  } catch { /* An SDK accessor can itself throw. Keep the fixed fallback. */ }
  if (error instanceof TypeError) return 'TYPE_ERROR';
  if (error instanceof ReferenceError) return 'REFERENCE_ERROR';
  if (error instanceof SyntaxError) return 'SYNTAX_ERROR';
  if (error instanceof RangeError) return 'RANGE_ERROR';
  return 'UNEXPECTED_ERROR';
}
