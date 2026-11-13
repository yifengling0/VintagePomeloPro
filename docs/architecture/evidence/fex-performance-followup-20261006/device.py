"""Bounded, redacted evidence for the connected test tablet."""
from pathlib import Path
import argparse
import datetime as dt
import hashlib
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parent
HDC = r'C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\toolchains\hdc.exe'
BUNDLE = 'com.vintage.pomelopro'
targets = subprocess.run([HDC, 'list', 'targets'], capture_output=True, timeout=8, check=True).stdout.decode().splitlines()
targets = [x.strip() for x in targets if re.fullmatch(r'[A-Za-z0-9]+', x.strip()) and x.strip() != 'Empty']
assert len(targets) == 1, 'Need the one connected test tablet'
TARGET = targets[0]


def redact(text):
    text = text.replace(TARGET, 'MatePad-USB')
    text = re.sub(r'7656119\d{10}', '7656119XXXXXXXXXX', text)
    return '\n'.join(line for line in text.splitlines() if not re.search(
        r'(?i)password|authorization|cookie|access.?token|refresh.?token|--?login\b', line))


def command(name, args, timeout=20, policy='evidence'):
    assert re.fullmatch(r'[a-z0-9-]+', name)
    p = subprocess.run([HDC, '-t', TARGET, *args], capture_output=True, timeout=timeout)
    output = redact((p.stdout + p.stderr).decode(errors='replace'))
    (ROOT / (name + '.txt')).write_text(output + '\n', encoding='utf-8')
    ledger_path = ROOT / 'device-commands.json'
    ledger = json.loads(ledger_path.read_text()) if ledger_path.exists() else []
    ledger.append(dict(time=dt.datetime.now().astimezone().isoformat(), target='MatePad-USB',
        command=args, timeoutSeconds=timeout, exitCode=p.returncode, policy=policy,
        artifact=name + '.txt', redactionStatus='serial/SteamID masked, credential lines omitted, screenshots local only'))
    ledger_path.write_text(json.dumps(ledger, ensure_ascii=False, indent=2) + '\n')
    if p.returncode or any(x in output.lower() for x in ['[fail]', 'permission denied', 'operation not permitted']):
        raise RuntimeError(f'{name}: inspect redacted artifact')
    return output


def shell(name, cmd, timeout=20, policy='evidence'):
    return command(name, ['shell', cmd], timeout, policy)


def shot(name):
    remote = '/data/local/tmp/vp-steam-white-' + name + '.png'
    shell(name + '-capture', 'uitest screenCap -p ' + remote)
    path = ROOT / (name + '.png')
    command(name + '-transfer', ['file', 'recv', remote, str(path)])
    assert path.is_file()
    print(json.dumps(dict(screenshot=str(path), bytes=path.stat().st_size,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest())))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['initial', 'shell', 'shot', 'recv'])
    parser.add_argument('name', nargs='?', default='initial')
    parser.add_argument('value', nargs='?', default='')
    opt = parser.parse_args()
    if opt.action == 'initial':
        for name, value in [
            ('processes', 'date; ps -A -o PID,PPID,UID,NAME'),
            ('foreground', 'aa dump -l'),
            ('bundle', 'bm dump -n ' + BUNDLE),
            ('log-files', 'ls -lt /data/app/el2/100/base/' + BUNDLE + '/temp | head -n 20'),
            ('runtime-identity', 'sha256sum /data/app/el2/100/base/' + BUNDLE + '/files/wine/bin/aarch64-windows/libwow64fex.dll /data/app/el2/100/base/' + BUNDLE + '/files/wine/bin/aarch64-windows/libarm64ecfex.dll')]:
            value = shell(name, value)
            print(name, 'chars=', len(value))
        shot('initial')
    elif opt.action == 'shell':
        output = shell(opt.name, opt.value)
        print(opt.name, 'chars=', len(output))
    elif opt.action == 'shot':
        shot(opt.name)
    elif opt.action == 'recv':
        path = ROOT / (opt.name + Path(opt.value).suffix)
        print(command(opt.name + '-transfer', ['file', 'recv', '-b', BUNDLE, opt.value, str(path)]))
        if path.suffix in {'.log', '.txt', '.json', '.bat'}:
            data = path.read_bytes()
            (ROOT / (opt.name + '-source-identity.json')).write_text(json.dumps(dict(
                sha256BeforeRedaction=hashlib.sha256(data).hexdigest(), bytesBeforeRedaction=len(data))) + '\n')
            path.write_text(redact(data.decode(errors='replace')) + '\n', encoding='utf-8')
