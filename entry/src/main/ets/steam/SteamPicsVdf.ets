/** Minimal, bounded parser for the text KeyValues in a PICS appinfo buffer. */
export class SteamPicsNode {
  value: string = '';
  children: Map<string, SteamPicsNode> = new Map<string, SteamPicsNode>();

  child(name: string): SteamPicsNode | null {
    return this.children.get(name.toLowerCase()) ?? null;
  }

  text(name: string): string {
    return this.child(name)?.value ?? '';
  }
}

export class SteamPicsVdf {
  static parse(bytes: Uint8Array): SteamPicsNode {
    if (bytes.length > 16 * 1024 * 1024) throw new Error('PICS appinfo is too large');
    const tokens: Array<string> = [];
    let index = 0;
    while (index < bytes.length) {
      const c = bytes[index];
      if (c === 0) break;
      if (c <= 32) { index++; continue; }
      if (c === 47 && index + 1 < bytes.length && bytes[index + 1] === 47) {
        while (index < bytes.length && bytes[index] !== 10) index++;
        continue;
      }
      if (c === 123 || c === 125) { tokens.push(String.fromCharCode(c)); index++; continue; }
      if (c === 34) {
        index++;
        let token = '';
        let closed = false;
        while (index < bytes.length) {
          const next = bytes[index++];
          if (next === 34) { closed = true; break; }
          if (next === 92 && index < bytes.length) {
            const escaped = bytes[index++];
            token += String.fromCharCode(escaped === 110 ? 10 : escaped);
          } else {
            token += String.fromCharCode(next);
          }
        }
        if (!closed) throw new Error('PICS appinfo has an unterminated string');
        tokens.push(token);
      } else {
        let token = '';
        while (index < bytes.length && bytes[index] > 32 && bytes[index] !== 123 && bytes[index] !== 125) {
          token += String.fromCharCode(bytes[index++]);
        }
        tokens.push(token);
      }
      if (tokens.length > 500000) throw new Error('PICS appinfo has too many tokens');
    }
    const root = new SteamPicsNode();
    const cursor: Array<number> = [0];
    SteamPicsVdf.readObject(tokens, cursor, root, 0);
    return root;
  }

  private static readObject(tokens: Array<string>, cursor: Array<number>, parent: SteamPicsNode,
    depth: number): void {
    if (depth > 48) throw new Error('PICS appinfo nesting is too deep');
    while (cursor[0] < tokens.length) {
      const key = tokens[cursor[0]++];
      if (key === '}') return;
      if (key === '{' || cursor[0] >= tokens.length) throw new Error('Invalid PICS appinfo');
      const value = tokens[cursor[0]++];
      const child = new SteamPicsNode();
      if (value === '{') SteamPicsVdf.readObject(tokens, cursor, child, depth + 1);
      else if (value === '}') throw new Error('Invalid PICS appinfo value');
      else child.value = value;
      parent.children.set(key.toLowerCase(), child);
    }
  }
}
