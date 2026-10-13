import { createHash } from 'node:crypto';
import { requireThat } from './errors.mjs';
export function validatePreferences(value) {
  const keys = ['schemaVersion','themeMode','uiSkin','consolePalette','consoleCardLayout','viewMode','useGlassEffect','desktopInputProfileId'];
  requireThat(value && Object.keys(value).every(key => keys.includes(key)) && keys.every(key => key in value) &&
    value.schemaVersion === 1 && ['system','light','dark'].includes(value.themeMode) &&
    ['classic','console'].includes(value.uiSkin) && ['violet','blue','pearl'].includes(value.consolePalette) &&
    ['focus3d','bento_tiles'].includes(value.consoleCardLayout) && ['grid','compact','large','files'].includes(value.viewMode) &&
    typeof value.useGlassEffect === 'boolean' && /^[a-zA-Z0-9_-]{1,128}$/.test(value.desktopInputProfileId) && !value.desktopInputProfileId.startsWith('app-'), 'INVALID_PREFERENCES');
  return Object.fromEntries(keys.map(key => [key,value[key]]));
}
export function preferencesEnvelope(payload, deviceClass, revision) {
  return {schemaVersion:1,deviceClass,revision,payloadSha256:createHash('sha256').update(JSON.stringify(payload)).digest('hex'),payload};
}
