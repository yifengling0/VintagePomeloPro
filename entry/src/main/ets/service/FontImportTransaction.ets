/** Same-filesystem font replacement. Only this operation's new directory is
 * discarded on failure; the previous fonts remain live or in backup. */
export interface FontImportFiles {
  exists(path: string): Promise<boolean>;
  mkdir(path: string): Promise<void>;
  copy(source: string, destination: string): Promise<void>;
  rename(source: string, destination: string): Promise<void>;
  remove(path: string): Promise<void>;
}

export async function replaceImportedFonts(files: FontImportFiles, source: string,
  destination: string, names: string[], token: string,
  progress: (done: number, total: number) => void): Promise<void> {
  const staged = destination + '.import-' + token;
  const backup = destination + '.backup-' + token;
  if (await files.exists(staged) || await files.exists(backup)) {
    throw new Error('字体导入临时目录已存在，请重试');
  }
  // Exclusive creation: never take ownership of an existing staging directory.
  await files.mkdir(staged);
  let oldMoved = false;
  let committed = false;
  try {
    for (let index = 0; index < names.length; index++) {
      await files.copy(source + '/' + names[index], staged + '/' + names[index]);
      progress(index + 1, names.length);
    }
    if (await files.exists(destination)) {
      await files.rename(destination, backup);
      oldMoved = true;
    }
    try {
      await files.rename(staged, destination);
      committed = true;
    } catch (error) {
      if (oldMoved) {
        try {
          await files.rename(backup, destination);
          oldMoved = false;
        } catch (_) {
          // Leave the complete backup for recovery; never delete it here.
          throw new Error('字体替换失败，旧字体已保留在 ' + backup + '，请恢复后重试');
        }
      }
      throw (error as Error);
    }
  } finally {
    if (!committed) {
      try { await files.remove(staged); } catch (_) {}
    }
  }
  // A cleanup failure after commit must not trigger rollback of the new fonts.
  if (oldMoved) {
    try { await files.remove(backup); } catch (_) {}
  }
}
