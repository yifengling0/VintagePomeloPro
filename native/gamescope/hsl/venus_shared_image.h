// SPDX-License-Identifier: MIT
// Private, explicitly negotiated HSL image transport; never DMA-BUF metadata.
// Fixed-width little-endian words. NativeBuffer, Vulkan and runtime handles
// stay in their owning ABI. Context identity comes from vtest, not this packet.
#ifndef HSL_VENUS_SHARED_IMAGE_H
#define HSL_VENUS_SHARED_IMAGE_H
#include <stdint.h>
#define HSL_IMAGE_COMMAND 0x48534943u
#define HSL_IMAGE_PARAM 0x48534901u
#define HSL_IMAGE_VERSION 1u
#define HSL_IMAGE_WORDS 64u
#define HSL_IMAGE_MAX_SLOTS 4u
enum hsl_image_op {
    HI_OFFER = 1, HI_CREATE, HI_IMPORT, HI_ACQUIRE, HI_PUBLISH,
    HI_CONSUME, HI_RELEASE, HI_DETACH
};
enum hsl_image_word {
    HI_VERSION, HI_OP, HI_KEY = 2, HI_WIDTH = 6, HI_HEIGHT, HI_FORMAT,
    HI_USAGE, HI_COUNT, HI_INDEX, HI_DEVICE = 12, HI_QUEUE = 14,
    HI_SEMAPHORE = 16, HI_IMAGES = 32
};
// Replies use the same descriptor/key offsets. Word 0 is VkResult (int32).
#define HI_RESULT 0u
static inline uint64_t hsl_image_u64(const uint32_t *words, unsigned index) {
    return (uint64_t)words[index] | ((uint64_t)words[index + 1] << 32);
}
static inline void hsl_image_set_u64(uint32_t *words, unsigned index, uint64_t value) {
    words[index] = (uint32_t)value; words[index + 1] = (uint32_t)(value >> 32);
}
#endif
