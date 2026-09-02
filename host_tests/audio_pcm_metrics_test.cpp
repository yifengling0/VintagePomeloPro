#include "audio/audio_pcm_metrics.h"
#include "audio/audio_diag_config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using winehua::AbsI32;
using winehua::AnalyzeStereoS16;
using winehua::ApplyGainQ15;
using winehua::ApplyStereoGainQ15;
using winehua::AudioDiagGainProfile;
using winehua::AudioDiagGainProfileName;
using winehua::GainQ15ForOrdinal;
using winehua::IsFullScaleS16;
using winehua::kAudioDiagGainProfile;
using winehua::kHalfGainQ15;
using winehua::kUnityGainQ15;
using winehua::PcmBlockMetrics;
using winehua::PcmContinuityState;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

static std::vector<int16_t> StereoFrames(uint32_t frames, int16_t left, int16_t right)
{
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i)
    {
        pcm[i * 2] = left;
        pcm[i * 2 + 1] = right;
    }
    return pcm;
}

int main()
{
    CHECK(kAudioDiagGainProfile == AudioDiagGainProfile::G00, "default diagnostic profile is G00");
    CHECK(std::strcmp(AudioDiagGainProfileName(AudioDiagGainProfile::G00), "G00") == 0,
          "G00 name");

    {
        auto pcm = StereoFrames(16, 10000, -10000);
        PcmContinuityState continuity;
        PcmBlockMetrics metrics = AnalyzeStereoS16(pcm.data(), 16, continuity, 1);
        CHECK(metrics.maxDeltaL == 0, "opposite L/R must not create L time delta");
        CHECK(metrics.maxDeltaR == 0, "opposite L/R must not create R time delta");
        CHECK(metrics.peakL == 10000, "opposite peak L");
        CHECK(metrics.peakR == 10000, "opposite peak R");
        CHECK(metrics.frames == 16, "stereo frame count is not sample count");
    }

    {
        std::vector<int16_t> pcm(12 * 2);
        for (uint32_t i = 0; i < 12; ++i)
        {
            pcm[i * 2] = static_cast<int16_t>(i * 100);
            pcm[i * 2 + 1] = static_cast<int16_t>(i * 80);
        }
        PcmContinuityState continuity;
        PcmBlockMetrics metrics = AnalyzeStereoS16(pcm.data(), 12, continuity, 1);
        CHECK(metrics.maxDeltaL == 100, "continuous L delta");
        CHECK(metrics.maxDeltaR == 80, "continuous R delta");
        CHECK(metrics.boundaryDeltaL == 0, "first block has no boundary");
        CHECK(metrics.lastL == 1100, "last L");
        CHECK(metrics.lastR == 880, "last R");
    }

    {
        auto first = StereoFrames(4, 0, 0);
        auto second = StereoFrames(4, 20000, -18000);
        PcmContinuityState continuity;
        AnalyzeStereoS16(first.data(), 4, continuity, 1);
        PcmBlockMetrics metrics = AnalyzeStereoS16(second.data(), 4, continuity, 1);
        CHECK(metrics.boundaryDeltaL == 20000, "callback boundary L");
        CHECK(metrics.boundaryDeltaR == 18000, "callback boundary R");
    }

    {
        CHECK(AbsI32(-32768) == 32768u, "-32768 abs is safe");
        CHECK(IsFullScaleS16(-32768), "-32768 is full scale");
        auto pcm = StereoFrames(2, -32768, 32767);
        PcmContinuityState continuity;
        PcmBlockMetrics metrics = AnalyzeStereoS16(pcm.data(), 2, continuity, 1);
        CHECK(metrics.peakL == 32768u, "peak of -32768");
        CHECK(metrics.peakR == 32767u, "peak of +32767");
        CHECK(metrics.fullScaleSamplesL == 2, "full-scale count L");
        CHECK(metrics.fullScaleSamplesR == 2, "full-scale count R");
    }

    {
        auto first = StereoFrames(8, 32767, 0);
        auto second = StereoFrames(5, 32767, 0);
        PcmContinuityState continuity;
        PcmBlockMetrics a = AnalyzeStereoS16(first.data(), 8, continuity, 1);
        PcmBlockMetrics b = AnalyzeStereoS16(second.data(), 5, continuity, 1);
        CHECK(a.maxFullScaleRunL == 8, "first full-scale run");
        CHECK(b.maxFullScaleRunL == 13, "full-scale run continues across callbacks");
        CHECK(b.fullScaleSamplesL == 5, "second block full-scale samples");
    }

    {
        auto first = StereoFrames(2, 1234, -2000);
        auto second = StereoFrames(2, -30000, 25000);
        PcmContinuityState continuity;
        AnalyzeStereoS16(first.data(), 2, continuity, 1);
        PcmBlockMetrics reset = AnalyzeStereoS16(second.data(), 2, continuity, 2);
        CHECK(reset.boundaryDeltaL == 0, "generation change resets continuity");
        CHECK(reset.boundaryDeltaR == 0, "generation change resets R continuity");
        CHECK(!continuity.havePrevious || continuity.seenGeneration == 2, "generation updated");
    }

    {
        PcmContinuityState continuity;
        PcmBlockMetrics empty = AnalyzeStereoS16(nullptr, 8, continuity, 1);
        CHECK(!empty.hasFrames, "null pcm has no frames");
        auto zeros = StereoFrames(8, 0, 0);
        PcmBlockMetrics zero = AnalyzeStereoS16(zeros.data(), 8, continuity, 1);
        CHECK(zero.peakL == 0 && zero.peakR == 0, "all-zero peak");
        CHECK(zero.fullScaleSamplesL == 0 && zero.fullScaleSamplesR == 0, "all-zero full-scale");
        CHECK(zero.maxDeltaL == 0 && zero.maxDeltaR == 0, "all-zero delta");
        CHECK(zero.frames == 8, "zero block still reports frames");
    }

    {
        std::vector<int16_t> pcm = StereoFrames(8, 0, 0);
        pcm[6] = 32767;
        pcm[7] = 100;
        PcmContinuityState continuity;
        PcmBlockMetrics metrics = AnalyzeStereoS16(pcm.data(), 8, continuity, 1);
        CHECK(metrics.fullScaleSamplesL == 1, "single 32767 is one sample");
        CHECK(metrics.maxFullScaleRunL == 1, "single 32767 is not a long platform");
        CHECK(metrics.nearFullScaleSamplesL == 1, "32767 counts as near-full");
    }

    {
        auto pcm = StereoFrames(6, 32767, -32768);
        PcmContinuityState continuity;
        PcmBlockMetrics metrics = AnalyzeStereoS16(pcm.data(), 6, continuity, 1);
        CHECK(metrics.fullScaleSamplesL == 6, "continuous +full-scale samples");
        CHECK(metrics.fullScaleSamplesR == 6, "continuous -full-scale samples");
        CHECK(metrics.maxFullScaleRunL == 6, "continuous +full-scale run");
        CHECK(metrics.maxFullScaleRunR == 6, "continuous -full-scale run");
    }

    for (int32_t value = -32768; value <= 32767; ++value)
    {
        const int16_t sample = static_cast<int16_t>(value);
        if (ApplyGainQ15(sample, kUnityGainQ15) != sample)
        {
            CHECK(false, "unity gain must be bit-exact for every S16 value");
            break;
        }
    }

    CHECK(ApplyGainQ15(0, kHalfGainQ15) == 0, "half gain keeps 0");
    CHECK(ApplyGainQ15(32767, kHalfGainQ15) == 16384, "half gain +full scale");
    CHECK(ApplyGainQ15(-32768, kHalfGainQ15) == -16384, "half gain -32768 does not overflow");
    CHECK(ApplyGainQ15(-32767, kHalfGainQ15) == -16384, "half gain negative rounding is symmetric");
    CHECK(ApplyGainQ15(2, kHalfGainQ15) == 1, "half gain small positive");
    CHECK(ApplyGainQ15(-2, kHalfGainQ15) == -1, "half gain small negative");

    {
        auto pcm = StereoFrames(3, 32767, -32768);
        ApplyStereoGainQ15(pcm.data(), 3, kUnityGainQ15);
        CHECK(pcm[0] == 32767 && pcm[1] == -32768, "unity stereo apply is a no-op");
        ApplyStereoGainQ15(pcm.data(), 3, kHalfGainQ15);
        CHECK(pcm[0] == 16384 && pcm[1] == -16384, "half stereo apply uses frame pairs");
        CHECK(pcm[4] == 16384 && pcm[5] == -16384, "last stereo frame scaled");
    }

    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G00, 0) == kUnityGainQ15, "G00 ordinal 0");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G00, 1) == kUnityGainQ15, "G00 ordinal 1");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G10, 0) == kHalfGainQ15, "G10 ordinal 0");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G10, 1) == kUnityGainQ15, "G10 ordinal 1");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G01, 0) == kUnityGainQ15, "G01 ordinal 0");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G01, 1) == kHalfGainQ15, "G01 ordinal 1");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G11, 0) == kHalfGainQ15, "G11 ordinal 0");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G11, 1) == kHalfGainQ15, "G11 ordinal 1");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G11, 2) == kUnityGainQ15, "ordinal > 1 stays unity");
    CHECK(GainQ15ForOrdinal(AudioDiagGainProfile::G11, 99) == kUnityGainQ15, "large ordinal stays unity");

    if (g_failures)
    {
        std::printf("audio_pcm_metrics_test: %d failed / %d checks\n", g_failures, g_checks);
        return 1;
    }

    std::printf("audio_pcm_metrics_test: %d checks passed\n", g_checks);
    return 0;
}
