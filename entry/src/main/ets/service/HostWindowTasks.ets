// Runtime window identities, independent of the game catalogue and either libc.
// Titles are presentation only; Linux Steam provenance comes from the native broker.
export interface HostWindowTask {
  key: string;
  toplevelId: number;
  clientPid: number;
  sessionId: string;
  title: string;
  source: string;
  minimized: boolean;
  owner: number;
  desktop: boolean;
}

export type HostTaskListener = (tasks: HostWindowTask[]) => void;
export type HostTaskActivator = (task: HostWindowTask, current: () => boolean) => Promise<boolean>;

interface WindowEventData {
  clientPid?: number;
  sessionId?: string;
  title?: string;
  modal?: number;
  owner?: number;
}

export class HostWindowTasks {
  private static instance: HostWindowTasks | null = null;
  private windows: Map<number, HostWindowTask> = new Map<number, HostWindowTask>();
  private listeners: Set<HostTaskListener> = new Set<HostTaskListener>();
  private generation: number = 0;
  private steamSessions: Set<string> = new Set<string>();
  private request: number = 0;
  private active: string = '';
  private requestedKey: string = '';
  private tail: Promise<void> = Promise.resolve();
  private activator: HostTaskActivator | null = null;
  private launcher: (() => Promise<void>) | null = null;

  static getInstance(): HostWindowTasks {
    if (!HostWindowTasks.instance) HostWindowTasks.instance = new HostWindowTasks();
    return HostWindowTasks.instance;
  }

  setActivator(activator: HostTaskActivator): void { this.activator = activator; }
  setLauncher(launcher: () => Promise<void>): void { this.launcher = launcher; }
  async showLauncher(): Promise<void> {
    ++this.request;
    this.requestedKey = '';
    this.active = '';
    if (this.launcher) await this.launcher();
  }

  subscribe(listener: HostTaskListener): void {
    this.listeners.add(listener);
    listener(this.snapshot());
  }

  unsubscribe(listener: HostTaskListener): void { this.listeners.delete(listener); }

  snapshot(): HostWindowTask[] {
    return Array.from(this.windows.values()).filter((task: HostWindowTask): boolean =>
      !task.desktop && task.owner === 0).map((task: HostWindowTask): HostWindowTask => ({
        key: task.key, toplevelId: task.toplevelId, clientPid: task.clientPid, sessionId: task.sessionId,
        title: task.title, source: task.source, minimized: task.minimized, owner: task.owner, desktop: task.desktop
      }));
  }

  private publish(): void {
    this.listeners.forEach((listener: HostTaskListener): void => listener(this.snapshot()));
  }

  reset(): void {
    ++this.request;
    this.requestedKey = '';
    this.windows.clear();
    this.steamSessions.clear();
    this.active = '';
    this.publish();
  }

  // created is authoritative; modal/desktop metadata can precede it.
  onEvent(id: number, event: string, text: string, linuxSteam: boolean = false): void {
    if (id <= 0) return;
    let data: WindowEventData = {};
    try { data = (JSON.parse(text) as WindowEventData) || {}; } catch (_) {}
    let task = this.windows.get(id);
    if (event === 'destroyed') {
      if (!task) return;
      const wasActive = this.active === task.key;
      this.windows.delete(id);
      if (wasActive) this.active = '';
      this.publish();
      // Only a foreground Steam-launched game may return to Steam on exit.
      if (wasActive && task.sessionId.length > 0 && task.source === 'steam-game') {
        const steam = this.snapshot().find((item: HostWindowTask): boolean => item.source === 'steam-linux');
        if (steam) void this.activate(steam.key);
      }
      return;
    }
    if (!task && (event === 'created' || event === 'modal' || event === 'desktop_root')) {
      task = { key: `${id}:${++this.generation}`, toplevelId: id, clientPid: 0,
        sessionId: '', title: '运行窗口', source: 'wine', minimized: false, owner: 0, desktop: false };
      this.windows.set(id, task);
    }
    if (!task) return;
    if (event === 'created') {
      task.clientPid = data.clientPid ?? 0;
      task.sessionId = data.sessionId ?? '';
      if (linuxSteam) task.source = 'steam-linux';
      else if (this.steamSessions.has(task.sessionId)) task.source = 'steam-game';
      if (!task.desktop && task.owner === 0) this.active = task.key;
    } else if (event === 'title') {
      task.title = data.title || '运行窗口';
    } else if (event === 'minimized') {
      task.minimized = true;
      if (this.active === task.key) this.active = '';
    } else if (event === 'restored') {
      task.minimized = false;
    } else if (event === 'desktop_root') {
      task.desktop = true;
    } else if (event === 'modal') {
      task.owner = data.modal === 1 ? (data.owner ?? 0) : 0;
    } else return;
    this.publish();
  }

  registerSteamGame(sessionId: string): void {
    if (sessionId.length === 0) return;
    this.steamSessions.add(sessionId);
    this.windows.forEach((task: HostWindowTask): void => {
      if (task.sessionId === sessionId && task.source !== 'steam-linux') task.source = 'steam-game';
    });
    this.publish();
  }

  associateSession(id: number, sessionId: string): void {
    const task = this.windows.get(id);
    if (!task || task.source === 'steam-linux') return;
    task.sessionId = sessionId;
    if (this.steamSessions.has(sessionId)) task.source = 'steam-game';
    this.publish();
  }

  noteInteraction(id: number): void {
    // An interaction invalidates an in-flight automatic/user switch.
    const task = this.windows.get(id);
    if (task && this.active !== task.key) {
      if (this.requestedKey !== task.key) ++this.request;
      this.active = task.key;
    }
  }

  noteBackground(id: number): void {
    if (this.windows.get(id)?.key === this.active) this.active = '';
  }

  focusTarget(id: number): number {
    let target = id;
    const seen: Set<number> = new Set<number>();
    for (let n = 0; n < 16 && !seen.has(target); ++n) {
      seen.add(target);
      const modal = Array.from(this.windows.values()).find((task: HostWindowTask): boolean =>
        task.owner === target && !task.minimized);
      if (!modal) break;
      target = modal.toplevelId;
    }
    return target;
  }

  activate(key: string): Promise<boolean> {
    const serial = ++this.request;
    this.requestedKey = key;
    const task = this.snapshot().find((item: HostWindowTask): boolean => item.key === key);
    if (!task || !this.activator) return Promise.resolve(false);
    const current = (): boolean => serial === this.request && this.windows.get(task.toplevelId)?.key === key;
    const run = this.tail.then(async (): Promise<boolean> => {
      if (!current() || !this.activator) return false;
      const success = await this.activator(task, current);
      if (!current() || !success) return false;
      const live = this.windows.get(task.toplevelId);
      if (live) live.minimized = false;
      this.active = key;
      this.publish();
      return true;
    });
    // Serialize show/raise operations; a stale completion cannot overtake a new switch.
    const settled = run.finally((): void => {
      if (serial === this.request) this.requestedKey = '';
    });
    this.tail = settled.then((): void => {}, (): void => {});
    return settled;
  }

  hasForegroundTask(): boolean { return this.active.length > 0; }
}
