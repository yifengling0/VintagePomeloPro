#!/usr/bin/env python3
"""Run the actual GNU client supervisor with controlled child exit results."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT / 'build/host_tests/models/steam-client-restart')
    output = parser.parse_args().output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    supervisor = output / 'steamclient-session'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-no-pie',
                    str(ROOT / 'entry/src/main/cpp/linux_steam/client_session_restart.c'),
                    '-o', str(supervisor)], check=True)
    child = output / 'client.py'
    child.write_text('''import json, os, signal, sys
from pathlib import Path
p = Path(os.environ['TEST_RECORD'])
records = json.loads(p.read_text()) if p.exists() else []
records.append({'args': sys.argv[1:], 'pid': os.getpid(),
 'expected': os.environ.get('STEAMOHOS_STEAMCLIENT_EXPECTED_PID'),
 'image': os.environ.get('STEAMOHOS_STEAMCLIENT_EXPECTED_IMAGE')})
p.write_text(json.dumps(records))
value = os.environ['TEST_EXITS'].split(',')[min(len(records)-1,len(os.environ['TEST_EXITS'].split(','))-1)]
if value == 'signal': os.kill(os.getpid(), signal.SIGTERM)
sys.exit(int(value))
''', encoding='utf-8')
    count = 0
    with tempfile.TemporaryDirectory(dir=output) as directory:
        record = Path(directory) / 'attempts.json'

        def case(exits, expected, attempts, generation='abc-123', extra=None):
            nonlocal count
            record.unlink(missing_ok=True)
            env = os.environ.copy()
            for key in list(env):
                if key.startswith(('STEAMOHOS_', 'HMOSDECK_')):
                    del env[key]
            env.update(STEAMOHOS_CODE_ROOT=directory, STEAMOHOS_TMP_ROOT=directory,
                       STEAMOHOS_STEAMCLIENT_GAME_BRIDGE='1',
                       HMOSDECK_SESSION_GENERATION=generation,
                       TEST_RECORD=str(record), TEST_EXITS=exits)
            arguments = ['-cef-disable-gpu', '-applaunch', '695630', 'steam://rungameid/695630',
                         'steam://run/695630', 'steam://open/library', *(extra or [])]
            result = subprocess.run([str(supervisor), sys.executable, str(child), *arguments],
                                    env=env, capture_output=True, text=True, timeout=10)
            assert result.returncode == expected, (exits, result.returncode, result.stderr)
            assert 'Sanitizer' not in result.stderr, result.stderr
            records = json.loads(record.read_text())
            assert len(records) == attempts, records
            assert result.stderr.count('HMOSDECK_STEAM_RESTART ') == attempts - 1
            assert records[0]['args'] == arguments
            for attempt in records:
                assert str(attempt['pid']) == attempt['expected'], attempt
                assert attempt['image'] == sys.executable, attempt
            for attempt in records[1:]:
                assert attempt['args'] == ['-cef-disable-gpu', 'steam://open/library', *(extra or [])], attempt
            assert len({r['pid'] for r in records}) == attempts
            count += 1

        case('42,0', 0, 2)
        case('42,42,0', 0, 3)
        case('42', 42, 3)  # Bounded lifetime budget, including repeated updates.
        case('0', 0, 1)  # User exit.
        case('1', 1, 1)  # Real failure.
        case('signal', -signal.SIGTERM, 1)  # Crash/cancellation cannot restart.
        case('42,signal', -signal.SIGTERM, 2)
        case('42', 42, 1, generation='')  # Diagnostics do not restart.
        case('42', 42, 1, generation='../invalid')
        case('42,0', 0, 2, extra=['-silent', 'steam://open/settings'])
        record.unlink(missing_ok=True)
        env = os.environ.copy()
        env.update(STEAMOHOS_CODE_ROOT=directory, STEAMOHOS_STEAMCLIENT_GAME_BRIDGE='1',
                   HMOSDECK_SESSION_GENERATION='abc-123')
        result = subprocess.run([str(supervisor), directory+'/missing-executable'], env=env,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == 127 and 'HMOSDECK_STEAM_RESTART' not in result.stderr
    print(f'GNU Steam restart supervisor: {count + 1} cases PASS (ASan/UBSan, bounded restart, one-shot Play, source identity, exits/signals).')


if __name__ == '__main__':
    main()
