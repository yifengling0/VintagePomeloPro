// SPDX-License-Identifier: LGPL-2.1-or-later
// Execute the production staging owner with deterministic Vulkan completions
// and allocation faults. This validates ownership, not device performance.
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace gamescope {
template<class T> struct OwningRc : std::shared_ptr<T> {
    using std::shared_ptr<T>::shared_ptr;
    OwningRc(T *p) : std::shared_ptr<T>(p) {}
};
struct IBackendFb {};
}
static bool textureFailure = false, dma = false;
struct CVulkanTexture {
    struct createFlags { bool bSampled{}, bTransferSrc{}, bTransferDst{}, bFlippable{}; };
    bool local = false, complete = false;
    bool BInit(uint32_t, uint32_t, unsigned, uint32_t, createFlags, void *, int, int,
        void *, gamescope::OwningRc<gamescope::IBackendFb>) { return !textureFailure; }
    void markLocalQueueDependency() { local = true; }
    void markHostUploadComplete() { complete = true; }
};
static uint32_t DRMFormatGetBPP(uint32_t format) { return format == 1 ? 4 : 0; }
struct Backend { bool UsesDmaBufImages() { return dma; } } backend;
static Backend *GetBackend() { return &backend; }
struct Device {
    unsigned allocations = 0, waits = 0, fault = 0;
    uint64_t submitted = 0, completed = 0, next = 1;
    std::map<VkBuffer, VkDeviceMemory> buffers;
    std::map<VkDeviceMemory, std::vector<uint8_t>> memories;
    struct Command {
        Device *device;
        VkBuffer buffer{};
        gamescope::OwningRc<CVulkanTexture> texture;
        void copyBufferToImage(VkBuffer b, int, size_t, gamescope::OwningRc<CVulkanTexture> t) {
            buffer = b; texture = std::move(t);
        }
    };
    struct Pending { VkBuffer buffer; uint8_t expected; gamescope::OwningRc<CVulkanTexture> texture; };
    std::map<uint64_t, Pending> pending;
    struct Api {
        Device *d;
        VkResult CreateBuffer(VkDevice, const VkBufferCreateInfo *i, void *, VkBuffer *b) {
            if (d->fault == 1) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *b = (VkBuffer)d->next++; d->buffers[*b] = {}; size = i->size; return VK_SUCCESS;
        }
        size_t size = 0;
        void GetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *r) {
            *r = {}; r->size = size; r->memoryTypeBits = 1;
        }
        VkResult AllocateMemory(VkDevice, const VkMemoryAllocateInfo *i, void *, VkDeviceMemory *m) {
            if (d->fault == 3) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *m = (VkDeviceMemory)d->next++;
            d->memories[*m].resize(i->allocationSize); ++d->allocations; return VK_SUCCESS;
        }
        VkResult BindBufferMemory(VkDevice, VkBuffer b, VkDeviceMemory m, int) {
            if (d->fault == 4) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            d->buffers[b] = m; return VK_SUCCESS;
        }
        VkResult MapMemory(VkDevice, VkDeviceMemory m, int, VkDeviceSize, int, void **out) {
            if (d->fault == 5) return VK_ERROR_MEMORY_MAP_FAILED;
            *out = d->memories.at(m).data(); return VK_SUCCESS;
        }
        void UnmapMemory(VkDevice, VkDeviceMemory) {}
        void DestroyBuffer(VkDevice, VkBuffer b, void *) {
            for (auto &[seq, p] : d->pending) assert(p.buffer != b);
            assert(d->buffers.erase(b) == 1);
        }
        void FreeMemory(VkDevice, VkDeviceMemory m, void *) { assert(d->memories.erase(m) == 1); }
    } vk{this};
    VkDevice device() { return {}; }
    uint32_t findMemoryType(unsigned, unsigned) { return fault == 2 ? ~0u : 0; }
    uint64_t completedSeqNo() { return completed; }
    void finish(uint64_t seq) {
        for (auto i = pending.begin(); i != pending.end() && i->first <= seq;) {
            auto &bytes = memories.at(buffers.at(i->second.buffer));
            // A CPU overwrite before GPU completion corrupts this assertion.
            for (auto value : bytes) assert(value == i->second.expected);
            i = pending.erase(i);
        }
        completed = seq;
    }
    void wait(uint64_t seq) { ++waits; finish(seq); }
    void garbageCollect() {}
    std::unique_ptr<Command> commandBuffer() { return std::make_unique<Command>(Command{this, {}, {}}); }
    uint64_t submit(std::unique_ptr<Command> c) {
        auto &data = memories.at(buffers.at(c->buffer));
        pending.emplace(++submitted, Pending{c->buffer, data[0], c->texture});
        return submitted;
    }
} g_device;

#include "../native/gamescope/hsl/HslShmUpload.h"

int main() {
    std::vector<uint8_t> pixels(64 * 48 * 4);
    auto upload = [&](auto &pool, unsigned width = 64) {
        return pool.upload(pixels.data(), width, 48, 1, width * 4, nullptr);
    };
    {
        hsl::ShmUploads pool;
        for (unsigned n = 0; n < 120; ++n) {
            std::fill(pixels.begin(), pixels.end(), n);
            auto texture = upload(pool);
            assert(texture && texture->local && !texture->complete);
            g_device.finish(g_device.submitted);
        }
        assert(g_device.allocations == 1 && g_device.waits == 0);
    }
    assert(g_device.memories.empty() && g_device.buffers.empty());
    g_device = Device{}; g_device.vk.d = &g_device;
    {
        hsl::ShmUploads pool;
        for (unsigned n = 0; n < 4; ++n) {
            std::fill(pixels.begin(), pixels.end(), n);
            assert(upload(pool));
        }
        assert(g_device.allocations == 3 && g_device.waits == 1);
        g_device.finish(g_device.submitted);
        // Resize can recycle only completed staging, and never changes an
        // earlier destination snapshot. Invalid formats/rows are rejected.
        assert(upload(pool, 32));
        assert(!pool.upload(pixels.data(), 64, 48, 0, 256, nullptr));
        assert(!pool.upload(pixels.data(), 64, 48, 1, 255, nullptr));
        textureFailure = true; assert(!upload(pool)); textureFailure = false;
    }
    assert(g_device.pending.empty() && g_device.memories.empty() && g_device.buffers.empty());
    for (unsigned fault = 1; fault <= 5; ++fault) {
        g_device.fault = fault;
        { hsl::ShmUploads pool; assert(!upload(pool)); }
        assert(g_device.memories.empty() && g_device.buffers.empty());
    }
    g_device.fault = 0; dma = true;
    {
        hsl::ShmUploads pool; auto texture = upload(pool);
        assert(texture && texture->complete && !texture->local && g_device.pending.empty());
    }
    assert(g_device.memories.empty() && g_device.buffers.empty());
    puts("HSL_SHM_UPLOAD_PASS reuse=120 pressure=4 faults=5 resize=1 dma=1 drain=1");
}
