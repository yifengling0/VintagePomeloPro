"""Dynamic-target automation; keep RichMan8's existing D3D8/VirGL path."""
from pathlib import Path
import argparse
import json
import re
import shlex
import time
from urllib.parse import quote
from device import BUNDLE, ROOT, command, shell, shot

BASE = '/data/app/el2/100/base/' + BUNDLE
LOG = BASE + '/temp/wine_stderr_20261006.log'

def stop(name):
    shell(name, 'aa force-stop ' + BUNDLE, policy='automation')
    for i in range(8):
        state = shell(name + '-state-' + str(i), 'ps -A -o PID,PPID,UID,NAME')
        if not re.search(r'\bcom\.vintage\.pomelopro(?::|\s|$)', state): return
        time.sleep(0.5)
    raise RuntimeError('Target process tree still running')

def launch(name, exe, env=None, args=None, backend='wined3d'):
    # This backend is RichMan8's existing D3D8 path, explicitly preserved in
    # every cell. No Steam launch or D3D11 backend experiment occurs here.
    params = {'winehua.mode': 'game', 'winehua.game_path': quote(exe, safe=''),
              'winehua.d3d_backend': backend,
              'winehua.game_args_json': quote(json.dumps(args or []), safe=''),
              'winehua.d3d_env_json': quote(json.dumps([dict(key=k, value=v)
                  for k, v in (env or {}).items()]), safe='')}
    cmd = 'aa start -a EntryAbility -b ' + BUNDLE
    for key, value in params.items(): cmd += ' --ps ' + shlex.quote(key) + ' ' + shlex.quote(value)
    result = shell(name, cmd, policy='automation')
    assert 'success' in result.lower(), result

def receive(name, remote, sandbox=False):
    path = ROOT / (name + Path(remote).suffix)
    command(name + '-transfer', ['file', 'recv'] + (['-b', BUNDLE] if sandbox else []) + [remote, str(path)])
    assert path.is_file()
    return path

def snapshot(name):
    shot(name)
    return ROOT / (name + '.png')

def input_click(name, x, y):
    return shell(name, f'uinput -T -c {x} {y} 60', policy='automation')

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=['stop', 'launch', 'install', 'send', 'shot', 'log'])
    p.add_argument('name')
    p.add_argument('value', nargs='?', default='')
    p.add_argument('--env', default='{}')
    p.add_argument('--args', default='[]')
    opt = p.parse_args()
    if opt.action == 'stop': stop(opt.name)
    elif opt.action == 'launch': launch(opt.name, opt.value, json.loads(opt.env), json.loads(opt.args))
    elif opt.action == 'install': print(command(opt.name, ['install', '-r', opt.value], 120, 'automation'))
    elif opt.action == 'send': print(command(opt.name, ['file', 'send', '-b', BUNDLE, opt.value,
        '/data/storage/el2/base/files/.wine/drive_c/' + Path(opt.value).name], 30, 'automation'))
    elif opt.action == 'shot': snapshot(opt.name)
    elif opt.action == 'log':
        text = shell(opt.name, 'date; tail -n 1600 ' + LOG)
        print(opt.name, 'lines', len(text.splitlines()))
