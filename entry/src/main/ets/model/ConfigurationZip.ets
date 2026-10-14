import { configurationRelativePath } from './UserConfiguration';
export interface ConfigurationZipMember { name: string; bytes: Uint8Array; }
function crc(bytes: Uint8Array): number {
  let value = 0xffffffff;
  for (const b of bytes) { value ^= b; for (let bit = 0; bit < 8; bit++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0); }
  return (value ^ 0xffffffff) >>> 0;
}
function nameBytes(name: string): Uint8Array {
  // Package names are manifest.json or content-addressed ASCII asset names.
  if (!/^(manifest\.json|assets\/[a-f0-9]{64}\.[a-z0-9]{1,8})$/.test(name)) throw new Error('CONFIG_UNSAFE_PATH');
  return Uint8Array.from(Array.from(name).map((c: string): number => c.charCodeAt(0)));
}
/** A bounded, deterministic STORE ZIP. No arbitrary extraction, links, encryption or compression bombs. */
export function encodeConfigurationZip(members: ConfigurationZipMember[]): Uint8Array {
  if (members.length < 1 || members.length > 257) throw new Error('CONFIG_QUOTA');
  const size = members.reduce((n: number, m: ConfigurationZipMember): number => n + 76 + 2 * nameBytes(m.name).length + m.bytes.length, 22);
  if (size > 72 * 1024 * 1024) throw new Error('CONFIG_QUOTA');
  const output = new Uint8Array(size), view = new DataView(output.buffer); let offset = 0;
  const offsets: number[] = []; const hashes: number[] = [];
  for (const member of members) {
    const name = nameBytes(member.name), sum = crc(member.bytes); offsets.push(offset); hashes.push(sum);
    view.setUint32(offset, 0x04034b50, true); view.setUint16(offset + 4, 20, true); view.setUint16(offset + 6, 0x800, true);
    view.setUint32(offset + 14, sum, true); view.setUint32(offset + 18, member.bytes.length, true); view.setUint32(offset + 22, member.bytes.length, true);
    view.setUint16(offset + 26, name.length, true); output.set(name, offset + 30); output.set(member.bytes, offset + 30 + name.length);
    offset += 30 + name.length + member.bytes.length;
  }
  const central = offset;
  members.forEach((member: ConfigurationZipMember, index: number): void => {
    const name = nameBytes(member.name);
    view.setUint32(offset, 0x02014b50, true); view.setUint16(offset + 4, 20, true); view.setUint16(offset + 6, 20, true); view.setUint16(offset + 8, 0x800, true);
    view.setUint32(offset + 16, hashes[index], true); view.setUint32(offset + 20, member.bytes.length, true); view.setUint32(offset + 24, member.bytes.length, true);
    view.setUint16(offset + 28, name.length, true); view.setUint32(offset + 42, offsets[index], true); output.set(name, offset + 46); offset += 46 + name.length;
  });
  view.setUint32(offset, 0x06054b50, true); view.setUint16(offset + 8, members.length, true); view.setUint16(offset + 10, members.length, true);
  view.setUint32(offset + 12, offset - central, true); view.setUint32(offset + 16, central, true);
  return output;
}
export function decodeConfigurationZip(bytes: Uint8Array): ConfigurationZipMember[] {
  if (bytes.length < 22 || bytes.length > 72 * 1024 * 1024) throw new Error('CONFIG_INVALID');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), end = bytes.length - 22;
  if (view.getUint32(end, true) !== 0x06054b50 || view.getUint32(end + 4, true) !== 0 || view.getUint16(end + 20, true) !== 0) throw new Error('CONFIG_INVALID');
  const count = view.getUint16(end + 10, true), central = view.getUint32(end + 16, true);
  if (!count || count > 257 || count !== view.getUint16(end + 8, true) || central + view.getUint32(end + 12, true) !== end) throw new Error('CONFIG_INVALID');
  const result: ConfigurationZipMember[] = [], names = new Set<string>(); let local = 0, offset = central;
  for (let index = 0; index < count; index++) {
    if (offset + 46 > end || view.getUint32(offset, true) !== 0x02014b50 || view.getUint16(offset + 10, true) !== 0 ||
      view.getUint16(offset + 8, true) !== 0x800 || view.getUint32(offset + 42, true) !== local) throw new Error('CONFIG_INVALID');
    const length = view.getUint32(offset + 24, true), nameLength = view.getUint16(offset + 28, true);
    if (view.getUint32(offset + 20, true) !== length || view.getUint32(offset + 30, true) !== 0 || offset + 46 + nameLength > end ||
      local + 30 + nameLength + length > central) throw new Error('CONFIG_INVALID');
    const name = Array.from(bytes.slice(offset + 46, offset + 46 + nameLength)).map((b: number): string => String.fromCharCode(b)).join('');
    configurationRelativePath(name); nameBytes(name); if (names.has(name)) throw new Error('CONFIG_INVALID'); names.add(name);
    if (view.getUint32(local, true) !== 0x04034b50 || view.getUint16(local + 6, true) !== 0x800 || view.getUint16(local + 8, true) !== 0 ||
      view.getUint16(local + 26, true) !== nameLength || view.getUint16(local + 28, true) !== 0 || view.getUint32(local + 18, true) !== length ||
      view.getUint32(local + 22, true) !== length || bytes.slice(local + 30, local + 30 + nameLength).some((b: number, i: number): boolean => b !== name.charCodeAt(i))) throw new Error('CONFIG_INVALID');
    const data = bytes.slice(local + 30 + nameLength, local + 30 + nameLength + length), sum = crc(data);
    if (sum !== view.getUint32(offset + 16, true) || sum !== view.getUint32(local + 14, true)) throw new Error('CONFIG_HASH_MISMATCH');
    result.push({ name: name, bytes: data }); local += 30 + nameLength + length; offset += 46 + nameLength;
  }
  if (local !== central || offset !== end || !names.has('manifest.json')) throw new Error('CONFIG_INVALID'); return result;
}
