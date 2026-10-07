"""Real mutex contention through the production opt-in WGL timing helper."""
import os
import subprocess
import unittest

import wine_shm_state_cache_test as base
from winehua_perf_test import production_helpers

MAIN = r'''
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int holding;
static void *holder(void *unused)
{
    struct timespec delay = {0, 30000000};
    (void)unused;
    assert(!pthread_mutex_lock(&gate));
    assert(!pthread_mutex_lock(&wgl_lock));
    holding = 1;
    assert(!pthread_cond_signal(&ready));
    assert(!pthread_mutex_unlock(&gate));
    assert(!nanosleep(&delay, NULL));
    assert(!pthread_mutex_unlock(&wgl_lock));
    return NULL;
}
int main(int argc, char **argv)
{
    pthread_t worker;
    assert(argc == 2);
    perf_enabled = !strcmp(argv[1], "on");
    assert(!pthread_mutex_lock(&gate));
    assert(!pthread_create(&worker, NULL, holder, NULL));
    while (!holding) assert(!pthread_cond_wait(&ready, &gate));
    assert(!pthread_mutex_unlock(&gate));
    assert(!pthread_mutex_lock(&wgl_lock));
    /* Ownership is preserved: a second acquisition must observe a busy mutex. */
    assert(pthread_mutex_trylock(&wgl_lock) == EBUSY);
    assert(!pthread_mutex_unlock(&wgl_lock));
    assert(!pthread_join(worker, NULL));
    if (perf_enabled)
    {
        struct winehua_perf_counter *c = &winehua_wgl_lock_perf.counter;
        assert(c->count == 1 && c->total_us >= 10000 && c->results[0] == 1);
        assert(perf_clock_calls == 2);
    }
    else
    {
        assert(!perf_clock_calls && !perf_log_count && !winehua_perf_state.window_start);
    }
    puts("Production WGL lock ownership and opt-in contention timing PASS");
    return 0;
}
'''


class WglLockWait(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        source = cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        helpers = production_helpers(source, '#include <errno.h>\n')
        # Use the real monotonic clock, while retaining the disabled clock counter.
        helpers = helpers.replace(
            '(void)id; ++perf_clock_calls; if(perf_clock_fail)return -1; t->tv_sec=fake_us/1000000; t->tv_nsec=(fake_us%1000000)*1000; ++fake_us; return 0;',
            '++perf_clock_calls; return clock_gettime(id, t);')
        assert 'return clock_gettime(id, t);' in helpers
        helper = cls.first_replay['dlls/opengl32/winehua_wgl_lock_perf.h'].decode()
        body = helpers + '\npthread_mutex_t wgl_lock = PTHREAD_MUTEX_INITIALIZER;\n' + helper + MAIN
        file = cls.folder / 'lock.c'
        file.write_text(body)
        cls.binary = cls.folder / 'lock'
        subprocess.run(['cc', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra',
                        '-Wno-unused-function', '-Wno-unused-variable',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        str(file), '-pthread', '-o', str(cls.binary)], check=True)

    def test_enabled_and_disabled_mutex_semantics(self):
        for mode in ('off', 'on'):
            result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=5,
                                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1'})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_replay_is_idempotent(self):
        self.assertEqual(self.first_replay, self.second_replay)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + base.remaining)
