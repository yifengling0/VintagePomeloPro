import { common } from '@kit.AbilityKit';
import { fileUri, picker } from '@kit.CoreFileKit';

// Resolve the app-owned Download directory through the system on phones/tablets.
export async function publicDownloadDirectory(context: common.Context): Promise<string> {
  const choices = await new picker.DocumentViewPicker(context).save({ pickerMode: picker.DocumentPickerMode.DOWNLOAD });
  if (!choices || !choices.length) throw new Error('无法访问应用的 Download 目录');
  const path = new fileUri.FileUri(choices[0]).path.replace(/\/$/, '');
  if (!path.startsWith('/') || path.split('/').includes('..') || /[\x00-\x1f]/.test(path)) {
    throw new Error('应用 Download 目录无效');
  }
  return path;
}
