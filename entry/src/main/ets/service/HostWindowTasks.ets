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

interface PendingSteamGameLaunch { appId: string; intent: number; ticket: number; expiresAt: number; sessionId: string; promptKey: string; }

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
  private endedSteamSessions: Set<string> = new Set<string>();
  private pendingSteamReturn: string = '';
  private pendingSteamLaunch: PendingSteamGameLaunch | null = null;
  private request: number = 0;
  private foregroundChoice: number = 0;
  private active: string = '';
  private launcherForeground: boolean = false;
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
  markLauncherForeground(): void {
    ++this.request; ++this.foregroundChoice; this.requestedKey = ''; this.active = ''; this.launcherForeground = true;
    this.pendingSteamReturn = ''; this.pendingSteamLaunch = null;
  }
  markDesktopForeground(): void { this.launcherForeground = false; }
  isLauncherForeground(): boolean { return this.launcherForeground; }
  // Capture before asynchronous startup/dispatch, not after it completes.
  foregroundRevision(): number { return this.foregroundChoice; }
  async showLauncher(): Promise<void> {
    this.markLauncherForeground();
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
    this.endedSteamSessions.clear();
    this.pendingSteamReturn = ''; this.pendingSteamLaunch = null;
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
      // A splash/menu can disappear while the game is still running. Wait
      // for confirmed process completion and all its windows to disappear.
      if (wasActive && !this.launcherForeground && task.source === 'steam-game' &&
        (this.steamSessions.has(task.sessionId) || this.endedSteamSessions.has(task.sessionId))) {
        const sessionId = task.sessionId;
        this.pendingSteamReturn = sessionId;
        const peer = this.snapshot().find((item: HostWindowTask): boolean =>
          item.sessionId === sessionId && !item.minimized);
        if (peer) this.active = peer.key;
      }
      this.publish();
      this.returnAfterSteamGame(task.sessionId);
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
      if (!task.desktop && task.owner === 0) {
        this.pendingSteamReturn = ''; this.active = task.key;
      }
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
    if (event === 'created') this.activateExpectedSteamGame(task.sessionId);
  }

  // Native-child startup can publish a Wayland window before the broker's
  // ancestry record becomes visible. Retry provenance without replaying a
  // created event (which would also reset window focus/session metadata).
  refreshLinuxSteamOwnership(ownsProcess: (pid: number) => boolean): void {
    let changed = false;
    this.windows.forEach((task: HostWindowTask): void => {
      if (task.source !== 'steam-linux' && task.clientPid > 0 && ownsProcess(task.clientPid)) {
        task.source = 'steam-linux'; changed = true;
      }
    });
    if (changed) this.publish();
  }

  // An explicit Deck Play request may wait for a real game window without
  // raising Steam. The next user foreground choice cancels this intent.
  expectSteamGame(appId: string): number {
    const ticket = ++this.request;
    this.requestedKey = ''; this.pendingSteamReturn = '';
    this.pendingSteamLaunch = { appId, intent: ticket, ticket, expiresAt: Date.now() + 120000, sessionId: '', promptKey: '' };
    return ticket;
  }

  cancelExpectedSteamGame(ticket: number): void {
    if (this.pendingSteamLaunch?.intent === ticket) this.pendingSteamLaunch = null;
  }

  private activateExpectedSteamGame(sessionId: string): void {
    const pending = this.pendingSteamLaunch;
    if (!pending) return;
    if (pending.expiresAt < Date.now() || pending.ticket !== this.request) {
      this.pendingSteamLaunch = null; return;
    }
    if (!sessionId || pending.sessionId !== sessionId) return;
    const game = this.snapshot().find((task: HostWindowTask): boolean =>
      task.sessionId === sessionId && task.source === 'steam-game' && !task.minimized);
    if (!game) return;
    this.pendingSteamLaunch = null; // Exactly one restore, including splash replacement.
    void this.activate(game.key).catch((): boolean => false);
  }

  registerSteamGame(sessionId: string, appId: string = ''): void {
    if (sessionId.length === 0) return;
    this.steamSessions.add(sessionId);
    if (appId && this.pendingSteamLaunch?.appId === appId) this.pendingSteamLaunch.sessionId = sessionId;
    this.windows.forEach((task: HostWindowTask): void => {
      if (task.sessionId === sessionId && task.source !== 'steam-linux') task.source = 'steam-game';
    });
    this.publish();
    this.activateExpectedSteamGame(sessionId);
  }

  finishSteamGame(sessionId: string, confirmedExit: boolean = true): void {
    this.steamSessions.delete(sessionId);
    if (this.pendingSteamLaunch?.sessionId === sessionId) this.pendingSteamLaunch = null;
    if (confirmedExit) this.endedSteamSessions.add(sessionId);
    else {
      this.endedSteamSessions.delete(sessionId);
      if (this.pendingSteamReturn === sessionId) this.pendingSteamReturn = '';
    }
    this.returnAfterSteamGame(sessionId);
  }

  private returnAfterSteamGame(sessionId: string): void {
    if (!this.endedSteamSessions.has(sessionId) || Array.from(this.windows.values()).some(
      (task: HostWindowTask): boolean => task.sessionId === sessionId && !task.desktop)) return;
    this.endedSteamSessions.delete(sessionId);
    const shouldReturn = this.pendingSteamReturn === sessionId && !this.launcherForeground && this.active.length === 0;
    if (this.pendingSteamReturn === sessionId) this.pendingSteamReturn = '';
    if (shouldReturn) {
      const steam = this.snapshot().find((item: HostWindowTask): boolean => item.source === 'steam-linux');
      if (steam) void this.activate(steam.key);
    }
  }

  associateSession(id: number, sessionId: string): void {
    const task = this.windows.get(id);
    if (!task || task.source === 'steam-linux') return;
    task.sessionId = sessionId;
    if (this.steamSessions.has(sessionId)) task.source = 'steam-game';
    this.publish();
    this.activateExpectedSteamGame(sessionId);
  }

  noteInteraction(id: number): void {
    // An interaction invalidates an in-flight automatic/user switch.
    const task = this.windows.get(id);
    if (task) {
      this.pendingSteamReturn = '';
      const pending = this.pendingSteamLaunch;
      // Confirming the surfaced launch dialog is part of the same Play action.
      // Any other task interaction still cancels its one-shot game handoff.
      if (!pending || pending.promptKey !== task.key || pending.ticket !== this.request ||
        pending.expiresAt < Date.now() || pending.sessionId) this.pendingSteamLaunch = null;
    }
    if (task) {
      ++this.foregroundChoice;
      // Clicking the currently active task also cancels an in-flight raise of
      // a different task. Active identity alone cannot detect that choice.
      if (this.requestedKey !== task.key) ++this.request;
      if (this.pendingSteamLaunch) this.pendingSteamLaunch.ticket = this.request;
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

  activate(key: string): Promise<boolean> { return this.activateTask(key, false); }

  // A newly activated native Steam dialog can require input before a game
  // exists. Honor only the still-current Play request, retaining its one-shot
  // game activation. Explicit navigation or a started game cancels this path.
  activateSteamLaunchPrompt(key: string, sessionId: string): Promise<boolean> {
    const task = this.snapshot().find((item: HostWindowTask): boolean => item.key === key);
    const pending = this.pendingSteamLaunch;
    if (!task || (task.source !== 'steam-linux' && (!sessionId || task.sessionId !== sessionId)) ||
      !pending || pending.ticket !== this.request || pending.expiresAt < Date.now() || pending.sessionId) return Promise.resolve(false);
    pending.promptKey = key;
    return this.activateTask(key, true);
  }

  private activateTask(key: string, preserveLaunch: boolean): Promise<boolean> {
    const pending = preserveLaunch ? this.pendingSteamLaunch : null;
    if (!preserveLaunch) ++this.foregroundChoice;
    this.pendingSteamReturn = '';
    if (!preserveLaunch) this.pendingSteamLaunch = null;
    const serial = ++this.request;
    if (pending) pending.ticket = serial;
    this.requestedKey = key;
    const task = this.snapshot().find((item: HostWindowTask): boolean => item.key === key);
    if (!task || !this.activator) return Promise.resolve(false);
    const current = (): boolean => serial === this.request && this.windows.get(task.toplevelId)?.key === key &&
      (!preserveLaunch || (pending !== null && this.pendingSteamLaunch === pending && pending.ticket === serial &&
        pending.expiresAt >= Date.now() && !pending.sessionId));
    const run = this.tail.then(async (): Promise<boolean> => {
      if (!current() || !this.activator) return false;
      const success = await this.activator(task, current);
      if (!current() || !success) return false;
      const live = this.windows.get(task.toplevelId);
      if (live) live.minimized = false;
      this.launcherForeground = false; this.active = key;
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

  isSteamGameForeground(): boolean {
    return !this.launcherForeground && Array.from(this.windows.values()).some(
      (task: HostWindowTask): boolean => task.key === this.active && task.source === 'steam-game');
  }

  hasForegroundTask(): boolean { return !this.launcherForeground && this.active.length > 0; }
}
