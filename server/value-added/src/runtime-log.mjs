import { AsyncLocalStorage } from 'node:async_hooks';

const invocationLogger = new AsyncLocalStorage();

export function withRuntimeLogger(logger, work) {
  return invocationLogger.run(logger, work);
}

function write(level, message) {
  const logger = invocationLogger.getStore() ?? globalThis.logger;
  if (typeof logger?.[level] === 'function') logger[level](message);
  else console[level](message);
}

export const runtimeInfo = message => write('info', message);
export const runtimeError = message => write('error', message);
