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

    /* 長段分次向核心要：每次最多 2000 ms，1500 ms 後續約 */
    assert(fx_vibe_request_ms(5000) == FX_VIBE_REQUEST_MS && fx_vibe_renew_ms(5000) == FX_VIBE_RENEW_MS);
    assert(fx_vibe_request_ms(2000) == 2000 && fx_vibe_renew_ms(2000) == 2000);
    assert(fx_vibe_request_ms(15) == 15 && fx_vibe_renew_ms(15) == 15);
    puts("PASS: VibePattern segments merge, clamp, truncate and renew within the kernel lease");
    return 0;
}
