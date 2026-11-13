import { createHash } from 'node:crypto';
import { requireThat } from './errors.mjs';
const keys = (value, allowed) => requireThat(value && typeof value === 'object' && !Array.isArray(value) &&
  Object.keys(value).every(key => allowed.includes(key)), 'INVALID_PROFILE');
const string = (value, max = 128) => requireThat(typeof value === 'string' && value.length <= max &&
  !/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(value), 'INVALID_PROFILE');
const number = (value, min, max) => requireThat(Number.isFinite(value) && value >= min && value <= max, 'INVALID_PROFILE');
const optional = (value, min, max) => { if (value !== undefined) number(value, min, max); };
export function validateProfile(p) {
  keys(p, ['id', 'name', 'schemaVersion', 'builtInRevision', 'cursorSpeed', 'overlayOpacity', 'gamepadMappings', 'elements']);
  string(p.id); string(p.name);
  requireThat(/^[a-zA-Z0-9_-]{1,128}$/.test(p.id) && p.schemaVersion === 6, 'INVALID_PROFILE');
  number(p.cursorSpeed, .1, 5); number(p.overlayOpacity, 0, 100); optional(p.builtInRevision, 0, 1000000);
  requireThat(Array.isArray(p.elements) && p.elements.length <= 512 && Array.isArray(p.gamepadMappings) &&
    p.gamepadMappings.length <= 128, 'INVALID_PROFILE');
  const ids = new Set();
  for (const e of p.elements) {
    keys(e, ['id', 'type', 'shape', 'x', 'y', 'scale', 'widthScale', 'heightScale', 'panelWidthRatio',
      'panelHeightRatio', 'keyWidthRatio', 'keyHeightRatio', 'opacity', 'bindings', 'toggleSwitch', 'text',
      'iconId', 'visible', 'keyList', 'orientation', 'outputMode']);
    string(e.id); string(e.text);
    requireThat(e.id && !ids.has(e.id) && ['PANEL','BUTTON','D_PAD','STICK','TRACKPAD','RANGE_BUTTON'].includes(e.type) &&
      ['CIRCLE','RECT','ROUND_RECT','SQUARE'].includes(e.shape) && ['vertical','horizontal'].includes(e.orientation) &&
      ['keys','mouse'].includes(e.outputMode) && typeof e.toggleSwitch === 'boolean' && typeof e.visible === 'boolean', 'INVALID_PROFILE');
    ids.add(e.id); number(e.x,0,1); number(e.y,0,1); number(e.scale,.1,5); number(e.opacity,-1,100); number(e.iconId,0,65535);
    optional(e.widthScale,.2,12); optional(e.heightScale,.2,12);
    for (const k of ['panelWidthRatio','panelHeightRatio','keyWidthRatio','keyHeightRatio']) optional(e[k],0,1);
    requireThat(Array.isArray(e.bindings) && e.bindings.length <= 32 && Array.isArray(e.keyList) && e.keyList.length <= 256, 'INVALID_PROFILE');
    for (const b of e.bindings) {
      keys(b,['kind','code']); requireThat(['NONE','KEY','MOUSE'].includes(b.kind) && Number.isInteger(b.code), 'INVALID_PROFILE');
      number(b.code,0,65535);
    }
    for (const code of e.keyList) { requireThat(Number.isInteger(code),'INVALID_PROFILE'); number(code,0,65535); }
  }
  for (const m of p.gamepadMappings) {
    keys(m,['id','label','buttonCode','evdevCode']); string(m.id); string(m.label);
    number(m.buttonCode,0,65535); number(m.evdevCode,0,65535);
  }
  requireThat(Buffer.byteLength(JSON.stringify(p)) <= 262144, 'INVALID_PROFILE');
  return structuredClone(p);
}
export function profileEnvelope(payload, deviceClass, revision, baseRevision) {
  return { schemaVersion: 1, profileFamily: 'input/native-v6', deviceClass, variant: payload.id,
    revision, baseRevision, payloadSha256: createHash('sha256').update(JSON.stringify(payload)).digest('hex'), payload };
}
