// Test-only serializable reference store. Never imported by the production entry point.
export class MemoryStore {
  constructor() { this.ready = true; this.rows = new Map(); this.queue = Promise.resolve(); }
  async get(key) { return structuredClone(this.rows.get(key)?.value ?? null); }
  async list(scope) { return [...this.rows.values()].filter(x => x.scope === scope).map(x => structuredClone(x.value)); }
  async page(scope, cursor = '', limit = 25) {
    return [...this.rows.entries()].filter(([key, row]) => row.scope === scope && key > cursor)
      .sort(([a], [b]) => a.localeCompare(b)).slice(0, limit).map(([key, row]) => ({ key, value: structuredClone(row.value) }));
  }
  transaction(action) {
    const operation = this.queue.then(async () => {
      const working = structuredClone(this.rows);
      const result = await action({ get: async key => structuredClone(working.get(key)?.value ?? null),
        put: (key, value, scope) => working.set(key, { value: structuredClone(value), scope }),
        delete: key => working.delete(key) });
      this.rows = working;
      return result;
    });
    this.queue = operation.catch(() => {});
    return operation;
  }
}
