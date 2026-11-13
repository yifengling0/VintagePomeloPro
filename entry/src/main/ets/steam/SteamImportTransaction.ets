/** Journaled directory replacement. A prepared ZIP and existing mutable data
 * stay on the same filesystem, so upgrading never copies the game library. */
export interface SteamImportJournal {
  phase: string;
  hadClient: boolean;
  preserved: string[];
}
export interface SteamImportFiles {
  exists: (path: string) => boolean;
  rename: (from: string, to: string) => Promise<void>;
  remove: (path: string) => Promise<void>;
  journal: (value: SteamImportJournal | null) => Promise<void>;
}

export async function recoverSteamImport(files: SteamImportFiles, live: string, staged: string,
  backup: string, journal: SteamImportJournal): Promise<void> {
  if (journal.phase === 'committed' ||
    (journal.phase === 'replace' && files.exists(live) && !files.exists(staged))) {
    await files.journal({ phase: 'committed', hadClient: journal.hadClient, preserved: journal.preserved });
    return;
  }
  if (journal.phase === 'replace' && files.exists(backup)) {
    if (files.exists(live)) throw new Error('Steam 更新状态冲突，已保留全部目录');
    for (const name of journal.preserved) {
      if (!files.exists(backup + '/' + name) && files.exists(staged + '/' + name)) {
        await files.rename(staged + '/' + name, backup + '/' + name);
      }
    }
    await files.rename(backup, live);
  }
  if (files.exists(staged)) await files.remove(staged);
  await files.journal(null);
}

export async function commitSteamImport(files: SteamImportFiles, live: string, staged: string,
  backup: string, preserved: string[], prepareStage?: (path: string) => Promise<void>): Promise<void> {
  const journal: SteamImportJournal = { phase: 'replace', hadClient: files.exists(live), preserved };
  await files.journal(journal);
  try {
    if (journal.hadClient) await files.rename(live, backup);
    for (const name of preserved) {
      if (!files.exists(backup + '/' + name)) continue;
      if (files.exists(staged + '/' + name)) await files.remove(staged + '/' + name);
      await files.rename(backup + '/' + name, staged + '/' + name);
    }
    if (prepareStage) await prepareStage(staged);
    await files.rename(staged, live);
    await files.journal({ phase: 'committed', hadClient: journal.hadClient, preserved });
  } catch (error) {
    await recoverSteamImport(files, live, staged, backup, journal);
    throw (error as Error);
  }
}
