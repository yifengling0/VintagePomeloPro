"""Bounded device automation for the explicitly authorized target application."""
from pathlib import Path
import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import subprocess
from urllib.parse import quote

ROOT = Path(__file__).resolve().parent
HDC = r'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
TARGET = os.environ.get('VP_HDC_TARGET', '')
BUNDLE = 'com.vintage.pomelopro'


def redact(text):
    text = text.replace(TARGET, 'MatePad-USB')
    text = re.sub(r'7656119\d{10}', '7656119XXXXXXXXXX', text)
    return '\n'.join(line for line in text.splitlines() if not re.search(
        r'(?i)password|authorization|cookie|access.?token|refresh.?token|--?login\b', line))


def call(name, args, timeout=35, policy='evidence'):
    assert TARGET, 'Set VP_HDC_TARGET to the selected online HDC device'
    assert re.fullmatch(r'[a-z0-9-]+', name)
    try:
        p = subprocess.run([HDC, '-t', TARGET, *args], capture_output=True, timeout=timeout)
        output, code = redact((p.stdout + p.stderr).decode(errors='replace')), p.returncode
    except subprocess.TimeoutExpired:
        output, code = 'Timed out', -1
    (ROOT / (name + '.txt')).write_text(output + '\n', encoding='utf-8')
    ledger_path = ROOT / 'device-commands.json'
    ledger = json.loads(ledger_path.read_text(encoding='utf-8')) if ledger_path.exists() else []
    ledger.append(dict(time=dt.datetime.now().astimezone().isoformat(), target='MatePad-USB',
        command=args, timeoutSeconds=timeout, exitCode=code, policy=policy,
        artifact=name + '.txt', decision='allowed',
        redactionStatus='serial and SteamID masked; credential lines omitted; screenshots local only'))
    ledger_path.write_text(json.dumps(ledger, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if code or any(v in output.lower() for v in ['[fail]', 'permission denied', 'operation not permitted']):
        raise RuntimeError(name + ': command failed; inspect artifact')
    print(name, 'chars=', len(output), flush=True)
    return output


def shell(name, command, timeout=35, policy='evidence'):
    return call(name, ['shell', command], timeout, policy)


def receive(name, remote, sandbox=False):
    path = ROOT / (name + Path(remote).suffix)
    args = ['file', 'recv'] + (['-b', BUNDLE] if sandbox else []) + [remote, str(path)]
    call(name + '-transfer', args)
    assert path.is_file(), 'No received file'
    print(str(path), 'bytes=', path.stat().st_size, 'sha256=', hashlib.file_digest(path.open('rb'), 'sha256').hexdigest())
    return path


def launch(name, exe, env, args=None):
    parameters = {'winehua.mode': 'game', 'winehua.game_path': quote(exe, safe=''),
        'winehua.d3d_backend': 'wined3d', 'winehua.game_args_json': quote(json.dumps(args or []), safe=''),
        'winehua.d3d_env_json': quote(json.dumps([dict(key=k, value=v) for k, v in env.items()]), safe='')}
    command = 'aa start -a EntryAbility -b ' + BUNDLE
    for key, value in parameters.items():
        command += ' --ps ' + shlex.quote(key) + ' ' + shlex.quote(value)
    output = shell(name, command, policy='automation')
    assert 'success' in output.lower(), output
    return output


def snapshot(name):
    remote = '/data/local/tmp/vp-' + name + '.png'
    shell(name + '-capture', 'uitest screenCap -p ' + remote)
    return receive(name, remote)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=['shell', 'probe', 'launch', 'shot', 'recv', 'stop'])
    p.add_argument('name')
    p.add_argument('value', nargs='?', default='')
    p.add_argument('--environment', default='{}')
    p.add_argument('--sandbox', action='store_true')
    p.add_argument('--timeout', type=int, default=35)
    p.add_argument('--summary', action='store_true')
    opt = p.parse_args()
    if opt.action == 'shell':
        result = shell(opt.name, opt.value, opt.timeout)
        if not opt.summary:
            print(result)
    elif opt.action == 'probe':
        env = dict(HODLL='libwow64fex.dll', FEX_JITPERF='0', FEX_BLOCKJITNAMING='0',
            FEX_LIBRARYJITNAMING='0', FEX_GLOBALJITNAMING='0', FEX_JITMAPDIR='',
            WINEHUA_WINEDEBUG='-all,+err', WINEDEBUG='-all,+err')
        env.update(json.loads(opt.environment))
        print(launch(opt.name, r'C:\vp32-route-probe.exe', env))
    elif opt.action == 'launch':
        print(launch(opt.name, opt.value, json.loads(opt.environment)))
    elif opt.action == 'shot':
        snapshot(opt.name)
    elif opt.action == 'recv':
        receive(opt.name, opt.value, opt.sandbox)
    elif opt.action == 'stop':
        print(shell(opt.name, 'aa force-stop ' + BUNDLE, policy='automation'))
        state = shell(opt.name + '-state', 'ps -A -o PID,PPID,UID,NAME')
        assert not re.search(r'\bcom\.vintage\.pomelopro(?::|\s|$)', state), 'Target still running'
