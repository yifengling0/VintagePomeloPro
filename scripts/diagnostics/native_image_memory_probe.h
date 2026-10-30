/* Bounded optimal-image control for the real OHOS driver. These are test
 * pixels, never a readback of a presented frame. Every exported allocation
 * is paired with the same external handle on its resource. */
static int run_image_memory_cases(VkPhysicalDevice physical, VkDevice device,
                                 VkQueue queue, uint32_t family)
{
#define IMAGE_PROC(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
    IMAGE_PROC(vkCreateImage);
    IMAGE_PROC(vkDestroyImage);
    IMAGE_PROC(vkGetImageMemoryRequirements2);
    IMAGE_PROC(vkBindImageMemory);
    IMAGE_PROC(vkCmdCopyBufferToImage);
    IMAGE_PROC(vkCmdCopyImageToBuffer);
    IMAGE_PROC(vkResetCommandPool);
    IMAGE_PROC(vkResetFences);
#undef IMAGE_PROC
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_properties = probe_image_properties;
    if (!image_properties || !vkCreateImage || !vkDestroyImage ||
        !vkGetImageMemoryRequirements2 || !vkBindImageMemory ||
        !vkCmdCopyBufferToImage || !vkCmdCopyImageToBuffer ||
        !vkResetCommandPool || !vkResetFences) return 0;

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
        VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    const uint32_t handles[] = {0, 0x2000, 0x8000};
    unsigned attempted = 0, passed = 0;
    for (unsigned h = 0; h < 3; ++h) {
        for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
            VkPhysicalDeviceExternalImageFormatInfo external_query = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                .handleType = handles[h]};
            VkPhysicalDeviceImageFormatInfo2 query = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                .pNext = handles[h] ? &external_query : NULL,
                .format = formats[f], .type = VK_IMAGE_TYPE_2D,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT};
            VkExternalImageFormatProperties external_props = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                .pNext = handles[h] ? &external_props : NULL};
            VkResult result = image_properties(physical, &query, &props);
            const VkExternalMemoryProperties ep = external_props.externalMemoryProperties;
            printf("NATIVE_IMAGE stage=capability format=%u handle=0x%x result=%d features=0x%x compatible=0x%x maxBytes=%" PRIu64 "\n",
                formats[f], handles[h], result, ep.externalMemoryFeatures,
                ep.compatibleHandleTypes, props.imageFormatProperties.maxResourceSize);
            if (result != VK_SUCCESS || f >= 2) continue;
            if (handles[h] && (!(ep.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
                              !(ep.compatibleHandleTypes & handles[h]))) continue;

            /* One dedicated image and two tiny staging buffers per case. */
            ++attempted;
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory allocation = VK_NULL_HANDLE;
            VkCommandPool pool = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            struct allocation staging[2] = {{0}};
            OH_NativeBuffer *native = NULL;
            unsigned wrong = 0;
#define IMAGE_TRY(call) do { result = (call); if (result != VK_SUCCESS) { \
    printf("NATIVE_IMAGE stage=failed call=%s result=%d\n", #call, result); goto image_done; } } while (0)
            VkExternalMemoryImageCreateInfo external_create = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .handleTypes = handles[h]};
            VkImageCreateInfo create = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = handles[h] ? &external_create : NULL,
                .imageType = VK_IMAGE_TYPE_2D, .format = formats[f],
                .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = query.usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
            IMAGE_TRY(vkCreateImage(device, &create, NULL, &image));
            VkMemoryDedicatedRequirements dedicated_req = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                .pNext = &dedicated_req};
            VkImageMemoryRequirementsInfo2 req_info = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image};
            vkGetImageMemoryRequirements2(device, &req_info, &req);
            uint32_t type = memory.memoryTypeCount;
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((req.memoryRequirements.memoryTypeBits & (1u << i)) &&
                    (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    type = i; break;
                }
            printf("NATIVE_IMAGE stage=requirements format=%u handle=0x%x bytes=%" PRIu64
                   " alignment=%" PRIu64 " bits=0x%x type=%u dedicated=%u preferred=%u\n",
                formats[f], handles[h], req.memoryRequirements.size, req.memoryRequirements.alignment,
                req.memoryRequirements.memoryTypeBits, type, dedicated_req.requiresDedicatedAllocation,
                dedicated_req.prefersDedicatedAllocation);
            if (type == memory.memoryTypeCount || req.memoryRequirements.size > 4 * 1024 * 1024) {
                result = VK_ERROR_FEATURE_NOT_PRESENT; goto image_done;
            }
            VkMemoryDedicatedAllocateInfo dedicated = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image};
            VkExportMemoryAllocateInfo export = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                .pNext = &dedicated, .handleTypes = handles[h]};
            VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = handles[h] ? (const void *)&export : (const void *)&dedicated,
                .allocationSize = req.memoryRequirements.size, .memoryTypeIndex = type};
            IMAGE_TRY(vkAllocateMemory(device, &allocate, NULL, &allocation));
            IMAGE_TRY(vkBindImageMemory(device, image, allocation, 0));
            if (handles[h]) {
                VkMemoryGetNativeBufferInfoOHOS get = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_GET_NATIVE_BUFFER_INFO_OHOS, .memory = allocation};
                if (!get_native_buffer) { result = VK_ERROR_EXTENSION_NOT_PRESENT; goto image_done; }
                IMAGE_TRY(get_native_buffer(device, &get, &native));
            }
            for (unsigned i = 0; i < 2; ++i)
                IMAGE_TRY(make_regular_allocation(physical, device, handles[h], &staging[i]));
            VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = family};
            IMAGE_TRY(vkCreateCommandPool(device, &pool_info, NULL, &pool));
            VkCommandBufferAllocateInfo cb_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            VkCommandBuffer cb;
            IMAGE_TRY(vkAllocateCommandBuffers(device, &cb_info, &cb));
            VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            IMAGE_TRY(vkCreateFence(device, &fence_info, NULL, &fence));
            for (unsigned round = 0; round < 8; ++round) {
                for (unsigned i = 0; i < 4096; ++i)
                    staging[0].alias[i] = 0x13579bdfu ^ (i * 2654435761u) ^ round;
                memset(staging[1].alias, 0, BYTES);
                for (unsigned i = 0; i < 2; ++i) {
                    VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                        .memory = staging[i].memory, .size = VK_WHOLE_SIZE};
                    IMAGE_TRY(vkFlushMappedMemoryRanges(device, 1, &range));
                }
                IMAGE_TRY(vkResetCommandPool(device, pool, 0));
                VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                IMAGE_TRY(vkBeginCommandBuffer(cb, &begin));
                VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
                VkImageMemoryBarrier transition = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = round ? VK_ACCESS_TRANSFER_READ_BIT : 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = round ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &host, 0, NULL, 1, &transition);
                VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .imageExtent = {64, 64, 1}};
                vkCmdCopyBufferToImage(cb, staging[0].buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                transition.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, NULL, 0, NULL, 1, &transition);
                vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, staging[1].buffer, 1, &copy);
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    0, 1, &host, 0, NULL, 0, NULL);
                IMAGE_TRY(vkEndCommandBuffer(cb));
                VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                    .commandBufferCount = 1, .pCommandBuffers = &cb};
                IMAGE_TRY(vkResetFences(device, 1, &fence));
                IMAGE_TRY(vkQueueSubmit(queue, 1, &submit, fence));
                IMAGE_TRY(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
                VkMappedMemoryRange read = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = staging[1].memory, .size = VK_WHOLE_SIZE};
                IMAGE_TRY(vkInvalidateMappedMemoryRanges(device, 1, &read));
                for (unsigned i = 0; i < 4096; ++i)
                    wrong += staging[1].alias[i] != (0x13579bdfu ^ (i * 2654435761u) ^ round);
            }
image_done:
            /* A timed-out GPU may still use every object: leave cleanup to
             * process teardown, never destroy pending submissions. */
            if (result == VK_TIMEOUT) _Exit(3);
            if (fence) vkDestroyFence(device, fence, NULL);
            if (pool) vkDestroyCommandPool(device, pool, NULL);
            for (unsigned i = 0; i < 2; ++i) release_allocation(device, &staging[i]);
            if (native) OH_NativeBuffer_Unreference(native);
            if (image) vkDestroyImage(device, image, NULL);
            if (allocation) vkFreeMemory(device, allocation, NULL);
            passed += result == VK_SUCCESS && !wrong;
            printf("NATIVE_IMAGE stage=case-done format=%u handle=0x%x result=%d wrong=%u\n",
                formats[f], handles[h], result, wrong);
#undef IMAGE_TRY
        }
    }
    printf("NATIVE_IMAGE stage=complete attempted=%u passed=%u\n", attempted, passed);
    return attempted > 0 && passed == attempted;
}
