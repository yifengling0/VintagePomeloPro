#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "ohos_audio_mix.h"
int main(void)
{
    float m[OHOS_AUDIO_MAX_CHANNELS][2];
    unsigned int n, i, bit;
    assert(!ohos_audio_build_matrix(0, 0, m));
    assert(!ohos_audio_build_matrix(19, 0, m));
    assert(!ohos_audio_build_matrix(12, 3, m));
    assert(!ohos_audio_build_matrix(2, 0x80000001u, m));
    assert(ohos_audio_build_matrix(1, 0, m) && m[0][0] == 1 && m[0][1] == 1);
    assert(ohos_audio_build_matrix(2, 0, m));
    assert(m[0][0] == 1 && m[0][1] == 0 && m[1][0] == 0 && m[1][1] == 1);
    for (n = 1; n <= 18; ++n)
    {
        unsigned int mask = (1u << n) - 1;
        float left = 0, right = 0;
        assert(ohos_audio_build_matrix(n, mask, m));
        for (i = 0; i < n; ++i)
        {
            assert(isfinite(m[i][0]) && isfinite(m[i][1]));
            assert(m[i][0] + m[i][1] > 0);
            left += m[i][0]; right += m[i][1];
        }
        ohos_audio_limit_stereo(&left, &right);
        assert(left <= 1.000001f && right <= 1.000001f);
        for (i = n; i < 18; ++i) assert(m[i][0] == 0 && m[i][1] == 0);
    }
    assert(ohos_audio_build_matrix(12, 0x2d63f, m));
    /* Stereo embedded in 12 channels must retain the same gain as stereo. */
    assert(m[0][0] == 1.0f && m[1][1] == 1.0f);
    for (i = 0; i <= 1000; ++i)
    {
        float left = (int)i / 500.0f - 1.0f, right = -left * 0.5f;
        float before_left = left, before_right = right;
        ohos_audio_limit_stereo(&left, &right);
        assert(left == before_left && right == before_right);
    }
    {
        float left = 4.0f, right = -2.0f;
        ohos_audio_limit_stereo(&left, &right);
        assert(left == 1.0f && right == -0.5f);
        left = -0.25f; right = -2.0f;
        ohos_audio_limit_stereo(&left, &right);
        assert(left == -0.125f && right == -1.0f);
    }
    /* L/R, center, LFE, back and side L/R, then front/back height pairs. */
    for (i = 0; i < 12; ++i)
    {
        if (i == 2 || i == 3) assert(fabsf(m[i][0] - m[i][1]) < 0.000001f);
        else if (!(i & 1)) assert(m[i][0] > 0 && m[i][1] == 0);
        else assert(m[i][0] == 0 && m[i][1] > 0);
    }
    for (bit = 0; bit < 18; ++bit)
    {
        assert(ohos_audio_build_matrix(1, 1u << bit, m));
        assert(m[0][0] + m[0][1] > 0);
    }
    puts("PASS mono/stereo identity, 12-channel routing, all speaker bits, unity gain, linked peak limiting and invalid masks");
}
