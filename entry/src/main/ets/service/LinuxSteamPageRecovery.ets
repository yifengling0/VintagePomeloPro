import { fileIo as fs } from '@kit.CoreFileKit';

// Observe only new CEF failures in this session. Never relax Chromium's origin
// checks or restart Steam: the caller can request a browser-initiated library
// navigation, which also works after a renderer has been terminated.
export class LinuxSteamPageRecovery {
  private path: string = '';
  private offset: number = 0;
  private inode: string = '';
  private partial: string = '';
  private lastFailure: string = '';
  private pending: boolean = false;
  private nextRead: number = 0;
  private nextAttempt: number = 0;
  private attempts: number = 0;
  private source: string = '';
  private sourceAttempts: number = 0;

  start(path: string): void {
    this.path = path; this.offset = 0; this.inode = ''; this.partial = '';
    this.lastFailure = ''; this.pending = false; this.nextRead = 0;
    this.nextAttempt = 0; this.attempts = 0;
    this.source = ''; this.sourceAttempts = 0;
    // Existing logs can contain yesterday's crash. They cannot trigger a new
    // session's navigation, even when Steam appends to the same file.
    try {
      const stat = fs.lstatSync(path);
      if (stat.isFile() && !stat.isSymbolicLink()) { this.offset = stat.size; this.inode = stat.ino.toString(); }
    } catch (_) {}
  }

  poll(): void {
    if (!this.path || Date.now() < this.nextRead) return;
    this.nextRead = Date.now() + 1000;
    try {
      const stat = fs.lstatSync(this.path);
      if (!stat.isFile() || stat.isSymbolicLink()) return;
      const file = fs.openSync(this.path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
      try {
        const current = fs.statSync(file.fd);
        if (!current.isFile()) return;
        if (current.ino.toString() !== this.inode || current.size < this.offset) {
          this.inode = current.ino.toString(); this.offset = 0; this.partial = '';
        }
        // Bounded work on the UI thread, even if verbose client logging grows.
        if (current.size - this.offset > 65536) { this.offset = current.size - 65536; this.partial = ''; }
        const length = Math.min(65536, current.size - this.offset);
        if (length <= 0) return;
        const buffer = new ArrayBuffer(length);
        const count = fs.readSync(file.fd, buffer, { offset: this.offset, length });
        if (count <= 0) return;
        this.offset += count;
        // The diagnostic marker is ASCII. Do not decode a partial UTF-8 log
        // record or retain account/URL content from other CEF messages.
        let text = this.partial;
        const bytes = new Uint8Array(buffer, 0, count);
        for (let i = 0; i < bytes.length; i++) text += bytes[i] < 128 ? String.fromCharCode(bytes[i]) : '?';
        const lines = text.split('\n');
        this.partial = (lines.pop() ?? '').slice(-512);
        for (const line of lines) {
          const failure = /^\[(\d+):\d+:\d+\/\d+\.\d+:ERROR:bad_message\.cc\(\d+\)\] Terminating renderer for bad IPC message, reason 213\r?$/.exec(line);
          if (!failure || line === this.lastFailure) continue;
          // Big Picture legitimately replaces the browser helper. A later
          // round-trip must not inherit the previous helper's exhausted quota.
          if (failure[1] !== this.source) {
            this.source = failure[1]; this.sourceAttempts = 0; this.nextAttempt = 0;
          }
          this.lastFailure = line; this.pending = true;
        }
      } finally { fs.closeSync(file); }
    } catch (_) { /* Logs are optional; unavailable evidence never restarts a client. */ }
  }

  needed(): boolean { return this.pending && this.sourceAttempts < 2 && Date.now() >= this.nextAttempt; }
  take(): number {
    if (!this.needed()) return 0;
    this.pending = false; this.attempts++; this.sourceAttempts++; this.nextAttempt = Date.now() + 30000;
    return this.attempts;
  }
  stop(): void { this.path = ''; this.pending = false; }
}
