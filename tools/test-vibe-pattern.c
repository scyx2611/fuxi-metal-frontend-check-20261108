#include <assert.h>
#include <stdio.h>
#include "FuxiVibePattern.h"

int main(void)
{
    fx_vibe_pattern pattern;

    /* 一般的開關交替；結尾關段丟掉 */
    fx_vibe_reset(&pattern);
    assert(fx_vibe_append(&pattern, true, 200));
    assert(fx_vibe_append(&pattern, false, 150));
    assert(fx_vibe_append(&pattern, true, 400));
    assert(fx_vibe_append(&pattern, false, 800));
    assert(fx_vibe_finish(&pattern) == 600);
    assert(pattern.count == 3 && pattern.total_ms == 750 && !pattern.truncated);
    assert(pattern.segment[0].on && pattern.segment[0].ms == 200);
    assert(!pattern.segment[1].on && pattern.segment[1].ms == 150);
    assert(pattern.segment[2].on && pattern.segment[2].ms == 400);

    /* 同狀態合併、0 與負值略過、太短的開拉到 10 ms、開頭的關段保留當延遲 */
    fx_vibe_reset(&pattern);
    assert(fx_vibe_append(&pattern, false, 50));
    assert(fx_vibe_append(&pattern, true, 3));
    assert(fx_vibe_append(&pattern, true, 0));
    assert(fx_vibe_append(&pattern, true, -5));
    assert(fx_vibe_append(&pattern, true, 20));
    assert(fx_vibe_finish(&pattern) == 30);
    assert(pattern.count == 2 && !pattern.segment[0].on && pattern.segment[0].ms == 50);
    assert(pattern.segment[1].on && pattern.segment[1].ms == 30);

    /* 全是關：沒東西要震 */
    fx_vibe_reset(&pattern);
    assert(fx_vibe_append(&pattern, false, 100));
    assert(fx_vibe_finish(&pattern) == 0 && pattern.count == 0 && pattern.total_ms == 0);

    /* 單段上限與總長截斷 */
    fx_vibe_reset(&pattern);
    assert(fx_vibe_append(&pattern, true, 999999));
    assert(pattern.segment[0].ms == FX_VIBE_SEGMENT_MS);
    assert(fx_vibe_append(&pattern, false, FX_VIBE_SEGMENT_MS));
    assert(fx_vibe_append(&pattern, true, FX_VIBE_SEGMENT_MS + 5)); /* 剛好填滿 30 秒 */
    assert(!pattern.truncated && pattern.total_ms == FX_VIBE_TOTAL_MS);
    assert(!fx_vibe_append(&pattern, false, 10));
    assert(pattern.truncated && pattern.total_ms == FX_VIBE_TOTAL_MS);
    assert(fx_vibe_finish(&pattern) == 2 * FX_VIBE_SEGMENT_MS);

    /* 跨過 30 秒的那段被截短，截斷後結尾的關段一樣丟掉 */
    fx_vibe_reset(&pattern);
    assert(fx_vibe_append(&pattern, true, 10000));
    assert(fx_vibe_append(&pattern, false, 10000));
    assert(fx_vibe_append(&pattern, true, 5000));
    assert(!fx_vibe_append(&pattern, false, 10000));
    assert(pattern.truncated && pattern.total_ms == FX_VIBE_TOTAL_MS && pattern.segment[3].ms == 5000);
    assert(!fx_vibe_append(&pattern, true, 100));
    assert(fx_vibe_finish(&pattern) == 15000 && pattern.count == 3 && pattern.total_ms == 25000);

    /* 段數上限 */
    fx_vibe_reset(&pattern);
    unsigned added = 0;
    for (unsigned index = 0; index < 200; index++) {
        if (!fx_vibe_append(&pattern, (index & 1) == 0, 20)) break;
        added++;
    }
    assert(added == FX_VIBE_SEGMENTS && pattern.count == FX_VIBE_SEGMENTS && pattern.truncated);

    /* 舊格式：開 100、關 50、總長 400 → 開關開關開（最後一段開被總長截成 100） */
    assert(fx_vibe_periodic(&pattern, 100, 50, 400) == 300);
    assert(pattern.count == 5 && pattern.total_ms == 400);
    assert(pattern.segment[0].on && pattern.segment[0].ms == 100 && !pattern.segment[1].on);
    assert(pattern.segment[4].on && pattern.segment[4].ms == 100);
    /* 總長落在開段中間：那段截短；落在關段上：結尾的關段丟掉 */
    assert(fx_vibe_periodic(&pattern, 100, 100, 250) == 150 && pattern.count == 3 && pattern.total_ms == 250);
    assert(fx_vibe_periodic(&pattern, 100, 100, 150) == 100 && pattern.count == 1 && pattern.total_ms == 100);
    /* 沒有總長或關段：只震一段；總長比 on 長就震總長 */
    assert(fx_vibe_periodic(&pattern, 300, 0, 0) == 300 && pattern.count == 1);
    assert(fx_vibe_periodic(&pattern, 200, 0, 500) == 500 && pattern.count == 1);
    assert(fx_vibe_periodic(&pattern, 0, 100, 1000) == 0 && pattern.count == 0);
    /* 很長的循環被 30 秒總長截斷 */
    assert(fx_vibe_periodic(&pattern, 1000, 1000, 1000000) == 15000 && pattern.truncated);

    fx_vibe_reset(&pattern);
    assert(fx_vibe_atom(&pattern, 50, 20));
    assert(fx_vibe_atom(&pattern, 60, 30));
    assert(fx_vibe_atom(&pattern, 65, 10));
    assert(fx_vibe_atom(&pattern, 120, 15));
    assert(fx_vibe_finish(&pattern) == 55 && pattern.count == 4 && pattern.total_ms == 135);
    assert(!pattern.segment[0].on && pattern.segment[0].ms == 50);
    assert(pattern.segment[1].on && pattern.segment[1].ms == 40);
    assert(!pattern.segment[2].on && pattern.segment[2].ms == 30);
    assert(pattern.segment[3].on && pattern.segment[3].ms == 15);
    assert(!fx_vibe_atom(&pattern, 20, 10));
    assert(!fx_vibe_atom(&pattern, -1, 10));
    assert(!fx_vibe_atom(&pattern, 200, 0));
    assert(!fx_vibe_atom(&pattern, FX_VIBE_TOTAL_MS + 1, 10));
    fx_vibe_reset(&pattern);
    assert(!fx_vibe_atom(&pattern, FX_VIBE_TOTAL_MS - 5, 15));
    assert(pattern.truncated && pattern.total_ms == FX_VIBE_TOTAL_MS);

    /* Envelope 換成等效全開時間：充電震動的包絡（0.4 s，前段爬升、尾段衰減）約 135 ms，從 90 ms 起 */
    {
        const double rise_t[] = {0.0, 0.05, 0.1, 0.12, 0.13, 0.2, 0.4};
        const double rise_l[] = {0.0, 0.1, 0.6, 1.0, 1.0, 0.4, 0.0};
        long long offset = -1;
        assert(fx_vibe_envelope(rise_t, rise_l, 7, 400, &offset) == 135 && offset == 90);
        /* 方波：整段 1 → 全長；只有一點時維持到 duration */
        const double square_t[] = {0.0, 0.1}, square_l[] = {1.0, 1.0};
        assert(fx_vibe_envelope(square_t, square_l, 2, 100, &offset) == 100 && offset == 0);
        assert(fx_vibe_envelope(square_t, square_l, 1, 50, &offset) == 50 && offset == 0);
        /* 超過 duration 的部分內插截掉；起點往前挪，讓整段落在 duration 內 */
        const double ramp_t[] = {0.0, 0.2}, ramp_l[] = {0.0, 1.0};
        assert(fx_vibe_envelope(ramp_t, ramp_l, 2, 100, &offset) == 25 && offset == 75);
        /* level 夾在 0..1；全 0 → 0（不震）；時間倒退、NaN、點數 0 → -1（改用整段 Duration） */
        const double clamp_t[] = {0.0, 0.1}, clamp_l[] = {-1.0, 5.0};
        assert(fx_vibe_envelope(clamp_t, clamp_l, 2, 100, &offset) == 50 && offset == 50);
        const double zero_t[] = {0.0, 0.3}, zero_l[] = {0.0, 0.0};
        assert(fx_vibe_envelope(zero_t, zero_l, 2, 300, &offset) == 0);
        const double back_t[] = {0.2, 0.1}, back_l[] = {1.0, 1.0};
        assert(fx_vibe_envelope(back_t, back_l, 2, 300, &offset) == -1);
        const double nan_l[] = {__builtin_nan(""), 1.0};
        assert(fx_vibe_envelope(square_t, nan_l, 2, 100, &offset) == -1);
        assert(fx_vibe_envelope(square_t, square_l, 0, 100, &offset) == -1);
        assert(fx_vibe_envelope(square_t, square_l, 2, 0, &offset) == -1);
        /* 很小但非 0 的面積至少 1 ms（之後 fx_vibe_append 拉到 10 ms） */
        const double tiny_t[] = {0.0, 0.001}, tiny_l[] = {0.0, 0.5};
        long long tiny = fx_vibe_envelope(tiny_t, tiny_l, 2, 1, &offset);
        assert(tiny == 1 && offset == 0);
    }

    /* 振幅：同振幅的開段才合併；關段不帶振幅 */
    fx_vibe_reset(&pattern);
    assert(fx_vibe_append_level(&pattern, true, 10, 100) && fx_vibe_append_level(&pattern, true, 10, 100));
    assert(pattern.count == 1 && pattern.segment[0].ms == 20 && pattern.segment[0].level == 100);
    assert(fx_vibe_append_level(&pattern, true, 10, 50) && pattern.count == 2 && pattern.segment[1].level == 50);
    assert(fx_vibe_append_level(&pattern, false, 10, 7) && pattern.count == 3 && pattern.segment[2].level == 0);
    assert(fx_vibe_append(&pattern, true, 10) && pattern.count == 4 && pattern.segment[3].level == 0);

    /* 振幅模式：充電震動的包絡照形狀分階，前面爬升、峰值 255、後面衰減，不再是一段全開 */
    {
        const double rise_t[] = {0.0, 0.05, 0.1, 0.12, 0.13, 0.2, 0.4};
        const double rise_l[] = {0.0, 0.1, 0.6, 1.0, 1.0, 0.4, 0.0};
        fx_vibe_reset(&pattern);
        long long on = fx_vibe_shape(&pattern, 0, 400, rise_t, rise_l, 7);
        unsigned on_ms = fx_vibe_finish(&pattern);
        assert(on > 135 && on <= 400 && (long long)on_ms == on && pattern.total_ms <= 400);
        assert(!pattern.segment[0].on && pattern.count <= 40 && !pattern.truncated);
        unsigned peak = 0, peak_index = 0;
        for (unsigned index = 0; index < pattern.count; index++) {
            if (!pattern.segment[index].on) continue;
            assert(pattern.segment[index].level >= 1);
            if (pattern.segment[index].level > peak) {
                peak = pattern.segment[index].level;
                peak_index = index;
            }
        }
        assert(peak == 255);
        for (unsigned index = 2; index <= peak_index; index++)
            assert(pattern.segment[index].level >= pattern.segment[index - 1].level);
        for (unsigned index = peak_index + 1; index < pattern.count; index++)
            assert(pattern.segment[index].level <= pattern.segment[index - 1].level);
        /* 換成核心級數：峰值 = 上限，其餘照比例至少 1 */
        fx_vibe_scale(&pattern, 128);
        assert(pattern.segment[peak_index].level == 128 && !pattern.segment[0].level);
        for (unsigned index = 1; index < pattern.count; index++) assert(pattern.segment[index].level >= 1);
        fx_vibe_scale(&pattern, 0);
        for (unsigned index = 0; index < pattern.count; index++) assert(!pattern.segment[index].level);

        /* 方波：一段全長 255；重疊的 atom 只接後面；有空隙先補關段 */
        const double square_t[] = {0.0, 0.1}, square_l[] = {1.0, 1.0};
        fx_vibe_reset(&pattern);
        assert(fx_vibe_shape(&pattern, 0, 100, square_t, square_l, 2) == 100);
        assert(pattern.count == 1 && pattern.segment[0].on && pattern.segment[0].level == 255 && pattern.total_ms == 100);
        fx_vibe_reset(&pattern);
        assert(fx_vibe_atom(&pattern, 0, 100));
        assert(fx_vibe_shape(&pattern, 50, 100, square_t, square_l, 2) == 50);
        assert(pattern.count == 2 && pattern.segment[0].level == 0 && pattern.segment[1].level == 255);
        assert(pattern.segment[1].ms == 50 && pattern.total_ms == 150);
        assert(fx_vibe_shape(&pattern, 200, 30, square_t, square_l, 1) == 30);
        assert(pattern.count == 4 && !pattern.segment[2].on && pattern.segment[2].ms == 50 && pattern.total_ms == 230);
        fx_vibe_scale(&pattern, 90);
        assert(pattern.segment[0].level == 90 && pattern.segment[1].level == 90 && pattern.segment[3].level == 90);

        /* 半高方波 → 128；點列不對 -1 且不動 pattern；全 0 → 0、沒有開段 */
        const double half_l[] = {0.5, 0.5};
        fx_vibe_reset(&pattern);
        assert(fx_vibe_shape(&pattern, 0, 40, square_t, half_l, 2) == 40 && pattern.segment[0].level == 128);
        fx_vibe_scale(&pattern, 200);
        assert(pattern.segment[0].level == 100);
        const double back_t[] = {0.2, 0.1}, zero_l[] = {0.0, 0.0};
        fx_vibe_reset(&pattern);
        assert(fx_vibe_shape(&pattern, 0, 100, back_t, square_l, 2) == -1 && !pattern.count);
        assert(fx_vibe_shape(&pattern, 0, 0, square_t, square_l, 2) == -1 && !pattern.count);
        assert(fx_vibe_shape(&pattern, 0, 100, square_t, square_l, 0) == -1 && !pattern.count);
        const double nan_l[] = {__builtin_nan(""), 1.0};
        assert(fx_vibe_shape(&pattern, 0, 100, square_t, nan_l, 2) == -1 && !pattern.count);
        assert(fx_vibe_shape(&pattern, 0, 100, square_t, zero_l, 2) == 0 && !fx_vibe_finish(&pattern) && !pattern.count);

        /* 很長的包絡：段數用完就停在那裡並標 truncated */
        double many_t[FX_VIBE_ENVELOPE_POINTS], many_l[FX_VIBE_ENVELOPE_POINTS];
        for (unsigned index = 0; index < FX_VIBE_ENVELOPE_POINTS; index++) {
            many_t[index] = index * 0.3;
            many_l[index] = (index & 1) ? 1.0 : 0.0;
        }
        fx_vibe_reset(&pattern);
        for (unsigned round = 0; round < 40 && !pattern.truncated; round++)
            assert(fx_vibe_shape(&pattern, pattern.total_ms, 9000, many_t, many_l, FX_VIBE_ENVELOPE_POINTS) >= 0);
        assert(pattern.truncated && pattern.count <= FX_VIBE_SEGMENTS);
    }

    /* 這次的級數：上限 × Intensity × gain（負數＝沒給、NaN 不算、超過 1 夾住），非 0 至少 1；上限 0＝不送 */
    assert(fx_vibe_amplitude(0, 1.0, 1.0) == 0);
    assert(fx_vibe_amplitude(200, -1.0, -1.0) == 200);
    assert(fx_vibe_amplitude(200, 0.5, 1.0) == 100 && fx_vibe_amplitude(200, 2.0, 0.5) == 100);
    assert(fx_vibe_amplitude(200, 0.001, 1.0) == 1 && fx_vibe_amplitude(300, 1.0, 1.0) == 255);
    assert(fx_vibe_amplitude(200, __builtin_nan(""), 0.25) == 50);

    /* 長段分次向核心要：每次最多 2000 ms，1500 ms 後續約 */
    assert(fx_vibe_request_ms(5000) == FX_VIBE_REQUEST_MS && fx_vibe_renew_ms(5000) == FX_VIBE_RENEW_MS);
    assert(fx_vibe_request_ms(2000) == 2000 && fx_vibe_renew_ms(2000) == 2000);
    assert(fx_vibe_request_ms(15) == 15 && fx_vibe_renew_ms(15) == 15);
    puts("PASS: VibePattern segments merge, clamp, truncate and renew within the kernel lease");
    return 0;
}
