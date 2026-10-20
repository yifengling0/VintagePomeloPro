// Explicit developer smoke suite only. Preserve old resources by renaming on
// the same volume; never uninstall or erase account/game data to test downloads.
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';
import { prepareLinuxSteamExternalStorage } from './LinuxSteamDiagnosticData';
import { ensureSmokeDirectory } from './ManagedSmokePayloadService';

export async function isolateLinuxSteamForCleanDownload(context: common.Context): Promise<string> {
  const state = '/data/storage/el2/base/files/steamohos-native';
  const root = await prepareLinuxSteamExternalStorage(context);
  const savedRoot = root + '-before-clean-20261020';
  const savedState = state + '-before-clean-20261020';
  if (fs.accessSync(savedRoot) || fs.accessSync(savedState)) throw new Error('已有隔离备份，拒绝覆盖');
  if (fs.lstatSync(root).isSymbolicLink() || fs.lstatSync(state).isSymbolicLink()) throw new Error('无效测试目录');
  fs.renameSync(root, savedRoot);
  try { fs.renameSync(state, savedState); }
  catch (error) { fs.renameSync(savedRoot, root); throw error as Error; }
  if (!ensureSmokeDirectory(root) || !ensureSmokeDirectory(state)) throw new Error('无法创建新的隔离测试目录，旧资源仍保留');
  ConfigurationFiles.json(`${state}/external-root.path`, root);
  const result = JSON.stringify({ schemaVersion: 1, resourceDirectoryInitiallyEmpty: true,
    savedRoot, savedState, root, appPrivateDataErased: false, appUninstalled: false });
  ConfigurationFiles.json(`${state}/clean-download-isolation.json`, result);
  return result;
}

export async function restoreLinuxSteamAfterCleanDownload(context: common.Context): Promise<string> {
  const state = '/data/storage/el2/base/files/steamohos-native';
  const root = await prepareLinuxSteamExternalStorage(context);
  const savedRoot = root + '-before-clean-20261020';
  const savedState = state + '-before-clean-20261020';
  const trialRoot = root + '-clean-download-trial-20261020';
  const trialState = state + '-clean-download-trial-20261020';
  for (const source of [root, state, savedRoot, savedState]) {
    if (!fs.lstatSync(source).isDirectory() || fs.lstatSync(source).isSymbolicLink()) throw new Error('隔离恢复目录无效');
  }
  if (fs.accessSync(trialRoot) || fs.accessSync(trialState)) throw new Error('已有下载测试记录，拒绝覆盖');
  let step = 0;
  try {
    fs.renameSync(root, trialRoot); step = 1;
    fs.renameSync(savedRoot, root); step = 2;
    fs.renameSync(state, trialState); step = 3;
    fs.renameSync(savedState, state); step = 4;
  } catch (error) {
    if (step >= 3) fs.renameSync(trialState, state);
    if (step >= 2) fs.renameSync(root, savedRoot);
    if (step >= 1) fs.renameSync(trialRoot, root);
    throw error as Error;
  }
  return JSON.stringify({ schemaVersion: 1, oldResourcesRestored: true, root, trialRoot, trialState });
}
