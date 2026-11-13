#include <assert.h>
#include <stdint.h>
#include "../entry/src/main/cpp/linux_steam/gnu_fixture/proton_audio_clock.h"

int main(void)
{
    /* Actual device uptime when audio started stuttering; 10 MHz Wine QPC. */
    assert(proton_audio_qpc_100ns(UINT64_C(4210000000000), 10000000) == UINT64_C(4210000000000));
    assert(proton_audio_qpc_100ns(UINT64_C(1844674407370), 10000000) == UINT64_C(1844674407370));
    assert(proton_audio_qpc_100ns(UINT64_C(1844674407371), 10000000) == UINT64_C(1844674407371));
    assert(proton_audio_qpc_100ns(UINT64_C(421000000000000), 1000000000) == UINT64_C(4210000000000));
    assert(proton_audio_qpc_100ns(UINT64_C(10104000000000), 24000000) == UINT64_C(4210000000000));
    assert(proton_audio_qpc_100ns(25, 24) == 10416666);
    assert(proton_audio_qpc_100ns(0, 10000000) == 0);
    return 0;
}
