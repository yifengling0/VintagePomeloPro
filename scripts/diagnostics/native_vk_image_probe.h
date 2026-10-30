/* SPDX-License-Identifier: MIT
 * Synthetic image/mixed-allocation test for the Steam -> FEX -> Venus path.
 * No application pixels, presented frames or assets are read back. */
#ifndef PROBE_IMAGE_ROUNDS
#define PROBE_IMAGE_ROUNDS 32
#endif
_Static_assert(PROBE_IMAGE_ROUNDS > 0 && PROBE_IMAGE_ROUNDS <= 128,
               "bounded image probe rounds");

struct image_probe_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t *map;
};

static int run_image_probe(VkDevice device, VkQueue queue, uint32_t family, unsigned type,
                          const VkPhysicalDeviceMemoryProperties *memory,
                          const VkPhysicalDeviceProperties *properties)
{
#define IMAGE_FUNCTIONS(X) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements2) X(vkGetBufferMemoryRequirements2) \
    X(vkBindImageMemory) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkResetCommandPool) X(vkResetFences)
#define IMAGE_LOAD(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(device, #n); \
    if (!n) { fprintf(stderr,"VK_IMAGE missing=%s\n",#n); return 2; }
    IMAGE_FUNCTIONS(IMAGE_LOAD)
#undef IMAGE_LOAD
#undef IMAGE_FUNCTIONS

    /* Two odd-sized images, four mip levels and two array layers. Buffer
     * guards surround the payload in each staging buffer. */
    enum { MIPS = 4, LAYERS = 2, REGIONS = MIPS * LAYERS, GUARD = 256,
           WORDS = 65536, MAX_ALLOCATION = 16 * 1024 * 1024 };
    VkBufferImageCopy regions[REGIONS] = {{0}};
    unsigned cursor = GUARD;
    for (unsigned layer = 0; layer < LAYERS; ++layer) {
        for (unsigned mip = 0; mip < MIPS; ++mip) {
            unsigned width = 127u >> mip, height = 65u >> mip;
            regions[layer * MIPS + mip] = (VkBufferImageCopy){
                .bufferOffset = cursor * 4u,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1},
                .imageExtent = {width, height, 1},
            };
            cursor += width * height;
        }
    }
    if (cursor + GUARD >= WORDS) return 2;
    const uint32_t guard = 0x79bdf135;
    VkResult result = VK_SUCCESS;
    unsigned mismatches = 0, completed = 0;
    struct image_probe_buffer staging[2] = {{0}};
    VkImage images[2] = {VK_NULL_HANDLE};
    VkDeviceMemory image_memory[2] = {VK_NULL_HANDLE};
    unsigned allocation_count = 0;
    unsigned image_type = VK_MAX_MEMORY_TYPES;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
#define IMAGE_TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    fprintf(stderr,"VK_IMAGE error line=%d result=%d call=%s\n", \
            __LINE__,result,#call); goto image_done; } } while (0)

    for (unsigned b = 0; b < 2; ++b) {
        VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = WORDS * 4u, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        IMAGE_TRY(vkCreateBuffer(device, &info, NULL, &staging[b].buffer));
        VkMemoryDedicatedRequirements dedicated = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
            .pNext = &dedicated};
        VkBufferMemoryRequirementsInfo2 query = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
            .buffer = staging[b].buffer};
        vkGetBufferMemoryRequirements2(device, &query, &req);
        if (!(req.memoryRequirements.memoryTypeBits & (1u << type)) ||
            req.memoryRequirements.size > MAX_ALLOCATION) {
            result = VK_ERROR_FEATURE_NOT_PRESENT; goto image_done;
        }
        VkMemoryDedicatedAllocateInfo binding = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .buffer = staging[b].buffer};
        VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = dedicated.requiresDedicatedAllocation ? &binding : NULL,
            .allocationSize = req.memoryRequirements.size, .memoryTypeIndex = type};
        IMAGE_TRY(vkAllocateMemory(device, &allocate, NULL, &staging[b].memory));
        IMAGE_TRY(vkBindBufferMemory(device, staging[b].buffer, staging[b].memory, 0));
        IMAGE_TRY(vkMapMemory(device, staging[b].memory, 0, VK_WHOLE_SIZE, 0,
                             (void **)&staging[b].map));
    }

    VkMemoryRequirements2 requirements[2];
    VkMemoryDedicatedRequirements dedicated[2];
    const VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    for (unsigned i = 0; i < 2; ++i) {
        VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = formats[i], .extent = {127, 65, 1},
            .mipLevels = MIPS, .arrayLayers = LAYERS, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        IMAGE_TRY(vkCreateImage(device, &info, NULL, &images[i]));
        dedicated[i] = (VkMemoryDedicatedRequirements){
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        requirements[i] = (VkMemoryRequirements2){
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated[i]};
        VkImageMemoryRequirementsInfo2 query = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = images[i]};
        vkGetImageMemoryRequirements2(device, &query, &requirements[i]);
        const VkMemoryRequirements req = requirements[i].memoryRequirements;
        fprintf(stderr,"VK_IMAGE requirements image=%u type=%u bytes=%llu align=%llu bits=%x dedicated=%u preferred=%u\n",
            i, type, (unsigned long long)req.size, (unsigned long long)req.alignment,
            req.memoryTypeBits, dedicated[i].requiresDedicatedAllocation,
            dedicated[i].prefersDedicatedAllocation);
        if (!req.memoryTypeBits || !req.alignment ||
            !req.size || req.size > MAX_ALLOCATION) {
            result = VK_ERROR_FEATURE_NOT_PRESENT; goto image_done;
        }
    }
    const uint32_t compatible = requirements[0].memoryRequirements.memoryTypeBits &
                                requirements[1].memoryRequirements.memoryTypeBits;
    /* Prefer driver-private GPU memory; allow the software Vulkan control
     * implementation's sole HOST_VISIBLE heap as a probe-only fallback. */
    for (unsigned pass = 0; pass < 2 && image_type == VK_MAX_MEMORY_TYPES; ++pass)
        for (unsigned i = 0; i < memory->memoryTypeCount; ++i) {
            const uint32_t flags = memory->memoryTypes[i].propertyFlags;
            if ((compatible & (1u << i)) && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                !(flags & (VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) &&
                (pass || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) {
                image_type = i;
                break;
            }
        }
    if (image_type == VK_MAX_MEMORY_TYPES) {
        result = VK_ERROR_FEATURE_NOT_PRESENT; goto image_done;
    }
    fprintf(stderr,"VK_IMAGE selected staging_type=%u image_type=%u flags=%x compatible=%x\n",
        type, image_type, memory->memoryTypes[image_type].propertyFlags, compatible);
    VkDeviceSize offsets[2] = {0, 0};
    bool separate = dedicated[0].requiresDedicatedAllocation ||
                    dedicated[1].requiresDedicatedAllocation;
    if (!separate) {
        VkDeviceSize alignment = requirements[1].memoryRequirements.alignment;
        if (alignment < properties->limits.bufferImageGranularity)
            alignment = properties->limits.bufferImageGranularity;
        VkDeviceSize end = requirements[0].memoryRequirements.size;
        offsets[1] = ((end + alignment - 1) / alignment) * alignment;
        if (offsets[1] > MAX_ALLOCATION ||
            requirements[1].memoryRequirements.size > MAX_ALLOCATION - offsets[1]) {
            result = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto image_done;
        }
    }
    allocation_count = separate ? 2 : 1;
    for (unsigned i = 0; i < allocation_count; ++i) {
        VkMemoryDedicatedAllocateInfo binding = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = images[i]};
        VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = separate ? &binding : NULL, .memoryTypeIndex = image_type,
            .allocationSize = separate ? requirements[i].memoryRequirements.size :
                offsets[1] + requirements[1].memoryRequirements.size};
        IMAGE_TRY(vkAllocateMemory(device, &allocate, NULL, &image_memory[i]));
    }
    for (unsigned i = 0; i < 2; ++i)
        IMAGE_TRY(vkBindImageMemory(device, images[i], image_memory[separate ? i : 0], offsets[i]));
    fprintf(stderr,"VK_IMAGE mode=%s rounds=%u mips=%u layers=%u presented_readbacks=0\n",
        separate ? "dedicated" : "suballocated", PROBE_IMAGE_ROUNDS, MIPS, LAYERS);

    VkCommandPoolCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family};
    IMAGE_TRY(vkCreateCommandPool(device, &cp, NULL, &pool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    IMAGE_TRY(vkAllocateCommandBuffers(device, &ca, &cb));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    IMAGE_TRY(vkCreateFence(device, &fi, NULL, &fence));
    for (unsigned round = 0; round < PROBE_IMAGE_ROUNDS; ++round) {
        for (unsigned image = 0; image < 2; ++image) {
            uint32_t seed = 0x2468ace0u ^ round ^ (image << 24);
            for (unsigned i = 0; i < WORDS; ++i) {
                staging[0].map[i] = i >= GUARD && i < cursor ? pattern(i, seed) : guard;
                staging[1].map[i] = guard;
            }
            for (unsigned b = 0; b < 2; ++b) {
                VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = staging[b].memory, .size = VK_WHOLE_SIZE};
                IMAGE_TRY(vkFlushMappedMemoryRanges(device, 1, &range));
            }
            IMAGE_TRY(vkResetCommandPool(device, pool, 0));
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            IMAGE_TRY(vkBeginCommandBuffer(cb, &begin));
            VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask =
                    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
            VkImageMemoryBarrier transition = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = round ? VK_ACCESS_TRANSFER_READ_BIT : 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = round ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[image],
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, MIPS, 0, LAYERS}};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, NULL, 1, &transition);
            vkCmdCopyBufferToImage(cb, staging[0].buffer, images[image],
                VK_IMAGE_LAYOUT_GENERAL, REGIONS, regions);
            transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            transition.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, 1, &transition);
            vkCmdCopyImageToBuffer(cb, images[image], VK_IMAGE_LAYOUT_GENERAL,
                staging[1].buffer, REGIONS, regions);
            memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            memory.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                0, 1, &memory, 0, NULL, 0, NULL);
            IMAGE_TRY(vkEndCommandBuffer(cb));
            IMAGE_TRY(vkResetFences(device, 1, &fence));
            VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1, .pCommandBuffers = &cb};
            IMAGE_TRY(vkQueueSubmit(queue, 1, &submit, fence));
            IMAGE_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
            for (unsigned b = 0; b < 2; ++b) {
                VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = staging[b].memory, .size = VK_WHOLE_SIZE};
                IMAGE_TRY(vkInvalidateMappedMemoryRanges(device, 1, &range));
            }
            unsigned wrong = 0;
            for (unsigned i = 0; i < WORDS; ++i) {
                uint32_t expected = i >= GUARD && i < cursor ? pattern(i, seed) : guard;
                wrong += staging[0].map[i] != expected;
                wrong += staging[1].map[i] != expected;
            }
            mismatches += wrong;
            ++completed;
            if (wrong) {
                fprintf(stderr,"VK_IMAGE mismatch round=%u image=%u words=%u\n",round,image,wrong);
                goto image_done;
            }
        }
    }
image_done:
    /* Never destroy resources while a timed-out GPU submission still uses them. */
    if (result == VK_TIMEOUT) _Exit(3);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    for (unsigned i = 0; i < 2; ++i)
        if (images[i]) vkDestroyImage(device, images[i], NULL);
    for (unsigned i = 0; i < allocation_count; ++i)
        if (image_memory[i]) vkFreeMemory(device, image_memory[i], NULL);
    for (unsigned b = 0; b < 2; ++b) {
        if (staging[b].map) vkUnmapMemory(device, staging[b].memory);
        if (staging[b].buffer) vkDestroyBuffer(device, staging[b].buffer, NULL);
        if (staging[b].memory) vkFreeMemory(device, staging[b].memory, NULL);
    }
    fprintf(stderr,"VK_IMAGE complete type=%u submissions=%u mismatches=%u result=%d\n",
        type, completed, mismatches, result);
    return result == VK_SUCCESS && !mismatches && completed == 2 * PROBE_IMAGE_ROUNDS ? 0 : 2;
#undef IMAGE_TRY
}
