#!/usr/bin/env python3
"""Build the existing Wine AudioBroker backend for the isolated GNU experiment.

The original GE PE driver is retained. Its Unix ABI must match byte-for-byte;
the experiment's opt-in loader redirects only that driver's Unix library.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shlex
import subprocess

from wine_audio_contract import shared_audio_format


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audio_flow_overlay(source):
    """Opt-in, two-second counters; no PCM capture or per-buffer logging."""
    def replace(old, new):
        nonlocal source
        assert source.count(old) == 1, 'Direct audio source anchor changed: '+old[:80]
        source = source.replace(old, new)

    replace('#include <string.h>', '#include <string.h>\n#include <time.h>\n#include <unistd.h>')
    clock_header = Path(__file__).resolve().parents[1]/'entry/src/main/cpp/linux_steam/gnu_fixture/proton_audio_clock.h'
    source = clock_header.read_text() + '\n' + source
    replace('    return (stamp.QuadPart * (UINT64)10000000) / freq.QuadPart;',
            '    return proton_audio_qpc_100ns(stamp.QuadPart, freq.QuadPart);')
    replace('    OhosAudioClient *client;', r'''
    int flow_debug;
    UINT64 flow_started_ns, flow_report_ns, flow_last_wake_ns, flow_last_release_ns;
    UINT64 flow_wakes, flow_wake_max_ns, flow_lock_max_ns, flow_release_max_ns;
    UINT64 flow_release_calls, flow_frames, flow_silent_frames, flow_zero_frames;
    UINT64 flow_status_calls, flow_status_max_ns, flow_qpc_origin;
    UINT32 flow_padding_min, flow_padding_max;
    OhosAudioClient *client;''')
    replace('static BOOL is_capture_stream', r'''
static UINT64 flow_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (UINT64)now.tv_sec * 1000000000 + now.tv_nsec;
}

static BOOL is_capture_stream''')
    replace('    return status.queued_frames;', r'''
    if (stream->flow_debug)
    {
        stream->flow_padding_min = min(stream->flow_padding_min, status.queued_frames);
        stream->flow_padding_max = max(stream->flow_padding_max, status.queued_frames);
    }
    return status.queued_frames;''')
    replace('    ret = ohos_audio_client_get_status(stream->client, stream->broker_stream.stream_id, &status);', r'''
    UINT64 begin = stream->flow_debug ? flow_monotonic_ns() : 0;
    ret = ohos_audio_client_get_status(stream->client, stream->broker_stream.stream_id, &status);
    if (stream->flow_debug)
    {
        stream->flow_status_calls++;
        stream->flow_status_max_ns = max(stream->flow_status_max_ns, flow_monotonic_ns() - begin);
    }''')
    replace('    memset(&req, 0, sizeof(req));', r'''
    stream->flow_debug = !access("/data/storage/el2/base/files/steamohos-native/proton-probe/audio-flow-debug", R_OK);
    stream->flow_padding_min = ~0u;
    memset(&req, 0, sizeof(req));''')
    replace('static NTSTATUS ohos_timer_loop(void *args)', r'''
static void flow_report_locked(struct ohos_stream *stream, UINT64 now)
{
    UINT64 qpc;
    if (!stream->flow_report_ns)
    {
        stream->flow_started_ns = stream->flow_report_ns = now;
        stream->flow_qpc_origin = query_qpc_100ns();
        return;
    }
    if (now - stream->flow_report_ns < 2000000000) return;
    qpc = query_qpc_100ns();
    /* This is the producer's timer thread, not AudioRenderer's callback.
       A bounded line every two seconds carries counts, never audio data. */
    fprintf(stderr, "HMOSDECK_AUDIO_FLOW stream=%u age_ms=%llu span_us=%llu rate=%u channels=%u period=%u capacity=%u started=%d wakes=%llu wake_max_us=%llu lock_max_us=%llu releases=%llu release_max_us=%llu frames=%llu silent=%llu zero=%llu submitted=%llu position=%llu pad_min=%u pad_max=%u status_calls=%llu status_max_us=%llu qpc_delta_us=%llu mono_delta_us=%llu\\n",
        stream->broker_stream.stream_id,
        (unsigned long long)((now-stream->flow_started_ns)/1000000),
        (unsigned long long)((now-stream->flow_report_ns)/1000),
        stream->fmt->Format.nSamplesPerSec, stream->fmt->Format.nChannels,
        stream->client_period_frames, stream->client_buffer_frames, stream->started,
        (unsigned long long)stream->flow_wakes,
        (unsigned long long)(stream->flow_wake_max_ns/1000),
        (unsigned long long)(stream->flow_lock_max_ns/1000),
        (unsigned long long)stream->flow_release_calls,
        (unsigned long long)(stream->flow_release_max_ns/1000),
        (unsigned long long)stream->flow_frames,
        (unsigned long long)stream->flow_silent_frames,
        (unsigned long long)stream->flow_zero_frames,
        (unsigned long long)stream->client_frames_submitted,
        (unsigned long long)stream->last_position,
        stream->flow_padding_min, stream->flow_padding_max,
        (unsigned long long)stream->flow_status_calls,
        (unsigned long long)(stream->flow_status_max_ns/1000),
        (unsigned long long)((qpc-stream->flow_qpc_origin)/10),
        (unsigned long long)((now-stream->flow_started_ns)/1000));
    stream->flow_report_ns = now;
    stream->flow_wakes = stream->flow_wake_max_ns = stream->flow_lock_max_ns = 0;
    stream->flow_release_calls = stream->flow_release_max_ns = stream->flow_frames = 0;
    stream->flow_silent_frames = stream->flow_zero_frames = 0;
    stream->flow_status_calls = stream->flow_status_max_ns = 0;
    stream->flow_padding_min = ~0u;
    stream->flow_padding_max = 0;
}

static NTSTATUS ohos_timer_loop(void *args)''')
    replace('        NtDelayExecution(FALSE, &delay);\n        ohos_lock(stream);', r'''
        NtDelayExecution(FALSE, &delay);
        UINT64 wake = stream->flow_debug ? flow_monotonic_ns() : 0;
        ohos_lock(stream);
        if (stream->flow_debug)
        {
            UINT64 now = flow_monotonic_ns();
            if (stream->flow_last_wake_ns)
                stream->flow_wake_max_ns = max(stream->flow_wake_max_ns, wake-stream->flow_last_wake_ns);
            stream->flow_last_wake_ns = wake;
            stream->flow_lock_max_ns = max(stream->flow_lock_max_ns, now-wake);
            stream->flow_wakes++;
            flow_report_locked(stream, now);
        }''')
    replace('    stream->client_frames_submitted += params->written_frames;', r'''
    if (stream->flow_debug)
    {
        UINT64 now = flow_monotonic_ns();
        if (stream->flow_last_release_ns)
            stream->flow_release_max_ns = max(stream->flow_release_max_ns, now-stream->flow_last_release_ns);
        stream->flow_last_release_ns = now;
        stream->flow_release_calls++;
        stream->flow_frames += params->written_frames;
        if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT) stream->flow_silent_frames += params->written_frames;
        for (UINT32 i=0; i<mix_frames; i++)
            if (!stream->mix_buffer[2*i] && !stream->mix_buffer[2*i+1]) stream->flow_zero_frames++;
    }
    stream->client_frames_submitted += params->written_frames;''')
    return source.replace('mono_delta_us=%llu\\\\n', 'mono_delta_us=%llu\\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['cc', 'wine-source', 'widl', 'abi-header', 'package', 'output']:
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    src = a.wine_source.resolve(strict=True)
    task = a.output.resolve()
    task.mkdir(parents=True, exist_ok=True)
    if task.is_relative_to(src):
        p.error('Keep generated files outside the Wine source')
    assert a.abi_header.read_bytes() == (src/'dlls/mmdevapi/unixlib.h').read_bytes(), 'GE Unix audio ABI mismatch'
    assert subprocess.check_output([str(a.cc), '-dumpmachine'], text=True).strip() == 'aarch64-none-linux-gnu'
    backend = src/'dlls/wineohos.drv'
    include = task/'include'
    overlay = task/'wineohos.drv'
    include.mkdir(exist_ok=True)
    overlay.mkdir(exist_ok=True)
    # Preserve the original protocol, converter, WASAPI clock and MIDI code.
    # Only bootstrap discovery differs between a GNU exec and OHOS Wine.
    client = (backend/'ohos_audio_client.c').read_text()
    helper = r'''
#include <sys/un.h>
static int gnu_audio_bootstrap(int *out) {
    const char *path=getenv("STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET");
    if(path&&path[0]=='/'&&strlen(path)<sizeof(((struct sockaddr_un*)0)->sun_path)) {
        int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
        struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
        if(fd<0)return -errno;
        if(connect(fd,(struct sockaddr*)&address,sizeof(address))) {int err=-errno;close(fd);return err;}
        *out=fd;return 0;
    }
    int original=-1,ret=parse_env_fd("STEAMOHOS_AUDIO_BOOTSTRAP_FD",&original);
    if(ret)return ret;
    *out=fcntl(original,F_DUPFD_CLOEXEC,3);return *out<0?-errno:0;
}
'''
    needle = 'int ohos_audio_client_connect(OhosAudioClient **out_client)'
    assert client.count(needle) == 1
    client = client.replace(needle, helper+'\n'+needle)
    client = client.replace('ret = parse_env_fd("WINE_OHOS_AUDIO_BOOTSTRAP_FD", &bootstrap_fd);',
                            'ret = gnu_audio_bootstrap(&bootstrap_fd);')
    client = client.replace('if (!client) return -ENOMEM;', 'if (!client) { close(bootstrap_fd); return -ENOMEM; }')
    client = client.replace('        free(client);\n        return -errno;', '        close(bootstrap_fd);\n        free(client);\n        return -errno;')
    client = client.replace('        close(private_pair[0]);\n        close(private_pair[1]);',
                            '        close(bootstrap_fd);\n        close(private_pair[0]);\n        close(private_pair[1]);')
    client = client.replace('    close(private_pair[1]);\n    client->fd',
                            '    close(bootstrap_fd);\n    close(private_pair[1]);\n    client->fd')
    (overlay/'ohos_audio_client.c').write_text(client)
    audio_source = overlay/'ohos.c'
    audio_source.write_bytes((backend/'ohos.c').read_bytes())
    # The backup tree precedes the product's Float32 WASAPI contract. Apply
    # the same maintained patches to this private file, never the source tree.
    patches = [Path(__file__).resolve().parents[1]/'patches/wine'/name for name in
               ['0058-ohos-shared-wasapi-float32.patch',
                '0062-wineohos-shared-audio-format-contract.patch']]
    for patch in patches:
        subprocess.run(['patch', '--batch', '--fuzz=0', '--no-backup-if-mismatch',
                        str(audio_source)], input=patch.read_bytes(), check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    audio_source.write_text(audio_flow_overlay(audio_source.read_text()))
    sources = [overlay/'ohos.c', overlay/'ohos_audio_client.c', backend/'ohos_midi.c']
    inputs = {str(path):sha(path) for path in [*sources, a.cc, a.widl, a.abi_header,
        backend/'ohos.c', backend/'ohos_audio_client.c',
        backend/'audio_ipc_protocol.h', backend/'ohos_audio_client.h', backend/'ohos_midi.h',
        backend/'tsf.h', backend/'tml.h', *patches,
        Path(__file__).with_name('wine_audio_contract.py'), Path(__file__)]}
    output = task/'libwineohos-gnu.so'
    identity = task/'direct-audio-build.json'
    if identity.exists() and output.exists():
        previous = json.loads(identity.read_text())
        dependencies = previous.get('dependencies', {})
        if (previous.get('inputs') == inputs and previous.get('sha256') == sha(output)
                and dependencies and all(Path(path).is_file() and sha(Path(path)) == digest
                                         for path, digest in dependencies.items())):
            shared_audio_format(output.read_bytes())
            print('Reused verified GNU direct audio backend')
            return
    # WIDL-generated includes are small; resolve only the actual header closure.
    flags = [str(a.cc), '-O2', '-fPIC', '-shared', '-D_GNU_SOURCE', '-D__WINESRC__',
             '-DWINE_UNIX_LIB', '-D_NTSYSTEM_', '-I'+str(include), '-I'+str(src/'include'),
             '-I'+str(backend)]
    nt = a.package/'files/lib/wine/aarch64-unix/ntdll.so'
    command = flags+list(map(str,sources))+['-L'+str(nt.parent), '-l:ntdll.so', '-lm', '-pthread',
        '-Wl,-z,relro,-z,now', '-Wl,--no-undefined', '-Wl,-soname,libwineohos-gnu.so', '-o', str(output)]
    with (task/'build.log').open('w') as log:
        for _ in range(80):
            result = subprocess.run(command, capture_output=True, text=True)
            log.write(result.stdout+result.stderr)
            if not result.returncode:
                break
            missing = re.search(r'fatal error: ([\w/.-]+)\.h: No such file', result.stderr)
            if not missing:
                raise RuntimeError(result.stderr[-8000:])
            name = missing[1]
            source = src/'include'/(name+'.idl')
            if not source.is_file():
                raise RuntimeError('Missing non-IDL header '+name+'\n'+result.stderr[-2000:])
            dest = include/(name+'.h')
            dest.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(a.widl), '-m64', '-I'+str(src/'include'), '-h', '-o', str(dest), str(source)],
                           check=True, stdout=log, stderr=log)
        else:
            raise RuntimeError('Header closure exceeded limit')
    symbols = subprocess.check_output(['readelf','-Ws',str(output)],text=True)
    original = subprocess.check_output(['readelf','-Ws',str(a.package/'files/lib/wine/aarch64-unix/winealsa.so')],text=True)
    for name in ['__wine_unix_call_funcs','__wine_unix_call_wow64_funcs']:
        def size(text):
            return int(re.search(r'\S+\s+(\d+)\s+OBJECT\s+GLOBAL\s+DEFAULT\s+\d+\s+'+name+r'\b',text)[1])
        assert size(symbols) == size(original) == 296, 'Audio function table ABI mismatch'
    # Include the compiler's actual Wine/generated header closure, not only
    # the top-level ABI header, so a header change cannot reuse a stale DSO.
    dependencies = {str(nt): sha(nt)}
    for source in sources:
        listing = subprocess.check_output(flags + ['-MM', str(source)], text=True)
        for path in shlex.split(listing.replace('\\\n', ' ').split(':', 1)[1]):
            dependency = Path(path).resolve(strict=True)
            dependencies[str(dependency)] = sha(dependency)
    audio_format = shared_audio_format(output.read_bytes())
    identity.write_text(json.dumps({'inputs':inputs,'dependencies':dependencies,
        'bytes':output.stat().st_size,'sha256':sha(output),
        'abiHeaderSha256':sha(a.abi_header),'unixCallSlots':37,'productRuntimeModified':False,
        'sharedAudio':audio_format,
        'scope':'Private GNU Proton; product Wine AudioBroker and Float32 contract, GNU bootstrap and opt-in flow counters'},indent=2)+'\n')
    print('Built GNU direct audio backend',output.stat().st_size,sha(output))


if __name__ == '__main__':
    main()
