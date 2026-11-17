#ifndef FUXI_VIBE_PATTERN_H
#define FUXI_VIBE_PATTERN_H
/*
 * VibePattern（{"VibePattern": [@YES, ms, @NO, ms, ...], "Intensity": x}）整理成開關段落。
 * 純 C、不碰 CF，主機測試 tools/test-vibe-pattern.c 直接用。
 * 規則：同狀態（開的段另要同振幅）相鄰合併；單段最長 FX_VIBE_SEGMENT_MS，開的段最短 FX_VIBE_MIN_ON_MS
 * （HAP525 停止約 10 ms，太短的開感覺不到）；總長最多 FX_VIBE_TOTAL_MS，超過就截斷；結尾的關段丟掉。
 * 振幅（level）：0＝核心沿用暫存器現值（舊行為）；1..255 經 fx_vibe_scale 換成實際送給核心的級數。
 */
#include <stdbool.h>
#include <stdint.h>

enum {
    FX_VIBE_SEGMENTS = 160,
    FX_VIBE_TOTAL_MS = 30000,
    FX_VIBE_SEGMENT_MS = 10000,
    FX_VIBE_MIN_ON_MS = 10,
    FX_VIBE_REQUEST_MS = 2000, /* 核心 kern.fuxivibrate 單次上限 */
    FX_VIBE_RENEW_MS = 1500,   /* 長段每隔這麼久續一次，比上限短 */
    FX_VIBE_STEP_MS = 10,      /* 包絡取樣間隔 */
    FX_VIBE_STEPS = 16,        /* 包絡量化成這麼多階，相鄰同階合併 */
    FX_VIBE_SLACK_MS = 30      /* 下一段也是開時多要的時間，讓核心在換段之間不停 */
};

typedef struct {
    uint32_t ms;
    uint8_t on;
    uint8_t level;
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

/* 回傳 false 表示已經滿了（之後的段落都不會加入）。level 只對開的段有意義。 */
static inline bool fx_vibe_append_level(fx_vibe_pattern *pattern, bool on, long long ms, uint8_t level)
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
    if (!on) level = 0;
    if (pattern->count && pattern->segment[pattern->count - 1].on == (on ? 1 : 0) &&
        pattern->segment[pattern->count - 1].level == level) {
        uint32_t merged = pattern->segment[pattern->count - 1].ms + (uint32_t)ms;
        pattern->segment[pattern->count - 1].ms = merged;
    } else if (pattern->count == FX_VIBE_SEGMENTS) {
        pattern->truncated = 1;
        return false;
    } else {
        pattern->segment[pattern->count].on = on ? 1 : 0;
        pattern->segment[pattern->count].level = level;
        pattern->segment[pattern->count].ms = (uint32_t)ms;
        pattern->count++;
    }
    pattern->total_ms += (unsigned)ms;
    return !pattern->truncated;
}

static inline bool fx_vibe_append(fx_vibe_pattern *pattern, bool on, long long ms)
{
    return fx_vibe_append_level(pattern, on, ms, 0);
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

static inline bool fx_vibe_atom(fx_vibe_pattern *pattern, long long start_ms, long long duration_ms)
{
    if (start_ms < 0 || duration_ms <= 0 || start_ms > FX_VIBE_TOTAL_MS) return false;
    if (duration_ms > FX_VIBE_SEGMENT_MS) duration_ms = FX_VIBE_SEGMENT_MS;
    long long end_ms = start_ms + duration_ms;
    if (start_ms < pattern->total_ms) {
        if (!pattern->count || !pattern->segment[pattern->count - 1].on ||
            start_ms < pattern->total_ms - pattern->segment[pattern->count - 1].ms) return false;
        if (end_ms <= pattern->total_ms) return true;
        return fx_vibe_append(pattern, true, end_ms - pattern->total_ms);
    }
    while (pattern->total_ms < start_ms)
        if (!fx_vibe_append(pattern, false, start_ms - pattern->total_ms)) return false;
    return fx_vibe_append(pattern, true, duration_ms);
}

/*
 * Atoms 的 Envelope（Level 0..1、Time 秒，相對 atom 起點的點列）換成一段等效的全開時間。
 * 核心驅動只有開／關，iPhone 則照包絡調振幅；整段 Duration 全開會比 iPhone 強很多
 * （2026-11-17 充電震動 Duration 0.4 s 被當成 400 ms 全開，使用者回報偏強）。
 * 做法：梯形法算 [0, duration] 內 level 對時間的面積（毫秒）；第一點之前視為第一點的 level，
 * 最後一點之後維持最後的 level。起點取 level 第一次達到最大值一半的時間，必要時往前挪，
 * 讓整段落在 duration 內。回傳等效毫秒（0＝整段沒有振幅）；點數不對、時間倒退或含 NaN 回 -1，
 * 呼叫端改用整段 Duration。
 */
enum { FX_VIBE_ENVELOPE_POINTS = 32 };

static inline long long fx_vibe_envelope(const double *time_s, const double *level, unsigned count,
    long long duration_ms, long long *offset_ms)
{
    double t[FX_VIBE_ENVELOPE_POINTS + 1], l[FX_VIBE_ENVELOPE_POINTS + 1], peak = 0.0;
    *offset_ms = 0;
    if (count == 0 || count > FX_VIBE_ENVELOPE_POINTS || duration_ms <= 0) return -1;
    if (duration_ms > FX_VIBE_SEGMENT_MS) duration_ms = FX_VIBE_SEGMENT_MS;
    for (unsigned index = 0; index < count; index++) {
        if (time_s[index] != time_s[index] || level[index] != level[index]) return -1;
        if (time_s[index] < 0.0 || time_s[index] > 600.0) return -1;
        t[index] = time_s[index] * 1000.0;
        if (index && t[index] < t[index - 1]) return -1;
        l[index] = level[index] < 0.0 ? 0.0 : level[index] > 1.0 ? 1.0 : level[index];
        if (l[index] > peak) peak = l[index];
    }
    if (peak <= 0.0) return 0;
    double end = (double)duration_ms;
    unsigned points = count;
    if (t[points - 1] < end) {
        t[points] = end;
        l[points] = l[points - 1];
        points++;
    }
    double area = (t[0] < end ? t[0] : end) * l[0];
    for (unsigned index = 1; index < points && t[index - 1] < end; index++) {
        double from = t[index - 1], to = t[index], start = l[index - 1], stop = l[index];
        if (to > end) {
            stop = start + (stop - start) * (end - from) / (to - from);
            to = end;
        }
        area += (to - from) * (start + stop) / 2.0;
    }
    double half = peak / 2.0, offset = 0.0;
    if (l[0] < half) {
        for (unsigned index = 1; index < points; index++) {
            if (l[index] < half) continue;
            offset = t[index - 1] + (t[index] - t[index - 1]) * (half - l[index - 1]) / (l[index] - l[index - 1]);
            break;
        }
    }
    long long on = (long long)(area + 0.5);
    if (on == 0 && area > 0.0) on = 1;
    if (on > duration_ms) on = duration_ms;
    long long shift = (long long)(offset + 0.5);
    if (shift + on > duration_ms) shift = duration_ms - on;
    *offset_ms = shift < 0 ? 0 : shift;
    return on;
}

/*
 * 振幅模式（ADT fuxi,haptic-amp，2026-11-17）：核心能調 DIRECT_PLAY 振幅後，Envelope 不再換成等效全開時間，
 * 而是像 iPhone 一樣照包絡調振幅：整段 Duration 每 FX_VIBE_STEP_MS 取包絡平均，量化成 FX_VIBE_STEPS 階的
 * 相對振幅（255＝包絡 1.0；0 就是關），相鄰同階合併。使用者回報全開版本「太大、太脆」，iPhone 較悶。
 * 包絡在第一點之前視為第一點的 level，最後一點之後維持最後的 level（與 fx_vibe_envelope 相同）。
 * 起點落在已排內容之內（重疊的 atom）時，被蓋住的部分沿用前面的，只接後面的部分。
 * 回傳加入的開段毫秒數（滿了就停在那裡，呼叫端看 truncated）；點列不對回 -1，呼叫端改用整段 Duration。
 */
static inline double fx_vibe_level_at(const double *t, const double *l, unsigned count, double at)
{
    if (at <= t[0]) return l[0];
    for (unsigned index = 1; index < count; index++) {
        if (at > t[index]) continue;
        double span = t[index] - t[index - 1];
        return span <= 0.0 ? l[index] : l[index - 1] + (l[index] - l[index - 1]) * (at - t[index - 1]) / span;
    }
    return l[count - 1];
}

static inline long long fx_vibe_shape(fx_vibe_pattern *pattern, long long start_ms, long long duration_ms,
    const double *time_s, const double *level, unsigned count)
{
    double t[FX_VIBE_ENVELOPE_POINTS], l[FX_VIBE_ENVELOPE_POINTS];
    if (count == 0 || count > FX_VIBE_ENVELOPE_POINTS || duration_ms <= 0 || start_ms < 0 ||
        start_ms > FX_VIBE_TOTAL_MS)
        return -1;
    if (duration_ms > FX_VIBE_SEGMENT_MS) duration_ms = FX_VIBE_SEGMENT_MS;
    for (unsigned index = 0; index < count; index++) {
        if (time_s[index] != time_s[index] || level[index] != level[index]) return -1;
        if (time_s[index] < 0.0 || time_s[index] > 600.0) return -1;
        t[index] = time_s[index] * 1000.0;
        if (index && t[index] < t[index - 1]) return -1;
        l[index] = level[index] < 0.0 ? 0.0 : level[index] > 1.0 ? 1.0 : level[index];
    }
    long long on = 0;
    for (long long step = 0; step < duration_ms; step += FX_VIBE_STEP_MS) {
        long long length = duration_ms - step < FX_VIBE_STEP_MS ? duration_ms - step : FX_VIBE_STEP_MS;
        long long at = start_ms + step, from = at;
        if (at + length <= (long long)pattern->total_ms) continue;
        if (from < (long long)pattern->total_ms) from = pattern->total_ms;
        double sum = 0.0;
        for (unsigned sample = 0; sample < 4; sample++)
            sum += fx_vibe_level_at(t, l, count, (double)step + ((double)sample + 0.5) * (double)length / 4.0);
        unsigned steps = (unsigned)(sum / 4.0 * FX_VIBE_STEPS + 0.5);
        uint8_t relative = (uint8_t)((steps * 255u + FX_VIBE_STEPS / 2) / FX_VIBE_STEPS);
        if ((long long)pattern->total_ms < from && !fx_vibe_append(pattern, false, from - pattern->total_ms))
            return on;
        if (!fx_vibe_append_level(pattern, relative != 0, at + length - from, relative)) return on;
        if (relative) on += at + length - from;
    }
    return on;
}

/*
 * 相對振幅換成送給核心的級數：amplitude 是包絡 1.0 時要的級數（1..255）。
 * level 0 的開段（沒有包絡的段落）用 amplitude；其餘照比例，至少 1。amplitude 0：全部回 0（核心沿用現值）。
 */
static inline void fx_vibe_scale(fx_vibe_pattern *pattern, unsigned amplitude)
{
    if (amplitude > 255u) amplitude = 255u;
    for (unsigned index = 0; index < pattern->count; index++) {
        fx_vibe_segment *segment = &pattern->segment[index];
        if (!segment->on || !amplitude) {
            segment->level = 0;
            continue;
        }
        unsigned scaled = segment->level ? (segment->level * amplitude + 127u) / 255u : amplitude;
        segment->level = (uint8_t)(scaled ? scaled : 1u);
    }
}

/* 依強度（Intensity、gain，各自 0..1，負數＝沒給）把 fuxi,haptic-amp 的上限換成這次的級數；0＝不送振幅。 */
static inline unsigned fx_vibe_amplitude(unsigned maximum, double intensity, double gain)
{
    if (!maximum) return 0;
    if (maximum > 255u) maximum = 255u;
    double factor = 1.0;
    if (intensity == intensity && intensity >= 0.0) factor *= intensity > 1.0 ? 1.0 : intensity;
    if (gain == gain && gain >= 0.0) factor *= gain > 1.0 ? 1.0 : gain;
    unsigned level = (unsigned)(maximum * factor + 0.5);
    return level ? level : 1u;
}

/*
 * FigVibrator 舊格式（OnDuration／OffDuration／TotalDuration，秒）：開 on、關 off 輪流，直到 total；
 * total <= 0 或 off <= 0 只震一段 on。回傳開的總毫秒數。
 */
static inline unsigned fx_vibe_periodic(fx_vibe_pattern *pattern, long long on_ms, long long off_ms, long long total_ms)
{
    fx_vibe_reset(pattern);
    if (on_ms <= 0) return 0;
    if (total_ms <= 0 || off_ms <= 0) {
        (void)fx_vibe_append(pattern, true, total_ms > on_ms ? total_ms : on_ms);
        return fx_vibe_finish(pattern);
    }
    long long used = 0;
    while (used < total_ms) {
        long long on = on_ms < total_ms - used ? on_ms : total_ms - used;
        if (!fx_vibe_append(pattern, true, on)) break;
        used += on;
        if (used >= total_ms) break;
        long long off = off_ms < total_ms - used ? off_ms : total_ms - used;
        if (!fx_vibe_append(pattern, false, off)) break;
        used += off;
    }
    return fx_vibe_finish(pattern);
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
