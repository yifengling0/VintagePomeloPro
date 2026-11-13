#define _POSIX_C_SOURCE 200809L

/*
 * Offscreen replay for one captured DXVK Legacy Heaven material fragment
 * shader. The fragment SPIR-V remains an external diagnostic input so the
 * product runtime does not redistribute benchmark-owned shader binaries.
 * The harness supplies the final DXVK descriptor layout, deterministic UBOs,
 * six format-correct sampled images, and matching fragment input locations.
 * The same source is also built natively against Lavapipe for an exact A/B.
 */

#include <vulkan/vulkan.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPLAY_WIDTH 64u
#define REPLAY_HEIGHT 64u
#define REPLAY_IMAGE_WIDTH 4u
#define REPLAY_IMAGE_HEIGHT 4u
#define REPLAY_IMAGE_COUNT 6u
#define REPLAY_IMAGE_CAPACITY 7u
#define REPLAY_UBO_COUNT 15u
#define REPLAY_SAMPLE_COUNT 9u

#define CAPTURE_WIDTH 640u
#define CAPTURE_HEIGHT 360u
#define CAPTURE_IMAGE_CAPACITY 7u
#define CAPTURE_UBO_CAPACITY 12u
#define CAPTURE_MAX_MIPS 11u
#define CAPTURE_VERTEX_BYTES 386560u
#define CAPTURE_INDEX_BYTES 696u
#define CAPTURE_OUTPUT_BYTES (CAPTURE_WIDTH * CAPTURE_HEIGHT * 8u)
#define BUFFER_VISIBILITY_PROBE_BYTES 4096u
#define BUFFER_VISIBILITY_PROBE_COUNT 9u

#ifdef WINEHUA_HOST_DIRECT_REPLAY
#if defined(__aarch64__)
#define REPLAY_NATIVE_ARCH "aarch64"
#elif defined(__x86_64__)
#define REPLAY_NATIVE_ARCH "x86_64"
#else
#define REPLAY_NATIVE_ARCH "unknown"
#endif
#define REPLAY_EXECUTION_PATH "host-direct"
#define REPLAY_WINE_UNIX_ARCH "not-applicable"
#define REPLAY_VENUS_ARCH "not-applicable"
#define REPLAY_BOX64_ENABLED "false"
#else
#define REPLAY_NATIVE_ARCH "runtime"
#define REPLAY_EXECUTION_PATH "guest-venus"
#define REPLAY_WINE_UNIX_ARCH "x86_64"
#define REPLAY_VENUS_ARCH "x86_64"
#define REPLAY_BOX64_ENABLED "true"
#endif

struct replay_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
};

struct replay_image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    bool owns_image;
    VkDeviceSize upload_offsets[CAPTURE_MAX_MIPS];
};

struct capture_image_spec {
    VkFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint32_t binding;
    uint32_t source_binding;
    VkImageAspectFlags aspect;
};

struct replay_state {
    const char *run_id;
    const char *test_id;
    const char *result_path;
    const char *rgba_path;
    const char *vertex_path;
    const char *fragment_path;
    const char *fragment_sha256;
    const char *layout_mode;
    const char *bool_mode;
    const char *capture_dir;
    const char *capture_color_mode;
    const char *capture_depth_mode;
    const char *capture_depth_compare;
    const char *capture_cull_mode;
    const char *capture_profile;
    uint32_t capture_image_count;
    uint32_t capture_ubo_count;
    uint32_t capture_vertex_bytes;
    uint32_t capture_index_bytes;
    uint32_t capture_index_count;
    uint32_t capture_first_index;
    int32_t capture_vertex_offset;
    uint32_t capture_instance_count;
    uint32_t capture_first_instance;
    uint32_t capture_draw_id;
    struct capture_image_spec capture_images[CAPTURE_IMAGE_CAPACITY];
    uint32_t capture_ubo_sizes[CAPTURE_UBO_CAPACITY];
    uint32_t vertex_code_hash;
    uint32_t fragment_code_hash;
    uint32_t vertex_code_size;
    uint32_t fragment_code_size;
    uint32_t vertex_first_word;
    uint32_t vertex_last_word;
    uint32_t fragment_first_word;
    uint32_t fragment_last_word;
    bool captured_mode;
    bool verify_inputs;
    bool skip_draw;
    bool input_verification_performed;
    bool inputs_uploaded;
    bool vs_transform_probe;
    bool requires_terminate_invocation;
    bool terminate_invocation_supported;
    bool terminate_invocation_enabled;
    uint32_t bool_spec_count;
    uint64_t started_ms;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    struct replay_buffer upload;
    struct replay_buffer vertex;
    struct replay_buffer index;
    struct replay_buffer ubos[REPLAY_UBO_COUNT];
    struct replay_image images[REPLAY_IMAGE_CAPACITY];
    VkImage target;
    VkDeviceMemory target_memory;
    VkImageView target_view;
    VkImage depth;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkDeviceSize target_upload_offset;
    VkDeviceSize depth_upload_offset;
    VkDeviceSize cpu_upload_bytes;
    VkDeviceSize output_bytes;
    uint32_t baseline_checksum;
    struct replay_buffer readback;
    struct replay_buffer input_readback;
    VkDeviceSize input_vertex_offset;
    VkDeviceSize input_index_offset;
    VkDeviceSize input_ubo_offsets[CAPTURE_UBO_CAPACITY];
    VkDeviceSize input_upload_offset;
    uint32_t input_image_expected[CAPTURE_IMAGE_CAPACITY][CAPTURE_MAX_MIPS];
    uint32_t input_image_actual[CAPTURE_IMAGE_CAPACITY][CAPTURE_MAX_MIPS];
    uint32_t input_vertex_expected;
    uint32_t input_vertex_actual;
    uint32_t input_index_expected;
    uint32_t input_index_actual;
    uint32_t input_ubo_expected[CAPTURE_UBO_CAPACITY];
    uint32_t input_ubo_actual[CAPTURE_UBO_CAPACITY];
    uint32_t input_upload_expected;
    uint32_t input_upload_actual;
    uint32_t input_checked_count;
    uint32_t input_mismatch_count;
    uint32_t buffer_probe_pass_count;
    uint32_t buffer_probe_fail_count;
    VkDescriptorSetLayout set_layout;
    uint32_t layout_binding_count;
    uint32_t dynamic_binding_count;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor_set;
    VkPipelineLayout pipeline_layout;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkShaderModule vertex_shader;
    VkShaderModule fragment_shader;
    VkPipeline pipeline;
    uint32_t checksum;
    uint32_t changed_pixels;
    uint32_t nonzero_drawn_pixels;
    uint32_t opaque_pixels;
    uint32_t sample_values[REPLAY_SAMPLE_COUNT];
    bool specialization_applied;
    uint64_t queue_submits;
    uint64_t gpu_copies;
    char stage[64];
    char message[256];
    char pipeline_stage[64];
    VkResult failure_result;
};

static uint32_t fnv1a32(const void *data, size_t size);
static void destroy_buffer(struct replay_state *s, struct replay_buffer *buffer);

static const VkFormat replay_formats[REPLAY_IMAGE_COUNT] = {
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
};

static const uint32_t replay_ubo_sizes[REPLAY_UBO_COUNT] = {
    144u, 16u, 48u, 1536u, 16u,
    64u, 144u, 32u, 64u, 512u,
    144u, 16u, 48u, 1536u, 16u,
};

static const uint32_t legacy_capture_ubo_sizes[9] = {
    144u, 16u, 48u, 1536u, 16u, 64u, 144u, 48u, 512u,
};

static const struct capture_image_spec legacy_capture_images[4] = {
    { VK_FORMAT_R8G8B8A8_UNORM, 512u, 512u, 10u, 13u },
    { VK_FORMAT_R8G8_SNORM,     512u, 512u, 10u, 14u },
    { VK_FORMAT_R8_UNORM,        16u,  16u,  5u, 15u },
    { VK_FORMAT_R8G8B8A8_UNORM,   4u,   4u,  1u, 16u },
};

static const uint32_t draw0_capture_ubo_sizes[10] = {
    144u, 16u, 48u, 1536u, 16u, 64u, 144u, 32u, 64u, 512u,
};

static const struct capture_image_spec draw0_capture_images[6] = {
    { VK_FORMAT_R8G8B8A8_UNORM, 512u,  512u, 10u, 16u },
    { VK_FORMAT_R8G8_SNORM,     1024u, 1024u, 10u, 17u },
    { VK_FORMAT_R8G8B8A8_UNORM, 256u,  256u,  9u, 18u },
    { VK_FORMAT_R8G8_SNORM,     256u,  256u,  9u, 19u },
    { VK_FORMAT_R8_UNORM,        16u,   16u,  5u, 20u },
    { VK_FORMAT_R8G8B8A8_UNORM,   4u,    4u,  1u, 21u },
};

static const uint32_t f647_capture_ubo_sizes[CAPTURE_UBO_CAPACITY] = {
    144u, 16u, 48u, 320u, 1536u, 16u,
    320u, 256u, 32u, 48u, 48u, 512u,
};

/* Frame-500 draw 2 is the same indexed mesh in Heaven's depth/G-buffer pass
 * and material pass.  These profiles deliberately omit fragment resources:
 * a diagnostic fragment shader writes gl_FragCoord.z/w so the two vertex
 * transforms can be compared without texture, material, or depth-test noise. */
static const uint32_t pass0_draw2_capture_ubo_sizes[9] = {
    64u, 144u, 16u, 48u, 1536u, 16u, 32u, 48u, 512u,
};

static const uint32_t pass2_draw2_capture_ubo_sizes[10] = {
    144u, 16u, 48u, 1536u, 16u, 64u, 144u, 32u, 64u, 512u,
};

/* Frame 220 contains the same mesh in the G-buffer pass (0) and the main
 * HDR/material pass (3). Keep this profile separate from the older shadow
 * depth pair: its draw has a smaller captured vertex/index range. */
static const uint32_t material_pass0_capture_ubo_sizes[9] = {
    64u, 144u, 16u, 48u, 1536u, 16u, 32u, 48u, 512u,
};

static const uint32_t material_pass3_capture_ubo_sizes[10] = {
    144u, 16u, 48u, 1536u, 16u, 64u, 144u, 32u, 64u, 512u,
};

static const struct capture_image_spec
f647_capture_images[CAPTURE_IMAGE_CAPACITY] = {
    { VK_FORMAT_R8G8B8A8_UNORM,  512u,  512u, 10u, 19u, 19u,
      VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_R8G8_SNORM,     1024u, 1024u, 11u, 20u, 20u,
      VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_R8G8B8A8_UNORM,  512u,  512u, 10u, 21u, 21u,
      VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_R8G8B8A8_UNORM,  256u,  256u,  9u, 22u, 22u,
      VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_R8G8_SNORM,      256u,  256u,  9u, 23u, 23u,
      VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_R8G8B8A8_UNORM,  256u,  256u,  9u, 24u, 22u,
      VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_D24_UNORM_S8_UINT, 2048u, 2048u, 1u, 25u, 25u,
      VK_IMAGE_ASPECT_DEPTH_BIT },
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *argument_value(int argc, char **argv, const char *name,
                                  const char *fallback)
{
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return fallback;
}

static bool configure_capture_profile(struct replay_state *s)
{
    uint32_t i;
    bool draw0 = s->capture_profile && !strcmp(s->capture_profile, "draw0");
    bool draw170 = s->capture_profile && !strcmp(s->capture_profile, "draw170");
    bool f647 = s->capture_profile && !strcmp(s->capture_profile, "f647");
    bool pass0_draw2 = s->capture_profile &&
        !strcmp(s->capture_profile, "pass0-draw2-depth");
    bool pass2_draw2 = s->capture_profile &&
        !strcmp(s->capture_profile, "pass2-draw2-depth");
    bool material_pass0 = s->capture_profile &&
        !strcmp(s->capture_profile, "material-pass0-depth");
    bool material_pass3 = s->capture_profile &&
        !strcmp(s->capture_profile, "material-pass3-depth");
    bool depth_pair = pass0_draw2 || pass2_draw2 ||
        material_pass0 || material_pass3;

    s->capture_image_count = depth_pair ? 0u : f647 ? 7u : draw0 ? 6u : 4u;
    s->capture_ubo_count = pass0_draw2 || material_pass0 ? 9u :
        pass2_draw2 || material_pass3 ? 10u :
        f647 ? 12u : draw0 ? 10u : 9u;
    s->capture_vertex_bytes = material_pass0 || material_pass3 ? 142624u :
        depth_pair ? 417024u :
        (draw0 || f647) ? 1090560u :
        draw170 ? 258784u : 386560u;
    s->capture_index_bytes = material_pass0 || material_pass3 ? 6666u :
        depth_pair ? 36336u :
        (draw0 || f647) ? 38190u :
        draw170 ? 4746u : 696u;
    s->capture_index_count = material_pass0 || material_pass3 ? 3333u :
        depth_pair ? 18168u :
        (draw0 || f647) ? 19095u :
        draw170 ? 2373u : 348u;
    /* Target-draw capture files contain only the selected index range. */
    s->capture_first_index = 0u;
    s->capture_vertex_offset = material_pass0 || material_pass3 ? 0 :
        depth_pair ? 9238 : draw170 ? 7331 : 0;
    s->capture_instance_count = draw170 ? 2u : 1u;
    s->capture_first_instance = 0u;
    s->capture_draw_id = depth_pair ? 2u :
        f647 ? 390u : draw0 ? 0u : draw170 ? 170u : 148u;
    if (pass0_draw2) {
        memcpy(s->capture_ubo_sizes, pass0_draw2_capture_ubo_sizes,
               sizeof(pass0_draw2_capture_ubo_sizes));
    } else if (pass2_draw2) {
        memcpy(s->capture_ubo_sizes, pass2_draw2_capture_ubo_sizes,
               sizeof(pass2_draw2_capture_ubo_sizes));
    } else if (material_pass0) {
        memcpy(s->capture_ubo_sizes, material_pass0_capture_ubo_sizes,
               sizeof(material_pass0_capture_ubo_sizes));
    } else if (material_pass3) {
        memcpy(s->capture_ubo_sizes, material_pass3_capture_ubo_sizes,
               sizeof(material_pass3_capture_ubo_sizes));
    } else if (f647) {
        memcpy(s->capture_images, f647_capture_images,
               sizeof(f647_capture_images));
        memcpy(s->capture_ubo_sizes, f647_capture_ubo_sizes,
               sizeof(f647_capture_ubo_sizes));
    } else if (draw0) {
        memcpy(s->capture_images, draw0_capture_images,
               sizeof(draw0_capture_images));
        memcpy(s->capture_ubo_sizes, draw0_capture_ubo_sizes,
               sizeof(draw0_capture_ubo_sizes));
    } else {
        memcpy(s->capture_images, legacy_capture_images,
               sizeof(legacy_capture_images));
        memcpy(s->capture_ubo_sizes, legacy_capture_ubo_sizes,
               sizeof(legacy_capture_ubo_sizes));
    }
    for (i = s->capture_image_count; i < CAPTURE_IMAGE_CAPACITY; ++i)
        memset(&s->capture_images[i], 0, sizeof(s->capture_images[i]));
    for (i = s->capture_ubo_count; i < CAPTURE_UBO_CAPACITY; ++i)
        s->capture_ubo_sizes[i] = 0;
    return depth_pair || f647 || draw0 || draw170 || !s->capture_profile ||
        !s->capture_profile[0] ||
        !strcmp(s->capture_profile, "legacy");
}

static bool capture_profile_is(const struct replay_state *s, const char *name)
{
    return s->capture_profile && !strcmp(s->capture_profile, name);
}

static bool capture_profile_is_depth_pair(const struct replay_state *s)
{
    return capture_profile_is(s, "pass0-draw2-depth") ||
        capture_profile_is(s, "pass2-draw2-depth") ||
        capture_profile_is(s, "material-pass0-depth") ||
        capture_profile_is(s, "material-pass3-depth");
}

static VkImageAspectFlags capture_image_aspect(
        const struct capture_image_spec *image)
{
    return image->aspect ? image->aspect : VK_IMAGE_ASPECT_COLOR_BIT;
}

static uint32_t capture_source_binding(const struct capture_image_spec *image)
{
    return image->source_binding ? image->source_binding : image->binding;
}

static VkImageLayout capture_sampled_layout(
        const struct capture_image_spec *image)
{
    return capture_image_aspect(image) & VK_IMAGE_ASPECT_DEPTH_BIT ?
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static VkImageAspectFlags capture_image_barrier_aspect(
        const struct capture_image_spec *image)
{
    return image->format == VK_FORMAT_D24_UNORM_S8_UINT ?
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT :
        capture_image_aspect(image);
}

static int32_t capture_alias_index(const struct replay_state *s, uint32_t index)
{
    const uint32_t source_binding = capture_source_binding(&s->capture_images[index]);
    uint32_t i;
    if (source_binding == s->capture_images[index].binding)
        return -1;
    for (i = 0; i < index; ++i)
        if (s->capture_images[i].binding == source_binding)
            return (int32_t)i;
    return -1;
}

static void json_safe(char *out, size_t size, const char *in)
{
    size_t n = 0;
    if (!size) return;
    while (in && *in && n + 1 < size) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\' || c < 0x20 || c > 0x7e) c = '_';
        out[n++] = (char)c;
    }
    out[n] = 0;
}

static void record_failure(struct replay_state *s, const char *stage,
                           const char *message, VkResult result)
{
    snprintf(s->stage, sizeof(s->stage), "%s", stage ? stage : "unknown");
    snprintf(s->message, sizeof(s->message), "%s", message ? message : "failure");
    s->failure_result = result;
}

static VkResult record_pipeline_step(struct replay_state *s, const char *stage,
                                     VkResult result)
{
    snprintf(s->pipeline_stage, sizeof(s->pipeline_stage), "%s", stage);
    fprintf(stderr, "[venus-heaven-replay] stage=%s result=%d\n", stage,
            (int)result);
    fflush(stderr);
    return result;
}

static VkResult record_shader_load_failure(struct replay_state *s)
{
    snprintf(s->pipeline_stage, sizeof(s->pipeline_stage), "shader-load");
    snprintf(s->message, sizeof(s->message),
             "shader load failed vertex=%s fragment=%s errno=%d",
             s->vertex_path ? s->vertex_path : "", s->fragment_path ? s->fragment_path : "",
             errno);
    fprintf(stderr, "[venus-heaven-replay] stage=shader-load result=%d vertex=%s fragment=%s errno=%d\\n",
            (int)VK_ERROR_INITIALIZATION_FAILED, s->vertex_path ? s->vertex_path : "",
            s->fragment_path ? s->fragment_path : "", errno);
    fflush(stderr);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void write_result(const struct replay_state *s, const char *status)
{
    char temporary[1024];
    char safe_message[512], safe_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE + 16];
    char safe_vertex[512], safe_fragment[512], safe_fragment_sha256[128];
    char safe_rgba[512], safe_layout[64], safe_capture_profile[64];
    const char *uniform_bindings = "[0,1,2,3,4,5,6,7,8,9]";
    const char *vertex_uniform_bindings = "[0,1,2,3,4]";
    const char *fragment_uniform_bindings = "[5,6,7,8,9]";
    const char *uniform_bytes = "[144,16,48,1536,16,64,144,32,64,512]";
    const char *sampler_bindings = "[10,11,12,13,14,15]";
    const char *image_bindings = "[16,17,18,19,20,21]";
    const char *image_formats = "[\"R8G8B8A8_UNORM\",\"R8G8_SNORM\","
        "\"R8G8B8A8_UNORM\",\"R8G8_SNORM\",\"R8_UNORM\","
        "\"R8G8B8A8_UNORM\"]";
    const char *sampled_layout = "SHADER_READ_ONLY_OPTIMAL";
    uint32_t output_width = REPLAY_WIDTH;
    uint32_t output_height = REPLAY_HEIGHT;
    uint64_t cpu_read_bytes = REPLAY_WIDTH * REPLAY_HEIGHT * 4u;
    uint64_t cpu_upload_bytes = 4112u;
    FILE *file;
    int fd;
    if (!s->result_path || !s->result_path[0]) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%d", s->result_path, getpid());
    json_safe(safe_message, sizeof(safe_message), s->message);
    json_safe(safe_device, sizeof(safe_device), s->properties.deviceName);
    json_safe(safe_vertex, sizeof(safe_vertex), s->vertex_path);
    json_safe(safe_fragment, sizeof(safe_fragment), s->fragment_path);
    json_safe(safe_fragment_sha256, sizeof(safe_fragment_sha256),
              s->fragment_sha256 ? s->fragment_sha256 : "unknown");
    json_safe(safe_rgba, sizeof(safe_rgba), s->rgba_path);
    json_safe(safe_layout, sizeof(safe_layout), s->layout_mode);
    json_safe(safe_capture_profile, sizeof(safe_capture_profile),
              s->capture_profile ? s->capture_profile : "legacy");
    if (s->layout_mode && !strcmp(s->layout_mode, "exact")) {
        uniform_bindings = "[0,1,2,3,15,160,161,162,163,164]";
        vertex_uniform_bindings = "[160,161,162,163,164]";
        fragment_uniform_bindings = "[0,1,2,3,15]";
        sampler_bindings = "[16,17,19,20,28,29]";
        image_bindings = "[32,33,35,36,44,45]";
    } else if (s->captured_mode) {
        if (capture_profile_is(s, "f647")) {
            uniform_bindings = "[0,1,2,3,4,5,6,7,8,9,10,11]";
            vertex_uniform_bindings = "[0,1,2,3,4,5]";
            fragment_uniform_bindings = "[6,7,8,9,10,11]";
            uniform_bytes = "[144,16,48,320,1536,16,320,256,32,48,48,512]";
            sampler_bindings = "[12,13,14,15,16,17,18]";
            image_bindings = "[19,20,21,22,23,24,25]";
            image_formats = "[\"R8G8B8A8_UNORM\",\"R8G8_SNORM\","
                "\"R8G8B8A8_UNORM\",\"R8G8B8A8_UNORM\","
                "\"R8G8_SNORM\",\"R8G8B8A8_UNORM\","
                "\"D24_UNORM_S8_UINT\"]";
            sampled_layout = "MIXED_SHADER_AND_DEPTH_STENCIL_READ_ONLY_OPTIMAL";
        } else if (s->capture_profile && strcmp(s->capture_profile, "draw0")) {
            uniform_bindings = "[0,1,2,3,4,5,6,7,8]";
            vertex_uniform_bindings = "[0,1,2,3,4]";
            fragment_uniform_bindings = "[5,6,7,8]";
            uniform_bytes = "[144,16,48,1536,16,64,144,48,512]";
            sampler_bindings = "[9,10,11,12]";
            image_bindings = "[13,14,15,16]";
            image_formats = "[\"R8G8B8A8_UNORM\",\"R8G8_SNORM\","
                "\"R8_UNORM\",\"R8G8B8A8_UNORM\"]";
            if (capture_profile_is_depth_pair(s)) {
                const bool material_pass3 =
                    capture_profile_is(s, "material-pass3-depth");
                uniform_bindings = material_pass3 ?
                    "[0,1,2,3,4,5,6,7,8,9]" :
                    "[0,1,2,3,4,5,6,7,8]";
                vertex_uniform_bindings = "[0,1,2,3,4]";
                fragment_uniform_bindings = material_pass3 ?
                    "[5,6,7,8,9]" : "[5,6,7,8]";
                uniform_bytes = material_pass3 ?
                    "[144,16,48,1536,16,64,144,32,64,512]" :
                    "[64,144,16,48,1536,16,32,48,512]";
                sampler_bindings = "[]";
                image_bindings = "[]";
                image_formats = "[]";
                sampled_layout = "UNDEFINED";
            }
        }
        output_width = CAPTURE_WIDTH;
        output_height = CAPTURE_HEIGHT;
        cpu_read_bytes = s->output_bytes;
        cpu_upload_bytes = s->cpu_upload_bytes;
    }
    file = fopen(temporary, "w");
    if (!file) return;
    fprintf(file,
            "{\n"
            "  \"schemaVersion\":1,\n"
            "  \"runId\":\"%s\",\n"
            "  \"testId\":\"%s\",\n"
            "  \"status\":\"%s\",\n"
            "  \"stage\":\"%s\",\n"
            "  \"message\":\"%s\",\n"
            "  \"vulkanResult\":%d,\n"
            "  \"device\":{\"name\":\"%s\",\"vendorId\":%u,\"deviceId\":%u,"
            "\"apiVersion\":\"%u.%u.%u\",\"queueFamily\":%u},\n"
            "  \"shader\":{\"vertex\":\"%s\",\"fragment\":\"%s\","
            "\"fragmentSha256\":\"%s\","
            "\"vertexCode\":{\"size\":%u,\"fnv1a32\":\"0x%08x\","
            "\"firstWord\":\"0x%08x\",\"lastWord\":\"0x%08x\"},"
            "\"fragmentCode\":{\"size\":%u,\"fnv1a32\":\"0x%08x\","
            "\"firstWord\":\"0x%08x\",\"lastWord\":\"0x%08x\"},"
            "\"specialization\":{\"1216\":12816,\"applied\":%s,\"boolMode\":\"%s\",\"boolEntryCount\":%u},"
            "\"terminateInvocation\":{\"required\":%s,\"supported\":%s,\"enabled\":%s}},\n"
            "  \"descriptorContract\":{\"set\":0,\"layoutMode\":\"%s\",\"captureProfile\":\"%s\",\"bindingCount\":%u,"
            "\"dynamicBindingCount\":%u,\"uniformBuffers\":%s,"
            "\"uniformBufferBytes\":%s,"
            "\"vertexUniformBindings\":%s,\"fragmentUniformBindings\":%s,"
            "\"samplers\":%s,"
            "\"sampledImages\":%s,"
            "\"formats\":%s,"
            "\"layout\":\"%s\"},\n"
            "  \"inputVerification\":{\"enabled\":%s,\"performed\":%s,"
            "\"checkedCount\":%u,\"mismatchCount\":%u,"
            "\"imageExpectedFnv\":\"0x%08x\",\"imageActualFnv\":\"0x%08x\","
            "\"vertexExpectedFnv\":\"0x%08x\",\"vertexActualFnv\":\"0x%08x\","
            "\"indexExpectedFnv\":\"0x%08x\",\"indexActualFnv\":\"0x%08x\","
            "\"uboExpectedFnv\":\"0x%08x\",\"uboActualFnv\":\"0x%08x\","
            "\"uploadExpectedFnv\":\"0x%08x\",\"uploadActualFnv\":\"0x%08x\","
            "\"bufferProbePassCount\":%u,\"bufferProbeFailCount\":%u},\n"
            "  \"graphics\":{\"extent\":\"%ux%u\",\"rgbaOutput\":\"%s\","
            "\"captureDrawId\":%u,\"skipDraw\":%s,\"indexCount\":%u,\"firstIndex\":%u,"
            "\"vertexOffset\":%d,\"instanceCount\":%u,\"firstInstance\":%u,"
            "\"checksum\":\"0x%08x\",\"changedPixels\":%u,"
            "\"nonzeroDrawnPixels\":%u,\"opaquePixels\":%u,"
            "\"sampleValues\":[\"0x%08x\",\"0x%08x\",\"0x%08x\","
            "\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"]},\n"
            "  \"architecture\":{\"executionPath\":\"%s\","
            "\"peArchitecture\":\"not-applicable\","
            "\"wineUnixArchitecture\":\"%s\",\"vulkanLoaderArchitecture\":\"%s\","
            "\"venusIcdArchitecture\":\"%s\",\"hostArchitecture\":\"%s\","
            "\"wow64ThunkEnabled\":false,\"box64Enabled\":%s},\n"
            "  \"metrics\":{\"cpuReadBytes\":%" PRIu64 ",\"cpuUploadBytes\":%" PRIu64 ","
            "\"gpuCopyCount\":%" PRIu64 ",\"queueSubmitCount\":%" PRIu64 ","
            "\"perFrameDeviceWaitIdle\":0,\"fallbackDetected\":false,\"durationMs\":%" PRIu64 "}\n"
            "}\n",
            s->run_id, s->test_id, status, s->stage, safe_message,
            (int)s->failure_result, safe_device, s->properties.vendorID,
            s->properties.deviceID, VK_API_VERSION_MAJOR(s->properties.apiVersion),
            VK_API_VERSION_MINOR(s->properties.apiVersion),
            VK_API_VERSION_PATCH(s->properties.apiVersion), s->queue_family,
            safe_vertex, safe_fragment, safe_fragment_sha256,
            s->vertex_code_size, s->vertex_code_hash,
            s->vertex_first_word, s->vertex_last_word,
            s->fragment_code_size, s->fragment_code_hash,
            s->fragment_first_word, s->fragment_last_word,
            s->specialization_applied ? "true" : "false",
            s->bool_mode ? s->bool_mode : "default", s->bool_spec_count,
            s->requires_terminate_invocation ? "true" : "false",
            s->terminate_invocation_supported ? "true" : "false",
            s->terminate_invocation_enabled ? "true" : "false",
            safe_layout, safe_capture_profile, s->layout_binding_count,
            s->dynamic_binding_count, uniform_bindings, uniform_bytes,
            vertex_uniform_bindings, fragment_uniform_bindings,
            sampler_bindings, image_bindings, image_formats, sampled_layout,
            s->verify_inputs ? "true" : "false",
            s->input_verification_performed ? "true" : "false",
            s->input_checked_count, s->input_mismatch_count,
            fnv1a32(s->input_image_expected, sizeof(s->input_image_expected)),
            fnv1a32(s->input_image_actual, sizeof(s->input_image_actual)),
            s->input_vertex_expected, s->input_vertex_actual,
            s->input_index_expected, s->input_index_actual,
            fnv1a32(s->input_ubo_expected, sizeof(s->input_ubo_expected)),
            fnv1a32(s->input_ubo_actual, sizeof(s->input_ubo_actual)),
            s->input_upload_expected, s->input_upload_actual,
            s->buffer_probe_pass_count, s->buffer_probe_fail_count,
            output_width, output_height,
            safe_rgba, s->capture_draw_id, s->skip_draw ? "true" : "false",
            s->capture_index_count,
            s->capture_first_index, s->capture_vertex_offset,
            s->capture_instance_count, s->capture_first_instance, s->checksum,
            s->changed_pixels, s->nonzero_drawn_pixels, s->opaque_pixels,
            s->sample_values[0], s->sample_values[1], s->sample_values[2],
            s->sample_values[3], s->sample_values[4], s->sample_values[5],
            s->sample_values[6], s->sample_values[7], s->sample_values[8],
            REPLAY_EXECUTION_PATH, REPLAY_WINE_UNIX_ARCH, REPLAY_NATIVE_ARCH,
            REPLAY_VENUS_ARCH, REPLAY_NATIVE_ARCH, REPLAY_BOX64_ENABLED,
            cpu_read_bytes, cpu_upload_bytes, s->gpu_copies,
            s->queue_submits, now_ms() - s->started_ms);
    fflush(file);
    fd = fileno(file);
    if (fd >= 0) fsync(fd);
    fclose(file);
    rename(temporary, s->result_path);
}

static int load_spirv(const char *path, uint32_t **code, size_t *size)
{
    FILE *file;
    long length;
    *code = NULL;
    *size = 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    length = ftell(file);
    if (length <= 0 || (length & 3)) { fclose(file); return 0; }
    rewind(file);
    *code = malloc((size_t)length);
    if (!*code || fread(*code, 1, (size_t)length, file) != (size_t)length) {
        free(*code);
        *code = NULL;
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

static VkDeviceSize align4(VkDeviceSize value)
{
    return (value + 3u) & ~(VkDeviceSize)3u;
}

static VkDeviceSize align_to(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int capture_path(const struct replay_state *s, const char *name,
                        char *path, size_t size)
{
    int length;
    if (!s->capture_dir || !s->capture_dir[0] || !name || !name[0])
        return 0;
    length = snprintf(path, size, "%s/%s", s->capture_dir, name);
    return length > 0 && (size_t)length < size;
}

static int read_exact_file(const char *path, void *data, size_t size)
{
    FILE *file;
    long length;
    if (!path || !data) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    length = ftell(file);
    if (length < 0 || (size_t)length != size) { fclose(file); return 0; }
    rewind(file);
    if (fread(data, 1, size, file) != size) { fclose(file); return 0; }
    fclose(file);
    return 1;
}

static uint32_t fnv1a32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static void record_shader_code(struct replay_state *s,
                               const uint32_t *vertex_code,
                               size_t vertex_size,
                               const uint32_t *fragment_code,
                               size_t fragment_size)
{
    size_t vertex_words = vertex_size / sizeof(uint32_t);
    size_t fragment_words = fragment_size / sizeof(uint32_t);

    s->vertex_code_size = (uint32_t)vertex_size;
    s->fragment_code_size = (uint32_t)fragment_size;
    s->vertex_code_hash = fnv1a32(vertex_code, vertex_size);
    s->fragment_code_hash = fnv1a32(fragment_code, fragment_size);
    s->vertex_first_word = vertex_words ? vertex_code[0] : 0;
    s->vertex_last_word = vertex_words ? vertex_code[vertex_words - 1] : 0;
    s->fragment_first_word = fragment_words ? fragment_code[0] : 0;
    s->fragment_last_word = fragment_words ? fragment_code[fragment_words - 1] : 0;
    fprintf(stderr,
            "[venus-heaven-replay] shader-code vertexSize=%u vertexFnv=0x%08x"
            " vertexFirst=0x%08x vertexLast=0x%08x fragmentSize=%u"
            " fragmentFnv=0x%08x fragmentFirst=0x%08x fragmentLast=0x%08x\n",
            s->vertex_code_size, s->vertex_code_hash,
            s->vertex_first_word, s->vertex_last_word,
            s->fragment_code_size, s->fragment_code_hash,
            s->fragment_first_word, s->fragment_last_word);
    fflush(stderr);
}

static uint32_t format_bytes_per_texel(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return 1u;
    case VK_FORMAT_R8G8_SNORM:
        return 2u;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return 4u;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8u;
    default:
        return 0u;
    }
}

static int find_memory_type(struct replay_state *s, uint32_t bits,
                            VkMemoryPropertyFlags required, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(s->physical, &memory);
    for (i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & required) == required) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static VkResult create_buffer(struct replay_state *s, VkDeviceSize size,
                              VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags properties,
                              struct replay_buffer *buffer)
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type;
    VkResult result;
    buffer->size = size;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(s->device, &info, NULL, &buffer->buffer);
    if (result != VK_SUCCESS) return result;
    vkGetBufferMemoryRequirements(s->device, buffer->buffer, &requirements);
    if (!find_memory_type(s, requirements.memoryTypeBits, properties, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(s->device, &allocation, NULL, &buffer->memory);
    if (result != VK_SUCCESS) return result;
    return vkBindBufferMemory(s->device, buffer->buffer, buffer->memory, 0);
}

/*
 * HOST_COHERENT makes writes visible to a native Vulkan driver, but WineHua's
 * Guest Venus mapping is a shadow mapping.  The remote renderer receives
 * those writes only through the Vulkan flush command.  Keep this explicit in
 * the replay so its input contract matches a DXVK mapped upload.
 */
static VkResult flush_replay_buffer(struct replay_state *s,
                                    const struct replay_buffer *buffer)
{
    VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    range.memory = buffer->memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    return vkFlushMappedMemoryRanges(s->device, 1, &range);
}

static VkResult create_image(struct replay_state *s, VkFormat format,
                             uint32_t width, uint32_t height,
                             VkImageUsageFlags usage, VkImage *image,
                             VkDeviceMemory *memory)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type;
    VkResult result;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(s->device, &info, NULL, image);
    if (result != VK_SUCCESS) return result;
    vkGetImageMemoryRequirements(s->device, *image, &requirements);
    if (!find_memory_type(s, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !find_memory_type(s, requirements.memoryTypeBits, 0, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(s->device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) return result;
    return vkBindImageMemory(s->device, *image, *memory, 0);
}

static VkResult create_image_levels(struct replay_state *s, VkFormat format,
                                    uint32_t width, uint32_t height,
                                    uint32_t mip_levels,
                                    VkImageUsageFlags usage, VkImage *image,
                                    VkDeviceMemory *memory)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t type;
    VkResult result;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = mip_levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(s->device, &info, NULL, image);
    if (result != VK_SUCCESS) return result;
    vkGetImageMemoryRequirements(s->device, *image, &requirements);
    if (!find_memory_type(s, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !find_memory_type(s, requirements.memoryTypeBits, 0, &type))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(s->device, &allocation, NULL, memory);
    if (result != VK_SUCCESS) return result;
    return vkBindImageMemory(s->device, *image, *memory, 0);
}

static void image_barrier(VkCommandBuffer command, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

static void image_barrier_range(VkCommandBuffer command, VkImage image,
                                VkImageAspectFlags aspect,
                                uint32_t mip_levels,
                                VkImageLayout old_layout,
                                VkImageLayout new_layout,
                                VkAccessFlags src_access,
                                VkAccessFlags dst_access,
                                VkPipelineStageFlags src_stage,
                                VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0,
                         0, NULL, 0, NULL, 1, &barrier);
}

static VkResult init_vulkan(struct replay_state *s)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkPhysicalDevice *devices = NULL;
    VkQueueFamilyProperties *queues = NULL;
    VkExtensionProperties *extensions = NULL;
    VkPhysicalDeviceFeatures supported_features = { 0 };
    VkPhysicalDeviceFeatures enabled_features = { 0 };
    const char *enabled_extensions[1];
    uint32_t device_count = 0, queue_count = 0, extension_count = 0, i;
    float priority = 1.0f;
    VkResult result;
    /* Match the init contract of the passing Venus graphics replay.  In
     * particular, do not let an application-name/API-version negotiation
     * change the device path used by this diagnostic. */
    app.apiVersion = VK_API_VERSION_1_1;
    instance_info.pApplicationInfo = NULL;
    result = vkCreateInstance(&instance_info, NULL, &s->instance);
    if (result != VK_SUCCESS) return result;
    result = vkEnumeratePhysicalDevices(s->instance, &device_count, NULL);
    if (result != VK_SUCCESS || !device_count)
        return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    devices = calloc(device_count, sizeof(*devices));
    if (!devices) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = vkEnumeratePhysicalDevices(s->instance, &device_count, devices);
    if (result != VK_SUCCESS) { free(devices); return result; }
    s->physical = devices[0];
    free(devices);
    vkGetPhysicalDeviceProperties(s->physical, &s->properties);
    vkGetPhysicalDeviceFeatures(s->physical, &supported_features);
    result = vkEnumerateDeviceExtensionProperties(s->physical, NULL,
                                                   &extension_count, NULL);
    if (result != VK_SUCCESS) return result;
    extensions = calloc(extension_count ? extension_count : 1,
                        sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = vkEnumerateDeviceExtensionProperties(s->physical, NULL,
                                                   &extension_count, extensions);
    if (result != VK_SUCCESS) {
        free(extensions);
        return result;
    }
    for (i = 0; i < extension_count; ++i) {
        if (!strcmp(extensions[i].extensionName,
                    "VK_KHR_shader_terminate_invocation")) {
            s->terminate_invocation_supported = true;
            break;
        }
    }
    free(extensions);
    if (s->requires_terminate_invocation) {
        if (!s->terminate_invocation_supported)
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        enabled_extensions[0] = "VK_KHR_shader_terminate_invocation";
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = enabled_extensions;
        s->terminate_invocation_enabled = true;
    }
    if (s->captured_mode) {
        /* The selected Heaven pipeline was created on this same advertised
         * device contract. Enable every supported core feature so the exact
         * replay does not accidentally narrow that contract. */
        enabled_features = supported_features;
    } else {
        enabled_features.shaderStorageImageReadWithoutFormat =
            supported_features.shaderStorageImageReadWithoutFormat;
        enabled_features.shaderStorageImageWriteWithoutFormat =
            supported_features.shaderStorageImageWriteWithoutFormat;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &queue_count, NULL);
    queues = calloc(queue_count, sizeof(*queues));
    if (!queues) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(s->physical, &queue_count, queues);
    s->queue_family = UINT32_MAX;
    for (i = 0; i < queue_count; ++i) {
        if ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            s->queue_family = i;
            break;
        }
    }
    free(queues);
    if (s->queue_family == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    queue_info.queueFamilyIndex = s->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &enabled_features;
    result = vkCreateDevice(s->physical, &device_info, NULL, &s->device);
    if (result != VK_SUCCESS) return result;
    vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = s->queue_family;
    result = vkCreateCommandPool(s->device, &pool_info, NULL, &s->command_pool);
    if (result != VK_SUCCESS) return result;
    command_info.commandPool = s->command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(s->device, &command_info, &s->command);
    if (result != VK_SUCCESS) return result;
    return vkCreateFence(s->device, &fence_info, NULL, &s->fence);
}

static void fill_upload_pattern(uint8_t *data)
{
    uint32_t image, x, y;
    memset(data, 0, REPLAY_IMAGE_COUNT * 256u);
    for (image = 0; image < REPLAY_IMAGE_COUNT; ++image) {
        uint8_t *base = data + image * 256u;
        for (y = 0; y < REPLAY_IMAGE_HEIGHT; ++y) {
            for (x = 0; x < REPLAY_IMAGE_WIDTH; ++x) {
                uint32_t pixel = y * REPLAY_IMAGE_WIDTH + x;
                if (image == 0 || image == 2 || image == 5) {
                    uint8_t *p = base + pixel * 4u;
                    p[0] = (uint8_t)(32u + image * 21u + x * 37u);
                    p[1] = (uint8_t)(24u + y * 43u);
                    p[2] = (uint8_t)(192u - image * 17u + (x ^ y) * 9u);
                    p[3] = (uint8_t)(224u + ((x + y) & 1u) * 31u);
                } else if (image == 1 || image == 3) {
                    int8_t *p = (int8_t *)(base + pixel * 2u);
                    p[0] = (int8_t)(-96 + (int)x * 48 + (int)image * 4);
                    p[1] = (int8_t)(-88 + (int)y * 44 - (int)image * 3);
                } else {
                    base[pixel] = (uint8_t)(24u + x * 41u + y * 17u);
                }
            }
        }
    }
}

static VkResult create_resources(struct replay_state *s)
{
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkResult result;
    uint32_t i;
    result = create_buffer(s, REPLAY_IMAGE_COUNT * 256u,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->upload);
    if (result != VK_SUCCESS) return result;
    {
        void *mapped = NULL;
        result = vkMapMemory(s->device, s->upload.memory, 0, s->upload.size, 0, &mapped);
        if (result != VK_SUCCESS) return result;
        fill_upload_pattern(mapped);
        result = flush_replay_buffer(s, &s->upload);
        vkUnmapMemory(s->device, s->upload.memory);
        if (result != VK_SUCCESS) return result;
    }
    /* The captured VS consumes four vec4 attributes at locations 0..3. The
     * previous replay left these inputs unbound, making the real VS produce a
     * degenerate primitive and hiding descriptor/shader results. */
    result = create_buffer(s, 3u * 4u * sizeof(float) * 4u,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->vertex);
    if (result != VK_SUCCESS) return result;
    {
        static const float vertices[3][16] = {
            { -0.85f, -0.80f, 0.20f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,
               0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
            {  0.85f, -0.80f, 0.20f, 1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
               1.0f, 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
            {  0.00f,  0.85f, 0.20f, 1.0f,  0.5f, 1.0f, 1.0f, 1.0f,
               0.5f, 1.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f, 1.0f },
        };
        /*
         * The captured VS transforms location 1 through cb0/cb4, rather
         * than treating location 0 as a direct clip-space position.  This
         * probe gives that path an identity transform and a visible triangle,
         * proving that real-VS rasterization is sound independently from the
         * benchmark draw's original vertex/constant data.
         */
        static const float transform_probe_vertices[3][16] = {
            { 0.20f, 0.0f, 0.0f, 0.0f, -0.75f, -0.75f, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.20f,  1.0f, 1.0f, 1.0f, 1.0f },
            { 0.20f, 0.0f, 0.0f, 0.0f,  0.75f, -0.75f, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.20f,  1.0f, 1.0f, 1.0f, 1.0f },
            { 0.20f, 0.0f, 0.0f, 0.0f,  0.00f, 0.75f, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.20f,  1.0f, 1.0f, 1.0f, 1.0f },
        };
        void *mapped = NULL;
        result = vkMapMemory(s->device, s->vertex.memory, 0,
                             s->vertex.size, 0, &mapped);
        if (result != VK_SUCCESS) return result;
        memcpy(mapped, s->vs_transform_probe ? transform_probe_vertices : vertices,
               sizeof(vertices));
        result = flush_replay_buffer(s, &s->vertex);
        vkUnmapMemory(s->device, s->vertex.memory);
        if (result != VK_SUCCESS) return result;
    }
    for (i = 0; i < REPLAY_UBO_COUNT; ++i) {
        float values[384];
        uint32_t j, count = replay_ubo_sizes[i] / sizeof(float);
        void *mapped = NULL;
        result = create_buffer(s, replay_ubo_sizes[i], VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->ubos[i]);
        if (result != VK_SUCCESS) return result;
        memset(values, 0, sizeof(values));
        for (j = 0; j < count; ++j)
            values[j] = 0.125f + 0.03125f * (float)((j + i * 3u) % 11u);
        if (s->vs_transform_probe) {
            memset(values, 0, sizeof(values));
            if (i == 0) {
                values[0] = 1.0f;
                values[5] = 1.0f;
                values[10] = 1.0f;
                values[15] = 1.0f;
            } else if (i == 1) {
                values[0] = 1.0f;
            } else if (i == 2) {
                values[0] = 1.0f;
                values[5] = 1.0f;
                values[10] = 1.0f;
            } else if (i == 4) {
                values[0] = 1.0f;
                values[1] = 1.0f;
            }
        } else if (i == 7) {
            values[0] = 0.65f; values[1] = 0.55f;
            values[2] = 0.45f; values[3] = 1.0f;
        } else if (i == 8) {
            values[0] = 1.0f; values[1] = 1.0f;
            values[2] = 0.0f; values[3] = 0.0f;
            values[4] = 0.75f; values[5] = 0.5f;
            values[8] = 0.8f; values[9] = 0.7f;
            values[10] = 0.6f; values[11] = 0.5f;
        } else if (i == 9) {
            memset(values, 0, sizeof(values));
        }
        result = vkMapMemory(s->device, s->ubos[i].memory, 0,
                             s->ubos[i].size, 0, &mapped);
        if (result != VK_SUCCESS) return result;
        memcpy(mapped, values, replay_ubo_sizes[i]);
        result = flush_replay_buffer(s, &s->ubos[i]);
        vkUnmapMemory(s->device, s->ubos[i].memory);
        if (result != VK_SUCCESS) return result;
    }
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.minLod = 0.0f;
    sampler.maxLod = 0.0f;
    for (i = 0; i < REPLAY_IMAGE_COUNT; ++i) {
        VkFormatProperties properties;
        s->images[i].format = replay_formats[i];
        vkGetPhysicalDeviceFormatProperties(s->physical, replay_formats[i], &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        result = create_image(s, replay_formats[i], REPLAY_IMAGE_WIDTH,
                              REPLAY_IMAGE_HEIGHT,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT,
                              &s->images[i].image, &s->images[i].memory);
        if (result != VK_SUCCESS) return result;
        s->images[i].owns_image = true;
        view.image = s->images[i].image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = replay_formats[i];
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel = 0;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.baseArrayLayer = 0;
        view.subresourceRange.layerCount = 1;
        result = vkCreateImageView(s->device, &view, NULL, &s->images[i].view);
        if (result != VK_SUCCESS) return result;
        result = vkCreateSampler(s->device, &sampler, NULL, &s->images[i].sampler);
        if (result != VK_SUCCESS) return result;
    }
    result = create_image(s, VK_FORMAT_R8G8B8A8_UNORM, REPLAY_WIDTH,
                          REPLAY_HEIGHT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          &s->target, &s->target_memory);
    if (result != VK_SUCCESS) return result;
    view.image = s->target;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    result = vkCreateImageView(s->device, &view, NULL, &s->target_view);
    if (result != VK_SUCCESS) return result;
    return create_buffer(s, REPLAY_WIDTH * REPLAY_HEIGHT * 4u,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->readback);
}

static VkResult create_descriptors(struct replay_state *s)
{
    VkDescriptorSetLayoutBinding bindings[27];
    VkDescriptorSetLayoutCreateInfo layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    VkDescriptorPoolSize sizes[4];
    VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorBufferInfo buffer_info[REPLAY_UBO_COUNT];
    VkDescriptorImageInfo sampler_info[REPLAY_IMAGE_COUNT];
    VkDescriptorImageInfo image_info[REPLAY_IMAGE_COUNT];
    VkWriteDescriptorSet writes[27];
    VkResult result;
    const char *mode = s->layout_mode && s->layout_mode[0] ? s->layout_mode : "full";
    bool exact = !strcmp(mode, "exact");
    bool small = !strcmp(mode, "small");
    uint32_t ubo_count = small ? 1u : REPLAY_UBO_COUNT;
    uint32_t image_count = small ? 1u : REPLAY_IMAGE_COUNT;
    uint32_t sampler_base = small ? 1u : 10u;
    uint32_t image_base = small ? 2u : 16u;
    bool use_ubo = small || !strcmp(mode, "full") || !strcmp(mode, "ubo") ||
        !strcmp(mode, "dynamic");
    bool use_dynamic = !strcmp(mode, "dynamic");
    bool use_samplers = small || !strcmp(mode, "full") || !strcmp(mode, "sampler") ||
        !strcmp(mode, "images") || !strcmp(mode, "dynamic");
    bool use_images = small || !strcmp(mode, "full") || !strcmp(mode, "sampled") ||
        !strcmp(mode, "images") || !strcmp(mode, "dynamic");
    uint32_t i, write = 0, binding_count = 0, pool_count = 0;
    memset(sizes, 0, sizeof(sizes));
    memset(bindings, 0, sizeof(bindings));
    /* Reproduce the captured Heaven descriptor contract verbatim. */
    if (exact) {
      static const uint32_t exact_bindings[] = {
          160u, 161u, 162u, 163u, 164u,
          0u, 1u, 2u, 3u, 15u,
          16u, 17u, 19u, 20u, 28u, 29u,
          32u, 33u, 35u, 36u, 44u, 45u,
      };

      /* Keep pBindings sorted while preserving each binding's type and
       * stage visibility. */
      struct exact_layout_binding {
        uint32_t binding;
        VkDescriptorType type;
        VkShaderStageFlags stages;
      };
      static const struct exact_layout_binding layout_contract[] = {
          { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 3u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 15u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 16u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 17u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 19u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 20u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 28u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 29u, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 32u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 33u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 35u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 36u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 44u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 45u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT },
          { 160u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 161u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 162u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 163u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
          { 164u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT },
      };
      uint32_t index = 0;
      for (i = 0; i < sizeof(layout_contract) / sizeof(layout_contract[0]); ++i) {
        bindings[index].binding = layout_contract[i].binding;
        bindings[index].descriptorType = layout_contract[i].type;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = layout_contract[i].stages;
        ++index;
      }
      binding_count = index;
      sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      sizes[0].descriptorCount = 10u;
      sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
      sizes[1].descriptorCount = 6u;
      sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      sizes[2].descriptorCount = 6u;
      pool_count = 3u;
      layout.bindingCount = binding_count;
      layout.pBindings = bindings;
      result = vkCreateDescriptorSetLayout(s->device, &layout, NULL, &s->set_layout);
      if (result != VK_SUCCESS) return result;
      pool.maxSets = 1;
      pool.poolSizeCount = pool_count;
      pool.pPoolSizes = sizes;
      result = vkCreateDescriptorPool(s->device, &pool, NULL, &s->descriptor_pool);
      if (result != VK_SUCCESS) return result;
      allocate.descriptorPool = s->descriptor_pool;
      allocate.descriptorSetCount = 1;
      allocate.pSetLayouts = &s->set_layout;
      result = vkAllocateDescriptorSets(s->device, &allocate, &s->descriptor_set);
      if (result != VK_SUCCESS) return result;
      memset(writes, 0, sizeof(writes));
      for (i = 0; i < 10u; ++i) {
        buffer_info[i].buffer = s->ubos[i].buffer;
        buffer_info[i].offset = 0;
        buffer_info[i].range = replay_ubo_sizes[i];
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = exact_bindings[i];
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[write].pBufferInfo = &buffer_info[i];
        write++;
      }
      for (i = 0; i < 6u; ++i) {
        sampler_info[i].sampler = s->images[i].sampler;
        sampler_info[i].imageView = VK_NULL_HANDLE;
        sampler_info[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = exact_bindings[10u + i];
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[write].pImageInfo = &sampler_info[i];
        write++;
      }
      for (i = 0; i < 6u; ++i) {
        image_info[i].sampler = VK_NULL_HANDLE;
        image_info[i].imageView = s->images[i].view;
        image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = exact_bindings[16u + i];
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[write].pImageInfo = &image_info[i];
        write++;
      }
      s->layout_binding_count = binding_count;
      s->dynamic_binding_count = 0;
      vkUpdateDescriptorSets(s->device, write, writes, 0, NULL);
      return VK_SUCCESS;
    }
    if (use_ubo) {
      for (i = 0; i < ubo_count; ++i) {
        bindings[i].binding = i < 10u ? i : 150u + i;
        bindings[i].descriptorType = use_dynamic ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i].descriptorCount = 1;
        /* The reduced small contract is fragment-only, matching the passing
         * graphics replay.  The full Heaven contract retains vertex-stage
         * visibility for bindings 0..4 because the captured vertex shader
         * genuinely reads those UBOs. */
        bindings[i].stageFlags = small ? VK_SHADER_STAGE_FRAGMENT_BIT :
            (i < 5u ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) :
             (i < 10u ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT));
      }
      binding_count = ubo_count;
      sizes[pool_count].type = use_dynamic ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      sizes[pool_count++].descriptorCount = ubo_count;
    }
    if (use_samplers) {
      for (i = 0; i < image_count; ++i) {
        bindings[binding_count + i].binding = sampler_base + i;
        bindings[binding_count + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[binding_count + i].descriptorCount = 1;
        bindings[binding_count + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      binding_count += image_count;
      sizes[pool_count].type = VK_DESCRIPTOR_TYPE_SAMPLER;
      sizes[pool_count++].descriptorCount = image_count;
    }
    if (use_images) {
      for (i = 0; i < image_count; ++i) {
        bindings[binding_count + i].binding = image_base + i;
        bindings[binding_count + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[binding_count + i].descriptorCount = 1;
        bindings[binding_count + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      binding_count += image_count;
      sizes[pool_count].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      sizes[pool_count++].descriptorCount = image_count;
    }
    s->layout_binding_count = binding_count;
    s->dynamic_binding_count = use_dynamic ? ubo_count : 0;
    layout.bindingCount = binding_count;
    layout.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(s->device, &layout, NULL, &s->set_layout);
    if (result != VK_SUCCESS) return result;
    pool.maxSets = 1;
    /* A zero-descriptor layout is legal, but some implementations reject a
     * descriptor pool with no pool sizes. Keep one unused slot for that
     * diagnostic mode; it does not change the pipeline layout contract. */
    if (!pool_count) {
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = 1;
        pool_count = 1;
    }
    pool.poolSizeCount = pool_count;
    pool.pPoolSizes = sizes;
    result = vkCreateDescriptorPool(s->device, &pool, NULL, &s->descriptor_pool);
    if (result != VK_SUCCESS) return result;
    allocate.descriptorPool = s->descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &s->set_layout;
    result = vkAllocateDescriptorSets(s->device, &allocate, &s->descriptor_set);
    if (result != VK_SUCCESS) return result;
    memset(writes, 0, sizeof(writes));
    if (use_ubo) for (i = 0; i < ubo_count; ++i) {
        buffer_info[i].buffer = s->ubos[i].buffer;
        buffer_info[i].offset = 0;
        buffer_info[i].range = replay_ubo_sizes[i];
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = i < 10u ? i : 150u + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = use_dynamic ?
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[write].pBufferInfo = &buffer_info[i];
        write++;
    }
    if (use_samplers) for (i = 0; i < image_count; ++i) {
        sampler_info[i].sampler = s->images[i].sampler;
        sampler_info[i].imageView = VK_NULL_HANDLE;
        sampler_info[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = sampler_base + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[write].pImageInfo = &sampler_info[i];
        write++;
    }
    if (use_images) for (i = 0; i < image_count; ++i) {
        image_info[i].sampler = VK_NULL_HANDLE;
        image_info[i].imageView = s->images[i].view;
        image_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = image_base + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[write].pImageInfo = &image_info[i];
        write++;
    }
    vkUpdateDescriptorSets(s->device, write, writes, 0, NULL);
    return VK_SUCCESS;
}

static bool spirv_has_spec_id(const uint32_t *code, size_t size, uint32_t spec_id)
{
    size_t words, offset;
    if (!code || size < 20 || (size & 3) || code[0] != 0x07230203u)
        return false;
    words = size / sizeof(uint32_t);
    for (offset = 5; offset < words;) {
        uint16_t word_count = (uint16_t)(code[offset] >> 16);
        uint16_t opcode = (uint16_t)(code[offset] & 0xffffu);
        if (!word_count || offset + word_count > words)
            return false;
        if (opcode == 71 && word_count >= 4 && code[offset + 2] == 1u &&
            code[offset + 3] == spec_id)
            return true;
        offset += word_count;
    }
    return false;
}

static VkResult create_pipeline(struct replay_state *s)
{
    VkAttachmentDescription attachment = { 0 };
    VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkRenderPassCreateInfo render_pass = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkFramebufferCreateInfo framebuffer = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkPipelineLayoutCreateInfo pipeline_layout = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkVertexInputBindingDescription vertex_binding = {
        0u, 16u * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription vertex_attributes[4] = {
        { 0u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 0u },
        { 1u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 4u * sizeof(float) },
        { 2u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 8u * sizeof(float) },
        { 3u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, 12u * sizeof(float) },
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    VkPipelineViewportStateCreateInfo viewport = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    VkPipelineCreateFlags2CreateInfoKHR flags2 = {
        VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    static const uint32_t bool_spec_ids[] = {
        0u, 1u, 2u, 3u, 15u, 32u, 33u, 35u, 36u, 44u, 45u
    };
    VkSpecializationMapEntry spec_entries[1 + sizeof(bool_spec_ids) / sizeof(bool_spec_ids[0])];
    uint32_t spec_values[1 + sizeof(bool_spec_ids) / sizeof(bool_spec_ids[0])];
    VkSpecializationInfo specialization = { 0 };
    uint32_t spec_count = 0;
    uint32_t bool_value = 1u;
    VkShaderModuleCreateInfo shader = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    uint32_t *vertex_code = NULL, *fragment_code = NULL;
    size_t vertex_size = 0, fragment_size = 0;
    VkResult result;
    if (!load_spirv(s->vertex_path, &vertex_code, &vertex_size) ||
        !load_spirv(s->fragment_path, &fragment_code, &fragment_size)) {
        free(vertex_code);
        free(fragment_code);
        return record_shader_load_failure(s);
    }
    record_shader_code(s, vertex_code, vertex_size, fragment_code, fragment_size);
    shader.codeSize = vertex_size;
    shader.pCode = vertex_code;
    result = record_pipeline_step(s, "shader-module-vertex",
                                  vkCreateShaderModule(s->device, &shader, NULL,
                                                       &s->vertex_shader));
    if (result == VK_SUCCESS) {
        shader.codeSize = fragment_size;
        shader.pCode = fragment_code;
        result = record_pipeline_step(s, "shader-module-fragment",
                                      vkCreateShaderModule(s->device, &shader,
                                                           NULL,
                                                           &s->fragment_shader));
    }
    s->specialization_applied = spirv_has_spec_id(fragment_code, fragment_size, 1216u);
    if (s->specialization_applied) {
        spec_entries[spec_count].constantID = 1216u;
        spec_entries[spec_count].offset = spec_count * sizeof(uint32_t);
        spec_entries[spec_count].size = sizeof(uint32_t);
        spec_values[spec_count++] = 12816u;
    }
    if (s->bool_mode && !strcmp(s->bool_mode, "false"))
        bool_value = 0u;
    if (s->bool_mode && strcmp(s->bool_mode, "default")) {
        uint32_t i;
        for (i = 0; i < sizeof(bool_spec_ids) / sizeof(bool_spec_ids[0]); ++i) {
            if (!spirv_has_spec_id(fragment_code, fragment_size, bool_spec_ids[i]))
                continue;
            spec_entries[spec_count].constantID = bool_spec_ids[i];
            spec_entries[spec_count].offset = spec_count * sizeof(uint32_t);
            spec_entries[spec_count].size = sizeof(uint32_t);
            spec_values[spec_count++] = bool_value;
        }
    }
    specialization.mapEntryCount = spec_count;
    specialization.pMapEntries = spec_entries;
    specialization.dataSize = spec_count * sizeof(uint32_t);
    specialization.pData = spec_values;
    s->bool_spec_count = spec_count > (s->specialization_applied ? 1u : 0u) ?
        spec_count - (s->specialization_applied ? 1u : 0u) : 0u;
    free(vertex_code);
    free(fragment_code);
    if (result != VK_SUCCESS) return result;
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    render_pass.attachmentCount = 1;
    render_pass.pAttachments = &attachment;
    render_pass.subpassCount = 1;
    render_pass.pSubpasses = &subpass;
    result = record_pipeline_step(s, "render-pass",
                                  vkCreateRenderPass(s->device, &render_pass,
                                                      NULL, &s->render_pass));
    if (result != VK_SUCCESS) return result;
    framebuffer.renderPass = s->render_pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &s->target_view;
    framebuffer.width = REPLAY_WIDTH;
    framebuffer.height = REPLAY_HEIGHT;
    framebuffer.layers = 1;
    result = record_pipeline_step(s, "framebuffer",
                                  vkCreateFramebuffer(s->device, &framebuffer,
                                                       NULL, &s->framebuffer));
    if (result != VK_SUCCESS) return result;
    pipeline_layout.setLayoutCount = strcmp(s->layout_mode, "no-set") ? 1u : 0u;
    pipeline_layout.pSetLayouts = pipeline_layout.setLayoutCount ? &s->set_layout : NULL;
    result = record_pipeline_step(s, "pipeline-layout",
                                  vkCreatePipelineLayout(s->device,
                                                         &pipeline_layout, NULL,
                                                         &s->pipeline_layout));
    if (result != VK_SUCCESS) return result;
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s->fragment_shader;
    stages[1].pName = "main";
    /* Do not pass DXVK's specialization map to a replay shader which does not
     * declare that SpecId.  Lavapipe ignores unknown entries, while the
     * Maleoon compiler rejects them with VK_ERROR_INITIALIZATION_FAILED. */
    stages[1].pSpecializationInfo = spec_count ? &specialization : NULL;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4u;
    vertex_input.pVertexAttributeDescriptions = vertex_attributes;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &input_assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = s->pipeline_layout;
    pipeline.renderPass = s->render_pass;
    pipeline.subpass = 0;
    /* Force a synchronous Venus reply for this diagnostic.  Without the
     * EARLY_RETURN_ON_FAILURE bit the Guest may receive a provisional handle
     * while the Host compiler fails later, turning a useful VkResult into a
     * ring-fatal bind error. */
    flags2.flags = VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR;
    pipeline.pNext = &flags2;
    return record_pipeline_step(s, "graphics-pipeline",
                                vkCreateGraphicsPipelines(s->device,
                                                          VK_NULL_HANDLE, 1,
                                                          &pipeline, NULL,
                                                          &s->pipeline));
}

static VkResult render_and_readback(struct replay_state *s)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkClearValue clear;
    VkRenderPassBeginInfo render = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport viewport = {
        0.0f, 0.0f, (float)REPLAY_WIDTH, (float)REPLAY_HEIGHT, 0.0f, 1.0f
    };
    VkRect2D scissor = { { 0, 0 }, { REPLAY_WIDTH, REPLAY_HEIGHT } };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    uint32_t dynamic_offsets[REPLAY_UBO_COUNT] = { 0 };
    VkBufferImageCopy copy = { 0 };
    VkResult result;
    uint32_t i;
    result = vkBeginCommandBuffer(s->command, &begin);
    if (result != VK_SUCCESS) return result;
    for (i = 0; i < REPLAY_IMAGE_COUNT; ++i) {
        image_barrier(s->command, s->images[i].image,
                      VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&copy, 0, sizeof(copy));
        copy.bufferOffset = i * 256u;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = REPLAY_IMAGE_WIDTH;
        copy.imageExtent.height = REPLAY_IMAGE_HEIGHT;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s->command, s->upload.buffer,
                               s->images[i].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
        image_barrier(s->command, s->images[i].image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        s->gpu_copies++;
    }
    memset(&clear, 0, sizeof(clear));
    clear.color.float32[0] = 1.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 1.0f;
    clear.color.float32[3] = 1.0f;
    render.renderPass = s->render_pass;
    render.framebuffer = s->framebuffer;
    render.renderArea.extent.width = REPLAY_WIDTH;
    render.renderArea.extent.height = REPLAY_HEIGHT;
    render.clearValueCount = 1;
    render.pClearValues = &clear;
    vkCmdBeginRenderPass(s->command, &render, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->command, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
    {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(s->command, 0, 1, &s->vertex.buffer, &offset);
    }
    if (strcmp(s->layout_mode, "no-set"))
        vkCmdBindDescriptorSets(s->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                s->pipeline_layout, 0, 1, &s->descriptor_set,
                                s->dynamic_binding_count,
                                s->dynamic_binding_count ? dynamic_offsets : NULL);
    vkCmdSetViewport(s->command, 0, 1, &viewport);
    vkCmdSetScissor(s->command, 0, 1, &scissor);
    vkCmdDraw(s->command, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->command);
    image_barrier(s->command, s->target,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = REPLAY_WIDTH;
    copy.imageExtent.height = REPLAY_HEIGHT;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s->command, s->target,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s->readback.buffer, 1, &copy);
    s->gpu_copies++;
    result = vkEndCommandBuffer(s->command);
    if (result != VK_SUCCESS) return result;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s->command;
    result = vkQueueSubmit(s->queue, 1, &submit, s->fence);
    if (result != VK_SUCCESS) return result;
    s->queue_submits++;
    return vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 10000000000ull);
}

static int analyze_output(struct replay_state *s)
{
    static const uint32_t sample_x[REPLAY_SAMPLE_COUNT] = {
        4u, 31u, 59u, 4u, 31u, 59u, 4u, 31u, 59u,
    };
    static const uint32_t sample_y[REPLAY_SAMPLE_COUNT] = {
        4u, 4u, 4u, 31u, 31u, 31u, 59u, 59u, 59u,
    };
    VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    void *mapped = NULL;
    const uint8_t *pixels;
    uint32_t i;
    FILE *rgba;
    VkResult result = vkMapMemory(s->device, s->readback.memory, 0,
                                  s->readback.size, 0, &mapped);
    if (result != VK_SUCCESS) return 0;
    range.memory = s->readback.memory;
    range.size = VK_WHOLE_SIZE;
    result = vkInvalidateMappedMemoryRanges(s->device, 1, &range);
    if (result != VK_SUCCESS) {
        vkUnmapMemory(s->device, s->readback.memory);
        return 0;
    }
    pixels = mapped;
    s->checksum = 2166136261u;
    s->changed_pixels = 0;
    s->opaque_pixels = 0;
    for (i = 0; i < REPLAY_WIDTH * REPLAY_HEIGHT; ++i) {
        const uint8_t *p = &pixels[i * 4u];
        if (p[0] != 255u || p[1] != 0u || p[2] != 255u || p[3] != 255u)
            s->changed_pixels++;
        if (p[3] == 255u) s->opaque_pixels++;
        s->checksum ^= p[0]; s->checksum *= 16777619u;
        s->checksum ^= p[1]; s->checksum *= 16777619u;
        s->checksum ^= p[2]; s->checksum *= 16777619u;
        s->checksum ^= p[3]; s->checksum *= 16777619u;
    }
    for (i = 0; i < REPLAY_SAMPLE_COUNT; ++i) {
        const uint8_t *p = &pixels[(sample_y[i] * REPLAY_WIDTH + sample_x[i]) * 4u];
        s->sample_values[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (s->rgba_path && s->rgba_path[0]) {
        rgba = fopen(s->rgba_path, "wb");
        if (rgba) {
            fwrite(pixels, 1, REPLAY_WIDTH * REPLAY_HEIGHT * 4u, rgba);
            fflush(rgba);
            fsync(fileno(rgba));
            fclose(rgba);
        }
    }
    vkUnmapMemory(s->device, s->readback.memory);
    return 1;
}

static VkResult load_capture_buffer(struct replay_state *s,
                                    struct replay_buffer *buffer,
                                    const char *name,
                                    uint32_t *expected_hash)
{
    char path[1024];
    void *mapped = NULL;
    VkResult result;
    if (!capture_path(s, name, path, sizeof(path)))
        return VK_ERROR_INITIALIZATION_FAILED;
    result = vkMapMemory(s->device, buffer->memory, 0, buffer->size, 0, &mapped);
    if (result != VK_SUCCESS) return result;
    if (!read_exact_file(path, mapped, (size_t)buffer->size)) {
        vkUnmapMemory(s->device, buffer->memory);
        fprintf(stderr, "[venus-heaven-replay] capture read failed path=%s bytes=%" PRIu64 "\n",
                path, (uint64_t)buffer->size);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (expected_hash)
        *expected_hash = fnv1a32(mapped, (size_t)buffer->size);
    fprintf(stderr,
            "[venus-heaven-replay] input-cpu kind=buffer memory=0x%" PRIx64
            " bytes=%" PRIu64 " fnv=0x%08x name=%s\n",
            (uint64_t)(uintptr_t)buffer->memory, (uint64_t)buffer->size,
            expected_hash ? *expected_hash : fnv1a32(mapped, (size_t)buffer->size),
            name);
    result = flush_replay_buffer(s, buffer);
    vkUnmapMemory(s->device, buffer->memory);
    return result;
}

static VkResult create_captured_resources(struct replay_state *s)
{
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties properties;
    VkDeviceSize upload_size = 0;
    VkResult result;
    uint32_t i, mip;
    char name[256], path[1024];
    void *mapped = NULL;

    for (i = 0; i < s->capture_image_count; ++i) {
        uint32_t bpp = format_bytes_per_texel(s->capture_images[i].format);
        s->images[i].format = s->capture_images[i].format;
        s->images[i].width = s->capture_images[i].width;
        s->images[i].height = s->capture_images[i].height;
        s->images[i].mip_levels = s->capture_images[i].mip_levels;
        if (capture_alias_index(s, i) >= 0)
            continue;
        for (mip = 0; mip < s->capture_images[i].mip_levels; ++mip) {
            uint32_t width = s->capture_images[i].width >> mip;
            uint32_t height = s->capture_images[i].height >> mip;
            VkDeviceSize bytes;
            if (!width) width = 1;
            if (!height) height = 1;
            upload_size = align4(upload_size);
            s->images[i].upload_offsets[mip] = upload_size;
            bytes = (VkDeviceSize)width * height * bpp;
            upload_size += bytes;
        }
    }
    /* Four-byte alignment is sufficient for Vulkan, but some mobile drivers
     * round an RGBA16F buffer offset down to a full texel boundary. Use the
     * stricter, still-valid alignment so this replay measures the captured
     * workload rather than an avoidable staging-layout portability issue. */
    s->target_upload_offset = align_to(
        upload_size, format_bytes_per_texel(VK_FORMAT_R16G16B16A16_SFLOAT));
    upload_size = s->target_upload_offset + CAPTURE_OUTPUT_BYTES;
    s->depth_upload_offset = align4(upload_size);
    upload_size = s->depth_upload_offset + CAPTURE_WIDTH * CAPTURE_HEIGHT * 4u;

    result = create_buffer(s, upload_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->upload);
    if (result != VK_SUCCESS) return result;
    result = vkMapMemory(s->device, s->upload.memory, 0, s->upload.size, 0, &mapped);
    if (result != VK_SUCCESS) return result;
    memset(mapped, 0, (size_t)s->upload.size);
    for (i = 0; i < s->capture_image_count; ++i) {
        uint32_t bpp = format_bytes_per_texel(s->capture_images[i].format);
        if (capture_alias_index(s, i) >= 0)
            continue;
        for (mip = 0; mip < s->capture_images[i].mip_levels; ++mip) {
            uint32_t width = s->capture_images[i].width >> mip;
            uint32_t height = s->capture_images[i].height >> mip;
            size_t bytes;
            if (!width) width = 1;
            if (!height) height = 1;
            bytes = (size_t)width * height * bpp;
            if (mip)
                snprintf(name, sizeof(name),
                         "frame-180-pass-2-sampled-%u-mip-%u.bin",
                         capture_source_binding(&s->capture_images[i]), mip);
            else
                snprintf(name, sizeof(name), "frame-180-pass-2-sampled-%u.bin",
                         capture_source_binding(&s->capture_images[i]));
            if (!capture_path(s, name, path, sizeof(path)) ||
                !read_exact_file(path,
                                 (uint8_t *)mapped + s->images[i].upload_offsets[mip],
                                 bytes)) {
                vkUnmapMemory(s->device, s->upload.memory);
                fprintf(stderr, "[venus-heaven-replay] sampled mip read failed path=%s bytes=%zu\n",
                        path, bytes);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            s->input_image_expected[i][mip] = fnv1a32(
                (uint8_t *)mapped + s->images[i].upload_offsets[mip], bytes);
            fprintf(stderr,
                    "[venus-heaven-replay] input-cpu kind=image binding=%u mip=%u"
                    " uploadMemory=0x%" PRIx64 " offset=%" PRIu64
                    " bytes=%zu fnv=0x%08x\n",
                    s->capture_images[i].binding, mip,
                    (uint64_t)(uintptr_t)s->upload.memory,
                    (uint64_t)s->images[i].upload_offsets[mip], bytes,
                    s->input_image_expected[i][mip]);
        }
    }
    if (!capture_path(s, "frame-180-pass-2-color-0.bin", path, sizeof(path)) ||
        !read_exact_file(path, (uint8_t *)mapped + s->target_upload_offset,
                         CAPTURE_OUTPUT_BYTES)) {
        vkUnmapMemory(s->device, s->upload.memory);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    s->baseline_checksum = fnv1a32((uint8_t *)mapped + s->target_upload_offset,
                                   CAPTURE_OUTPUT_BYTES);
    s->input_upload_expected = fnv1a32(mapped, (size_t)s->target_upload_offset);
    if (!capture_path(s, "frame-180-pass-2-depth-1.bin", path, sizeof(path)) ||
        !read_exact_file(path, (uint8_t *)mapped + s->depth_upload_offset,
                         CAPTURE_WIDTH * CAPTURE_HEIGHT * 4u)) {
        vkUnmapMemory(s->device, s->upload.memory);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    result = flush_replay_buffer(s, &s->upload);
    vkUnmapMemory(s->device, s->upload.memory);
    if (result != VK_SUCCESS) return result;

    result = create_buffer(s, s->capture_vertex_bytes,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           (s->verify_inputs ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0),
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->vertex);
    if (result != VK_SUCCESS) return result;
    snprintf(name, sizeof(name),
             "frame-180-pass-2-draw-%u-vertex-0.bin", s->capture_draw_id);
    result = load_capture_buffer(s, &s->vertex, name,
                                 &s->input_vertex_expected);
    if (result != VK_SUCCESS) return result;
    result = create_buffer(s, s->capture_index_bytes,
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           (s->verify_inputs ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0),
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->index);
    if (result != VK_SUCCESS) return result;
    snprintf(name, sizeof(name),
             "frame-180-pass-2-draw-%u-index.bin", s->capture_draw_id);
    result = load_capture_buffer(s, &s->index, name,
                                 &s->input_index_expected);
    if (result != VK_SUCCESS) return result;

    for (i = 0; i < s->capture_ubo_count; ++i) {
        result = create_buffer(s, s->capture_ubo_sizes[i],
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               (s->verify_inputs ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0),
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->ubos[i]);
        if (result != VK_SUCCESS) return result;
        snprintf(name, sizeof(name),
                 "frame-180-pass-2-draw-%u-descriptor-buffer-%u.bin",
                 s->capture_draw_id, i);
        result = load_capture_buffer(s, &s->ubos[i], name,
                                     &s->input_ubo_expected[i]);
        if (result != VK_SUCCESS) return result;
    }

    for (i = 0; i < s->capture_image_count; ++i) {
        const int32_t alias = capture_alias_index(s, i);
        const VkImageAspectFlags aspect =
            capture_image_aspect(&s->capture_images[i]);
        vkGetPhysicalDeviceFormatProperties(s->physical, s->capture_images[i].format,
                                            &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        if (alias >= 0) {
            s->images[i].image = s->images[alias].image;
            s->images[i].memory = s->images[alias].memory;
            s->images[i].view = s->images[alias].view;
            s->images[i].owns_image = false;
        } else {
            result = create_image_levels(s, s->capture_images[i].format,
                                         s->capture_images[i].width,
                                         s->capture_images[i].height,
                                         s->capture_images[i].mip_levels,
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         (s->verify_inputs ?
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) |
                                         VK_IMAGE_USAGE_SAMPLED_BIT,
                                         &s->images[i].image,
                                         &s->images[i].memory);
            if (result != VK_SUCCESS) return result;
            s->images[i].owns_image = true;
            memset(&view, 0, sizeof(view));
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = s->images[i].image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = s->capture_images[i].format;
            view.subresourceRange.aspectMask = aspect;
            view.subresourceRange.levelCount = s->capture_images[i].mip_levels;
            view.subresourceRange.layerCount = 1;
            result = vkCreateImageView(s->device, &view, NULL,
                                       &s->images[i].view);
            if (result != VK_SUCCESS) return result;
        }

        memset(&sampler, 0, sizeof(sampler));
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        {
            const bool draw0 = capture_profile_is(s, "draw0");
            const bool f647 = capture_profile_is(s, "f647");
            const bool linear = f647 ? true : draw0 ? i < 4u : i != 3u;
            const bool mip_linear = f647 ? i < 6u :
                draw0 ? i < 4u : i < 2u;
            const bool anisotropic = f647 ? i < 6u :
                draw0 ? i < 4u : i < 2u;
            sampler.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            sampler.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            sampler.mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR :
                VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
            sampler.maxAnisotropy = anisotropic ? 2.0f : 1.0f;
            sampler.addressModeU = f647 && i == 6u ?
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER :
                VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler.addressModeV = sampler.addressModeU;
            sampler.addressModeW = sampler.addressModeU;
            sampler.compareEnable = f647 && i == 6u ? VK_TRUE : VK_FALSE;
            sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        }
        sampler.minLod = 0.0f;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        result = vkCreateSampler(s->device, &sampler, NULL, &s->images[i].sampler);
        if (result != VK_SUCCESS) return result;
    }

    result = create_image_levels(s, VK_FORMAT_R16G16B16A16_SFLOAT,
                                 CAPTURE_WIDTH, CAPTURE_HEIGHT, 1u,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                 &s->target, &s->target_memory);
    if (result != VK_SUCCESS) return result;
    memset(&view, 0, sizeof(view));
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = s->target;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    result = vkCreateImageView(s->device, &view, NULL, &s->target_view);
    if (result != VK_SUCCESS) return result;

    result = create_image_levels(s, VK_FORMAT_D24_UNORM_S8_UINT,
                                 CAPTURE_WIDTH, CAPTURE_HEIGHT, 1u,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                 &s->depth, &s->depth_memory);
    if (result != VK_SUCCESS) return result;
    memset(&view, 0, sizeof(view));
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = s->depth;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_D24_UNORM_S8_UINT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
        VK_IMAGE_ASPECT_STENCIL_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    result = vkCreateImageView(s->device, &view, NULL, &s->depth_view);
    if (result != VK_SUCCESS) return result;

    s->cpu_upload_bytes = upload_size + s->capture_vertex_bytes +
        s->capture_index_bytes;
    for (i = 0; i < s->capture_ubo_count; ++i)
        s->cpu_upload_bytes += s->capture_ubo_sizes[i];
    s->output_bytes = CAPTURE_OUTPUT_BYTES;
    result = create_buffer(s, s->output_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s->readback);
    if (result != VK_SUCCESS || !s->verify_inputs)
        return result;

    s->input_vertex_offset = align4(s->target_upload_offset);
    s->input_index_offset = align4(s->input_vertex_offset + s->capture_vertex_bytes);
    upload_size = align4(s->input_index_offset + s->capture_index_bytes);
    for (i = 0; i < s->capture_ubo_count; ++i) {
        s->input_ubo_offsets[i] = upload_size;
        upload_size = align4(upload_size + s->capture_ubo_sizes[i]);
    }
    s->input_upload_offset = upload_size;
    upload_size = align4(upload_size + s->target_upload_offset);
    return create_buffer(s, upload_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &s->input_readback);
}

static VkResult create_captured_descriptors(struct replay_state *s)
{
    VkDescriptorSetLayoutBinding bindings[26];
    VkDescriptorSetLayoutCreateInfo layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    VkDescriptorPoolSize sizes[3];
    VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorBufferInfo buffers[CAPTURE_UBO_CAPACITY];
    VkDescriptorImageInfo samplers[CAPTURE_IMAGE_CAPACITY];
    VkDescriptorImageInfo images[CAPTURE_IMAGE_CAPACITY];
    VkWriteDescriptorSet writes[26];
    VkResult result;
    const bool draw0 = s->capture_profile && !strcmp(s->capture_profile, "draw0");
    const bool f647 = capture_profile_is(s, "f647");
    const bool depth_pair = capture_profile_is_depth_pair(s);
    const uint32_t sampler_binding_base = f647 ? 12u : draw0 ? 10u : 9u;
    const uint32_t image_binding_base = f647 ? 19u : draw0 ? 16u : 13u;
    uint32_t i, write = 0;
    memset(bindings, 0, sizeof(bindings));
    memset(sizes, 0, sizeof(sizes));
    memset(writes, 0, sizeof(writes));
    for (i = 0; i < s->capture_ubo_count; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = depth_pair ? VK_SHADER_STAGE_VERTEX_BIT :
            i < (f647 ? 6u : 5u) ?
            VK_SHADER_STAGE_VERTEX_BIT :
            VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (i = 0; i < s->capture_image_count; ++i) {
        bindings[s->capture_ubo_count + i].binding = sampler_binding_base + i;
        bindings[s->capture_ubo_count + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[s->capture_ubo_count + i].descriptorCount = 1;
        bindings[s->capture_ubo_count + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[s->capture_ubo_count + s->capture_image_count + i].binding =
            image_binding_base + i;
        bindings[s->capture_ubo_count + s->capture_image_count + i].descriptorType =
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[s->capture_ubo_count + s->capture_image_count + i].descriptorCount = 1;
        bindings[s->capture_ubo_count + s->capture_image_count + i].stageFlags =
            VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    layout.bindingCount = s->capture_ubo_count + 2u * s->capture_image_count;
    layout.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(s->device, &layout, NULL, &s->set_layout);
    if (result != VK_SUCCESS) return result;
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = s->capture_ubo_count;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[1].descriptorCount = s->capture_image_count;
    sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[2].descriptorCount = s->capture_image_count;
    pool.maxSets = 1;
    pool.poolSizeCount = s->capture_image_count ? 3u : 1u;
    pool.pPoolSizes = sizes;
    result = vkCreateDescriptorPool(s->device, &pool, NULL, &s->descriptor_pool);
    if (result != VK_SUCCESS) return result;
    allocate.descriptorPool = s->descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &s->set_layout;
    result = vkAllocateDescriptorSets(s->device, &allocate, &s->descriptor_set);
    if (result != VK_SUCCESS) return result;
    for (i = 0; i < s->capture_ubo_count; ++i) {
        buffers[i].buffer = s->ubos[i].buffer;
        buffers[i].offset = 0;
        buffers[i].range = s->capture_ubo_sizes[i];
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[write].pBufferInfo = &buffers[i];
        ++write;
    }
    for (i = 0; i < s->capture_image_count; ++i) {
        samplers[i].sampler = s->images[i].sampler;
        samplers[i].imageView = VK_NULL_HANDLE;
        samplers[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = sampler_binding_base + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[write].pImageInfo = &samplers[i];
        ++write;
    }
    for (i = 0; i < s->capture_image_count; ++i) {
        images[i].sampler = VK_NULL_HANDLE;
        images[i].imageView = s->images[i].view;
        images[i].imageLayout = capture_sampled_layout(&s->capture_images[i]);
        writes[write].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write].dstSet = s->descriptor_set;
        writes[write].dstBinding = image_binding_base + i;
        writes[write].descriptorCount = 1;
        writes[write].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[write].pImageInfo = &images[i];
        ++write;
    }
    s->layout_binding_count = s->capture_ubo_count +
        2u * s->capture_image_count;
    s->dynamic_binding_count = 0;
    vkUpdateDescriptorSets(s->device, write, writes, 0, NULL);
    return VK_SUCCESS;
}

static VkResult create_captured_pipeline(struct replay_state *s)
{
    VkAttachmentDescription attachments[2];
    VkAttachmentReference color_reference = {
        0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkAttachmentReference depth_reference = {
        1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };
    VkSubpassDescription subpass = { 0 };
    VkRenderPassCreateInfo render_pass = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkFramebufferCreateInfo framebuffer = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView framebuffer_views[2];
    VkPipelineLayoutCreateInfo pipeline_layout = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkVertexInputBindingDescription vertex_binding = {
        0u, 32u, VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription vertex_attributes[4] = {
        { 0u, 0u, VK_FORMAT_R32G32_SFLOAT, 0u },
        { 1u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT, 8u },
        { 2u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT, 16u },
        { 3u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT, 24u },
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    VkPipelineViewportStateCreateInfo viewport = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    VkPipelineCreateFlags2CreateInfoKHR flags2 = {
        VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    VkSpecializationMapEntry spec_entry;
    uint32_t spec_value = 12816u;
    VkSpecializationInfo specialization = { 0 };
    VkShaderModuleCreateInfo shader = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    uint32_t *vertex_code = NULL, *fragment_code = NULL;
    size_t vertex_size = 0, fragment_size = 0;
    VkResult result;
    const bool f647 = capture_profile_is(s, "f647");

    if (!load_spirv(s->vertex_path, &vertex_code, &vertex_size) ||
        !load_spirv(s->fragment_path, &fragment_code, &fragment_size)) {
        free(vertex_code);
        free(fragment_code);
        return record_shader_load_failure(s);
    }
    record_shader_code(s, vertex_code, vertex_size, fragment_code, fragment_size);
    shader.codeSize = vertex_size;
    shader.pCode = vertex_code;
    result = record_pipeline_step(s, "captured-shader-module-vertex",
                                  vkCreateShaderModule(s->device, &shader, NULL,
                                                       &s->vertex_shader));
    if (result == VK_SUCCESS) {
        shader.codeSize = fragment_size;
        shader.pCode = fragment_code;
        result = record_pipeline_step(s, "captured-shader-module-fragment",
                                      vkCreateShaderModule(s->device, &shader,
                                                           NULL,
                                                           &s->fragment_shader));
    }
    s->specialization_applied = spirv_has_spec_id(fragment_code, fragment_size,
                                                  1216u);
    free(vertex_code);
    free(fragment_code);
    if (result != VK_SUCCESS) return result;
    if (s->specialization_applied) {
        spec_entry.constantID = 1216u;
        spec_entry.offset = 0;
        spec_entry.size = sizeof(spec_value);
        specialization.mapEntryCount = 1;
        specialization.pMapEntries = &spec_entry;
        specialization.dataSize = sizeof(spec_value);
        specialization.pData = &spec_value;
    }

    memset(attachments, 0, sizeof(attachments));
    attachments[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;
    render_pass.attachmentCount = 2;
    render_pass.pAttachments = attachments;
    render_pass.subpassCount = 1;
    render_pass.pSubpasses = &subpass;
    result = record_pipeline_step(s, "captured-render-pass",
                                  vkCreateRenderPass(s->device, &render_pass,
                                                     NULL, &s->render_pass));
    if (result != VK_SUCCESS) return result;
    framebuffer_views[0] = s->target_view;
    framebuffer_views[1] = s->depth_view;
    framebuffer.renderPass = s->render_pass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = framebuffer_views;
    framebuffer.width = CAPTURE_WIDTH;
    framebuffer.height = CAPTURE_HEIGHT;
    framebuffer.layers = 1;
    result = record_pipeline_step(s, "captured-framebuffer",
                                  vkCreateFramebuffer(s->device, &framebuffer,
                                                      NULL, &s->framebuffer));
    if (result != VK_SUCCESS) return result;
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &s->set_layout;
    result = record_pipeline_step(s, "captured-pipeline-layout",
                                  vkCreatePipelineLayout(s->device,
                                                         &pipeline_layout, NULL,
                                                         &s->pipeline_layout));
    if (result != VK_SUCCESS) return result;

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s->fragment_shader;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = s->specialization_applied ?
        &specialization : NULL;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = vertex_attributes;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.depthClampEnable = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = s->capture_cull_mode &&
        !strcmp(s->capture_cull_mode, "none") ? VK_CULL_MODE_NONE :
        VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = VK_FALSE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    multisample.sampleShadingEnable = VK_FALSE;
    depth_stencil.depthTestEnable = s->capture_depth_mode &&
        !strcmp(s->capture_depth_mode, "disabled") ? VK_FALSE : VK_TRUE;
    depth_stencil.depthWriteEnable = f647 ? VK_FALSE :
        depth_stencil.depthTestEnable;
    depth_stencil.depthCompareOp = s->capture_depth_compare &&
        !strcmp(s->capture_depth_compare, "always") ? VK_COMPARE_OP_ALWAYS :
        s->capture_depth_compare &&
        !strcmp(s->capture_depth_compare, "equal") ? VK_COMPARE_OP_EQUAL :
        VK_COMPARE_OP_LESS_OR_EQUAL;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;
    blend_attachment.blendEnable = f647 ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstColorBlendFactor = f647 ? VK_BLEND_FACTOR_ONE :
        VK_BLEND_FACTOR_ZERO;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = f647 ? VK_BLEND_FACTOR_ONE :
        VK_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    blend.logicOpEnable = VK_FALSE;
    blend.logicOp = VK_LOGIC_OP_COPY;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &input_assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth_stencil;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = s->pipeline_layout;
    pipeline.renderPass = s->render_pass;
    pipeline.subpass = 0;
    flags2.flags = VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR;
    pipeline.pNext = &flags2;
    return record_pipeline_step(s, "captured-graphics-pipeline",
                                vkCreateGraphicsPipelines(s->device,
                                                          VK_NULL_HANDLE, 1,
                                                          &pipeline, NULL,
                                                          &s->pipeline));
}

static void record_input_hash(struct replay_state *s, const char *kind,
                              uint32_t index, uint32_t mip,
                              uint32_t expected, uint32_t actual)
{
    const bool equal = expected == actual;
    ++s->input_checked_count;
    if (!equal)
        ++s->input_mismatch_count;
    fprintf(stderr,
            "[venus-heaven-replay] input-gpu kind=%s index=%u mip=%u"
            " expected=0x%08x actual=0x%08x equal=%u\n",
            kind, index, mip, expected, actual, equal ? 1u : 0u);
}

static VkResult run_buffer_visibility_probes(struct replay_state *s)
{
    static const VkDeviceSize sizes[BUFFER_VISIBILITY_PROBE_COUNT] = {
        64u * 1024u,
        256u * 1024u,
        512u * 1024u,
        1024u * 1024u,
        1536u * 1024u,
        2048u * 1024u,
        3072u * 1024u,
        4096u * 1024u,
        4862400u,
    };
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkBufferMemoryBarrier host_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    VkMappedMemoryRange mapped_range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    VkBufferCopy copy = { 0 };
    uint32_t i;

    copy.size = BUFFER_VISIBILITY_PROBE_BYTES;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s->command;

    for (i = 0; i < BUFFER_VISIBILITY_PROBE_COUNT; ++i) {
        struct replay_buffer source = { 0 };
        struct replay_buffer destination = { 0 };
        void *mapped = NULL;
        VkDeviceMemory mapped_memory = VK_NULL_HANDLE;
        uint32_t expected = 0;
        uint32_t actual = 0;
        VkResult result = create_buffer(
            s, sizes[i], VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &source);
        if (result != VK_SUCCESS)
            goto probe_done;
        result = create_buffer(
            s, BUFFER_VISIBILITY_PROBE_BYTES,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &destination);
        if (result != VK_SUCCESS)
            goto probe_done;

        result = vkMapMemory(s->device, source.memory, 0, source.size, 0, &mapped);
        if (result != VK_SUCCESS)
            goto probe_done;
        mapped_memory = source.memory;
        memset(mapped, 0, (size_t)source.size);
        for (uint32_t byte = 0; byte < BUFFER_VISIBILITY_PROBE_BYTES; ++byte)
            ((uint8_t *)mapped)[byte] = (uint8_t)(0x5au + i * 37u + byte * 13u);
        expected = fnv1a32(mapped, BUFFER_VISIBILITY_PROBE_BYTES);
        result = flush_replay_buffer(s, &source);
        vkUnmapMemory(s->device, source.memory);
        mapped = NULL;
        mapped_memory = VK_NULL_HANDLE;
        if (result != VK_SUCCESS)
            goto probe_done;
        mapped_memory = destination.memory;

        result = vkBeginCommandBuffer(s->command, &begin);
        if (result != VK_SUCCESS)
            goto probe_done;
        vkCmdCopyBuffer(s->command, source.buffer, destination.buffer, 1, &copy);
        ++s->gpu_copies;
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.buffer = destination.buffer;
        host_barrier.offset = 0;
        host_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(s->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL,
                             1, &host_barrier, 0, NULL);
        result = vkEndCommandBuffer(s->command);
        if (result != VK_SUCCESS)
            goto probe_done;
        result = vkQueueSubmit(s->queue, 1, &submit, s->fence);
        if (result != VK_SUCCESS)
            goto probe_done;
        ++s->queue_submits;
        result = vkWaitForFences(s->device, 1, &s->fence, VK_TRUE,
                                 10000000000ull);
        if (result != VK_SUCCESS)
            goto probe_done;

        result = vkMapMemory(s->device, destination.memory, 0,
                             destination.size, 0, &mapped);
        if (result != VK_SUCCESS)
            goto probe_done;
        mapped_range.memory = destination.memory;
        mapped_range.offset = 0;
        mapped_range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(s->device, 1, &mapped_range);
        if (result == VK_SUCCESS)
            actual = fnv1a32(mapped, BUFFER_VISIBILITY_PROBE_BYTES);
        vkUnmapMemory(s->device, destination.memory);
        mapped = NULL;
        mapped_memory = VK_NULL_HANDLE;
        if (result != VK_SUCCESS)
            goto probe_done;

        if (actual == expected)
            ++s->buffer_probe_pass_count;
        else
            ++s->buffer_probe_fail_count;
        fprintf(stderr,
                "[venus-heaven-replay] buffer-visibility allocation=%" PRIu64
                " copyBytes=%u expected=0x%08x actual=0x%08x equal=%u\n",
                (uint64_t)sizes[i], BUFFER_VISIBILITY_PROBE_BYTES,
                expected, actual, actual == expected ? 1u : 0u);

        result = vkResetFences(s->device, 1, &s->fence);
        if (result == VK_SUCCESS)
            result = vkResetCommandBuffer(s->command, 0);

probe_done:
        if (mapped)
            vkUnmapMemory(s->device, mapped_memory);
        destroy_buffer(s, &destination);
        destroy_buffer(s, &source);
        if (result != VK_SUCCESS)
            return result;
    }
    fflush(stderr);
    return VK_SUCCESS;
}

static VkResult verify_captured_inputs(struct replay_state *s)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkBufferMemoryBarrier host_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    VkMappedMemoryRange mapped_range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    VkBufferImageCopy image_copy;
    VkBufferCopy buffer_copy;
    void *mapped = NULL;
    VkResult result;
    uint32_t i, mip;

    result = vkBeginCommandBuffer(s->command, &begin);
    if (result != VK_SUCCESS)
        return result;

    for (i = 0; i < s->capture_image_count; ++i) {
        const VkImageAspectFlags aspect =
            capture_image_aspect(&s->capture_images[i]);
        const VkImageAspectFlags barrier_aspect =
            capture_image_barrier_aspect(&s->capture_images[i]);
        const VkImageLayout sampled_layout =
            capture_sampled_layout(&s->capture_images[i]);
        if (capture_alias_index(s, i) >= 0)
            continue;
        image_barrier_range(s->command, s->images[i].image,
                            barrier_aspect,
                            s->images[i].mip_levels,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (mip = 0; mip < s->images[i].mip_levels; ++mip) {
            uint32_t width = s->images[i].width >> mip;
            uint32_t height = s->images[i].height >> mip;
            if (!width) width = 1;
            if (!height) height = 1;
            memset(&image_copy, 0, sizeof(image_copy));
            image_copy.bufferOffset = s->images[i].upload_offsets[mip];
            image_copy.imageSubresource.aspectMask = aspect;
            image_copy.imageSubresource.mipLevel = mip;
            image_copy.imageSubresource.layerCount = 1;
            image_copy.imageExtent.width = width;
            image_copy.imageExtent.height = height;
            image_copy.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(s->command, s->upload.buffer,
                                   s->images[i].image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &image_copy);
            ++s->gpu_copies;
        }
        image_barrier_range(s->command, s->images[i].image,
                            barrier_aspect,
                            s->images[i].mip_levels,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (mip = 0; mip < s->images[i].mip_levels; ++mip) {
            uint32_t width = s->images[i].width >> mip;
            uint32_t height = s->images[i].height >> mip;
            if (!width) width = 1;
            if (!height) height = 1;
            memset(&image_copy, 0, sizeof(image_copy));
            image_copy.bufferOffset = s->images[i].upload_offsets[mip];
            image_copy.imageSubresource.aspectMask = aspect;
            image_copy.imageSubresource.mipLevel = mip;
            image_copy.imageSubresource.layerCount = 1;
            image_copy.imageExtent.width = width;
            image_copy.imageExtent.height = height;
            image_copy.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(s->command, s->images[i].image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   s->input_readback.buffer, 1, &image_copy);
            ++s->gpu_copies;
        }
        image_barrier_range(s->command, s->images[i].image,
                            barrier_aspect,
                            s->images[i].mip_levels,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            sampled_layout,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    memset(&buffer_copy, 0, sizeof(buffer_copy));
    buffer_copy.dstOffset = s->input_vertex_offset;
    buffer_copy.size = s->capture_vertex_bytes;
    vkCmdCopyBuffer(s->command, s->vertex.buffer, s->input_readback.buffer,
                    1, &buffer_copy);
    ++s->gpu_copies;
    buffer_copy.dstOffset = s->input_index_offset;
    buffer_copy.size = s->capture_index_bytes;
    vkCmdCopyBuffer(s->command, s->index.buffer, s->input_readback.buffer,
                    1, &buffer_copy);
    ++s->gpu_copies;
    for (i = 0; i < s->capture_ubo_count; ++i) {
        buffer_copy.dstOffset = s->input_ubo_offsets[i];
        buffer_copy.size = s->capture_ubo_sizes[i];
        vkCmdCopyBuffer(s->command, s->ubos[i].buffer,
                        s->input_readback.buffer, 1, &buffer_copy);
        ++s->gpu_copies;
    }
    buffer_copy.srcOffset = 0;
    buffer_copy.dstOffset = s->input_upload_offset;
    buffer_copy.size = s->target_upload_offset;
    vkCmdCopyBuffer(s->command, s->upload.buffer, s->input_readback.buffer,
                    1, &buffer_copy);
    ++s->gpu_copies;

    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.buffer = s->input_readback.buffer;
    host_barrier.offset = 0;
    host_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(s->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL,
                         1, &host_barrier, 0, NULL);

    result = vkEndCommandBuffer(s->command);
    if (result != VK_SUCCESS)
        return result;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s->command;
    result = vkQueueSubmit(s->queue, 1, &submit, s->fence);
    if (result != VK_SUCCESS)
        return result;
    ++s->queue_submits;
    result = vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 10000000000ull);
    if (result != VK_SUCCESS)
        return result;

    result = vkMapMemory(s->device, s->input_readback.memory, 0,
                         s->input_readback.size, 0, &mapped);
    if (result != VK_SUCCESS)
        return result;
    mapped_range.memory = s->input_readback.memory;
    mapped_range.offset = 0;
    mapped_range.size = VK_WHOLE_SIZE;
    result = vkInvalidateMappedMemoryRanges(s->device, 1, &mapped_range);
    if (result != VK_SUCCESS) {
        vkUnmapMemory(s->device, s->input_readback.memory);
        return result;
    }

    s->input_verification_performed = true;
    for (i = 0; i < s->capture_image_count; ++i) {
        const uint32_t bpp = format_bytes_per_texel(s->images[i].format);
        if (capture_alias_index(s, i) >= 0)
            continue;
        for (mip = 0; mip < s->images[i].mip_levels; ++mip) {
            uint32_t width = s->images[i].width >> mip;
            uint32_t height = s->images[i].height >> mip;
            size_t bytes;
            if (!width) width = 1;
            if (!height) height = 1;
            bytes = (size_t)width * height * bpp;
            s->input_image_actual[i][mip] = fnv1a32(
                (uint8_t *)mapped + s->images[i].upload_offsets[mip], bytes);
            record_input_hash(s, "image", s->capture_images[i].binding, mip,
                              s->input_image_expected[i][mip],
                              s->input_image_actual[i][mip]);
        }
    }
    s->input_vertex_actual = fnv1a32(
        (uint8_t *)mapped + s->input_vertex_offset, s->capture_vertex_bytes);
    record_input_hash(s, "vertex", 0, 0, s->input_vertex_expected,
                      s->input_vertex_actual);
    s->input_index_actual = fnv1a32(
        (uint8_t *)mapped + s->input_index_offset, s->capture_index_bytes);
    record_input_hash(s, "index", 0, 0, s->input_index_expected,
                      s->input_index_actual);
    for (i = 0; i < s->capture_ubo_count; ++i) {
        s->input_ubo_actual[i] = fnv1a32(
            (uint8_t *)mapped + s->input_ubo_offsets[i], s->capture_ubo_sizes[i]);
        record_input_hash(s, "ubo", i, 0, s->input_ubo_expected[i],
                          s->input_ubo_actual[i]);
    }
    s->input_upload_actual = fnv1a32(
        (uint8_t *)mapped + s->input_upload_offset,
        (size_t)s->target_upload_offset);
    record_input_hash(s, "upload-buffer", 0, 0, s->input_upload_expected,
                      s->input_upload_actual);
    vkUnmapMemory(s->device, s->input_readback.memory);
    fflush(stderr);

    s->inputs_uploaded = true;
    result = vkResetFences(s->device, 1, &s->fence);
    if (result != VK_SUCCESS)
        return result;
    result = vkResetCommandBuffer(s->command, 0);
    if (result != VK_SUCCESS)
        return result;
    if (s->input_mismatch_count) {
        result = run_buffer_visibility_probes(s);
        return result == VK_SUCCESS ? VK_ERROR_UNKNOWN : result;
    }
    return VK_SUCCESS;
}

static VkResult render_captured_and_readback(struct replay_state *s)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkRenderPassBeginInfo render = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport viewport = {
        0.0f, (float)CAPTURE_HEIGHT, (float)CAPTURE_WIDTH,
        -(float)CAPTURE_HEIGHT, 0.0f, 1.0f
    };
    VkRect2D scissor = { { 0, 0 }, { 32767u, 32767u } };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkBufferImageCopy copy;
    VkClearColorValue sentinel = { { 1.0f, 0.0f, 1.0f, 1.0f } };
    VkResult result;
    uint32_t i, mip;

    result = vkBeginCommandBuffer(s->command, &begin);
    if (result != VK_SUCCESS) return result;
    if (!s->inputs_uploaded) {
      for (i = 0; i < s->capture_image_count; ++i) {
        const VkImageAspectFlags aspect =
            capture_image_aspect(&s->capture_images[i]);
        const VkImageAspectFlags barrier_aspect =
            capture_image_barrier_aspect(&s->capture_images[i]);
        const VkImageLayout sampled_layout =
            capture_sampled_layout(&s->capture_images[i]);
        if (capture_alias_index(s, i) >= 0)
            continue;
        image_barrier_range(s->command, s->images[i].image,
                            barrier_aspect,
                            s->images[i].mip_levels,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (mip = 0; mip < s->images[i].mip_levels; ++mip) {
            uint32_t width = s->images[i].width >> mip;
            uint32_t height = s->images[i].height >> mip;
            if (!width) width = 1;
            if (!height) height = 1;
            memset(&copy, 0, sizeof(copy));
            copy.bufferOffset = s->images[i].upload_offsets[mip];
            copy.imageSubresource.aspectMask = aspect;
            copy.imageSubresource.mipLevel = mip;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent.width = width;
            copy.imageExtent.height = height;
            copy.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(s->command, s->upload.buffer,
                                   s->images[i].image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &copy);
            ++s->gpu_copies;
        }
        image_barrier_range(s->command, s->images[i].image,
                            barrier_aspect,
                            s->images[i].mip_levels,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            sampled_layout,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      }
    }

    image_barrier_range(s->command, s->target, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                        VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (s->capture_color_mode && !strcmp(s->capture_color_mode, "golden")) {
        memset(&copy, 0, sizeof(copy));
        copy.bufferOffset = s->target_upload_offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = CAPTURE_WIDTH;
        copy.imageExtent.height = CAPTURE_HEIGHT;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s->command, s->upload.buffer, s->target,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ++s->gpu_copies;
    } else {
        VkImageSubresourceRange range = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u
        };
        vkCmdClearColorImage(s->command, s->target,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &sentinel, 1, &range);
    }
    image_barrier_range(s->command, s->target, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    if (s->capture_depth_mode && !strcmp(s->capture_depth_mode, "loaded")) {
        /* The dump is a post-pass depth snapshot, so this mode is retained as
         * a state/lifetime diagnostic rather than the visible A/B baseline. */
        image_barrier_range(s->command, s->depth, VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&copy, 0, sizeof(copy));
        copy.bufferOffset = s->depth_upload_offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = CAPTURE_WIDTH;
        copy.imageExtent.height = CAPTURE_HEIGHT;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s->command, s->upload.buffer, s->depth,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ++s->gpu_copies;
        image_barrier_range(s->command, s->depth, VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        image_barrier_range(s->command, s->depth, VK_IMAGE_ASPECT_STENCIL_BIT, 1,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    } else {
        VkClearDepthStencilValue clear_depth = { 1.0f, 0u };
        VkImageSubresourceRange range = {
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            0u, 1u, 0u, 1u
        };
        image_barrier_range(s->command, s->depth,
                            VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_STENCIL_BIT, 1,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            0, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearDepthStencilImage(s->command, s->depth,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &clear_depth, 1, &range);
        image_barrier_range(s->command, s->depth,
                            VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_STENCIL_BIT, 1,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    }

    render.renderPass = s->render_pass;
    render.framebuffer = s->framebuffer;
    render.renderArea.extent.width = CAPTURE_WIDTH;
    render.renderArea.extent.height = CAPTURE_HEIGHT;
    render.clearValueCount = 0;
    render.pClearValues = NULL;
    vkCmdBeginRenderPass(s->command, &render, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->command, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
    {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(s->command, 0, 1, &s->vertex.buffer, &offset);
    }
    vkCmdBindIndexBuffer(s->command, s->index.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(s->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            s->pipeline_layout, 0, 1, &s->descriptor_set,
                            0, NULL);
    vkCmdSetViewport(s->command, 0, 1, &viewport);
    vkCmdSetScissor(s->command, 0, 1, &scissor);
    if (!s->skip_draw)
        vkCmdDrawIndexed(s->command, s->capture_index_count,
                         s->capture_instance_count, s->capture_first_index,
                         s->capture_vertex_offset, s->capture_first_instance);
    vkCmdEndRenderPass(s->command);
    image_barrier_range(s->command, s->target, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);

    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = CAPTURE_WIDTH;
    copy.imageExtent.height = CAPTURE_HEIGHT;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s->command, s->target,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s->readback.buffer, 1, &copy);
    ++s->gpu_copies;
    result = vkEndCommandBuffer(s->command);
    if (result != VK_SUCCESS) return result;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &s->command;
    result = vkQueueSubmit(s->queue, 1, &submit, s->fence);
    if (result != VK_SUCCESS) return result;
    ++s->queue_submits;
    return vkWaitForFences(s->device, 1, &s->fence, VK_TRUE, 10000000000ull);
}

static int analyze_captured_output(struct replay_state *s)
{
    VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    void *mapped = NULL;
    uint8_t *baseline = NULL;
    char path[1024];
    FILE *output;
    uint32_t i;
    static const uint8_t sentinel[8] = {
        0x00u, 0x3cu, 0x00u, 0x00u, 0x00u, 0x3cu, 0x00u, 0x3cu,
    };
    static const uint8_t zero[8] = { 0 };
    bool compare_golden = s->capture_color_mode &&
        !strcmp(s->capture_color_mode, "golden");
    VkResult result = vkMapMemory(s->device, s->readback.memory, 0,
                                  s->readback.size, 0, &mapped);
    if (result != VK_SUCCESS) return 0;
    range.memory = s->readback.memory;
    range.size = VK_WHOLE_SIZE;
    result = vkInvalidateMappedMemoryRanges(s->device, 1, &range);
    if (result != VK_SUCCESS) {
        vkUnmapMemory(s->device, s->readback.memory);
        return 0;
    }
    if (compare_golden) {
        baseline = malloc(CAPTURE_OUTPUT_BYTES);
        if (!baseline ||
            !capture_path(s, "frame-180-pass-2-color-0.bin", path, sizeof(path)) ||
            !read_exact_file(path, baseline, CAPTURE_OUTPUT_BYTES)) {
            free(baseline);
            vkUnmapMemory(s->device, s->readback.memory);
            return 0;
        }
    }
    s->checksum = fnv1a32(mapped, CAPTURE_OUTPUT_BYTES);
    s->changed_pixels = 0;
    s->nonzero_drawn_pixels = 0;
    s->opaque_pixels = CAPTURE_WIDTH * CAPTURE_HEIGHT;
    for (i = 0; i < CAPTURE_WIDTH * CAPTURE_HEIGHT; ++i) {
        const uint8_t *pixel = (const uint8_t *)mapped + i * 8u;
        const uint8_t *reference = compare_golden ? baseline + i * 8u : sentinel;
        if (memcmp(pixel, reference, 8u)) {
            ++s->changed_pixels;
            if (memcmp(pixel, zero, sizeof(zero)))
                ++s->nonzero_drawn_pixels;
        }
    }
    for (i = 0; i < REPLAY_SAMPLE_COUNT; ++i) {
        uint32_t x = (i % 3u) * (CAPTURE_WIDTH / 3u) + CAPTURE_WIDTH / 6u;
        uint32_t y = (i / 3u) * (CAPTURE_HEIGHT / 3u) + CAPTURE_HEIGHT / 6u;
        const uint8_t *pixel = (const uint8_t *)mapped +
            ((VkDeviceSize)y * CAPTURE_WIDTH + x) * 8u;
        s->sample_values[i] = (uint32_t)pixel[0] |
            ((uint32_t)pixel[1] << 8) | ((uint32_t)pixel[2] << 16) |
            ((uint32_t)pixel[3] << 24);
    }
    if (s->rgba_path && s->rgba_path[0]) {
        output = fopen(s->rgba_path, "wb");
        if (output) {
            fwrite(mapped, 1, CAPTURE_OUTPUT_BYTES, output);
            fflush(output);
            fsync(fileno(output));
            fclose(output);
        }
    }
    fprintf(stderr,
            "[venus-heaven-replay] captured output checksum=0x%08x baseline=0x%08x colorMode=%s changedPixels=%u nonzeroDrawnPixels=%u\n",
            s->checksum, s->baseline_checksum,
            compare_golden ? "golden" : "sentinel", s->changed_pixels,
            s->nonzero_drawn_pixels);
    free(baseline);
    vkUnmapMemory(s->device, s->readback.memory);
    return 1;
}

static void destroy_buffer(struct replay_state *s, struct replay_buffer *buffer)
{
    if (buffer->buffer) vkDestroyBuffer(s->device, buffer->buffer, NULL);
    if (buffer->memory) vkFreeMemory(s->device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

static void cleanup(struct replay_state *s)
{
    uint32_t i;
    /* A synchronous pipeline diagnostic can return an error after Venus has
     * already marked the ring fatal.  Waiting for device idle on that context
     * re-enters the broken ring and causes a secondary Box64 crash, hiding the
     * authoritative VkResult. */
    /* The captured replay has already waited on its submit fence. A second
     * device-wide wait crosses the Venus ring again during teardown and can
     * turn a completed diagnostic into a Box64 SIGSEGV. Object destruction
     * after a signaled fence is sufficient here. */
    if (s->device && s->failure_result == VK_SUCCESS && !s->captured_mode)
        vkDeviceWaitIdle(s->device);
    if (s->pipeline) vkDestroyPipeline(s->device, s->pipeline, NULL);
    if (s->fragment_shader) vkDestroyShaderModule(s->device, s->fragment_shader, NULL);
    if (s->vertex_shader) vkDestroyShaderModule(s->device, s->vertex_shader, NULL);
    if (s->framebuffer) vkDestroyFramebuffer(s->device, s->framebuffer, NULL);
    if (s->render_pass) vkDestroyRenderPass(s->device, s->render_pass, NULL);
    if (s->pipeline_layout) vkDestroyPipelineLayout(s->device, s->pipeline_layout, NULL);
    if (s->descriptor_pool) vkDestroyDescriptorPool(s->device, s->descriptor_pool, NULL);
    if (s->set_layout) vkDestroyDescriptorSetLayout(s->device, s->set_layout, NULL);
    destroy_buffer(s, &s->input_readback);
    destroy_buffer(s, &s->readback);
    if (s->depth_view) vkDestroyImageView(s->device, s->depth_view, NULL);
    if (s->depth) vkDestroyImage(s->device, s->depth, NULL);
    if (s->depth_memory) vkFreeMemory(s->device, s->depth_memory, NULL);
    if (s->target_view) vkDestroyImageView(s->device, s->target_view, NULL);
    if (s->target) vkDestroyImage(s->device, s->target, NULL);
    if (s->target_memory) vkFreeMemory(s->device, s->target_memory, NULL);
    for (i = 0; i < REPLAY_IMAGE_CAPACITY; ++i) {
        if (s->images[i].sampler)
            vkDestroySampler(s->device, s->images[i].sampler, NULL);
        if (s->images[i].owns_image && s->images[i].view)
            vkDestroyImageView(s->device, s->images[i].view, NULL);
        if (s->images[i].owns_image && s->images[i].image)
            vkDestroyImage(s->device, s->images[i].image, NULL);
        if (s->images[i].owns_image && s->images[i].memory)
            vkFreeMemory(s->device, s->images[i].memory, NULL);
    }
    for (i = 0; i < REPLAY_UBO_COUNT; ++i)
        destroy_buffer(s, &s->ubos[i]);
    destroy_buffer(s, &s->upload);
    destroy_buffer(s, &s->vertex);
    destroy_buffer(s, &s->index);
    if (s->fence) vkDestroyFence(s->device, s->fence, NULL);
    if (s->command_pool) vkDestroyCommandPool(s->device, s->command_pool, NULL);
    if (s->device) vkDestroyDevice(s->device, NULL);
    if (s->instance) vkDestroyInstance(s->instance, NULL);
}

#ifdef WINEHUA_HOST_DIRECT_REPLAY
__attribute__((visibility("default")))
int winehua_host_replay_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    struct replay_state state;
    VkResult result;
    int passed = 0;
    memset(&state, 0, sizeof(state));
    state.run_id = argument_value(argc, argv, "--run-id", "manual");
    state.test_id = argument_value(argc, argv, "--test-id",
                                   "venus-heaven-material-graphics-x64");
    state.result_path = argument_value(argc, argv, "--result", "");
    state.rgba_path = argument_value(argc, argv, "--rgba-output", "");
    state.vertex_path = argument_value(argc, argv, "--vertex-spv",
                                       "share/winehua/venus_heaven_material.vert.spv");
    state.fragment_path = argument_value(argc, argv, "--fragment-spv",
                                         "share/winehua/replay_external/heaven_final_fs.spv");
    state.requires_terminate_invocation =
        strstr(state.fragment_path, "terminate_invocation") != NULL;
    state.fragment_sha256 = "unknown";
    if (strstr(state.fragment_path, "heaven_final_fs.spv")) {
        state.fragment_sha256 =
            "ee6dbab51709fe8e057c01b7e8cbc92b14f29bbe370ccad39f70640ae2fdd05f";
    }
    state.layout_mode = argument_value(argc, argv, "--layout-mode", "full");
    state.bool_mode = argument_value(argc, argv, "--bool-mode", "default");
    state.capture_dir = argument_value(argc, argv, "--capture-dir", "");
    state.capture_color_mode = argument_value(argc, argv, "--capture-color",
                                              "sentinel");
    state.capture_depth_mode = argument_value(argc, argv, "--capture-depth",
                                              "loaded");
    state.capture_depth_compare = argument_value(argc, argv,
                                                 "--capture-depth-compare",
                                                 "less-equal");
    state.capture_cull_mode = argument_value(argc, argv, "--capture-cull",
                                             "back");
    state.capture_profile = argument_value(argc, argv, "--capture-profile",
                                           "legacy");
    if (!configure_capture_profile(&state))
        state.capture_profile = "legacy";
    state.captured_mode = !strcmp(state.layout_mode, "captured");
    state.verify_inputs = !strcmp(argument_value(argc, argv,
                                                  "--verify-inputs", "0"),
                                  "1");
    state.skip_draw = !strcmp(argument_value(argc, argv, "--skip-draw", "0"),
                              "1");
    if (!state.captured_mode)
        state.verify_inputs = false;
    if (!state.captured_mode)
        state.skip_draw = false;
    state.vs_transform_probe = !strcmp(argument_value(argc, argv,
                                                       "--vs-transform-probe",
                                                       "0"), "1");
    if (strcmp(state.bool_mode, "default") && strcmp(state.bool_mode, "true") &&
        strcmp(state.bool_mode, "false"))
        state.bool_mode = "default";
    if (strcmp(state.capture_depth_compare, "less-equal") &&
        strcmp(state.capture_depth_compare, "equal") &&
        strcmp(state.capture_depth_compare, "always"))
        state.capture_depth_compare = "less-equal";
    if (strcmp(state.layout_mode, "full") && strcmp(state.layout_mode, "empty") &&
        strcmp(state.layout_mode, "no-set") && strcmp(state.layout_mode, "small") &&
        strcmp(state.layout_mode, "ubo") && strcmp(state.layout_mode, "dynamic") &&
        strcmp(state.layout_mode, "sampler") && strcmp(state.layout_mode, "sampled") &&
        strcmp(state.layout_mode, "images") && strcmp(state.layout_mode, "exact") &&
        strcmp(state.layout_mode, "captured")) {
        state.layout_mode = "full";
        state.captured_mode = false;
    }
    state.started_ms = now_ms();
    record_failure(&state, "startup", "replay started", VK_SUCCESS);
    write_result(&state, "started");
    result = init_vulkan(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "host-vulkan", "Vulkan initialization failed", result);
        goto done;
    }
    result = state.captured_mode ? create_captured_resources(&state) :
        create_resources(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "resources", "deterministic replay resource creation failed", result);
        goto done;
    }
    result = state.captured_mode ? create_captured_descriptors(&state) :
        create_descriptors(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "descriptor", "exact DXVK descriptor contract creation failed", result);
        goto done;
    }
    result = state.captured_mode ? create_captured_pipeline(&state) :
        create_pipeline(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state,
                       state.pipeline_stage[0] ? state.pipeline_stage : "pipeline",
                       "captured Heaven material pipeline setup failed", result);
        goto done;
    }
    if (state.captured_mode && state.verify_inputs) {
        result = verify_captured_inputs(&state);
        if (result != VK_SUCCESS) {
            record_failure(&state, "input-verification",
                           state.input_mismatch_count ?
                           "GPU-visible replay input differs from captured input" :
                           "GPU input verification command failed",
                           result);
            goto done;
        }
    }
    result = state.captured_mode ? render_captured_and_readback(&state) :
        render_and_readback(&state);
    if (result != VK_SUCCESS) {
        record_failure(&state, "draw", "captured Heaven fragment draw or wait failed", result);
        goto done;
    }
    if (!(state.captured_mode ? analyze_captured_output(&state) :
          analyze_output(&state))) {
        record_failure(&state, "readback", "offscreen replay readback failed", VK_ERROR_MEMORY_MAP_FAILED);
        goto done;
    }
    if (!strcmp(state.layout_mode, "exact") && !state.opaque_pixels) {
        record_failure(&state, "graphics-output",
                       "exact replay produced no opaque pixels", VK_SUCCESS);
        goto done;
    }
    if (state.captured_mode && state.skip_draw) {
        if (state.checksum != state.baseline_checksum) {
            record_failure(&state, "baseline-copy",
                           "RGBA16F target upload/readback differs without a draw",
                           VK_SUCCESS);
            goto done;
        }
        record_failure(&state, "baseline-copy",
                       "RGBA16F target upload/readback is bit-exact without a draw",
                       VK_SUCCESS);
        passed = 1;
        goto done;
    }
    if (!state.changed_pixels) {
        record_failure(&state, "graphics-replay",
                       state.captured_mode ?
                       "captured indexed draw left the input color attachment unchanged" :
                       "fragment draw left the sentinel clear color unchanged",
                       VK_SUCCESS);
        goto done;
    }
    if (state.captured_mode && !state.nonzero_drawn_pixels) {
        record_failure(&state, "graphics-output",
                       "captured draw only wrote zero-valued RGBA16F pixels",
                       VK_SUCCESS);
        goto done;
    }
    record_failure(&state, "graphics-replay",
                   state.captured_mode ?
                   "exact captured Heaven draw executed; compare raw RGBA16F across drivers" :
                   "captured Heaven fragment executed; compare RGBA against reference",
                   VK_SUCCESS);
    passed = 1;
done:
    write_result(&state, passed ? "PASS" : "FAIL");
    cleanup(&state);
    return passed ? 0 : 1;
}
