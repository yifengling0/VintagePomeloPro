export interface LinuxSteamWindowPreferences {
  mode: number;
  width: number;
  height: number;
}

export function parseLinuxSteamWindowPreferences(text: string): LinuxSteamWindowPreferences | null {
  if (text.length > 96) return null;
  const fields = /^1 ([01]) ([0-9]{3,4}) ([0-9]{3,4})\s*$/.exec(text);
  if (!fields) return null;
  const width = Number(fields[2]); const height = Number(fields[3]);
  if (width < 320 || width > 4096 || height < 200 || height > 4096 || width * height > 8388608) return null;
  return { mode: Number(fields[1]), width, height };
}
