"""Compile the patched production functions with lifetime and Vulkan stubs.

The complete registered Wine patch chain is replayed from the pinned source;
platform/GPU calls are replaced, while the configure/acquire/present logic is
real. ASan also proves the old configure and stage-array defects reproduce.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
options, remaining = parser.parse_known_args()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


CONFIG_STUBS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL, INT, HWND;
typedef unsigned UINT, DWORD;
typedef struct { int left, top, right, bottom; } RECT;
#define FALSE 0
#define TRUE 1
#define TRACE(...) ((void)0)
#define WAYLAND_SURFACE_CONFIG_STATE_RESIZING 1
#define WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED 2
#define WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN 4
#define WAYLAND_SURFACE_CONFIG_STATE_TILED 8
#define SWP_FRAMECHANGED 1
#define SWP_NOACTIVATE 2
#define SWP_NOZORDER 4
#define SWP_NOOWNERZORDER 8
#define SWP_NOMOVE 16
#define SWP_NOSIZE 32
#define SWP_NOSENDCHANGING 64
#define GWL_STYLE 0
#define WS_MAXIMIZE 0x100
#define WM_ENTERSIZEMOVE 1
#define WM_EXITSIZEMOVE 2
struct config { int width, height; uint32_t state, serial; };
struct wayland_surface {
    struct config requested, processing, current;
    BOOL resizing;
    struct { RECT rect; uint32_t state; } window;
};
struct wayland_win_data {
    struct wayland_surface *wayland_surface;
    struct { RECT window; } rects;
};
static struct wayland_win_data *current_data;
static int released, positioned, xdg_toplevel = 1;
static struct wayland_win_data *wayland_win_data_get(HWND hwnd) {
    assert(hwnd == 7); return current_data;
}
static void wayland_win_data_release(struct wayland_win_data *data) {
    assert(data == current_data);
    free(data->wayland_surface); free(data); current_data = NULL; released++;
}
static BOOL wayland_surface_is_toplevel(struct wayland_surface *surface) {
    assert(surface); return xdg_toplevel;
}
static void wayland_surface_coords_from_window(struct wayland_surface *s,
        int width, int height, int *out_w, int *out_h) {
    assert(s); *out_w = width; *out_h = height;
}
static void wayland_surface_coords_to_window(struct wayland_surface *s,
        int width, int height, int *out_w, int *out_h) {
    assert(s); *out_w = width; *out_h = height;
}
static BOOL wayland_surface_config_is_compatible(struct config *c, int w, int h, uint32_t state) {
    (void)c; (void)w; (void)h; (void)state; return FALSE;
}
static void send_message(HWND hwnd, int message, int wp, int lp) {
    assert(hwnd == 7 && released); (void)message; (void)wp; (void)lp;
}
static DWORD NtUserGetWindowLongW(HWND hwnd, int field) {
    assert(hwnd == 7 && released); (void)field; return 0;
}
static void NtUserSetWindowLong(HWND hwnd, int field, DWORD style, BOOL notify) {
    assert(hwnd == 7 && released); (void)field; (void)style; (void)notify;
}
static void SetRect(RECT *r, int l, int t, int right, int bottom) {
    *r = (RECT){l, t, right, bottom};
}
static void OffsetRect(RECT *r, int x, int y) {
    r->left += x; r->right += x; r->top += y; r->bottom += y;
}
static void NtUserSetRawWindowPos(HWND hwnd, RECT rect, UINT flags, BOOL notify) {
    assert(hwnd == 7 && released && !notify);
    assert(rect.left == 13 && rect.top == 17 && rect.right == 653 && rect.bottom == 497);
    assert(flags & SWP_NOMOVE); positioned++;
}
'''
CONFIG_MAIN = r'''
int main(void) {
    for (int i = 0; i < 1000; ++i) {
        current_data = calloc(1, sizeof(*current_data));
        current_data->wayland_surface = calloc(1, sizeof(*current_data->wayland_surface));
        current_data->rects.window = (RECT){13,17,653,497};
        current_data->wayland_surface->requested = (struct config){640,480,
            WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED | WAYLAND_SURFACE_CONFIG_STATE_RESIZING, 1};
        released = 0;
        wayland_configure_window(7);
        assert(released == 1 && !current_data);
    }
    assert(positioned == 1000);
    wayland_configure_window(7); /* late configure after destroy */
    assert(positioned == 1000);
    puts("configure: destruction at unlock and 1000 coordinate snapshots passed");
}
'''

VULKAN_STUBS = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef int BOOL, VkResult;
typedef uintptr_t VkQueue, VkSemaphore, VkFence, VkCommandBuffer, VkSwapchainKHR, VkImage;
typedef uint32_t VkPipelineStageFlags;
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define VK_SUCCESS 0
#define VK_TIMEOUT 2
#define VK_SUBOPTIMAL_KHR 1000001003
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_DEVICE_LOST -4
#define VK_ERROR_INITIALIZATION_FAILED -3
#define VK_ERROR_EXTENSION_NOT_PRESENT -7
#define VK_NULL_HANDLE 0
#define VK_STRUCTURE_TYPE_SUBMIT_INFO 4
#define VK_PIPELINE_STAGE_ALL_COMMANDS_BIT 0x10000
#define VK_IMAGE_LAYOUT_PRESENT_SRC_KHR 1000001002
#define ERR(...) ((void)0)
typedef struct {
    int sType; const void *pNext;
    uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores;
    const VkPipelineStageFlags *pWaitDstStageMask;
    uint32_t commandBufferCount; const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount; const VkSemaphore *pSignalSemaphores;
} VkSubmitInfo;
typedef struct {
    int sType; const void *pNext;
    uint32_t waitSemaphoreCount; const VkSemaphore *pWaitSemaphores;
    uint32_t swapchainCount; const VkSwapchainKHR *pSwapchains;
    const uint32_t *pImageIndices; VkResult *pResults;
} VkPresentInfoKHR;
struct vulkan_queue;
struct vulkan_device {
    struct vulkan_queue *queues;
    VkResult (*p_vkQueueSubmit)(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
};
struct vulkan_queue { struct vulkan_device *device; struct { VkQueue queue; } host; };
struct semaphore { struct { struct { VkSemaphore semaphore; } host; } obj; };
struct fence { struct { struct { VkFence fence; } host; } obj; };
struct surface { void *client; uint32_t winehua_surface_id, winehua_owner_pid; int hwnd; };
struct swapchain {
    struct { struct { VkSwapchainKHR swapchain; } client; } obj;
    struct vulkan_device *device; struct surface *surface; BOOL winehua_private;
    uint32_t image_count, next_image, serial, format;
    struct { uint32_t width, height; } extents;
    VkImage *images; BOOL *acquired; VkCommandBuffer acquire_command;
    pthread_mutex_t mutex; pthread_cond_t cond;
};
static int submissions, presents, updates, clock_reads, fail_alloc;
static VkResult submit_result;
static void *test_malloc(size_t size) { return fail_alloc ? NULL : malloc(size); }
static int test_clock_gettime(clockid_t id, struct timespec *value) {
    clock_reads++; return clock_gettime(id, value);
}
static struct swapchain *swapchain_from_handle(VkSwapchainKHR handle) { return (void *)handle; }
static struct semaphore *semaphore_from_handle(VkSemaphore handle) { return (void *)handle; }
static struct fence *fence_from_handle(VkFence handle) { return (void *)handle; }
static int winehua_present_image_trace_enabled(void) { return 0; }
static void client_surface_update(void *client) { assert(client); updates++; }
static void client_surface_present(void *client) { assert(client); }
static int present_image(void) { presents++; return 0; }
#define winehua_present_image(...) present_image()
static VkResult submit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    assert(queue == 19 && count == 1); (void)fence; submissions++;
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        assert(info->pWaitDstStageMask[i] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        assert(info->pWaitSemaphores[i] == i + 100);
    }
    return submit_result;
}
#define malloc test_malloc
#define clock_gettime test_clock_gettime
'''
VULKAN_MAIN = r'''
#undef malloc
#undef clock_gettime
static struct surface surface = {.client = (void *)1};
static struct vulkan_device device;
static struct vulkan_queue queue = {.device = &device, .host = {.queue = 19}};
static void init(struct swapchain *s, BOOL private_route) {
    memset(s, 0, sizeof(*s)); s->device = &device; s->surface = &surface;
    s->winehua_private = private_route; s->image_count = 1;
    s->images = calloc(1, sizeof(*s->images)); s->acquired = calloc(1, sizeof(*s->acquired));
    pthread_mutex_init(&s->mutex, NULL); pthread_cond_init(&s->cond, NULL);
}
static void cleanup(struct swapchain *s) {
    pthread_mutex_destroy(&s->mutex); pthread_cond_destroy(&s->cond);
    free(s->images); free(s->acquired);
}
static void stages(void) {
    const int counts[] = {0, 1, 2, 16, 17, 64};
    for (unsigned n = 0; n < ARRAY_SIZE(counts); ++n) {
        struct swapchain s; init(&s, TRUE); s.acquired[0] = TRUE;
        struct semaphore sems[64]; VkSemaphore handles[64];
        for (int i = 0; i < counts[n]; ++i) { sems[i].obj.host.semaphore = i + 100; handles[i] = (uintptr_t)&sems[i]; }
        VkSwapchainKHR chain = (uintptr_t)&s; uint32_t index = 0; VkResult status = 123;
        VkPresentInfoKHR info = {.waitSemaphoreCount = counts[n], .pWaitSemaphores = handles,
            .swapchainCount = 1, .pSwapchains = &chain, .pImageIndices = &index, .pResults = &status};
        submissions = presents = updates = 0;
        assert(winehua_queue_present(&queue, &info) == VK_SUCCESS);
        assert(submissions == (counts[n] != 0) && presents == 1 && status == VK_SUCCESS && !s.acquired[0]);
        if (counts[n] > 16) {
            s.acquired[0] = TRUE; fail_alloc = 1; submissions = presents = 0;
            assert(winehua_queue_present(&queue, &info) == VK_ERROR_OUT_OF_HOST_MEMORY);
            assert(submissions == 0 && presents == 0 && s.acquired[0]); fail_alloc = 0;
        }
        if (counts[n]) {
            submit_result = VK_ERROR_DEVICE_LOST; submissions = presents = 0; s.acquired[0] = TRUE;
            assert(winehua_queue_present(&queue, &info) == VK_ERROR_DEVICE_LOST);
            assert(submissions == 1 && presents == 0 && s.acquired[0]); submit_result = 0;
        }
        cleanup(&s);
    }
    puts("present: semaphore counts 0/1/2/16/17/64, allocation and submit failures passed");
}
static void *wakeups(void *arg) {
    struct swapchain *s = arg;
    for (int i = 0; i < 24; ++i) {
        struct timespec pause = {0, 5000000}; nanosleep(&pause, NULL);
        pthread_mutex_lock(&s->mutex); pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}
static void *release_image(void *arg) {
    struct swapchain *s = arg; struct timespec pause = {0, 5000000}; nanosleep(&pause, NULL);
    pthread_mutex_lock(&s->mutex); s->acquired[0] = FALSE;
    pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->mutex); return NULL;
}
static void deadline(void) {
    struct swapchain s; init(&s, TRUE); s.acquired[0] = TRUE;
    pthread_t thread; uint32_t index = 99;
    clock_reads = 0; pthread_create(&thread, NULL, wakeups, &s);
    assert(winehua_swapchain_acquire(&s, 30000000, 0, 0, &index) == VK_TIMEOUT);
    assert(clock_reads == 1 && index == 99 && s.acquired[0]);
    pthread_join(thread, NULL);
    clock_reads = 0;
    assert(winehua_swapchain_acquire(&s, 0, 0, 0, &index) == VK_TIMEOUT);
    assert(clock_reads == 0);
    pthread_create(&thread, NULL, release_image, &s);
    assert(winehua_swapchain_acquire(&s, UINT64_MAX, 0, 0, &index) == VK_SUCCESS);
    assert(index == 0 && clock_reads == 0); pthread_join(thread, NULL);
    s.acquired[0] = FALSE;
    struct semaphore sem = {.obj = {.host = {.semaphore = 77}}};
    struct fence f = {.obj = {.host = {.fence = 88}}};
    submissions = 0; submit_result = VK_ERROR_DEVICE_LOST;
    assert(winehua_swapchain_acquire(&s, 0, (uintptr_t)&sem, (uintptr_t)&f, &index) == VK_ERROR_DEVICE_LOST);
    assert(submissions == 1 && !s.acquired[0]); submit_result = 0;
    cleanup(&s); puts("acquire: repeated wakeups, zero/infinite timeout and failed submit passed");
}
static void routes(void) {
    struct swapchain a, b; init(&a, TRUE); init(&b, FALSE);
    VkSwapchainKHR chains[] = {(uintptr_t)&a, (uintptr_t)&b};
    VkResult statuses[] = {123, 123}; uint32_t indexes[] = {0,0};
    VkPresentInfoKHR info = {.swapchainCount = 2, .pSwapchains = chains,
        .pImageIndices = indexes, .pResults = statuses};
    for (int i = 0; i < 2; ++i) {
        submissions = presents = updates = 0;
        assert(dispatch(&queue, &info) == VK_ERROR_INITIALIZATION_FAILED);
        assert(statuses[0] == VK_ERROR_INITIALIZATION_FAILED && statuses[1] == VK_ERROR_INITIALIZATION_FAILED);
        assert(!submissions && !presents && !updates);
        chains[0] = (uintptr_t)&b; chains[1] = (uintptr_t)&a;
    }
    info.pResults = NULL;
    assert(dispatch(&queue, &info) == VK_ERROR_INITIALIZATION_FAILED);
    info.swapchainCount = 1; chains[0] = (uintptr_t)&b;
    assert(dispatch(&queue, &info) == 77); /* normal route untouched */
    chains[0] = (uintptr_t)&a; assert(dispatch(&queue, &info) == VK_SUCCESS);
    info.swapchainCount = 0; assert(dispatch(&queue, &info) == 77);
    cleanup(&a); cleanup(&b); puts("routing: both mixed orders reject before submit; homogeneous routes passed");
}
int main(int argc, char **argv) {
    assert(argc == 2); device.queues = &queue; device.p_vkQueueSubmit = submit;
    if (!strcmp(argv[1], "stages")) stages();
    else if (!strcmp(argv[1], "deadline")) deadline();
    else if (!strcmp(argv[1], "routes")) routes();
    else abort();
}
'''


class ProtonStabilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='wine-proton-stability-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.folder = Path(cls.temporary.name)
        script = (ROOT / 'scripts/build_wine.sh').read_text()
        helper = function(script, 'ensure_wine_patch() {')
        cls.patches = [ROOT / 'patches/wine' / name for name in re.findall(
            r'ensure_wine_patch "\$SCRIPT_DIR/\.\./patches/wine/([^"]+)"', script)]
        relatives = set()
        added = set()
        for patch in cls.patches:
            relatives.update(re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.M))
            added.update(re.findall(r'^--- /dev/null\n\+\+\+ b/(.+)$', patch.read_text(), re.M))
        cls.sources = {}
        candidate = cls.folder / 'wine'
        for relative in relatives:
            if relative in added:
                continue
            contents = subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=options.wine_src)
            cls.sources[relative] = contents.decode()
            path = candidate / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(contents)
        command = ('set -euo pipefail\nlog() { :; }\n' + helper +
                   '\nWINE_SRC="$1"\nshift\nfor patch_file in "$@"; do ensure_wine_patch "$patch_file" test; done\n')
        replay = ['bash', '-c', command, 'patch-replay', str(candidate), *map(str, cls.patches)]
        subprocess.run(replay, check=True, capture_output=True, text=True)
        cls.replayed = {relative: (candidate / relative).read_bytes() for relative in relatives}
        subprocess.run(replay, check=True, capture_output=True, text=True)
        cls.replayed_again = {relative: (candidate / relative).read_bytes() for relative in relatives}
        cls.binaries = {}
        for label in ('baseline', 'candidate'):
            config = (cls.sources['dlls/winewayland.drv/window.c'] if label == 'baseline'
                      else (candidate / 'dlls/winewayland.drv/window.c').read_text())
            cls.compile(label + '-configure', CONFIG_STUBS +
                        function(config, 'static void wayland_configure_window(HWND hwnd)') + CONFIG_MAIN)
            source = (cls.sources['dlls/win32u/vulkan.c'] if label == 'baseline'
                      else (candidate / 'dlls/win32u/vulkan.c').read_text())
            functions = function(source, 'static VkResult winehua_swapchain_acquire(')
            if 'static VkResult winehua_validate_present_swapchains(' in source:
                functions += function(source, 'static VkResult winehua_validate_present_swapchains(')
            functions += function(source, 'static VkResult winehua_queue_present(')
            entry = function(source, 'static VkResult win32u_vkQueuePresentKHR(')
            guard_start = entry.index('    if (')
            guard_end = entry.index('    if (!(swapchains =', guard_start)
            dispatch = ('static VkResult dispatch(struct vulkan_queue *queue, VkPresentInfoKHR *present_info)\n'
                        '{\nVkResult res; BOOL private_route;\n' + entry[guard_start:guard_end] +
                        '\n(void)res; (void)private_route; return 77;\n}\n')
            cls.compile(label + '-vulkan', VULKAN_STUBS + functions + dispatch + VULKAN_MAIN)

    @classmethod
    def compile(cls, label, source):
        path = cls.folder / (label + '.c')
        path.write_text(source)
        binary = cls.folder / label
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                        str(path), '-o', str(binary), '-pthread'], check=True)
        cls.binaries[label] = binary

    def run_function(self, label, mode=None):
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1')
        return subprocess.run([str(self.binaries[label]), *([mode] if mode else [])],
                              capture_output=True, text=True, env=environment, timeout=10)

    def test_complete_patch_chain_is_idempotent(self):
        self.assertEqual(self.replayed, self.replayed_again)

    def test_configure_survives_destruction_at_unlock(self):
        result = self.run_function('candidate-configure')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_present_arrays_and_error_paths(self):
        result = self.run_function('candidate-vulkan', 'stages')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_timeout_is_not_restarted_by_other_image_signals(self):
        result = self.run_function('candidate-vulkan', 'deadline')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_both_mixed_orders_reject_before_any_submit(self):
        result = self.run_function('candidate-vulkan', 'routes')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_old_configure_uaf_is_reproduced(self):
        result = self.run_function('baseline-configure')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('heap-use-after-free', result.stderr)

    def test_old_stage_mask_overread_is_reproduced(self):
        result = self.run_function('baseline-vulkan', 'stages')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stack-buffer-overflow', result.stderr)

    def test_old_timeout_restart_is_reproduced(self):
        result = self.run_function('baseline-vulkan', 'deadline')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('clock_reads == 1', result.stderr)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + remaining)
