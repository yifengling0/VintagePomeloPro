#include "audio/audio_pcm_metrics.h"

#include <algorithm>
#include <cmath>

namespace winehua {

namespace {

uint32_t ExchangeMax32(std::atomic<uint32_t>& target)
{
    return target.exchange(0, std::memory_order_relaxed);
}

} // namespace

uint32_t AbsI32(int32_t value)
{
    if (value >= 0)
        return static_cast<uint32_t>(value);
    return static_cast<uint32_t>(static_cast<int64_t>(0) - static_cast<int64_t>(value));
}

bool IsFullScaleS16(int32_t value)
{
    return value == 32767 || value == -32768;
}

uint32_t RmsFromSumSquares(uint64_t sumSquares, uint64_t frames)
{
    if (!frames)
        return 0;
    return static_cast<uint32_t>(
        std::llround(std::sqrt(static_cast<double>(sumSquares) / static_cast<double>(frames))));
}

int32_t DcFromSum(int64_t sum, uint64_t frames)
{
    if (!frames)
        return 0;
    return static_cast<int32_t>(sum / static_cast<int64_t>(frames));
}

PcmBlockMetrics AnalyzeStereoS16(const int16_t* pcm,
                                  uint32_t frames,
                                  PcmContinuityState& continuity,
                                  uint64_t generation)
{
    PcmBlockMetrics result;
    result.frames = frames;

    if (!pcm || !frames)
        return result;

    if (continuity.seenGeneration != generation)
    {
        continuity = {};
        continuity.seenGeneration = generation;
    }

    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        const int32_t left = pcm[frame * 2];
        const int32_t right = pcm[frame * 2 + 1];

        if (!result.hasFrames)
        {
            result.firstL = left;
            result.firstR = right;
            result.hasFrames = true;

            if (continuity.havePrevious)
            {
                result.boundaryDeltaL = AbsI32(left - continuity.previousL);
                result.boundaryDeltaR = AbsI32(right - continuity.previousR);
            }
        }

        result.peakL = std::max(result.peakL, AbsI32(left));
        result.peakR = std::max(result.peakR, AbsI32(right));

        result.sumSquaresL += static_cast<uint64_t>(static_cast<int64_t>(left) * left);
        result.sumSquaresR += static_cast<uint64_t>(static_cast<int64_t>(right) * right);
        result.sumL += left;
        result.sumR += right;

        if (AbsI32(left) >= static_cast<uint32_t>(kNearFullScaleThreshold))
            ++result.nearFullScaleSamplesL;
        if (AbsI32(right) >= static_cast<uint32_t>(kNearFullScaleThreshold))
            ++result.nearFullScaleSamplesR;

        if (IsFullScaleS16(left))
        {
            ++result.fullScaleSamplesL;
            ++continuity.currentFullScaleRunL;
            result.maxFullScaleRunL = std::max(result.maxFullScaleRunL,
                                                 continuity.currentFullScaleRunL);
        }
        else
        {
            continuity.currentFullScaleRunL = 0;
        }

        if (IsFullScaleS16(right))
        {
            ++result.fullScaleSamplesR;
            ++continuity.currentFullScaleRunR;
            result.maxFullScaleRunR = std::max(result.maxFullScaleRunR,
                                                 continuity.currentFullScaleRunR);
        }
        else
        {
            continuity.currentFullScaleRunR = 0;
        }

        if (frame > 0)
        {
            const int32_t previousLeft = pcm[(frame - 1) * 2];
            const int32_t previousRight = pcm[(frame - 1) * 2 + 1];
            result.maxDeltaL = std::max(result.maxDeltaL, AbsI32(left - previousLeft));
            result.maxDeltaR = std::max(result.maxDeltaR, AbsI32(right - previousRight));
        }

        result.lastL = left;
        result.lastR = right;
    }

    continuity.previousL = result.lastL;
    continuity.previousR = result.lastR;
    continuity.havePrevious = true;
    return result;
}

int16_t ApplyGainQ15(int16_t sample, uint32_t gainQ15)
{
    if (gainQ15 == kUnityGainQ15)
        return sample;

    const int64_t product = static_cast<int64_t>(sample) * static_cast<int64_t>(gainQ15);
    int32_t scaled;
    if (product >= 0)
        scaled = static_cast<int32_t>((product + 16384) >> 15);
    else
        scaled = -static_cast<int32_t>(((-product) + 16384) >> 15);

    if (scaled > 32767)
        scaled = 32767;
    if (scaled < -32768)
        scaled = -32768;
    return static_cast<int16_t>(scaled);
}

void ApplyStereoGainQ15(int16_t* pcm, uint32_t frames, uint32_t gainQ15)
{
    if (!pcm || gainQ15 == kUnityGainQ15)
        return;

    const uint32_t samples = frames * 2;
    for (uint32_t i = 0; i < samples; ++i)
        pcm[i] = ApplyGainQ15(pcm[i], gainQ15);
}

uint32_t GainQ15ForOrdinal(AudioDiagGainProfile profile, uint32_t ordinal)
{
    if (ordinal > 1)
        return kUnityGainQ15;

    switch (profile)
    {
    case AudioDiagGainProfile::G10:
        return ordinal == 0 ? kHalfGainQ15 : kUnityGainQ15;
    case AudioDiagGainProfile::G01:
        return ordinal == 0 ? kUnityGainQ15 : kHalfGainQ15;
    case AudioDiagGainProfile::G11:
        return kHalfGainQ15;
    case AudioDiagGainProfile::G00:
    default:
        return kUnityGainQ15;
    }
}

const char* AudioDiagGainProfileName(AudioDiagGainProfile profile)
{
    switch (profile)
    {
    case AudioDiagGainProfile::G10:
        return "G10";
    case AudioDiagGainProfile::G01:
        return "G01";
    case AudioDiagGainProfile::G11:
        return "G11";
    case AudioDiagGainProfile::G00:
    default:
        return "G00";
    }
}

void PcmMetricAccumulators::Add(const PcmBlockMetrics& metrics)
{
    if (!metrics.frames)
        return;

    frames.fetch_add(metrics.frames, std::memory_order_relaxed);
    AtomicMax(peakL, metrics.peakL);
    AtomicMax(peakR, metrics.peakR);
    sumSquaresL.fetch_add(metrics.sumSquaresL, std::memory_order_relaxed);
    sumSquaresR.fetch_add(metrics.sumSquaresR, std::memory_order_relaxed);
    sumL.fetch_add(metrics.sumL, std::memory_order_relaxed);
    sumR.fetch_add(metrics.sumR, std::memory_order_relaxed);
    fullScaleSamplesL.fetch_add(metrics.fullScaleSamplesL, std::memory_order_relaxed);
    fullScaleSamplesR.fetch_add(metrics.fullScaleSamplesR, std::memory_order_relaxed);
    nearFullScaleSamplesL.fetch_add(metrics.nearFullScaleSamplesL, std::memory_order_relaxed);
    nearFullScaleSamplesR.fetch_add(metrics.nearFullScaleSamplesR, std::memory_order_relaxed);
    AtomicMax(maxFullScaleRunL, metrics.maxFullScaleRunL);
    AtomicMax(maxFullScaleRunR, metrics.maxFullScaleRunR);
    AtomicMax(maxDeltaL, metrics.maxDeltaL);
    AtomicMax(maxDeltaR, metrics.maxDeltaR);
    AtomicMax(maxBoundaryDeltaL, metrics.boundaryDeltaL);
    AtomicMax(maxBoundaryDeltaR, metrics.boundaryDeltaR);
}

PcmMetricSnapshot PcmMetricAccumulators::ExchangeReset()
{
    PcmMetricSnapshot snap;
    snap.frames = frames.exchange(0, std::memory_order_relaxed);
    snap.peakL = ExchangeMax32(peakL);
    snap.peakR = ExchangeMax32(peakR);
    snap.sumSquaresL = sumSquaresL.exchange(0, std::memory_order_relaxed);
    snap.sumSquaresR = sumSquaresR.exchange(0, std::memory_order_relaxed);
    snap.sumL = sumL.exchange(0, std::memory_order_relaxed);
    snap.sumR = sumR.exchange(0, std::memory_order_relaxed);
    snap.fullScaleSamplesL = ExchangeMax32(fullScaleSamplesL);
    snap.fullScaleSamplesR = ExchangeMax32(fullScaleSamplesR);
    snap.nearFullScaleSamplesL = ExchangeMax32(nearFullScaleSamplesL);
    snap.nearFullScaleSamplesR = ExchangeMax32(nearFullScaleSamplesR);
    snap.maxFullScaleRunL = ExchangeMax32(maxFullScaleRunL);
    snap.maxFullScaleRunR = ExchangeMax32(maxFullScaleRunR);
    snap.maxDeltaL = ExchangeMax32(maxDeltaL);
    snap.maxDeltaR = ExchangeMax32(maxDeltaR);
    snap.maxBoundaryDeltaL = ExchangeMax32(maxBoundaryDeltaL);
    snap.maxBoundaryDeltaR = ExchangeMax32(maxBoundaryDeltaR);
    snap.validCallbacks = ExchangeMax32(validCallbacks);
    snap.invalidCallbacks = ExchangeMax32(invalidCallbacks);
    return snap;
}

} // namespace winehua
