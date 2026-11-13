/* Standalone BC1/BC3 -> ASTC 4x4 feasibility benchmark, not a runtime patch.
 * Link an externally supplied Arm astcenc 5.2.0 library and Wine's MIT bcdec.h.
 * Bounded input: <=4 fixtures, each <=512x512 and divisible by 4. Metrics are
 * additional error relative to BC-decoded content, not original source artwork.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "astcenc.h"
#define BCDEC_STATIC
#define WINE_UNUSED 1
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

static FILE *out;
static HANDLE finished;
static DWORD WINAPI watchdog(void *) {
  if (WaitForSingleObject(finished, 180000) == WAIT_TIMEOUT) {
    std::fprintf(out, "benchmark_complete=0 error=180_second_deadline\n");
    std::fflush(out); ExitProcess(12);
  }
  return 0;
}

static double now_ms() {
  LARGE_INTEGER count, freq;
  QueryPerformanceCounter(&count); QueryPerformanceFrequency(&freq);
  return double(count.QuadPart) * 1000.0 / double(freq.QuadPart);
}

static int check(astcenc_error error, const char *operation) {
  if (error == ASTCENC_SUCCESS) return 0;
  std::fprintf(out, "codec_error operation=%s code=%d\n", operation, int(error));
  return 1;
}

static double psnr(double mse) { return mse ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0; }

static int run(const char *name, uint32_t kind, uint32_t width, uint32_t height,
               const std::vector<uint8_t>& bc) {
  std::vector<uint8_t> rgba(size_t(width) * height * 4), decoded(rgba.size());
  std::vector<uint8_t> astc(size_t(width) * height);
  double start = now_ms();
  for (uint32_t by = 0; by < height / 4; ++by) for (uint32_t bx = 0; bx < width / 4; ++bx) {
    const uint8_t *src = bc.data() + (size_t(by) * (width / 4) + bx) * (kind == 1 ? 8 : 16);
    uint8_t *dst = rgba.data() + size_t(by * 4) * width * 4 + bx * 16;
    if (kind == 1) bcdec_bc1(src, dst, int(width * 4));
    else bcdec_bc3(src, dst, int(width * 4));
  }
  std::fprintf(out, "fixture=%s format=BC%u width=%u height=%u bc_bytes=%zu rgba_bytes=%zu astc_bytes=%zu bc_decode_ms=%.3f\n",
    name, kind, width, height, bc.size(), rgba.size(), astc.size(), now_ms() - start);
  void *input_slice = rgba.data(), *output_slice = decoded.data();
  astcenc_image image{width, height, 1, ASTCENC_TYPE_U8, &input_slice};
  astcenc_image output{width, height, 1, ASTCENC_TYPE_U8, &output_slice};
  const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  for (int srgb = 0; srgb < 2; ++srgb) for (float quality : {ASTCENC_PRE_FASTEST, ASTCENC_PRE_FAST, ASTCENC_PRE_MEDIUM}) {
    astcenc_config config;
    if (check(astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR,
       4, 4, 1, quality, ASTCENC_FLG_USE_DECODE_UNORM8, &config), "config")) return 1;
    astcenc_context *context = nullptr;
    start = now_ms();
    if (check(astcenc_context_alloc(&config, 1, &context), "context")) return 1;
    double context_ms = now_ms() - start;
    start = now_ms();
    int failure = check(astcenc_compress_image(context, &image, &swizzle, astc.data(), astc.size(), 0), "compress");
    double encode_ms = now_ms() - start;
    start = now_ms();
    if (!failure) failure = check(astcenc_decompress_image(context, astc.data(), astc.size(), &output, &swizzle, 0), "decompress");
    double decode_ms = now_ms() - start;
    astcenc_context_free(context);
    if (failure) return 1;
    double rgb_error = 0, alpha_error = 0, premul_error = 0;
    unsigned max_rgb_error = 0, max_alpha_error = 0;
    size_t alpha_over_8 = 0, edge_pixels = 0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
      int ae = std::abs(int(rgba[i + 3]) - int(decoded[i + 3]));
      alpha_error += ae * ae;
      max_alpha_error = std::max(max_alpha_error, unsigned(ae));
      alpha_over_8 += ae > 8;
      edge_pixels += rgba[i + 3] > 0 && rgba[i + 3] < 255;
      for (int c = 0; c < 3; ++c) {
        int ce = std::abs(int(rgba[i + c]) - int(decoded[i + c]));
        rgb_error += ce * ce;
        max_rgb_error = std::max(max_rgb_error, unsigned(ce));
        double pe = rgba[i + c] * (rgba[i + 3] / 255.0) - decoded[i + c] * (decoded[i + 3] / 255.0);
        premul_error += pe * pe;
      }
    }
    double pixels = double(width) * height;
    std::fprintf(out, "result srgb=%d quality=%.0f threads=1 context_ms=%.3f encode_ms=%.3f decode_ms=%.3f megapixels_per_second=%.4f rgb_psnr_db=%.3f premul_rgb_psnr_db=%.3f alpha_psnr_db=%.3f max_rgb_error=%u max_alpha_error=%u alpha_error_over_8=%zu partial_alpha_pixels=%zu\n",
      srgb, quality, context_ms, encode_ms, decode_ms, pixels / (encode_ms * 1000.0),
      psnr(rgb_error / (pixels * 3)), psnr(premul_error / (pixels * 3)), psnr(alpha_error / pixels),
      max_rgb_error, max_alpha_error, alpha_over_8, edge_pixels);
  }
  return 0;
}

int main(int argc, char **argv) {
  out = std::fopen(argc > 2 ? argv[2] : "C:\\windows\\temp\\vp-bc-astc-benchmark.tsv", "w");
  if (!out) return 2;
  std::setvbuf(out, nullptr, _IONBF, 0);
  std::fprintf(out, "bc_astc_benchmark=1 encoder=astcenc_5.2.0 isa=sse2 execution=wine_PE64 watchdog_seconds=180\n");
  FILE *input = std::fopen(argc > 1 ? argv[1] : "C:\\windows\\temp\\vp-bc-fixtures.bin", "rb");
  uint32_t header[3];
  if (!input || std::fread(header, sizeof(header), 1, input) != 1 ||
      header[0] != 0x43425056 || header[1] != 1 || header[2] < 1 || header[2] > 4) {
    std::fprintf(out, "benchmark_complete=0 error=invalid_fixture_header\n");
    if (input) std::fclose(input);
    std::fclose(out); return 3;
  }
  finished = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE thread = finished ? CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr) : nullptr;
  if (!thread) { std::fclose(input); std::fclose(out); return 4; }
  int failures = 0;
  for (uint32_t i = 0; i < header[2]; ++i) {
    char name[32]; uint32_t info[4];
    if (std::fread(name, sizeof(name), 1, input) != 1 || std::fread(info, sizeof(info), 1, input) != 1) { ++failures; break; }
    name[sizeof(name) - 1] = 0;
    uint32_t kind = info[0], width = info[1], height = info[2], bytes = info[3];
    if ((kind != 1 && kind != 3) || !width || !height || width > 512 || height > 512 ||
        width % 4 || height % 4 || bytes != (width / 4) * (height / 4) * (kind == 1 ? 8 : 16)) { ++failures; break; }
    std::vector<uint8_t> data(bytes);
    if (std::fread(data.data(), bytes, 1, input) != 1) { ++failures; break; }
    failures += run(name, kind, width, height, data);
  }
  std::fclose(input);
  SetEvent(finished); WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread); CloseHandle(finished);
  std::fprintf(out, "benchmark_complete=1 codec_failures=%d quality_qualification=pending\n", failures);
  std::fclose(out); return failures ? 1 : 0;
}
