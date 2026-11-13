"""Execute the production upload submit paths against a dependency-aware Vulkan double.

The GPU double leaves a guest timeline wait unresolved. Private upload slots must
retire independently; an upload error must never be hidden by a successful guest
submit. This is a host regression, not qualification of an OHOS GPU.
"""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'thirdparty/virglrenderer/src/venus/vkr_queue.c'
TASK = ROOT / 'build/host_tests/venus-shadow-submission'


def function(text, name):
    start = text.index('static VkResult\n' + name + '(')
    return text[start:text.index('\n}', start) + 3]


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "venus-protocol/vulkan.h"
#define __OHOS__ 1
#define VKR_WINEHUA_INLINE_MAX_GUEST_SUBMITS 64u
#define VKR_WINEHUA_SHADOW_UPLOAD_SLOT_COUNT 8u
static void __attribute__((unused)) vkr_log(const char *format,...) {(void)format;}
struct vkr_context { bool fatal; };
struct vn_dispatch_context { struct vkr_context *data; };
struct vkr_shadow_upload_slot {
    VkCommandBuffer command; bool in_flight; uint64_t retire_value;
};
struct vkr_queue {
    struct { uint64_t id; } base;
    struct vkr_shadow_upload_slot shadow_upload_slots[8];
    uint32_t shadow_upload_slot;
    uint64_t shadow_upload_next_value;
    VkSemaphore shadow_upload_timeline;
    bool shadow_upload_prepared;
    unsigned shadow_upload_mutex;
};
struct vn_device_proc_table { PFN_vkQueueSubmit QueueSubmit; PFN_vkQueueSubmit2 QueueSubmit2; };
struct vn_command_vkQueueSubmit {
    VkQueue queue; uint32_t submitCount; const VkSubmitInfo *pSubmits;
    VkFence fence; VkResult ret;
};
struct vn_command_vkQueueSubmit2 {
    VkQueue queue; uint32_t submitCount; const VkSubmitInfo2 *pSubmits;
    VkFence fence; VkResult ret;
};
struct vkr_shadow_sync_stats { uint64_t bytes,copies,cache_ops,scanned,elapsed_us; };
static struct vkr_context context;
static struct vkr_queue *active;
static VkResult upload_result,driver_result;
static unsigned calls,guest_calls,cpu_fallbacks,generation_depth;
static VkResult host_wait_result;
static uint64_t completed_upload;
static bool guest_blocked;
static const VkSemaphore guest_wait=(VkSemaphore)(uintptr_t)101;
static const VkSemaphore guest_signal=(VkSemaphore)(uintptr_t)102;
static const VkFence guest_fence=(VkFence)(uintptr_t)103;
static const VkCommandBuffer guest_command=(VkCommandBuffer)(uintptr_t)104;
static void __attribute__((unused)) vkr_context_set_fatal(struct vkr_context *ctx) {ctx->fatal=true;}
static uint64_t __attribute__((unused)) vkr_ohos_queue_now_ns(void) {return 0;}
static void __attribute__((unused)) vkr_device_memory_disable_shadow_upload_coverage(struct vkr_context *ctx) {(void)ctx;cpu_fallbacks++;}
static struct vkr_shadow_sync_stats __attribute__((unused)) vkr_device_memory_sync_shadows_to_host(struct vkr_context *ctx) {(void)ctx;cpu_fallbacks++;return (struct vkr_shadow_sync_stats){0};}
static VkResult vkr_device_memory_submit_shadow_upload(struct vkr_queue *queue) {(void)queue;return upload_result;}
static VkResult submit1(VkQueue queue,uint32_t count,const VkSubmitInfo *infos,VkFence fence) {
    (void)queue;calls++;assert(fence==guest_fence);
    bool blocked=guest_blocked;
    for(uint32_t i=0;i<count;i++) {
        const VkSubmitInfo *s=&infos[i];
        if(s->waitSemaphoreCount) {assert(s->waitSemaphoreCount==1 && s->pWaitSemaphores[0]==guest_wait);blocked=true;}
        if(s->commandBufferCount && s->pCommandBuffers[0]==guest_command) {guest_calls++;assert(s->signalSemaphoreCount==1 && s->pSignalSemaphores[0]==guest_signal);}
        if(s->signalSemaphoreCount && s->pSignalSemaphores[0]==active->shadow_upload_timeline && !blocked && driver_result==VK_SUCCESS) {
            const VkTimelineSemaphoreSubmitInfo *t=s->pNext;
            assert(t && t->signalSemaphoreValueCount==1);completed_upload=t->pSignalSemaphoreValues[0];
        }
    }
    guest_blocked=blocked;return driver_result;
}
static VkResult submit2(VkQueue queue,uint32_t count,const VkSubmitInfo2 *infos,VkFence fence) {
    (void)queue;calls++;assert(fence==guest_fence);
    bool blocked=guest_blocked;
    for(uint32_t i=0;i<count;i++) {
        const VkSubmitInfo2 *s=&infos[i];
        if(s->waitSemaphoreInfoCount) {assert(s->waitSemaphoreInfoCount==1 && s->pWaitSemaphoreInfos[0].semaphore==guest_wait);blocked=true;}
        if(s->commandBufferInfoCount && s->pCommandBufferInfos[0].commandBuffer==guest_command) {guest_calls++;assert(s->signalSemaphoreInfoCount==1 && s->pSignalSemaphoreInfos[0].semaphore==guest_signal);}
        if(s->signalSemaphoreInfoCount && s->pSignalSemaphoreInfos[0].semaphore==active->shadow_upload_timeline && !blocked && driver_result==VK_SUCCESS) {
            assert(s->pSignalSemaphoreInfos[0].stageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            completed_upload=s->pSignalSemaphoreInfos[0].value;
        }
    }
    guest_blocked=blocked;return driver_result;
}
static void reset(void) {
    context=(struct vkr_context){0};calls=guest_calls=cpu_fallbacks=0;
    upload_result=driver_result=VK_SUCCESS;completed_upload=0;guest_blocked=false;
    host_wait_result=VK_SUCCESS;generation_depth=0;
}
static void mtx_lock(unsigned *depth) {assert(!*depth);++*depth;}
static void mtx_unlock(unsigned *depth) {assert(*depth==1);--*depth;}
static void vkr_device_memory_shadow_generation_begin(struct vkr_context *ctx,bool submit) {(void)ctx;assert(submit && !generation_depth);generation_depth++;}
static void vkr_device_memory_shadow_generation_end(struct vkr_context *ctx) {(void)ctx;assert(generation_depth==1);generation_depth--;}
static VkResult vkr_device_memory_prepare_shadow_upload(struct vkr_context *ctx,struct vkr_queue *queue,bool perf,uint64_t id) {(void)ctx;(void)perf;(void)id;queue->shadow_upload_prepared=true;return upload_result;}
static void vkr_ohos_perf_record_prepare_phases(struct vkr_queue *queue) {(void)queue;}
static VkResult vkr_ohos_wait_deferred_shadow_host_copy(struct vkr_context *ctx,struct vkr_queue *queue,struct vn_device_proc_table *vk) {(void)ctx;(void)queue;(void)vk;return host_wait_result;}
'''

MAIN = r'''
int main(int argc,char **argv) {
    static struct vkr_queue queue={.shadow_upload_timeline=(VkSemaphore)(uintptr_t)201};
    struct vn_device_proc_table vk={submit1,submit2};active=&queue;
    for(unsigned i=0;i<8;i++) queue.shadow_upload_slots[i].command=(VkCommandBuffer)(uintptr_t)(300+i);
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo guest={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&guest_wait,.pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&guest_command,.signalSemaphoreCount=1,.pSignalSemaphores=&guest_signal};
    VkCommandBufferSubmitInfo command={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,.commandBuffer=guest_command};
    VkSemaphoreSubmitInfo wait={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,.semaphore=guest_wait,.value=9,.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSemaphoreSubmitInfo signal={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,.semaphore=guest_signal,.value=10,.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 guest2={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO_2,.waitSemaphoreInfoCount=1,.pWaitSemaphoreInfos=&wait,.commandBufferInfoCount=1,.pCommandBufferInfos=&command,.signalSemaphoreInfoCount=1,.pSignalSemaphoreInfos=&signal};
    struct vn_command_vkQueueSubmit a={.submitCount=1,.pSubmits=&guest,.fence=guest_fence};
    struct vn_command_vkQueueSubmit2 b={.submitCount=1,.pSubmits=&guest2,.fence=guest_fence};
    if(argc>1 && !strcmp(argv[1],"prepare")) {
        const VkResult errors[]={VK_TIMEOUT,VK_ERROR_DEVICE_LOST,VK_ERROR_OUT_OF_DEVICE_MEMORY};
        for(unsigned api=0;api<2;api++) for(unsigned phase=0;phase<2;phase++) for(unsigned i=0;i<3;i++) {
            reset();a.ret=b.ret=VK_SUCCESS;
            if(phase) host_wait_result=errors[i];else upload_result=errors[i];
            if(api) preflight2(&queue,&vk,&b);else preflight1(&queue,&vk,&a);
            assert((api?b.ret:a.ret)==(errors[i]==VK_TIMEOUT?VK_ERROR_DEVICE_LOST:errors[i]));
            assert(context.fatal && !queue.shadow_upload_prepared && !generation_depth && !queue.shadow_upload_mutex);
            assert(!calls && !guest_calls && !cpu_fallbacks);
        }
        return 0;
    }
    if(argc<2 || strcmp(argv[1],"failure")) for(unsigned api=0;api<2;api++) {
        reset();queue.shadow_upload_slot=0;queue.shadow_upload_next_value=0;
        for(unsigned n=0;n<16;n++) {
            queue.shadow_upload_prepared=true;
            unsigned slot=queue.shadow_upload_slot;
            assert((api? vkr_ohos_queue_submit2_inline_upload(&queue,&vk,&b): vkr_ohos_queue_submit_inline_upload(&queue,&vk,&a))==VK_SUCCESS);
            assert(guest_blocked && queue.shadow_upload_slots[slot].in_flight);
            /* Guest work may still wait for a future signal. Reusing its
             * separate upload command must not depend on that signal. */
            assert(completed_upload>=queue.shadow_upload_slots[slot].retire_value);
            assert(!queue.shadow_upload_prepared);
            /* Release this guest batch before the next call. A later upload
             * cannot bypass an earlier blocked guest batch on the same queue. */
            guest_blocked=false;
        }
        assert(guest_calls==16);
        VkSubmitInfo many[64];VkSubmitInfo2 many2[64];
        for(unsigned n=0;n<64;n++) {many[n]=guest;many2[n]=guest2;}
        for(unsigned count=0;count<=64;count+=64) {
            reset();queue.shadow_upload_prepared=true;
            a.submitCount=b.submitCount=count;a.pSubmits=many;b.pSubmits=many2;
            assert((api? vkr_ohos_queue_submit2_inline_upload(&queue,&vk,&b):vkr_ohos_queue_submit_inline_upload(&queue,&vk,&a))==VK_SUCCESS);
            assert(calls==1 && guest_calls==count && completed_upload);
        }
        a.submitCount=b.submitCount=1;a.pSubmits=&guest;b.pSubmits=&guest2;
        reset();queue.shadow_upload_prepared=true;
        unsigned slot=queue.shadow_upload_slot;
        queue.shadow_upload_slots[slot].in_flight=false;
        driver_result=VK_ERROR_DEVICE_LOST;
        assert((api? vkr_ohos_queue_submit2_inline_upload(&queue,&vk,&b): vkr_ohos_queue_submit_inline_upload(&queue,&vk,&a))==driver_result);
        assert(queue.shadow_upload_slot==slot && !queue.shadow_upload_slots[slot].in_flight);
    }
    if(argc>1 && !strcmp(argv[1],"retirement")) return 0;
    const VkResult errors[]={VK_ERROR_DEVICE_LOST,VK_ERROR_OUT_OF_DEVICE_MEMORY,VK_ERROR_OUT_OF_HOST_MEMORY,VK_TIMEOUT};
    for(unsigned api=0;api<2;api++) for(unsigned i=0;i<4;i++) {
        reset();upload_result=errors[i];
        VkResult result=api? dispatch_submit2(&queue,&vk,&b,false): dispatch_submit1(&queue,&vk,&a,false);
        assert(result!=VK_SUCCESS && context.fatal);
        assert(!calls && !guest_calls && !cpu_fallbacks);
        assert(result==(errors[i]==VK_TIMEOUT? VK_ERROR_DEVICE_LOST:errors[i]));
    }
    for(unsigned api=0;api<2;api++) {
        reset();
        assert((api? dispatch_submit2(&queue,&vk,&b,false):dispatch_submit1(&queue,&vk,&a,false))==VK_SUCCESS);
        assert(calls==1 && guest_calls==1 && !context.fatal);
        reset();queue.shadow_upload_prepared=true;driver_result=VK_ERROR_DEVICE_LOST;
        assert((api? dispatch_submit2(&queue,&vk,&b,true):dispatch_submit1(&queue,&vk,&a,true))==VK_ERROR_DEVICE_LOST);
        assert(context.fatal && !queue.shadow_upload_prepared);
    }
    return 0;
}
'''


class SubmissionContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        text = SOURCE.read_text()
        body = PREAMBLE
        for name in ('vkr_ohos_queue_submit_inline_upload',
                     'vkr_ohos_queue_submit2_inline_upload'):
            body += function(text, name)
        if 'static VkResult\nvkr_ohos_abort_shadow_submission(' in text:
            body += function(text, 'vkr_ohos_abort_shadow_submission')
        for api, signature in ((1, 'vkQueueSubmit('), (2, 'vkQueueSubmit2(')):
            dispatch = text[text.index('vkr_dispatch_' + signature):]
            pre_begin = dispatch.index('   VkResult upload_prepare_result = VK_SUCCESS;')
            pre_end = dispatch.index('   struct vkr_shadow_sync_stats shadow_stats =') if api == 1 else dispatch.index('   vkr_device_memory_sync_shadows_to_host(dispatch->data);')
            # Keep every production early return and unlock. Only the timing
            # declarations and successful caller's cleanup are supplied here.
            preamble = f'''
static void preflight{api}(struct vkr_queue *queue,struct vn_device_proc_table *vk,
    struct vn_command_vkQueueSubmit{'' if api == 1 else '2'} *args) {{
    struct vn_dispatch_context state={{&context}},*dispatch=&state;
    const bool gpu_upload=true,perf_summary=false,perf_account=false;
    const uint64_t submit_id=1;(void)perf_account;
#ifdef __OHOS__
'''
            pre = dispatch[pre_begin:pre_end]
            if api == 1:
                # The next block opens an OHOS guard before the sync call.
                pre = pre[:pre.rfind('#ifdef __OHOS__')]
            body += preamble + pre
            if api == 1:
                body += '    (void)prepare_end_ns;\n'
            body += '''
    vkr_device_memory_shadow_generation_end(dispatch->data);
    mtx_unlock(&queue->shadow_upload_mutex);
}
'''
            begin = dispatch.index('   VkResult upload_submit_result = upload_prepare_result;')
            end = dispatch.index('   if (args->ret == VK_SUCCESS && vkr_ohos_submit_postwait_enabled())', begin)
            body += f'''
static VkResult dispatch_submit{api}(struct vkr_queue *queue,struct vn_device_proc_table *vk,
    struct vn_command_vkQueueSubmit{'' if api == 1 else '2'} *args,bool inline_upload) {{
    struct vn_dispatch_context state={{&context}},*dispatch=&state;
    const bool gpu_upload=true;const VkResult upload_prepare_result=VK_SUCCESS;
    const bool perf_account=false;(void)perf_account;
    struct vkr_shadow_sync_stats shadow_stats={{0}};
    (void)gpu_upload;(void)dispatch;(void)shadow_stats;
#ifdef __OHOS__
''' + dispatch[begin:end] + '\n#endif\n' + ('    (void)upload_end_ns;\n' if api == 1 else '') + '    return args->ret;\n}\n'
        body += MAIN
        TASK.mkdir(parents=True, exist_ok=True)
        source, binary = TASK / 'submission.c', TASK / 'submission'
        source.write_text(body)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'thirdparty/virglrenderer/src/venus'),
                        source, '-o', binary], check=True, timeout=30)
        cls.binary = binary

    def test_upload_retires_before_unresolved_guest_wait(self):
        subprocess.run([self.binary, 'retirement'], check=True, timeout=10)

    def test_upload_failure_never_submits_guest_or_falls_back_to_cpu(self):
        subprocess.run([self.binary, 'failure'], check=True, timeout=10)

    def test_prepare_and_host_wait_failures_release_both_locks(self):
        subprocess.run([self.binary, 'prepare'], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
