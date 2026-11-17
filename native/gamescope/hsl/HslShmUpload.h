// SPDX-License-Identifier: LGPL-2.1-or-later
// HSL: GNU Vulkan staging ownership. Include after rendervulkan.hpp.
#pragma once

#include <array>
#include <memory>

namespace hsl {
// Snapshots never alias the client's wl_shm storage. Only upload buffers are
// reused, after their actual timeline point has completed. The compositor's
// command buffers retain each destination texture until GPU use has finished.
class ShmUploads {
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkDeviceSize capacity = 0;
        uint64_t sequence = 0;

        ~Slot() { clear(); }
        void clear() {
            if (sequence) g_device.wait(sequence);
            sequence = 0;
            if (mapped) g_device.vk.UnmapMemory(g_device.device(), memory);
            if (buffer) g_device.vk.DestroyBuffer(g_device.device(), buffer, nullptr);
            if (memory) g_device.vk.FreeMemory(g_device.device(), memory, nullptr);
            buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
            mapped = nullptr; capacity = 0;
        }
        bool init(VkDeviceSize bytes) {
            clear();
            VkBufferCreateInfo bi = {};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = bytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            if (g_device.vk.CreateBuffer(g_device.device(), &bi, nullptr, &buffer) != VK_SUCCESS)
                return false;
            VkMemoryRequirements requirements;
            g_device.vk.GetBufferMemoryRequirements(g_device.device(), buffer, &requirements);
            uint32_t type = g_device.findMemoryType(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, requirements.memoryTypeBits);
            if (type == ~0u) return false;
            VkMemoryAllocateInfo ai = {};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = requirements.size; ai.memoryTypeIndex = type;
            if (g_device.vk.AllocateMemory(g_device.device(), &ai, nullptr, &memory) != VK_SUCCESS ||
                g_device.vk.BindBufferMemory(g_device.device(), buffer, memory, 0) != VK_SUCCESS ||
                g_device.vk.MapMemory(g_device.device(), memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
                return false;
            capacity = bytes;
            return true;
        }
    };
    // At most three 64 MiB logical buffers. Driver allocation padding is
    // queried separately. Larger surfaces keep the synchronous one-shot path.
    static constexpr VkDeviceSize maxBytes = 64ull << 20;
    std::array<Slot, 3> slots;
    uint64_t uploads = 0, allocations = 0, pressureWaits = 0;
public:
    gamescope::OwningRc<CVulkanTexture> upload(const void *src, uint32_t width,
        uint32_t height, uint32_t format, size_t stride,
        gamescope::OwningRc<gamescope::IBackendFb> fb) {
        const auto bpp = DRMFormatGetBPP(format);
        if (!src || !width || !height || !bpp || stride % bpp ||
            stride / bpp < width || stride / bpp > UINT32_MAX ||
            stride > SIZE_MAX / height) return nullptr;
        const VkDeviceSize bytes = stride * height;
        Slot oneShot;
        Slot *slot = &oneShot;
        if (bytes <= maxBytes) {
            const uint64_t completed = g_device.completedSeqNo();
            // Prefer a completed buffer of the right size, then an idle slot,
            // then the oldest submission. Never overwrite an in-flight copy.
            slot = nullptr;
            for (auto &candidate : slots)
                if (candidate.sequence <= completed && candidate.capacity >= bytes) {
                    slot = &candidate; break;
                }
            if (!slot) for (auto &candidate : slots)
                if (candidate.sequence <= completed) { slot = &candidate; break; }
            if (!slot) {
                slot = &slots[0];
                for (auto &candidate : slots)
                    if (candidate.sequence < slot->sequence) slot = &candidate;
                ++pressureWaits;
                g_device.wait(slot->sequence);
            }
            slot->sequence = 0;
            g_device.garbageCollect();
        }
        if (slot->capacity < bytes) {
            if (!slot->init(bytes)) { slot->clear(); return nullptr; }
            ++allocations;
        }
        memcpy(slot->mapped, src, bytes);
        gamescope::OwningRc<CVulkanTexture> texture = new CVulkanTexture();
        CVulkanTexture::createFlags flags;
        flags.bSampled = true; flags.bTransferSrc = true; flags.bTransferDst = true;
        flags.bFlippable = GetBackend()->UsesDmaBufImages();
        if (!texture->BInit(width, height, 1u, format, flags, nullptr, 0, 0, nullptr, fb))
            return nullptr;
        auto command = g_device.commandBuffer();
        command->copyBufferToImage(slot->buffer, 0, stride / bpp, texture);
        slot->sequence = g_device.submit(std::move(command));
        if (slot == &oneShot || GetBackend()->UsesDmaBufImages()) {
            g_device.wait(slot->sequence);
            slot->sequence = 0;
            texture->markHostUploadComplete();
        } else {
            // This means ordered GPU work on the compositor queue, not CPU
            // completion. copyBufferToImage installs the image barrier.
            texture->markLocalQueueDependency();
        }
        if (++uploads == 1 || uploads % 120 == 0)
            fprintf(stderr, "HSL_GAMESCOPE_SHM_UPLOAD frames=%llu allocations=%llu pressureWaits=%llu slots=3\n",
                (unsigned long long)uploads, (unsigned long long)allocations,
                (unsigned long long)pressureWaits);
        return texture;
    }
};
} // namespace hsl
