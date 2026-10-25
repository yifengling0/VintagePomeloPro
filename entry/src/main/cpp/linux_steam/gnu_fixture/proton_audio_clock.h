#ifndef HMOSDECK_PROTON_AUDIO_CLOCK_H
#define HMOSDECK_PROTON_AUDIO_CLOCK_H

#include <stdint.h>

/* AudioClock's QPC timestamp must use the same epoch as Windows QPC. The
 * intermediate product overflows uint64_t after 2.14 days at Wine's 10 MHz,
 * even though the final 100 ns timestamp is still well within range. */
static inline uint64_t proton_audio_qpc_100ns(uint64_t counter, uint64_t frequency)
{
    return (uint64_t)(((__uint128_t)counter * UINT64_C(10000000)) / frequency);
}

#endif
