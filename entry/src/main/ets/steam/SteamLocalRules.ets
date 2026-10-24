/** Text KeyValues used by libraryfolders.vdf and appmanifest_*.acf.
 * Parse decoded UTF-8, not individual UTF-8 bytes (game names may be Chinese). */
export class SteamVdfNode {
  value: string = '';
  children: Map<string, SteamVdfNode> = new Map<string, SteamVdfNode>();
  child(key: string): SteamVdfNode | undefined { return this.children.get(key.toLowerCase()); }
  text(key: string): string { return this.child(key)?.value ?? ''; }
  set(key: string, value: string): void {
    const node = new SteamVdfNode();
    node.value = value;
    this.children.set(key.toLowerCase(), node);
  }
}

export function parseSteamVdf(text: string): SteamVdfNode {
  if (text.length > 4 * 1024 * 1024) throw new Error('Steam 清单过大');
  const tokens: string[] = [];
  let offset = 0;
  while (offset < text.length) {
    const c = text[offset++];
    if (c <= ' ' || c === '\uFEFF') continue;
    if (c === '/' && text[offset] === '/') {
      while (offset < text.length && text[offset] !== '\n') offset++;
    } else if (c === '{' || c === '}') tokens.push(c);
    else if (c === '"') {
      let token = '';
      let closed = false;
      while (offset < text.length) {
        const next = text[offset++];
        if (next === '"') { closed = true; break; }
        if (next === '\\' && offset < text.length) {
          const escaped = text[offset++];
          token += escaped === 'n' ? '\n' : escaped === 't' ? '\t' :
            (escaped === '\\' || escaped === '"') ? escaped : '\\' + escaped;
        } else token += next;
      }
      if (!closed) throw new Error('Steam 清单字符串未结束');
      tokens.push(token);
    } else {
      let token = c;
      while (offset < text.length && text[offset] > ' ' && text[offset] !== '{' && text[offset] !== '}') {
        token += text[offset++];
      }
      tokens.push(token);
    }
    if (tokens.length > 100000) throw new Error('Steam 清单条目过多');
  }
  const root = new SteamVdfNode();
  const stack: SteamVdfNode[] = [root];
  for (let i = 0; i < tokens.length;) {
    const key = tokens[i++];
    if (key === '}') {
      if (stack.length === 1) throw new Error('Steam 清单括号不匹配');
      stack.pop();
      continue;
    }
    if (key === '{' || i >= tokens.length) throw new Error('Steam 清单缺少值');
    const value = tokens[i++];
    if (value === '}') throw new Error('Steam 清单缺少值');
    const node = new SteamVdfNode();
    stack[stack.length - 1].children.set(key.toLowerCase(), node);
    if (value === '{') {
      stack.push(node);
      if (stack.length > 32) throw new Error('Steam 清单嵌套过深');
    } else node.value = value;
  }
  if (stack.length !== 1) throw new Error('Steam 清单未写入完成');
  return root;
}

function quoteVdf(value: string): string {
  return '"' + value.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n') + '"';
}

export function serializeSteamVdf(node: SteamVdfNode, indent: string = ''): string {
  let result = '';
  node.children.forEach((child: SteamVdfNode, key: string): void => {
    result += indent + quoteVdf(key);
    if (child.children.size > 0) {
      result += '\n' + indent + '{\n' + serializeSteamVdf(child, indent + '\t') + indent + '}\n';
    } else result += '\t\t' + quoteVdf(child.value) + '\n';
  });
  return result;
}

export function safeSteamRelativePath(path: string): boolean {
  return path.length > 0 && !/[\x00-\x1f:]/.test(path) && !path.startsWith('/') &&
    !path.startsWith('\\') && path.replace(/\\/g, '/').split('/').every((part: string): boolean =>
      part.length > 0 && part !== '.' && part !== '..');
}

export function steamWindowsPath(path: string, libraryRoot: string, driveC: string): string {
  const p = path.replace(/\\/g, '/').replace(/\/+$/, '');
  if (p.toLowerCase().startsWith(libraryRoot.toLowerCase() + '/')) return 'z:' + p.substring(libraryRoot.length);
  if (p.toLowerCase().startsWith(driveC.toLowerCase() + '/')) return 'c:' + p.substring(driveC.length);
  return p.toLowerCase().replace(/^z:\//, 'z:/');
}

export function resolveSteamLibraryPath(path: string, libraryRoot: string, driveC: string): string {
  const p = path.replace(/\\/g, '/').replace(/\/+$/, '');
  const drive = p.substring(0, 2).toLowerCase();
  const relative = p.substring(3);
  if (p[2] !== '/' || !safeSteamRelativePath(relative)) return '';
  return drive === 'z:' ? libraryRoot + '/' + relative : drive === 'c:' ? driveC + '/' + relative : '';
}

/** Snapshot shared by ordinary discovery and cached-card reconciliation. */
export interface SteamCatalogScope {
  excludedRoots: string[];
  libraryRoots: string[];
  legacyGameRoots: string[];
}

function normalizedCatalogPath(path: string): string {
  return path.replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
}

export function steamCatalogPathKind(path: string, scope: SteamCatalogScope): string {
  const value = normalizedCatalogPath(path);
  if (!value) return 'game';
  // Old built-in downloads can coexist with a manually copied client under
  // games/steam/<AppID>. Keep those directories as ordinary local games.
  for (const root of scope.legacyGameRoots) {
    const boundary = normalizedCatalogPath(root);
    if (value === boundary || value.startsWith(boundary + '/')) return 'game';
    if (boundary.startsWith(value + '/')) return 'container';
  }
  for (const root of scope.excludedRoots) {
    const boundary = normalizedCatalogPath(root);
    if (boundary && (value === boundary || value.startsWith(boundary + '/'))) return 'excluded';
  }
  // A wrapper may also contain ordinary sibling games. Traverse it instead
  // of turning it into a card or discarding its entire subtree.
  for (const root of scope.libraryRoots.concat(scope.excludedRoots)) {
    const boundary = normalizedCatalogPath(root);
    if (boundary && (value === boundary || boundary.startsWith(value + '/'))) return 'container';
  }
  return 'game';
}

export const STEAM_CLIENT_ID = 'steam-client-default';
export function steamGameCardId(appId: string): string { return 'steam-default-' + appId; }
export function isSteamAppId(value: string): boolean { return /^[1-9][0-9]{0,9}$/.test(value); }

export const STEAM_GPU_ARGUMENTS: string[] = ['-nocrashmonitor', '-noshaders', '-no-shared-textures',
  '-cef-single-process', '-cef-in-process-gpu', '-cef-disable-sandbox', '-disable-winh264',
  '-no-cef-sandbox', '-vrdisable', '-cef-disable-breakpad'];

export function steamLaunchArguments(action: string, appId: string, compatibility: boolean,
  gameArguments: string[] = [], clientAction: string = 'library'): string[] {
  const args = STEAM_GPU_ARGUMENTS.slice();
  if (compatibility) args.push('-cef-disable-gpu');
  if (action === 'play' || action === 'install') {
    if (!isSteamAppId(appId)) throw new Error('Steam AppID 无效');
    if (action === 'play') return args.concat(['-silent', '-applaunch', appId], gameArguments);
    args.push('steam://install/' + appId);
  } else if (action === 'open') {
    // An explicit URI restores a client hidden by an earlier -silent launch.
    if (clientAction === 'library') args.push('steam://open/main');
    else if (clientAction === 'downloads') args.push('steam://open/downloads');
    else if (clientAction === 'store') args.push('steam://store');
    else if (clientAction === 'account') args.push('steam://open/settings/account');
    else if (clientAction === 'bigPicture') args.push('steam://open/bigpicture');
    else if (clientAction === 'desktop') args.push('steam://close/bigpicture');
    else throw new Error('Steam 客户端操作无效');
  } else throw new Error('Steam 操作无效');
  return args;
}
