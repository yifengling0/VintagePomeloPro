import smokeNapi from 'libsmoke_napi.so';
import { ConfigurationFiles } from './ConfigurationFiles';

export async function withLinuxProtonMaintenance(action: (token: number) => Promise<void>): Promise<void> {
  ConfigurationFiles.ensure('/data/storage/el2/base/files/steamohos-native');
  if (!smokeNapi.beginLinuxProtonMaintenance || !smokeNapi.endLinuxProtonMaintenance)
    throw new Error('当前应用缺少 Proton 维护支持，请更新应用');
  const token = smokeNapi.beginLinuxProtonMaintenance();
  if (!token) throw new Error('Proton 正在使用，请退出游戏后重试');
  try { await action(token); } finally { smokeNapi.endLinuxProtonMaintenance(token); }
}
