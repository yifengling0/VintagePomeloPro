/** 错误 → 可读文案: Error 取 message, 其它对象才 stringify (Error 常序列化成 {}). */
export function describeSteamError(e: Object): string {
  const err = e as Error;
  if (err && typeof err.message === 'string' && err.message.length > 0) {
    return err.message;
  }
  const text = JSON.stringify(e);
  return text !== '{}' && text !== 'null' ? text : String(e);
}
