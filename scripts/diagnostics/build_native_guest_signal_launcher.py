#!/usr/bin/env python3
"""Build an opt-in x86_64 fault observer without changing release payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--stack-writes', action='store_true',
                        help='Observe bounded libc writes overlapping active guest stack frames')
    parser.add_argument('--signal-frames', action='store_true', help='Record bounded guest signal frame history')
    parser.add_argument('--config-root', type=Path, help='Explicit temporary XDG_CONFIG_HOME; never changes the default build')
    args = parser.parse_args()
    if args.config_root and (not args.config_root.is_absolute() or '..' in args.config_root.parts or len(str(args.config_root))>3000):
        parser.error('Use an absolute diagnostic configuration directory without traversal')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name('native_guest_signal_launcher.c')
    compiler = Path(shutil.which('gcc')).resolve(strict=True)
    if subprocess.check_output([str(compiler), '-dumpmachine'], text=True).strip() != 'x86_64-linux-gnu':
        raise ValueError('Requires x86_64 GNU compiler')
    binary = output / 'native_guest_signal_launcher'
    record = output / 'native-guest-signal-build.json'
    inputs = dict(sourceSha256=sha(source), compilerSha256=sha(compiler), builderSha256=sha(Path(__file__)),
                  stackWrites=args.stack_writes, signalFrames=args.signal_frames,
                  configRoot=str(args.config_root) if args.config_root else None)
    if args.stack_writes:inputs['stackObserverSha256']=sha(source.with_name('native_stack_write_observer.h'))
    if args.signal_frames:inputs['signalFramesSha256']=sha(source.with_name('native_guest_signal_frames.h'))
    old = json.loads(record.read_text()) if record.exists() else {}
    if old.get('inputs') != inputs or not binary.exists() or old.get('sha256') != sha(binary):
        subprocess.run([str(compiler), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-misleading-indentation', '-rdynamic',
                        *(['-DSTEAMOHOS_STACK_WRITE_OBSERVER=1', '-fno-builtin', '-fno-omit-frame-pointer'] if args.stack_writes else []),
                        *(['-DSTEAMOHOS_SIGNAL_FRAMES=1'] if args.signal_frames else []),
                        *(['-DSTEAMOHOS_DIAGNOSTIC_CONFIG='+json.dumps(str(args.config_root))] if args.config_root else []),
                        str(source), '-ldl', '-pthread',
                        '-Wl,-z,relro,-z,now', '-o', str(binary)], check=True, timeout=40)
    # An actual engine-side handler must still receive SIGSEGV, observe its own
    # unwrapped sigaction query, and return the original exit code.
    with tempfile.TemporaryDirectory(prefix='signal-check-', dir=output) as scratch:
        scratch = Path(scratch)
        fixture = scratch / 'fixture.c'
        fixture.write_text('''#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>
static void handler(int sig, siginfo_t *info, void *ctx) {
  (void)info; (void)ctx; _exit(sig == SIGSEGV ? 42 : 1);
}
int PlayerMain(int argc,char **argv) {
  (void)argc; (void)argv;
  struct sigaction a={.sa_sigaction=handler,.sa_flags=SA_SIGINFO}, previous;
  sigemptyset(&a.sa_mask);
  if(sigaction(SIGSEGV,&a,0)||sigaction(SIGSEGV,0,&previous)||previous.sa_sigaction!=handler)return 3;
  raise(SIGSEGV); return 4;
}
''')
        if args.config_root:
            expected=json.dumps(str(args.config_root))
            fixture.write_text(fixture.read_text().replace('#include <unistd.h>',
                '#include <unistd.h>\n#include <stdlib.h>\n#include <string.h>').replace(
                '  (void)argc; (void)argv;',
                '  (void)argc; (void)argv;\n  if(!getenv("XDG_CONFIG_HOME")||strcmp(getenv("XDG_CONFIG_HOME"),'+expected+'))return 5;'))
        shutil.copyfile(binary, scratch/'launcher')
        (scratch/'launcher').chmod(0o700)
        passed = True
        stack_checks = not args.stack_writes
        for cpp_entry in (False, True):
            if cpp_entry:
                fixture.write_text(fixture.read_text().replace('int PlayerMain(int argc,char **argv) {',
                    'int PlayerMain(int argc,char **argv) __asm__("_Z10PlayerMainiPPc");\nint PlayerMain(int argc,char **argv) {'))
            subprocess.run([str(compiler), '-shared', '-fPIC', str(fixture), '-o', str(scratch/'UnityPlayer.so')],
                           check=True, timeout=20)
            test = subprocess.run([str(scratch/'launcher')], capture_output=True, text=True, timeout=10)
            passed &= test.returncode == 42 and 'GUEST_FAULT rip=' in test.stderr and 'GUEST_FAULT signal=b' in test.stderr
            if args.stack_writes:
                clean = 'GNU_STACK_WRITE observer_ready=1' in test.stderr and 'reason=active_frame_overlap' not in test.stderr
                original=fixture.read_text()
                fixture.write_text(original.replace('#include <unistd.h>', '#include <unistd.h>\n#include <stdint.h>\n#include <string.h>').replace(
                    '  raise(SIGSEGV);',
                    '  uintptr_t *slot=(uintptr_t *)__builtin_frame_address(0)+1; uintptr_t value=*slot;\n'
                    '  void *(*volatile copy)(void *,const void *,size_t)=memcpy; copy(slot,&value,sizeof(value));\n'
                    '  raise(SIGSEGV);'))
                subprocess.run([str(compiler), '-shared', '-fPIC', '-fno-omit-frame-pointer', str(fixture), '-o', str(scratch/'UnityPlayer.so')],check=True,timeout=20)
                negative=subprocess.run([str(scratch/'launcher')],capture_output=True,text=True,timeout=10)
                fixture.write_text(original)
                detected=negative.returncode==42 and 'reason=active_frame_overlap' in negative.stderr
                stack_checks=clean and detected and (stack_checks if cpp_entry else True)
        passed &= stack_checks
    data = dict(inputs=inputs, sha256=sha(binary), bytes=binary.stat().st_size,
                hostHandlerForwardingPassed=passed, stackWriteChecksPassed=stack_checks, experimental=True, releasePayload=False)
    record.write_text(json.dumps(data, indent=2)+'\n')
    print(json.dumps(data))
    if not passed:
        raise SystemExit('Fault forwarding check failed')


if __name__ == '__main__':
    main()
