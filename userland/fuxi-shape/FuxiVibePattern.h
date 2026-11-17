#ifndef FUXI_VIBE_PATTERN_H
#define FUXI_VIBE_PATTERN_H
/*
 * VibePattern（{"VibePattern": [@YES, ms, @NO, ms, ...], "Intensity": x}）整理成開關段落。
 * 純 C、不碰 CF，主機測試 tools/test-vibe-pattern.c 直接用。
 * 規則：同狀態相鄰合併；單段最長 FX_VIBE_SEGMENT_MS，開的段最短 FX_VIBE_MIN_ON_MS（HAP525 停止約 10 ms，
 * 太短的開感覺不到）；總長最多 FX_VIBE_TOTAL_MS，超過就截斷；結尾的關段丟掉。
 */
#include <stdbool.h>
#include <stdint.h>

enum {
    FX_VIBE_SEGMENTS = 64,
    FX_VIBE_TOTAL_MS = 30000,
    FX_VIBE_SEGMENT_MS = 10000,
    FX_VIBE_MIN_ON_MS = 10,
    FX_VIBE_REQUEST_MS = 2000, /* 核心 kern.fuxivibrate 單次上限 */
    FX_VIBE_RENEW_MS = 1500    /* 長段每隔這麼久續一次，比上限短 */
};

typedef struct {
    uint32_t ms;
    uint8_t on;
} fx_vibe_segment;

typedef struct {
    fx_vibe_segment segment[FX_VIBE_SEGMENTS];
    unsigned count;
    unsigned total_ms;
    unsigned truncated;
} fx_vibe_pattern;

static inline void fx_vibe_reset(fx_vibe_pattern *pattern)
{
    pattern->count = 0;
    pattern->total_ms = 0;
    pattern->truncated = 0;
}

/* 回傳 false 表示已經滿了（之後的段落都不會加入）。 */
static inline bool fx_vibe_append(fx_vibe_pattern *pattern, bool on, long long ms)
{
    if (pattern->truncated) return false;
    if (ms <= 0) return true;
    if (ms > FX_VIBE_SEGMENT_MS) ms = FX_VIBE_SEGMENT_MS;
    if (on && ms < FX_VIBE_MIN_ON_MS) ms = FX_VIBE_MIN_ON_MS;
    if (pattern->total_ms >= FX_VIBE_TOTAL_MS) {
        pattern->truncated = 1;
        return false;
    }
    if (pattern->total_ms + (unsigned long long)ms > FX_VIBE_TOTAL_MS) {
        ms = FX_VIBE_TOTAL_MS - pattern->total_ms;
        pattern->truncated = 1;
    }
    if (pattern->count && pattern->segment[pattern->count - 1].on == (on ? 1 : 0)) {
        uint32_t merged = pattern->segment[pattern->count - 1].ms + (uint32_t)ms;
        pattern->segment[pattern->count - 1].ms = merged;
    } else if (pattern->count == FX_VIBE_SEGMENTS) {
        pattern->truncated = 1;
        return false;
    } else {
        pattern->segment[pattern->count].on = on ? 1 : 0;
        pattern->segment[pattern->count].ms = (uint32_t)ms;
        pattern->count++;
    }
    pattern->total_ms += (unsigned)ms;
    return !pattern->truncated;
}

/* 丟掉結尾的關段；回傳開的總毫秒數（0 表示沒東西要震）。 */
static inline unsigned fx_vibe_finish(fx_vibe_pattern *pattern)
{
    unsigned on_ms = 0;
    while (pattern->count && !pattern->segment[pattern->count - 1].on) {
        pattern->total_ms -= pattern->segment[pattern->count - 1].ms;
        pattern->count--;
    }
    for (unsigned index = 0; index < pattern->count; index++)
        if (pattern->segment[index].on) on_ms += pattern->segment[index].ms;
    return on_ms;
}

/* 開的段剩 remaining 毫秒時，這次要向核心要多久。 */
static inline unsigned fx_vibe_request_ms(unsigned remaining)
{
    return remaining > FX_VIBE_REQUEST_MS ? FX_VIBE_REQUEST_MS : remaining;
}

/* 下一次續約前要等多久。 */
static inline unsigned fx_vibe_renew_ms(unsigned remaining)
{
    return remaining > FX_VIBE_REQUEST_MS ? FX_VIBE_RENEW_MS : remaining;
}
#endif
