"""Exercise the actual Winebus wait loop with real sockets/poll on the host.
Device/event delivery and message handling are API boundaries. Not a tablet
performance test; the original and candidate execute the same socket scenarios.
"""
from pathlib import Path
import argparse, json, os, subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
args = parser.parse_args()

def function(text):
    start = text.index('NTSTATUS ohos_bus_wait(void *args)')
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREFIX = '#include <errno.h>\n\n#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <stdint.h>\n#include <pthread.h>\n#include <poll.h>\n#include <sys/socket.h>\n#include <time.h>\n#include <unistd.h>\n#include <assert.h>\n\ntypedef int NTSTATUS;\n#define STATUS_PENDING 1\n#define STATUS_SUCCESS 0\n#define TRACE(...) ((void)0)\nstruct list { struct list *next, *prev; };\nstruct bus_event { int unused; };\nstruct unix_device { struct list entry; };\nstruct ohos_device { struct unix_device unix_device; };\n#define LIST_ENTRY(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))\nstatic struct list event_queue, device_list;\nstatic pthread_mutex_t ohos_cs = PTHREAD_MUTEX_INITIALIZER;\nstatic int quit_requested, sock_fd = -1, neutral, consumed;\nstatic uint64_t deadline, iterations, polls, ready, nval, errors;\n\nstatic uint64_t now(clockid_t clock)\n{\n    struct timespec t;\n    assert(!clock_gettime(clock, &t));\n    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;\n}\nstatic void bus_event_cleanup(struct bus_event *event) { (void)event; }\nstatic void bus_event_queue_destroy(struct list *list) { (void)list; }\nstatic int bus_event_queue_pop(struct list *list, struct bus_event *event)\n{\n    (void)list; (void)event;\n    ++iterations;\n    if (now(CLOCK_MONOTONIC) >= deadline) quit_requested = 1;\n    return 0;\n}\nstatic const char *resolve_socket_path(void) { return NULL; }\nstatic int connect_socket(const char *path) { (void)path; return 0; }\nstatic void close_socket(void)\n{\n    if (sock_fd >= 0) close(sock_fd);\n    sock_fd = -1;\n}\nstatic int list_empty(struct list *list) { return list->next == list; }\nstatic struct list *list_head(struct list *list) { return list->next; }\nstatic void drop_socket_if(int fd) {\n    pthread_mutex_lock(&ohos_cs);\n    if (sock_fd == fd) close_socket();\n    pthread_mutex_unlock(&ohos_cs);\n}\nstatic void apply_neutral(struct unix_device *dev) { assert(dev); ++neutral; }\nstatic void process_one_message(struct unix_device *dev, int fd)\n{\n    char byte;\n    (void)dev;\n    ssize_t n = read(fd, &byte, 1);\n    assert(n >= 0);\n    if (n) ++consumed;\n    else { drop_socket_if(fd); apply_neutral(dev); }\n}\nstatic int tracked_poll(struct pollfd *fds, nfds_t count, int timeout)\n{\n    int ret = poll(fds, count, timeout);\n    ++polls;\n    ready += ret > 0;\n    errors += ret < 0;\n    nval += !!(fds[0].revents & POLLNVAL);\n    return ret;\n}\n#define poll tracked_poll\n'
SUFFIX = '\n#undef poll\nstatic void run_case(const char *name, int has_device, int unread, int invalid)\n{\n    struct ohos_device device;\n    struct bus_event event;\n    int pair[2];\n    uint64_t start, cpu, end, cpu_end;\n    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));\n    device_list.next = device_list.prev = &device_list;\n    if (has_device) device_list.next = device_list.prev = &device.unix_device.entry;\n    sock_fd = pair[0];\n    if (unread) assert(write(pair[1], "x", 1) == 1);\n    if (invalid == 1) close(pair[0]);\n    if (invalid == 2) { close(pair[1]); pair[1] = -1; }\n    neutral = consumed = 0;\n    quit_requested = 0;\n    iterations = polls = ready = nval = errors = 0;\n    start = now(CLOCK_MONOTONIC);\n    cpu = now(CLOCK_PROCESS_CPUTIME_ID);\n    deadline = start + 100000000ull;\n    assert(ohos_bus_wait(&event) == STATUS_SUCCESS);\n    end = now(CLOCK_MONOTONIC);\n    cpu_end = now(CLOCK_PROCESS_CPUTIME_ID);\n    if (pair[1] >= 0) close(pair[1]);\n    printf("{\\"case\\":\\"%s\\",\\"wallMs\\":%.3f,\\"processCpuMs\\":%.3f,"\n           "\\"iterations\\":%llu,\\"pollCalls\\":%llu,\\"ready\\":%llu,\\"nval\\":%llu,\\"errors\\":%llu}\\n",\n           name, (end-start)/1000000., (cpu_end-cpu)/1000000.,\n           (unsigned long long)iterations, (unsigned long long)polls,\n           (unsigned long long)ready, (unsigned long long)nval, (unsigned long long)errors);\n}\nint main(void)\n{\n    run_case("device-present-idle-socket", 1, 0, 0);\n    run_case("no-device-idle-socket", 0, 0, 0);\n    run_case("no-device-unread-socket", 0, 1, 0);\n    run_case("device-present-invalid-descriptor", 1, 0, 1);\n    run_case("device-present-readable-socket", 1, 1, 0);\n    run_case("device-present-peer-closed", 1, 0, 2);\n    return 0;\n}\n'

class PollProgress(unittest.TestCase):
    def test_real_socket_progress(self):
        with tempfile.TemporaryDirectory(prefix='winebus-progress-') as folder:
            folder = Path(folder)
            name = 'dlls/winebus.sys/bus_ohos.c'
            dest = folder / name
            dest.parent.mkdir(parents=True)
            original = (args.wine_src / name).read_text()
            dest.write_text(original)
            patch = ROOT / 'patches/wine/0041-winebus-ohos-poll-progress.patch'
            # Accept either the old or the already patched effective source.
            reverse = subprocess.run(['patch','-p1','-R','--dry-run','--batch','--forward','-i',str(patch)],cwd=folder,capture_output=True)
            if reverse.returncode == 0:
                subprocess.run(['patch','-p1','-R','--batch','--forward','-i',str(patch)],cwd=folder,check=True,capture_output=True)
            old = dest.read_text()
            subprocess.run(['patch','-p1','--batch','--forward','-i',str(patch)],cwd=folder,check=True,capture_output=True)
            new = dest.read_text()
            results = {}
            for label, source in [('old',old),('candidate',new)]:
                cpp = folder / (label+'.c')
                exe = folder / label
                cpp.write_text(PREFIX+function(source)+SUFFIX)
                flags = ['-fsanitize='+os.environ['WINEBUS_TEST_SANITIZERS']] if os.environ.get('WINEBUS_TEST_SANITIZERS') else []
                subprocess.run(['cc','-O2','-Wall','-Wextra','-pthread',*flags,str(cpp),'-o',str(exe)],check=True,capture_output=True)
                run = subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=5)
                results[label] = [json.loads(line) for line in run.stdout.splitlines()]
            print(json.dumps(results,indent=2), flush=True)
            for i in (2,3):
                self.assertGreater(results['old'][i]['pollCalls'],1000)
                self.assertLess(results['candidate'][i]['pollCalls'],20)
                self.assertLess(results['candidate'][i]['processCpuMs'],results['old'][i]['processCpuMs']/5)
            self.assertEqual(results['candidate'][2]['pollCalls'],0)
            self.assertEqual(results['candidate'][3]['nval'],1)
            for row in results['candidate']:
                self.assertLess(row['iterations'],30)
            self.assertEqual(results['candidate'][4]['ready'],1)
            for _ in range(2):
                subprocess.run(['patch','-p1','-R','--batch','--forward','-i',str(patch)],cwd=folder,check=True,capture_output=True)
                self.assertEqual(dest.read_text(),old)
                subprocess.run(['patch','-p1','--batch','--forward','-i',str(patch)],cwd=folder,check=True,capture_output=True)
                self.assertEqual(dest.read_text(),new)

if __name__ == '__main__': unittest.main(argv=['winebus'],verbosity=2)
