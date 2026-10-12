// Test-only serializable reference store. Never imported by the production entry point.
export class MemoryStore {
  constructor() { this.ready = true; this.rows = new Map(); this.queue = Promise.resolve(); }
  async get(key) { return structuredClone(this.rows.get(key)?.value ?? null); }
  async list(scope) { return [...this.rows.values()].filter(x => x.scope === scope).map(x => structuredClone(x.value)); }
  transaction(action) {
    const operation = this.queue.then(async () => {
      const working = structuredClone(this.rows);
      const result = await action({ get: async key => structuredClone(working.get(key)?.value ?? null),
        put: (key, value, scope) => working.set(key, { value: structuredClone(value), scope }) });
      this.rows = working;
      return result;
    });
    this.queue = operation.catch(() => {});
    return operation;
  }
}
