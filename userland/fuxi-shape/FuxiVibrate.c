/*
 * 震動：把 iOS 的震動請求交給核心 HAP525 執行期（ADT fuxi,haptic=2/3，sysctl kern.fuxivibrate，ms 0..2000）。
 *
 * mediaserverd（fxinj bit 2 才會載入）：
 *   -[AVHapticServer(VibeSynthEngine) playVibePattern:gain:synchronizer:flags:atTime:completionHandler:]
 *     系統聲音的震動（來電、通知、kSystemSoundID_Vibrate）走這裡；解析 VibePattern（或 FigVibrator 舊格式
 *     OnDuration／OffDuration／TotalDuration 秒數）自己排開關，認不得的格式震 15 ms；原實作照呼叫。
 *     前提：ADT /product/haptics closed-loop=1（loader --product-haptics），MediaExperience 才會建立震動器、
 *     走 VibeSynthEngine → HapticServerManager_VibeEnginePlay → 這個方法；沒有的話請求在 MediaExperience 就被丟掉。
 *   -[AVHapticServer(VibeSynthEngine) cancelPatternWithOptions:] 立即停。
 *   AVHapticServerInstance 的 load／prepare／start／stop 只記前幾次，用來確認 CoreHaptics 的請求怎麼進來。
 * SpringBoard：
 *   -[SBFTapticEngine actuateFeedback:]、-[_UITapticEngine actuateFeedback:] 先震 15 ms 再呼叫原實作；
 *   -[SBFTapticEngine supportsFeedbackActuation] 在核心執行期可用時回 YES。
 *   ADT fuxi,haptic=3（開機自測）時，30 秒後經系統路徑各試一次 AudioServicesPlaySystemSound。
 * mediaserverd 在 fuxi,haptic=3 時另跑探針：10 秒與 75 秒各讀一次震動器建立的關卡，77 秒直接呼叫
 *   CMSessionMgrVibrateForSystemSoundWithOptions 看回傳值。
 * 一般 App（Preferences 經 fuxi,inject bit 4；其他 App 要 bit 0 且 libfuxi-app.dylib 是這份）：
 *   CoreHaptics engine 建不起來時，UIFeedbackGenerator 每次要播到 CoreHaptics engine 就改送
 *   AudioServicesPlaySystemSound(1519)（mediaserverd 轉成 15 ms Atoms 震動；App 沒有 kern.fuxivibrate 寫入權）。
 *
 * 參數一律以 method_getTypeEncoding 驗證後原樣傳遞；不認得的簽名就不掛。紀錄寫到 stderr
 * （mediaserverd 要配 loader --stderr-log mediaserverd 才進核心紀錄）。
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "FuxiErrorAudit.h"
#include "FuxiVibePattern.h"

typedef mach_port_t vb_io_object_t;
extern vb_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(vb_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(vb_io_object_t object);

enum { VB_NONE, VB_MEDIA, VB_SPRINGBOARD, VB_APP };
enum { VB_LOG, VB_PULSE, VB_CANCEL };
enum { VB_LOG_CALLS = 6, VB_PLAY_LOGS = 12, VB_ERROR_LOGS = 8, VB_TAP_MS = 15, VB_FALLBACK_MS = 15 };

static unsigned vb_role;
static const char *vb_name = "?";
/*
 * ADT /chosen 開關（預設關，2026-11-17）：
 *   fuxi,haptic-ui  SpringBoard：CoreHaptics engine 從未建成時（mediaserverd 的 AVHapticServer 起不來，
 *                   client 回 -4810），每次 UI 播放改用 kern.fuxivibrate 輕觸這麼多毫秒（8..40）。
 *                   一般 App：非 0 才回退（讀不到 ADT 時視為開，例如沙盒擋 IORegistry）；長度由 mediaserverd 決定。
 *   fuxi,haptic-env mediaserverd：Atoms 依 Envelope 面積換成等效全開時間（FuxiVibePattern.h）。
 *   fuxi,haptic-amp 兩者：核心 DIRECT_PLAY 振幅上限（1..255，需要帶振幅欄位的核心）。mediaserverd 照 Envelope
 *                   逐 10 ms 調振幅（優先於 haptic-env），並乘上 Intensity 與 gain；SpringBoard 的輕觸用這個值。
 *                   沒設時一律送 0，核心沿用 Android 留下的振幅（舊行為）。
 */
static unsigned vb_ui_ms, vb_envelope, vb_envelope_logs, vb_amp_max;
static unsigned vb_app_fallback; /* 一般 App：經系統聲音 1519 回退 */
static unsigned vb_native_pcm;   /* haptic=9 且 hal-haptic>=4：原生 24kHz PCM 串流，停用舊 DIRECT_PLAY UI 回退 */
static unsigned vb_core_ok; /* SpringBoard／App：任一 CoreHaptics engine 建成過就不再回退 */

__attribute__((format(printf, 1, 2))) static void vb_say(const char *format, ...)
{
    char line[1024];
    int prefix = snprintf(line, sizeof(line), "fuxi: %s: ", vb_name);
    if (prefix < 0 || prefix >= (int)sizeof(line)) return;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, ap);
    va_end(ap);
    if (n < 0) return;
    size_t length = (size_t)prefix + (size_t)n;
    if (length > sizeof(line) - 1) {
        length = sizeof(line) - 1;
        line[length - 1] = '\n';
    }
    (void)write(2, line, length);
}

/* iOS 16.7（20H392）shared cache 的未滑動位址；slide 由匯出符號算（見各處核對） */
static const uint64_t VB_ME_VIBRATE = 0x195ad742cULL;  /* _CMSessionMgrVibrateForSystemSoundWithOptions */

static uintptr_t vb_strip(const void *pointer)
{
    return (uintptr_t)pointer & 0x0000007fffffffffULL;
}

/* ---- 核心介面 ---- */
static bool vb_ready(int *error)
{
    int value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname("kern.fuxivibrate", &value, &size, NULL, 0) != 0) {
        *error = errno;
        return false;
    }
    *error = size == sizeof(value) ? 0 : EINVAL;
    return *error == 0;
}

static unsigned vb_write_errors;

/* 核心值：bit 0..15 毫秒、bit 16..23 振幅（0＝沿用）。 */
static bool vb_write(unsigned ms, unsigned level)
{
    int value = (int)(ms | (level & 0xffu) << 16);
    if (sysctlbyname("kern.fuxivibrate", NULL, NULL, &value, sizeof(value)) == 0) return true;
    int error = errno;
    if (__atomic_fetch_add(&vb_write_errors, 1u, __ATOMIC_RELAXED) < VB_ERROR_LOGS)
        vb_say("vibrate write ms=%u level=%u failed errno=%d\n", ms, level, error);
    return false;
}

/* ---- 開關排程（一條工作執行緒，新請求或取消會打斷目前的 pattern） ---- */
static pthread_mutex_t vb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vb_cond = PTHREAD_COND_INITIALIZER;
static fx_vibe_pattern vb_pending;
static void *vb_pending_completion;
static unsigned vb_generation;
static unsigned vb_worker;

extern void *_Block_copy(const void *block);
extern void _Block_release(const void *block);

typedef struct {
    const void *isa;
    int flags;
    int reserved;
    void (*invoke)(void *block);
} vb_completion;

static void vb_complete(void *block)
{
    if (block == NULL) return;
    ((vb_completion *)block)->invoke(block);
    _Block_release(block);
}

static void vb_finish(void *block)
{
    if (block) dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), block, vb_complete);
}

static uint64_t vb_now_ms(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull;
}

/* 持鎖等到 until（uptime ms）或 generation 改變；改變了回 true。 */
static bool vb_wait(unsigned seen, uint64_t until)
{
    for (;;) {
        if (vb_generation != seen) return true;
        uint64_t now = vb_now_ms();
        if (now >= until) return false;
        uint64_t left = until - now;
        struct timespec delay = {(time_t)(left / 1000), (long)(left % 1000) * 1000000L};
        (void)pthread_cond_timedwait_relative_np(&vb_cond, &vb_lock, &delay);
    }
}

static void *vb_run(void *argument)
{
    (void)argument;
    static fx_vibe_pattern pattern;
    unsigned seen = 0;
    (void)pthread_mutex_lock(&vb_lock);
    for (;;) {
        while (vb_generation == seen) (void)pthread_cond_wait(&vb_cond, &vb_lock);
        seen = vb_generation;
        pattern = vb_pending;
        void *completion = vb_pending_completion;
        vb_pending_completion = NULL;
        bool interrupted = false;
        uint64_t end = vb_now_ms(); /* 照整份 pattern 的時間軸排，不讓每段的排程延遲累積 */
        for (unsigned index = 0; index < pattern.count && !interrupted; index++) {
            end += pattern.segment[index].ms;
            if (!pattern.segment[index].on) {
                interrupted = vb_wait(seen, end);
                continue;
            }
            /* 下一段也是開（換振幅）：多要一點時間、段尾不停，核心只改振幅不重新啟動 */
            bool next_on = index + 1 < pattern.count && pattern.segment[index + 1].on;
            for (;;) {
                uint64_t now = vb_now_ms();
                if (now >= end) break;
                unsigned remaining = (unsigned)(end - now);
                bool written = vb_write(fx_vibe_request_ms(remaining + (next_on ? FX_VIBE_SLACK_MS : 0)),
                    pattern.segment[index].level);
                if (!written) {
                    interrupted = true;
                    break;
                }
                if (vb_wait(seen, now + fx_vibe_renew_ms(remaining))) {
                    interrupted = true;
                    break;
                }
            }
            if (!next_on || interrupted) (void)vb_write(0, 0);
        }
        (void)pthread_mutex_unlock(&vb_lock);
        vb_finish(completion);
        (void)pthread_mutex_lock(&vb_lock);
    }
    return NULL;
}

static bool vb_start_worker(void)
{
    if (__atomic_load_n(&vb_worker, __ATOMIC_ACQUIRE)) return true;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    pthread_t thread;
    int result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (result == 0) result = pthread_create(&thread, &attributes, vb_run, NULL);
    (void)pthread_attr_destroy(&attributes);
    if (result != 0) {
        vb_say("vibrate worker pthread_create rc=%d\n", result);
        return false;
    }
    __atomic_store_n(&vb_worker, 1u, __ATOMIC_RELEASE);
    return true;
}

static bool vb_play(const fx_vibe_pattern *pattern, const void *completion)
{
    void *copied = completion ? _Block_copy(completion) : NULL;
    if (completion && copied == NULL) return false;
    void *replaced = NULL;
    (void)pthread_mutex_lock(&vb_lock);
    bool ready = vb_start_worker();
    if (ready) {
        replaced = vb_pending_completion;
        vb_pending = *pattern;
        vb_pending_completion = copied;
        vb_generation++;
        (void)pthread_cond_broadcast(&vb_cond);
    }
    (void)pthread_mutex_unlock(&vb_lock);
    if (!ready && copied) _Block_release(copied);
    vb_finish(replaced);
    return ready;
}

static void vb_cancel(void)
{
    (void)pthread_mutex_lock(&vb_lock);
    void *completion = vb_pending_completion;
    vb_pending_completion = NULL;
    fx_vibe_reset(&vb_pending);
    vb_generation++;
    (void)pthread_cond_broadcast(&vb_cond);
    (void)vb_write(0, 0);
    (void)pthread_mutex_unlock(&vb_lock);
    vb_finish(completion);
}

static void vb_pulse(unsigned ms)
{
    fx_vibe_pattern pattern;
    fx_vibe_reset(&pattern);
    (void)fx_vibe_append(&pattern, true, ms);
    (void)fx_vibe_finish(&pattern);
    fx_vibe_scale(&pattern, __atomic_load_n(&vb_amp_max, __ATOMIC_RELAXED));
    (void)vb_play(&pattern, NULL);
}

/* ---- VibePattern 字典 ---- */
static bool vb_number(CFTypeRef value, long long *out, double *real)
{
    if (value == NULL) return false;
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        *out = CFBooleanGetValue((CFBooleanRef)value) ? 1 : 0;
        if (real) *real = (double)*out;
        return true;
    }
    if (CFGetTypeID(value) != CFNumberGetTypeID()) return false;
    double number = 0.0;
    if (!CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &number) || number != number) return false;
    if (real) *real = number;
    *out = number > 1e9 ? 1000000000ll : number < -1e9 ? -1000000000ll : (long long)number;
    return true;
}

/* FigVibrator 舊格式的秒數（CMTimeMakeWithSeconds 用的）轉毫秒；沒有或不是數字回 -1。 */
static long long vb_seconds_ms(CFDictionaryRef dictionary, CFStringRef key)
{
    long long whole = 0;
    double seconds = 0.0;
    if (!vb_number(CFDictionaryGetValue(dictionary, key), &whole, &seconds)) return -1;
    if (seconds < 0.0) return -1;
    if (seconds > 600.0) seconds = 600.0;
    return (long long)(seconds * 1000.0 + 0.5);
}

/* Envelope 點列（{Level, Time}）讀進陣列；格式不對回 0（呼叫端改用整段 Duration）。 */
static unsigned vb_envelope_points(CFTypeRef value, double *time_s, double *level)
{
    if (value == NULL || CFGetTypeID(value) != CFArrayGetTypeID()) return 0;
    CFIndex count = CFArrayGetCount((CFArrayRef)value);
    if (count <= 0 || count > FX_VIBE_ENVELOPE_POINTS) return 0;
    for (CFIndex index = 0; index < count; index++) {
        CFTypeRef item = CFArrayGetValueAtIndex((CFArrayRef)value, index);
        long long ignored = 0;
        if (item == NULL || CFGetTypeID(item) != CFDictionaryGetTypeID()) return 0;
        if (!vb_number(CFDictionaryGetValue((CFDictionaryRef)item, CFSTR("Time")), &ignored, &time_s[index]) ||
            !vb_number(CFDictionaryGetValue((CFDictionaryRef)item, CFSTR("Level")), &ignored, &level[index]))
            return 0;
    }
    return (unsigned)count;
}

static int vb_atoms(CFTypeRef value, fx_vibe_pattern *pattern)
{
    if (value == NULL || CFGetTypeID(value) != CFArrayGetTypeID()) return 0;
    CFArrayRef atoms = (CFArrayRef)value;
    CFIndex count = CFArrayGetCount(atoms);
    if (count <= 0 || count > FX_VIBE_SEGMENTS) return 0;
    long long cursor = 0; /* 原時間軸上前一個 atom 的結尾；Envelope 縮短後下一個 atom 仍照原時間起 */
    bool shaped = __atomic_load_n(&vb_amp_max, __ATOMIC_RELAXED) != 0;
    bool envelope = shaped || __atomic_load_n(&vb_envelope, __ATOMIC_RELAXED) != 0;
    for (CFIndex index = 0; index < count; index++) {
        CFTypeRef item = CFArrayGetValueAtIndex(atoms, index);
        if (item == NULL || CFGetTypeID(item) != CFDictionaryGetTypeID()) return 0;
        CFDictionaryRef atom = (CFDictionaryRef)item;
        long long start = vb_seconds_ms(atom, CFSTR("StartTime"));
        long long duration = vb_seconds_ms(atom, CFSTR("Duration"));
        if (start < 0 && CFDictionaryContainsKey(atom, CFSTR("StartTime"))) return 0;
        if (start < 0) start = envelope ? cursor : pattern->total_ms;
        if (duration <= 0) {
            CFTypeRef type = CFDictionaryGetValue(atom, CFSTR("Type"));
            if (type && CFGetTypeID(type) == CFStringGetTypeID() &&
                (CFEqual(type, CFSTR("tap")) || CFEqual(type, CFSTR("Minitap")) ||
                 CFEqual(type, CFSTR("Microtap")) || CFEqual(type, CFSTR("LPWidetap"))))
                duration = VB_TAP_MS;
        }
        if (duration <= 0) return 0;
        long long on_start = start, on_ms = duration;
        if (start + duration > cursor) cursor = start + duration;
        if (envelope) {
            double times[FX_VIBE_ENVELOPE_POINTS], levels[FX_VIBE_ENVELOPE_POINTS];
            unsigned points = vb_envelope_points(CFDictionaryGetValue(atom, CFSTR("Envelope")), times, levels);
            if (shaped && points) {
                /* 振幅模式：整段照包絡調振幅；點列不對才退回下面的整段 Duration */
                unsigned before = pattern->count;
                long long shaped_on = fx_vibe_shape(pattern, start, duration, times, levels, points);
                if (__atomic_fetch_add(&vb_envelope_logs, 1u, __ATOMIC_RELAXED) < VB_PLAY_LOGS)
                    vb_say("vibe shape atom=%ld points=%u start=%lldms duration=%lldms on=%lldms segments=%u..%u\n",
                        (long)index, points, start, duration, shaped_on, before, pattern->count);
                if (pattern->truncated) return 3;
                if (shaped_on >= 0) continue;
            }
            long long offset = 0;
            long long effective = points ? fx_vibe_envelope(times, levels, points, duration, &offset) : -1;
            if (points && __atomic_fetch_add(&vb_envelope_logs, 1u, __ATOMIC_RELAXED) < VB_PLAY_LOGS)
                vb_say("vibe envelope atom=%ld points=%u duration=%lldms effective=%lldms offset=%lldms\n",
                    (long)index, points, duration, effective, offset);
            if (shaped && points) effective = -1; /* 包絡無效：整段 Duration，振幅照 atom 上限 */
            if (effective == 0) continue;
            if (effective > 0) {
                on_start = start + offset;
                on_ms = effective;
                /* 重疊的 atom：起點不能早於最後一個開段，否則 fx_vibe_atom 會整份拒絕 */
                if (pattern->count && on_start < (long long)pattern->total_ms) {
                    long long floor = (long long)pattern->total_ms - pattern->segment[pattern->count - 1].ms;
                    if (on_start < floor) on_start = floor;
                }
            }
        }
        if (!fx_vibe_atom(pattern, on_start, on_ms)) return pattern->truncated ? 3 : 0;
    }
    return 3;
}

/* 回傳 0 不能解析，1 解析到 VibePattern，2 解析到 OnDuration 舊格式（*intensity < 0 表示沒給）。 */
static int vb_parse(CFTypeRef object, fx_vibe_pattern *pattern, double *intensity)
{
    fx_vibe_reset(pattern);
    *intensity = -1.0;
    if (object == NULL || CFGetTypeID(object) != CFDictionaryGetTypeID()) return 0;
    CFDictionaryRef dictionary = (CFDictionaryRef)object;
    CFTypeRef level = CFDictionaryGetValue(dictionary, CFSTR("Intensity"));
    long long ignored = 0;
    if (level && !vb_number(level, &ignored, intensity)) *intensity = -1.0;
    CFTypeRef atoms = CFDictionaryGetValue(dictionary, CFSTR("Atoms"));
    if (atoms) return vb_atoms(atoms, pattern);
    CFTypeRef list = CFDictionaryGetValue(dictionary, CFSTR("VibePattern"));
    if (list == NULL || CFGetTypeID(list) != CFArrayGetTypeID()) {
        long long on = vb_seconds_ms(dictionary, CFSTR("OnDuration"));
        if (on <= 0) return 0;
        (void)fx_vibe_periodic(pattern, on, vb_seconds_ms(dictionary, CFSTR("OffDuration")),
            vb_seconds_ms(dictionary, CFSTR("TotalDuration")));
        return 2;
    }
    CFIndex count = CFArrayGetCount((CFArrayRef)list);
    for (CFIndex index = 0; index + 1 < count; index += 2) {
        long long on = 0, ms = 0;
        if (!vb_number(CFArrayGetValueAtIndex((CFArrayRef)list, index), &on, NULL) ||
            !vb_number(CFArrayGetValueAtIndex((CFArrayRef)list, index + 1), &ms, NULL))
            return 0;
        if (!fx_vibe_append(pattern, on != 0, ms)) break;
    }
    return 1;
}

static void vb_describe(const char *what, CFTypeRef object)
{
    char text[800] = "(null)";
    if (object) {
        CFStringRef description = CFCopyDescription(object);
        text[0] = 0;
        if (description) {
            CFIndex length = CFStringGetLength(description), used = 0;
            CFIndex characters = length > 760 ? 760 : length;
            (void)CFStringGetBytes(description, CFRangeMake(0, characters), kCFStringEncodingUTF8, '?', false,
                (UInt8 *)text, (CFIndex)sizeof(text) - 4, &used);
            text[used] = 0;
            if (characters < length) strcat(text, "...");
            CFRelease(description);
        }
        for (char *cursor = text; *cursor; cursor++)
            if (*cursor == '\n' || *cursor == '\r' || *cursor == '\t') *cursor = ' ';
    }
    vb_say("%s %s\n", what, text);
}

/* ---- 型別檢查 ---- */
static const char *vb_skip_qualifiers(const char *type)
{
    while (*type && strchr("rnNoORV", *type)) type++;
    return type;
}

static bool vb_integer_class(const char *type)
{
    type = vb_skip_qualifiers(type);
    return *type && strchr("@#:*^cCiIsSlLqQB", *type) != NULL;
}

static bool vb_float_class(const char *type)
{
    type = vb_skip_qualifiers(type);
    return *type == 'f' || *type == 'd';
}

static bool vb_result_ok(Method method)
{
    char type[32];
    method_getReturnType(method, type, sizeof(type));
    return vb_skip_qualifiers(type)[0] == 'v' || vb_integer_class(type);
}

static char vb_arg(Method method, unsigned index)
{
    char type[32];
    method_getArgumentType(method, index, type, sizeof(type));
    if (vb_float_class(type)) return vb_skip_qualifiers(type)[0];
    return vb_integer_class(type) ? 'x' : '?';
}

/* ---- 一般掛勾：所有參數都在整數暫存器的方法，原樣轉給原實作 ---- */
typedef uintptr_t (*vb_forward)(id, SEL, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

typedef struct {
    unsigned role;
    const char *class_name;
    const char *selector;
    unsigned action;
    IMP original;
    unsigned calls;
    unsigned done;
} vb_hook;

static vb_hook vb_hooks[] = {
    {VB_MEDIA, "AVHapticServer", "cancelPatternWithOptions:", VB_CANCEL, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "loadVibePattern:reply:", VB_LOG, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "loadHapticSequenceFromEvents:reply:", VB_LOG, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "loadHapticSequenceFromData:reply:", VB_LOG, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "loadHapticEvent:reply:", VB_LOG, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "prepareHapticSequence:reply:", VB_LOG, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "startRunning:", VB_LOG, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "stopRunning:", VB_LOG, NULL, 0, 0},
    {VB_MEDIA, "AVHapticServerInstance", "queryCapabilities:reply:", VB_LOG, NULL, 0, 0},
    {VB_SPRINGBOARD, "SBFTapticEngine", "actuateFeedback:", VB_PULSE, NULL, 0, 0},
    {VB_SPRINGBOARD, "_UITapticEngine", "actuateFeedback:", VB_PULSE, NULL, 0, 0},
};
enum { VB_HOOKS = sizeof(vb_hooks) / sizeof(vb_hooks[0]) };

static uintptr_t vb_dispatch(unsigned index, id self, SEL selector, uintptr_t a, uintptr_t b, uintptr_t c,
    uintptr_t d, uintptr_t e, uintptr_t f)
{
    vb_hook *hook = &vb_hooks[index];
    vb_forward original = (vb_forward)__atomic_load_n(&hook->original, __ATOMIC_ACQUIRE);
    unsigned call = __atomic_fetch_add(&hook->calls, 1u, __ATOMIC_RELAXED);
    const bool suppressed = hook->action == VB_PULSE && __atomic_load_n(&vb_native_pcm, __ATOMIC_ACQUIRE) != 0u;
    if (hook->action == VB_PULSE && !suppressed) vb_pulse(VB_TAP_MS);
    if (hook->action == VB_CANCEL) vb_cancel();
    if (call < VB_LOG_CALLS) {
        const char *first = "-";
        Method method = class_getInstanceMethod(object_getClass(self), selector);
        char type[8] = "";
        if (method && method_getNumberOfArguments(method) > 2) {
            method_getArgumentType(method, 2, type, sizeof(type));
            if (vb_skip_qualifiers(type)[0] == '@' && a) first = object_getClassName((id)a);
        }
        vb_say("haptic %s %s call=%u arg=%s/%s%s\n", hook->class_name, hook->selector, call + 1, type, first,
            hook->action == VB_PULSE ? (suppressed ? " pulse=suppressed(pcm)" : " pulse=15ms") :
            hook->action == VB_CANCEL ? " cancel" : "");
        if (hook->action == VB_LOG && call < 2 && strcmp(type, "@") == 0 && a) vb_describe("haptic arg", (CFTypeRef)a);
    }
    return original(self, selector, a, b, c, d, e, f);
}

#define VB_TRAMPOLINE(n) \
    static uintptr_t vb_trampoline_##n(id self, SEL selector, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, \
        uintptr_t e, uintptr_t f) \
    { \
        return vb_dispatch(n, self, selector, a, b, c, d, e, f); \
    }
VB_TRAMPOLINE(0)
VB_TRAMPOLINE(1)
VB_TRAMPOLINE(2)
VB_TRAMPOLINE(3)
VB_TRAMPOLINE(4)
VB_TRAMPOLINE(5)
VB_TRAMPOLINE(6)
VB_TRAMPOLINE(7)
VB_TRAMPOLINE(8)
VB_TRAMPOLINE(9)
VB_TRAMPOLINE(10)
static const IMP vb_trampolines[] = {
    (IMP)vb_trampoline_0, (IMP)vb_trampoline_1, (IMP)vb_trampoline_2, (IMP)vb_trampoline_3,
    (IMP)vb_trampoline_4, (IMP)vb_trampoline_5, (IMP)vb_trampoline_6, (IMP)vb_trampoline_7,
    (IMP)vb_trampoline_8, (IMP)vb_trampoline_9, (IMP)vb_trampoline_10,
};
_Static_assert(sizeof(vb_trampolines) / sizeof(vb_trampolines[0]) == VB_HOOKS, "one trampoline per hook");

static void vb_install(unsigned index)
{
    vb_hook *hook = &vb_hooks[index];
    if (hook->role != vb_role || __atomic_load_n(&hook->done, __ATOMIC_ACQUIRE)) return;
    Class target = objc_getClass(hook->class_name);
    if (target == NULL) return;
    Method method = class_getInstanceMethod(target, sel_registerName(hook->selector));
    if (method == NULL) return;
    if (__atomic_exchange_n(&hook->done, 1u, __ATOMIC_ACQ_REL)) return;
    unsigned count = method_getNumberOfArguments(method);
    bool usable = count >= 2 && count <= 8 && vb_result_ok(method);
    for (unsigned argument = 2; usable && argument < count; argument++) usable = vb_arg(method, argument) == 'x';
    if (!usable) {
        vb_say("haptic %s %s refused encoding %s\n", hook->class_name, hook->selector,
            method_getTypeEncoding(method) ? method_getTypeEncoding(method) : "?");
        return;
    }
    __atomic_store_n(&hook->original, method_getImplementation(method), __ATOMIC_RELEASE);
    (void)method_setImplementation(method, vb_trampolines[index]);
    vb_say("haptic %s %s hooked %s\n", hook->class_name, hook->selector, method_getTypeEncoding(method));
}

/* ---- mediaserverd：VibeSynthEngine ---- */
typedef uintptr_t (*vb_play_fp_t)(id, SEL, id, double, id, uintptr_t, double, id);
typedef uintptr_t (*vb_play_int_t)(id, SEL, id, double, id, uintptr_t, uintptr_t, id);
static IMP vb_play_original;
static char vb_gain_type, vb_time_type;
static unsigned vb_play_calls, vb_play_done;

static double vb_fp_value(double raw, char type)
{
    if (type == 'd') return raw;
    union {
        double d;
        uint64_t u;
    } bits = {raw};
    union {
        uint32_t u;
        float f;
    } single = {(uint32_t)bits.u};
    return (double)single.f;
}

static bool vb_vibe_request(id pattern, double gain, uintptr_t flags, double when, unsigned call,
    const void *completion)
{
    fx_vibe_pattern parsed;
    double intensity = -1.0;
    int ok = vb_parse((CFTypeRef)pattern, &parsed, &intensity);
    unsigned on_ms = ok ? fx_vibe_finish(&parsed) : 0;
    bool silent = intensity == 0.0 || gain <= 0.0;
    bool fallback = false;
    if (!ok && pattern && !silent) {
        /* 認不得的格式：系統確實要震，至少給一下 VB_FALLBACK_MS，紀錄裡留字典內容 */
        fx_vibe_reset(&parsed);
        (void)fx_vibe_append(&parsed, true, VB_FALLBACK_MS);
        on_ms = fx_vibe_finish(&parsed);
        fallback = true;
    }
    if (silent) fx_vibe_reset(&parsed);
    unsigned amplitude = fx_vibe_amplitude(__atomic_load_n(&vb_amp_max, __ATOMIC_RELAXED), intensity, gain);
    fx_vibe_scale(&parsed, amplitude);
    unsigned low = 0, high = 0;
    for (unsigned index = 0; index < parsed.count; index++) {
        unsigned level = parsed.segment[index].level;
        if (!parsed.segment[index].on) continue;
        if (!low || level < low) low = level;
        if (level > high) high = level;
    }
    bool queued = vb_play(&parsed, completion);
    if (call < VB_PLAY_LOGS) {
        vb_say("vibe play call=%u uptime-ms=%llu pattern=%s ok=%d segments=%u total=%ums on=%ums truncated=%u intensity=%.2f "
            "gain=%.2f flags=%#lx at=%.3f amplitude=%u level=%u..%u%s%s\n", call + 1, (unsigned long long)vb_now_ms(),
            pattern ? object_getClassName(pattern) : "nil", ok,
            parsed.count, parsed.total_ms, on_ms, parsed.truncated, intensity, gain, (unsigned long)flags, when,
            amplitude, low, high, silent ? " silent" : "", fallback ? " fallback" : "");
        if (call < 6) vb_describe("vibe pattern", (CFTypeRef)pattern);
    }
    return queued;
}

static uintptr_t vb_play_fp(id self, SEL selector, id pattern, double gain, id synchronizer, uintptr_t flags,
    double when, id handler)
{
    unsigned call = __atomic_fetch_add(&vb_play_calls, 1u, __ATOMIC_RELAXED);
    (void)vb_vibe_request(pattern, vb_fp_value(gain, vb_gain_type), flags, vb_fp_value(when, vb_time_type), call, NULL);
    uintptr_t result = ((vb_play_fp_t)vb_play_original)(self, selector, pattern, gain, synchronizer, flags, when,
        handler);
    if (call < VB_PLAY_LOGS) vb_say("vibe play call=%u original=%#lx\n", call + 1, (unsigned long)result);
    return result;
}

static uintptr_t vb_play_int(id self, SEL selector, id pattern, double gain, id synchronizer, uintptr_t flags,
    uintptr_t when, id handler)
{
    unsigned call = __atomic_fetch_add(&vb_play_calls, 1u, __ATOMIC_RELAXED);
    (void)vb_vibe_request(pattern, vb_fp_value(gain, vb_gain_type), flags, (double)when, call, NULL);
    uintptr_t result = ((vb_play_int_t)vb_play_original)(self, selector, pattern, gain, synchronizer, flags, when,
        handler);
    if (call < VB_PLAY_LOGS) vb_say("vibe play call=%u original=%#lx\n", call + 1, (unsigned long)result);
    return result;
}

static void vb_install_play(void)
{
    if (__atomic_load_n(&vb_play_done, __ATOMIC_ACQUIRE)) return;
    Class target = objc_getClass("AVHapticServer");
    if (target == NULL) return;
    Method method = class_getInstanceMethod(target,
        sel_registerName("playVibePattern:gain:synchronizer:flags:atTime:completionHandler:"));
    if (method == NULL) return;
    if (__atomic_exchange_n(&vb_play_done, 1u, __ATOMIC_ACQ_REL)) return;
    const char *encoding = method_getTypeEncoding(method) ? method_getTypeEncoding(method) : "?";
    char gain = method_getNumberOfArguments(method) == 8 ? vb_arg(method, 3) : '?';
    char when = method_getNumberOfArguments(method) == 8 ? vb_arg(method, 6) : '?';
    bool usable = method_getNumberOfArguments(method) == 8 && vb_result_ok(method) && vb_arg(method, 2) == 'x' &&
        (gain == 'f' || gain == 'd') && vb_arg(method, 4) == 'x' && vb_arg(method, 5) == 'x' &&
        (when == 'f' || when == 'd' || when == 'x') && vb_arg(method, 7) == 'x';
    if (!usable) {
        vb_say("vibe playVibePattern refused encoding %s\n", encoding);
        return;
    }
    vb_gain_type = gain;
    vb_time_type = when;
    vb_play_original = method_getImplementation(method);
    (void)method_setImplementation(method, when == 'x' ? (IMP)vb_play_int : (IMP)vb_play_fp);
    vb_say("vibe playVibePattern hooked %s\n", encoding);
}

/* ---- SpringBoard：supportsFeedbackActuation ---- */
static IMP vb_supports_original;
static unsigned vb_supports_done, vb_supports_logged;

static bool vb_supports(id self, SEL selector)
{
    bool original = ((bool (*)(id, SEL))vb_supports_original)(self, selector);
    if (!__atomic_exchange_n(&vb_supports_logged, 1u, __ATOMIC_ACQ_REL))
        vb_say("haptic SBFTapticEngine supportsFeedbackActuation original=%d reported=1\n", original);
    return true;
}

static void vb_install_supports(void)
{
    if (__atomic_load_n(&vb_supports_done, __ATOMIC_ACQUIRE)) return;
    Class target = objc_getClass("SBFTapticEngine");
    if (target == NULL) return;
    Method method = class_getInstanceMethod(target, sel_registerName("supportsFeedbackActuation"));
    if (method == NULL) return;
    if (__atomic_exchange_n(&vb_supports_done, 1u, __ATOMIC_ACQ_REL)) return;
    char type[8];
    method_getReturnType(method, type, sizeof(type));
    if (method_getNumberOfArguments(method) != 2 || (type[0] != 'B' && type[0] != 'c')) {
        vb_say("haptic supportsFeedbackActuation refused encoding %s\n", method_getTypeEncoding(method));
        return;
    }
    vb_supports_original = method_getImplementation(method);
    (void)method_setImplementation(method, (IMP)vb_supports);
}

static IMP vb_feedback_engine_original, vb_feedback_play_original, vb_feedback_now_original;
static unsigned vb_feedback_engine_done, vb_feedback_play_done, vb_feedback_now_done;
static unsigned vb_feedback_engine_calls, vb_feedback_play_calls, vb_feedback_now_calls;
static IMP vb_feedback_core_init_original, vb_feedback_core_finish_original, vb_feedback_request_original;
static unsigned vb_feedback_core_init_calls, vb_feedback_core_finish_calls, vb_feedback_request_calls;
static IMP vb_feedback_core_create_original;
static unsigned vb_feedback_core_create_calls;

static intptr_t vb_feedback_integer(id target, const char *name)
{
    if (target == NULL) return -1;
    SEL selector = sel_registerName(name);
    Method method = class_getInstanceMethod(object_getClass(target), selector);
    char type[32];
    if (method == NULL || method_getNumberOfArguments(method) != 2) return -1;
    method_getReturnType(method, type, sizeof(type));
    const char *result = vb_skip_qualifiers(type);
    IMP implementation = method_getImplementation(method);
    switch (*result) {
        case 'c': return ((int8_t (*)(id, SEL))implementation)(target, selector);
        case 'C': return ((uint8_t (*)(id, SEL))implementation)(target, selector);
        case 's': return ((int16_t (*)(id, SEL))implementation)(target, selector);
        case 'S': return ((uint16_t (*)(id, SEL))implementation)(target, selector);
        case 'i': return ((int32_t (*)(id, SEL))implementation)(target, selector);
        case 'I': return ((uint32_t (*)(id, SEL))implementation)(target, selector);
        case 'l': case 'q': return ((intptr_t (*)(id, SEL))implementation)(target, selector);
        case 'L': case 'Q': return (intptr_t)((uintptr_t (*)(id, SEL))implementation)(target, selector);
        case 'B': return ((bool (*)(id, SEL))implementation)(target, selector);
        default: return -1;
    }
}

/*
 * ---- UI 回退（fuxi,haptic-ui）----
 * CoreHaptics engine 啟動失敗時，UIKit 不會走到 -[_UIFeedbackEngine _playFeedback:atTime:withCompletionBlock:]
 * （2026-11-17 各趟 CoreHaptics engine 的播放紀錄都是 0 次），所以改在 UIFeedbackGenerator 的請求層判斷：
 * 問 +[_UIFeedbackEngine engineForFeedback:] 這次會交給誰，是 CoreHaptics 家族才補震；SystemSound engine
 * 已經經 mediaserverd 的 VibeSynth 震，不重複。40 ms 內的連續請求合併成一次。
 */
enum { VB_UI_GAP_MS = 40, VB_UI_LOGS = 24, VB_APP_SOUND = 1519 };
static unsigned vb_ui_calls, vb_ui_pulses;
static uint64_t vb_ui_last_ms;
typedef void (*vb_sound_t)(uint32_t);
static dispatch_once_t vb_sound_once;
static vb_sound_t vb_sound_play;

static void vb_sound_resolve(void *context)
{
    (void)context;
    void *toolbox = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY);
    vb_sound_play = toolbox ? (vb_sound_t)dlsym(toolbox, "AudioServicesPlaySystemSound") : NULL;
}

static void vb_app_sound(void *context)
{
    (void)context;
    dispatch_once_f(&vb_sound_once, NULL, vb_sound_resolve);
    if (vb_sound_play) vb_sound_play(VB_APP_SOUND);
}

static id vb_engine_for(id feedback)
{
    Class target = objc_getClass("_UIFeedbackEngine");
    SEL selector = sel_registerName("engineForFeedback:");
    Method method = target && feedback ? class_getClassMethod(target, selector) : NULL;
    char result[8];
    if (method == NULL || method_getNumberOfArguments(method) != 3) return NULL;
    method_getReturnType(method, result, sizeof(result));
    if (vb_skip_qualifiers(result)[0] != '@') return NULL;
    IMP implementation = vb_feedback_engine_original ? vb_feedback_engine_original : method_getImplementation(method);
    return ((id (*)(id, SEL, id))implementation)((id)target, selector, feedback);
}

static void vb_ui_fallback(id generator, id feedback)
{
    bool app = vb_role == VB_APP;
    unsigned ms = __atomic_load_n(&vb_ui_ms, __ATOMIC_RELAXED);
    if (app ? !__atomic_load_n(&vb_app_fallback, __ATOMIC_RELAXED) : !ms) return;
    if (__atomic_load_n(&vb_core_ok, __ATOMIC_ACQUIRE)) return;
    if (vb_feedback_integer(generator, "_isEnabled") == 0 || vb_feedback_integer(generator, "_isMuted") == 1) return;
    id engine = vb_engine_for(feedback);
    const char *name = engine ? object_getClassName(engine) : NULL;
    if (name == NULL || strstr(name, "CoreHaptics") == NULL) return;
    uint64_t now = vb_now_ms();
    uint64_t last = __atomic_load_n(&vb_ui_last_ms, __ATOMIC_RELAXED);
    const char *outcome = "coalesced";
    if ((last == 0 || now - last >= VB_UI_GAP_MS) &&
        __atomic_compare_exchange_n(&vb_ui_last_ms, &last, now, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        if (app) dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), NULL, vb_app_sound);
        else vb_pulse(ms);
        (void)__atomic_fetch_add(&vb_ui_pulses, 1u, __ATOMIC_RELAXED);
        outcome = app ? "sound-1519" : "pulse";
    }
    if (__atomic_fetch_add(&vb_ui_calls, 1u, __ATOMIC_RELAXED) < VB_UI_LOGS)
        vb_say("ui fallback uptime-ms=%llu generator=%s feedback=%s engine=%s result=%s pulses=%u\n",
            (unsigned long long)now, object_getClassName(generator),
            feedback ? object_getClassName(feedback) : "nil", name, outcome,
            __atomic_load_n(&vb_ui_pulses, __ATOMIC_RELAXED));
}

static bool vb_feedback_core_init(id self, SEL selector)
{
    bool result = ((bool (*)(id, SEL))vb_feedback_core_init_original)(self, selector);
    unsigned call = __atomic_fetch_add(&vb_feedback_core_init_calls, 1u, __ATOMIC_RELAXED);
    if (call < VB_PLAY_LOGS)
        vb_say("feedback core init call=%u uptime-ms=%llu engine=%s initialized=%u enabled=%ld suspended=%ld "
            "state=%ld\n", call + 1, (unsigned long long)vb_now_ms(), object_getClassName(self), result,
            (long)vb_feedback_integer(self, "isEnabled"),
            (long)vb_feedback_integer(self, "_internal_isSuspended"), (long)vb_feedback_integer(self, "_state"));
    return result;
}

static id vb_feedback_core_create(id self, SEL selector)
{
    id error = ((id (*)(id, SEL))vb_feedback_core_create_original)(self, selector);
    unsigned call = __atomic_fetch_add(&vb_feedback_core_create_calls, 1u, __ATOMIC_RELAXED);
    if (error == NULL) __atomic_store_n(&vb_core_ok, 1u, __ATOMIC_RELEASE);
    if (call < VB_PLAY_LOGS) {
        vb_say("feedback core create call=%u uptime-ms=%llu engine=%s error-present=%u error=%ld\n",
            call + 1, (unsigned long long)vb_now_ms(), object_getClassName(self), error != NULL,
            (long)vb_feedback_integer(error, "code"));
        FuxiErrorAuditEntry entries[FUXI_ERROR_AUDIT_DEPTH];
        unsigned count = fuxi_error_audit((CFTypeRef)error, entries);
        for (unsigned depth = 0; depth < count; depth++)
            vb_say("feedback core error call=%u depth=%u domain-id=%08x code=%ld reason=%s\n",
                call + 1, depth, entries[depth].domain, (long)entries[depth].code, entries[depth].reason);
    }
    return error;
}

static void vb_feedback_core_finish(id self, SEL selector, id error, id completion)
{
    unsigned call = __atomic_fetch_add(&vb_feedback_core_finish_calls, 1u, __ATOMIC_RELAXED);
    if (call < VB_PLAY_LOGS)
        vb_say("feedback core finish call=%u uptime-ms=%llu engine=%s error-present=%u error=%ld state=%ld\n",
            call + 1, (unsigned long long)vb_now_ms(), object_getClassName(self), error != NULL,
            (long)vb_feedback_integer(error, "code"), (long)vb_feedback_integer(self, "_state"));
    ((void (*)(id, SEL, id, id))vb_feedback_core_finish_original)(self, selector, error, completion);
}

static void vb_feedback_request(id self, SEL selector, id feedback, double interval, double *since, bool regular)
{
    unsigned call = __atomic_fetch_add(&vb_feedback_request_calls, 1u, __ATOMIC_RELAXED);
    if (call < 32)
        vb_say("feedback request call=%u uptime-ms=%llu generator=%s feedback=%s enabled=%ld muted=%ld "
            "output=%ld active=%ld interval=%.3f regular=%u\n", call + 1, (unsigned long long)vb_now_ms(),
            object_getClassName(self), feedback ? object_getClassName(feedback) : "nil",
            (long)vb_feedback_integer(self, "_isEnabled"), (long)vb_feedback_integer(self, "_isMuted"),
            (long)vb_feedback_integer(self, "_outputMode"), (long)vb_feedback_integer(self, "activationCount"),
            interval, regular);
    vb_ui_fallback(self, feedback); /* 原實作可能同步等 CoreHaptics 的 1 秒逾時，先震 */
    ((void (*)(id, SEL, id, double, double *, bool))vb_feedback_request_original)(
        self, selector, feedback, interval, since, regular);
}

/* 依 types 字串核對 ABI 才換實作：第 0 個字元是回傳值，其後是參數；'b' 代表 BOOL（B 或 c）。 */
typedef struct {
    const char *target, *selector, *types;
    IMP replacement, *original;
    unsigned done;
} vb_typed_hook;

static void vb_install_typed(const char *label, vb_typed_hook *hooks, size_t total)
{
    for (size_t index = 0; index < total; index++) {
        if (__atomic_load_n(&hooks[index].done, __ATOMIC_ACQUIRE)) continue;
        Class target = objc_getClass(hooks[index].target);
        Method method = target ? class_getInstanceMethod(target, sel_registerName(hooks[index].selector)) : NULL;
        if (method == NULL || __atomic_exchange_n(&hooks[index].done, 1u, __ATOMIC_ACQ_REL)) continue;
        size_t count = strlen(hooks[index].types);
        bool usable = method_getNumberOfArguments(method) == count + 1;
        for (unsigned argument = 0; usable && argument < count; argument++) {
            char type[64];
            if (argument == 0) method_getReturnType(method, type, sizeof(type));
            else method_getArgumentType(method, argument + 1, type, sizeof(type));
            char actual = vb_skip_qualifiers(type)[0];
            char expected = hooks[index].types[argument];
            usable = expected == 'b' ? actual == 'B' || actual == 'c' : actual == expected;
        }
        vb_say("%s hook %s %s %s\n", label, hooks[index].target, hooks[index].selector,
            usable ? "installed" : "refused ABI");
        if (!usable) continue;
        *hooks[index].original = method_getImplementation(method);
        (void)method_setImplementation(method, hooks[index].replacement);
    }
}

/* ---- engine 層播放紀錄（只記；回退在 vb_ui_fallback） ---- */
static IMP vb_engine_play_original;
static unsigned vb_engine_play_calls;

/*
 * -[_UIFeedbackEngine _playFeedback:atTime:withCompletionBlock:] 只在基底類別實作（UIKitCore 20H392 核對過）。
 * engine 啟動成功才會走到這裡，所以 CoreHaptics 失敗時看不到；留著確認 SystemSound／修好後的 CoreHaptics 路徑。
 */
static void vb_engine_play(id self, SEL selector, id feedback, double when, id completion)
{
    unsigned call = __atomic_fetch_add(&vb_engine_play_calls, 1u, __ATOMIC_RELAXED);
    const char *engine = object_getClassName(self);
    if (call < VB_UI_LOGS)
        vb_say("feedback engine play call=%u uptime-ms=%llu engine=%s feedback=%s at=%.3f state=%ld core-ok=%u "
            "pulses=%u\n", call + 1, (unsigned long long)vb_now_ms(), engine ? engine : "?",
            feedback ? object_getClassName(feedback) : "nil", when, (long)vb_feedback_integer(self, "_state"),
            __atomic_load_n(&vb_core_ok, __ATOMIC_ACQUIRE), __atomic_load_n(&vb_ui_pulses, __ATOMIC_RELAXED));
    ((void (*)(id, SEL, id, double, id))vb_engine_play_original)(self, selector, feedback, when, completion);
}

static void vb_install_core_feedback(void)
{
    static vb_typed_hook hooks[] = {
        {"_UIFeedbackCoreHapticsEngine", "_internal_initializeCoreHapticsEngine", "b",
            (IMP)vb_feedback_core_init, &vb_feedback_core_init_original, 0},
        {"_UIFeedbackCoreHapticsEngine", "_internal_createCoreHapticsEngine", "@",
            (IMP)vb_feedback_core_create, &vb_feedback_core_create_original, 0},
        {"_UIFeedbackCoreHapticsEngine", "_internal_coreHapticsEngineFinishedWithError:completion:", "v@@",
            (IMP)vb_feedback_core_finish, &vb_feedback_core_finish_original, 0},
        {"UIFeedbackGenerator", "_playFeedback:withMinimumIntervalPassed:since:prefersRegularPace:", "v@d^b",
            (IMP)vb_feedback_request, &vb_feedback_request_original, 0},
        {"_UIFeedbackEngine", "_playFeedback:atTime:withCompletionBlock:", "v@d@",
            (IMP)vb_engine_play, &vb_engine_play_original, 0}
    };
    vb_install_typed("feedback core", hooks, sizeof(hooks) / sizeof(hooks[0]));
}

static id vb_feedback_engine(id self, SEL selector, id feedback)
{
    id engine = ((id (*)(id, SEL, id))vb_feedback_engine_original)(self, selector, feedback);
    unsigned call = __atomic_fetch_add(&vb_feedback_engine_calls, 1u, __ATOMIC_RELAXED);
    if (call < VB_PLAY_LOGS)
        vb_say("feedback engine call=%u feedback=%s engine=%s enabled=%ld types=%ld playable=%ld\n",
            call + 1, feedback ? object_getClassName(feedback) : "nil", engine ? object_getClassName(engine) : "nil",
            (long)vb_feedback_integer(engine, "isEnabled"),
            (long)vb_feedback_integer(feedback, "_effectiveEnabledFeedbackTypes"),
            (long)vb_feedback_integer(feedback, "_effectivePlayableFeedbackTypes"));
    return engine;
}

static void vb_feedback_play(id self, SEL selector, id data, double when, id feedback, uintptr_t types, id player,
    id completion)
{
    unsigned call = __atomic_fetch_add(&vb_feedback_play_calls, 1u, __ATOMIC_RELAXED);
    if (call < VB_PLAY_LOGS)
        vb_say("feedback dispatch call=%u engine=%s feedback=%s types=%#lx enabled=%ld suspended=%ld "
            "state=%ld at=%.3f\n", call + 1, object_getClassName(self),
            feedback ? object_getClassName(feedback) : "nil", (unsigned long)types,
            (long)vb_feedback_integer(self, "isEnabled"),
            (long)vb_feedback_integer(self, "_internal_isSuspended"),
            (long)vb_feedback_integer(self, "_state"), when);
    ((void (*)(id, SEL, id, double, id, uintptr_t, id, id))vb_feedback_play_original)(self, selector, data, when,
        feedback, types, player, completion);
}

static void vb_feedback_now(id self, SEL selector, id data, id feedback, id options)
{
    unsigned call = __atomic_fetch_add(&vb_feedback_now_calls, 1u, __ATOMIC_RELAXED);
    if (call < VB_PLAY_LOGS)
        vb_say("feedback sound call=%u uptime-ms=%llu sound-id=%ld data-types=%ld feedback-types=%ld "
            "options=%ld feedback=%s\n", call + 1, (unsigned long long)vb_now_ms(),
            (long)vb_feedback_integer(data, "effectiveSystemSoundID"),
            (long)vb_feedback_integer(data, "effectiveEnabledFeedbackTypes"),
            (long)vb_feedback_integer(feedback, "_effectiveEnabledFeedbackTypes"),
            (long)vb_feedback_integer(options, "count"), feedback ? object_getClassName(feedback) : "nil");
    ((void (*)(id, SEL, id, id, id))vb_feedback_now_original)(self, selector, data, feedback, options);
}

static void vb_install_feedback(void)
{
    vb_install_core_feedback();
    Class target = objc_getClass("_UIFeedbackEngine");
    if (target == NULL) return;
    if (!__atomic_load_n(&vb_feedback_engine_done, __ATOMIC_ACQUIRE)) {
        Method method = class_getClassMethod(target, sel_registerName("engineForFeedback:"));
        if (method && !__atomic_exchange_n(&vb_feedback_engine_done, 1u, __ATOMIC_ACQ_REL)) {
            char result[32];
            method_getReturnType(method, result, sizeof(result));
            if (method_getNumberOfArguments(method) == 3 && vb_skip_qualifiers(result)[0] == '@' &&
                vb_arg(method, 2) == 'x') {
                vb_feedback_engine_original = method_getImplementation(method);
                (void)method_setImplementation(method, (IMP)vb_feedback_engine);
                vb_say("feedback engineForFeedback hooked %s\n", method_getTypeEncoding(method));
            } else {
                vb_say("feedback engineForFeedback refused encoding %s\n", method_getTypeEncoding(method));
            }
        }
    }
    if (!__atomic_load_n(&vb_feedback_play_done, __ATOMIC_ACQUIRE)) {
        Method method = class_getInstanceMethod(target, sel_registerName(
            "_internal_playFeedbackData:atTime:feedback:effectiveFeedbackType:existingPlayer:withCompletionBlock:"));
        if (method && !__atomic_exchange_n(&vb_feedback_play_done, 1u, __ATOMIC_ACQ_REL)) {
            char result[32];
            method_getReturnType(method, result, sizeof(result));
            bool usable = method_getNumberOfArguments(method) == 8 && vb_skip_qualifiers(result)[0] == 'v' &&
                vb_arg(method, 2) == 'x' && vb_arg(method, 3) == 'd';
            for (unsigned argument = 4; usable && argument < 8; argument++)
                usable = vb_arg(method, argument) == 'x';
            if (usable) {
                vb_feedback_play_original = method_getImplementation(method);
                (void)method_setImplementation(method, (IMP)vb_feedback_play);
                vb_say("feedback internal dispatch hooked %s\n", method_getTypeEncoding(method));
            } else {
                vb_say("feedback internal dispatch refused encoding %s\n", method_getTypeEncoding(method));
            }
        }
    }
    if (!__atomic_load_n(&vb_feedback_now_done, __ATOMIC_ACQUIRE)) {
        Class sound = objc_getClass("_UIFeedbackSystemSoundEngine");
        Method method = sound ? class_getInstanceMethod(sound,
            sel_registerName("_internal_playFeedbackDataNow:forFeedback:withOptions:")) : NULL;
        if (method && !__atomic_exchange_n(&vb_feedback_now_done, 1u, __ATOMIC_ACQ_REL)) {
            char result[32];
            method_getReturnType(method, result, sizeof(result));
            bool usable = method_getNumberOfArguments(method) == 5 && vb_skip_qualifiers(result)[0] == 'v';
            for (unsigned argument = 2; usable && argument < 5; argument++)
                usable = vb_arg(method, argument) == 'x';
            if (usable) {
                vb_feedback_now_original = method_getImplementation(method);
                (void)method_setImplementation(method, (IMP)vb_feedback_now);
                vb_say("feedback system sound hooked %s\n", method_getTypeEncoding(method));
            } else {
                vb_say("feedback system sound refused encoding %s\n", method_getTypeEncoding(method));
            }
        }
    }
}

/*
 * ---- mediaserverd：AVHapticServer 起不來的確認診斷（唯讀） ----
 * CoreHaptics client 對 com.apple.audio.hapticd 的 configure 回覆只等 1 秒；server 的 -init 依序
 * setupSSSClient → incrementInit:（HapticSynth 要音訊輸出），失敗就回 nil、不建 listener，client 得 -4810。
 * 型別字串取自 AudioToolbox 20H392：init @16@0:8、setupSSSClient B16@0:8、incrementInit: B24@0:8^@16、
 * listener:shouldAcceptNewConnection: B32@0:8@16@24。
 */
static IMP vb_server_init_original, vb_server_setup_original, vb_server_increment_original, vb_server_accept_original;
static unsigned vb_server_init_calls, vb_server_setup_calls, vb_server_increment_calls, vb_server_accept_calls;

/*
 * -init 在 27 ms 內回 nil 且沒走到 setupSSSClient：HapticSynth 建構子丟了 C++ 例外（AudioToolbox 20H392
 * -[AVHapticServer init] 0x1a0942208 先 new HapticSynth 0x1a09f5860，catch 後回 nil）。要知道是哪一個 throw，
 * 在第一、二次 -init 期間暫時把 shared cache 統一 GOT 裡的 __cxa_throw 指到記錄函式（只記這條執行緒），
 * 結束就放回原值。動手前逐一核對：__cxa_throw 與 MediaExperience 算出的 slide 相同、AudioToolbox 的
 * __cxa_throw stub 三個指令字、GOT 現值等於 __cxa_throw；任一不符就不碰。
 */
static const uint64_t VB_CXA_THROW = 0x1d1ae0cdcULL;     /* libc++abi ___cxa_throw */
static const uint64_t VB_CXA_GOT = 0x1dad0db80ULL;       /* AudioToolbox __cxa_throw stub 讀的 GOT */
static const uint64_t VB_CXA_STUB = 0x1a0a88938ULL;      /* adrp x16 / ldr x16,[x16,#0xb80] / br x16 */
static const uint32_t VB_CXA_STUB_WORDS[3] = {0xb01d1430U, 0xf945c210U, 0xd61f0200U};
static const uint64_t VB_HS_FLAG_A = 0x1dd5833e1ULL;     /* ProductSupportsClosedLoopHaptics 快取 */
static const uint64_t VB_HS_ONCE_A = 0x1e1f81be8ULL;
static const uint64_t VB_HS_FLAG_B = 0x1e1f81bf0ULL;     /* 手機／手錶／audio_only_haptic_server */
static const uint64_t VB_HS_ONCE_B = 0x1e1f81bf8ULL;
typedef void (*vb_throw_t)(void *, void *, void (*)(void *));
static vb_throw_t vb_throw_real;
static uintptr_t vb_throw_slide;
static pthread_t vb_throw_thread;
static unsigned vb_throw_armed, vb_throw_seen, vb_throw_other;

/* 讀別人的記憶體一律先複製（位址不可讀時回 false，不會當掉） */
static bool vb_copy(const void *from, void *to, size_t size)
{
    vm_size_t got = 0;
    return from != NULL && vm_read_overwrite(mach_task_self(), (vm_address_t)from, size, (vm_address_t)to, &got) ==
        KERN_SUCCESS && got == size;
}

/* 複製一段 C 字串到 out（最多 size-1 個字元）；不可讀就回 false */
static bool vb_copy_string(const void *from, char *out, size_t size)
{
    for (size_t want = size - 1; want >= 16; want /= 2) {
        if (vb_copy(from, out, want)) {
            out[want] = '\0';
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

/* shared cache 是不是 20H392 這份：__cxa_throw 與 MediaExperience 的 slide 相同，且 AudioToolbox stub 三字相符 */
static bool vb_cache_slide(uintptr_t *slide_out)
{
    uintptr_t real = vb_strip(dlsym(RTLD_DEFAULT, "__cxa_throw"));
    uintptr_t media = vb_strip(dlsym(RTLD_DEFAULT, "CMSessionMgrVibrateForSystemSoundWithOptions"));
    uintptr_t slide = real - (uintptr_t)VB_CXA_THROW;
    uint32_t words[3];
    bool layout = real && media && (slide & 0x3fff) == 0 && media - (uintptr_t)VB_ME_VIBRATE == slide &&
        vb_copy((const void *)((uintptr_t)VB_CXA_STUB + slide), words, sizeof(words));
    for (unsigned k = 0; layout && k < 3; k++) layout = words[k] == VB_CXA_STUB_WORDS[k];
    *slide_out = layout ? slide : 0;
    return layout;
}

__attribute__((noinline)) static void vb_throw_log(void *thrown, void *type, uintptr_t from)
{
    unsigned index = __atomic_fetch_add(&vb_throw_seen, 1u, __ATOMIC_RELAXED);
    if (index >= VB_LOG_CALLS) return;
    char name[96] = "?";
    uintptr_t raw = 0;
    if (type && vb_copy((const char *)type + 8, &raw, sizeof(raw)))
        (void)vb_copy_string((const void *)(raw & 0x7fffffffffffffffULL), name, sizeof(name)); /* 最高位＝非唯一 RTTI */
    Dl_info info;
    memset(&info, 0, sizeof(info));
    const char *image = "?";
    uintptr_t offset = 0;
    if (dladdr((const void *)from, &info) && info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        image = slash ? slash + 1 : info.dli_fname;
        offset = from - (uintptr_t)info.dli_fbase;
    }
    char detail[220] = "";
    if (strcmp(name, "12CAXException") == 0 && thrown) {
        /* 20H392 AudioToolbox 的建構子先寫 vptr，文字位於 +0x08、OSStatus 位於 +0x108。 */
        struct { char operation[256]; int32_t status; } copy;
        if (vb_copy((const char *)thrown + 8, &copy, sizeof(copy))) {
            uint32_t code = (uint32_t)copy.status;
            char fourcc[5] = {(char)(code >> 24), (char)(code >> 16), (char)(code >> 8), (char)code, 0};
            for (unsigned k = 0; k < 4; k++) if (fourcc[k] < 32 || fourcc[k] > 126) fourcc[k] = '.';
            copy.operation[sizeof(copy.operation) - 1] = '\0';
            (void)snprintf(detail, sizeof(detail), " status=%d fourcc=%s op=%.150s", (int)copy.status, fourcc,
                copy.operation);
        }
    } else if (thrown && (strcmp(name, "St13runtime_error") == 0 || strcmp(name, "St11logic_error") == 0 ||
        strcmp(name, "St16invalid_argument") == 0 || strcmp(name, "St12out_of_range") == 0 ||
        strcmp(name, "St11range_error") == 0 || strcmp(name, "St12length_error") == 0)) {
        uintptr_t message = 0; /* vptr 之後是 __libcpp_refstring 的字元指標 */
        char text[160];
        if (vb_copy((const char *)thrown + 8, &message, sizeof(message)) &&
            vb_copy_string((const void *)message, text, sizeof(text)))
            (void)snprintf(detail, sizeof(detail), " what=%s", text);
    }
    vb_say("haptic throw %u from=%s+%#lx static=%#llx type=%s%s\n", index + 1, image, (unsigned long)offset,
        (unsigned long long)(from - vb_throw_slide), name, detail);
}

static void vb_throw_hook(void *thrown, void *type, void (*destructor)(void *))
{
    uintptr_t from = (uintptr_t)__builtin_return_address(0) - 4; /* 呼叫 stub 的 bl */
    if (__atomic_load_n(&vb_throw_armed, __ATOMIC_ACQUIRE) && pthread_equal(pthread_self(), vb_throw_thread))
        vb_throw_log(thrown, type, from);
    else
        (void)__atomic_fetch_add(&vb_throw_other, 1u, __ATOMIC_RELAXED);
    __attribute__((musttail)) return vb_throw_real(thrown, type, destructor);
}

/* arm=true：核對後換成記錄函式；false：放回原值。回傳 GOT 位址（0＝沒換）。 */
static uintptr_t vb_throw_trace(bool arm, uintptr_t slot_address)
{
    if (!arm) {
        if (slot_address == 0) return 0;
        __atomic_store_n(&vb_throw_armed, 0u, __ATOMIC_RELEASE);
        uintptr_t *slot = (uintptr_t *)slot_address;
        if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) == (uintptr_t)vb_throw_hook)
            __atomic_store_n(slot, (uintptr_t)vb_throw_real, __ATOMIC_RELEASE);
        vm_address_t page = slot_address & ~(uintptr_t)(vm_page_size - 1);
        kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ);
        vb_say("haptic throw trace restored got=%#lx protect=%d other-threads=%u\n", (unsigned long)slot_address, kr,
            __atomic_load_n(&vb_throw_other, __ATOMIC_RELAXED));
        return 0;
    }
    uintptr_t slide = 0, current = 0;
    bool layout = vb_cache_slide(&slide);
    uintptr_t real = (uintptr_t)VB_CXA_THROW + slide;
    uintptr_t *slot = (uintptr_t *)((uintptr_t)VB_CXA_GOT + slide);
    layout = layout && vb_copy(slot, &current, sizeof(current)) && current == real;
    if (!layout) {
        vb_say("haptic throw trace refused layout (slide-match/stub/got)\n");
        return 0;
    }
    vm_address_t page = (uintptr_t)slot & ~(uintptr_t)(vm_page_size - 1);
    kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, false,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        vb_say("haptic throw trace vm_protect failed kr=%d\n", kr);
        return 0;
    }
    vb_throw_real = (vb_throw_t)real;
    vb_throw_slide = slide;
    vb_throw_thread = pthread_self();
    __atomic_store_n(&vb_throw_armed, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(slot, (uintptr_t)vb_throw_hook, __ATOMIC_RELEASE);
    return (uintptr_t)slot;
}

/* -init 失敗後：建構子用到的旗標與 HapticSynth AU（aumu/hsyn/appl）自己問一次 */
static void vb_server_explain(void)
{
    typedef unsigned char (*mg_bool_t)(CFStringRef);
    typedef int32_t (*mg_int_t)(CFStringRef, int32_t);
    typedef bool (*feature_t)(const char *, const char *);
    typedef struct { uint32_t type, subtype, manufacturer, flags, mask; } vb_component_t;
    typedef void *(*find_t)(void *, const vb_component_t *);
    typedef int32_t (*new_t)(void *, void **);
    typedef int32_t (*dispose_t)(void *);
    mg_bool_t mg = (mg_bool_t)dlsym(RTLD_DEFAULT, "MGGetBoolAnswer");
    mg_int_t mg_int = (mg_int_t)dlsym(RTLD_DEFAULT, "MGGetSInt32Answer");
    feature_t feature = (feature_t)dlsym(RTLD_DEFAULT, "_os_feature_enabled_impl");
    uintptr_t slide = 0;
    bool layout = vb_cache_slide(&slide);
    vb_say("haptic server explain closed-loop=%d device-class=%d audio-only-feature=%d layout=%u "
        "flag-a=%d once-a=%ld flag-b=%d once-b=%ld\n", mg ? mg(CFSTR("gqDnklGQnpv5ilgh5uHckw")) : -1,
        mg_int ? (int)mg_int(CFSTR("DeviceClassNumber"), 0) : -1,
        feature ? feature("CoreAudioServices", "audio_only_haptic_server") : -1, layout,
        layout ? *(volatile const uint8_t *)((uintptr_t)VB_HS_FLAG_A + slide) : -1,
        layout ? *(volatile const long *)((uintptr_t)VB_HS_ONCE_A + slide) : 0,
        layout ? *(volatile const uint8_t *)((uintptr_t)VB_HS_FLAG_B + slide) : -1,
        layout ? *(volatile const long *)((uintptr_t)VB_HS_ONCE_B + slide) : 0);
    find_t find = (find_t)dlsym(RTLD_DEFAULT, "AudioComponentFindNext");
    new_t create = (new_t)dlsym(RTLD_DEFAULT, "AudioComponentInstanceNew");
    dispose_t dispose = (dispose_t)dlsym(RTLD_DEFAULT, "AudioComponentInstanceDispose");
    const vb_component_t synth = {0x61756d75U /* aumu */, 0x6873796eU /* hsyn */, 0x6170706cU /* appl */, 0, 0};
    void *component = find ? find(NULL, &synth) : NULL;
    void *instance = NULL;
    int32_t rc = component && create ? create(component, &instance) : -1;
    vb_say("haptic server explain hsyn component=%u instance-rc=%d instance=%u\n", component != NULL, (int)rc,
        instance != NULL);
    if (instance && dispose) (void)dispose(instance);
}

static id vb_server_init(id self, SEL selector)
{
    unsigned call = __atomic_fetch_add(&vb_server_init_calls, 1u, __ATOMIC_RELAXED);
    uintptr_t traced = call < 2 ? vb_throw_trace(true, 0) : 0;
    uint64_t start = vb_now_ms();
    id result = ((id (*)(id, SEL))vb_server_init_original)(self, selector);
    uint64_t elapsed = vb_now_ms() - start;
    (void)vb_throw_trace(false, traced);
    if (call < VB_LOG_CALLS)
        vb_say("haptic server init call=%u uptime-ms=%llu elapsed-ms=%llu result=%s traced=%u\n", call + 1,
            (unsigned long long)start, (unsigned long long)elapsed, result ? "object" : "nil", traced != 0);
    if (call == 0 && result == NULL) vb_server_explain();
    return result;
}

static bool vb_server_setup(id self, SEL selector)
{
    unsigned call = __atomic_fetch_add(&vb_server_setup_calls, 1u, __ATOMIC_RELAXED);
    uint64_t start = vb_now_ms();
    bool result = ((bool (*)(id, SEL))vb_server_setup_original)(self, selector);
    if (call < VB_LOG_CALLS)
        vb_say("haptic server setupSSSClient call=%u uptime-ms=%llu elapsed-ms=%llu result=%u\n", call + 1,
            (unsigned long long)start, (unsigned long long)(vb_now_ms() - start), result);
    return result;
}

static bool vb_server_increment(id self, SEL selector, id *error)
{
    unsigned call = __atomic_fetch_add(&vb_server_increment_calls, 1u, __ATOMIC_RELAXED);
    uint64_t start = vb_now_ms();
    bool result = ((bool (*)(id, SEL, id *))vb_server_increment_original)(self, selector, error);
    if (call < VB_LOG_CALLS) {
        id failure = !result && error ? *error : NULL;
        vb_say("haptic server incrementInit call=%u uptime-ms=%llu elapsed-ms=%llu result=%u error-out=%u "
            "error-present=%u\n", call + 1, (unsigned long long)start, (unsigned long long)(vb_now_ms() - start),
            result, error != NULL, failure != NULL);
        FuxiErrorAuditEntry entries[FUXI_ERROR_AUDIT_DEPTH];
        unsigned count = failure ? fuxi_error_audit((CFTypeRef)failure, entries) : 0;
        for (unsigned depth = 0; depth < count; depth++)
            vb_say("haptic server incrementInit error call=%u depth=%u domain-id=%08x code=%ld reason=%s\n",
                call + 1, depth, entries[depth].domain, (long)entries[depth].code, entries[depth].reason);
    }
    return result;
}

static bool vb_server_accept(id self, SEL selector, id listener, id connection)
{
    unsigned call = __atomic_fetch_add(&vb_server_accept_calls, 1u, __ATOMIC_RELAXED);
    bool result = ((bool (*)(id, SEL, id, id))vb_server_accept_original)(self, selector, listener, connection);
    if (call < VB_LOG_CALLS)
        vb_say("haptic server accept call=%u uptime-ms=%llu pid=%ld result=%u\n", call + 1,
            (unsigned long long)vb_now_ms(), (long)vb_feedback_integer(connection, "processIdentifier"), result);
    return result;
}

static void vb_install_server(void)
{
    static vb_typed_hook hooks[] = {
        {"AVHapticServer", "init", "@", (IMP)vb_server_init, &vb_server_init_original, 0},
        {"AVHapticServer", "setupSSSClient", "b", (IMP)vb_server_setup, &vb_server_setup_original, 0},
        {"AVHapticServer", "incrementInit:", "b^", (IMP)vb_server_increment, &vb_server_increment_original, 0},
        {"AVHapticServer", "listener:shouldAcceptNewConnection:", "b@@", (IMP)vb_server_accept,
            &vb_server_accept_original, 0},
    };
    vb_install_typed("haptic server", hooks, sizeof(hooks) / sizeof(hooks[0]));
}

static void vb_image_added(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    if (vb_role == VB_MEDIA) {
        vb_install_play();
        vb_install_server();
    }
    if (vb_role == VB_SPRINGBOARD) {
        vb_install_supports();
        vb_install_feedback();
    }
    if (vb_role == VB_APP) vb_install_core_feedback();
    for (unsigned index = 0; index < VB_HOOKS; index++) vb_install(index);
}

/* ---- 自測（fuxi,haptic=3）：公開 UIFeedbackGenerator → 回退路徑 ---- */
typedef id (*vb_msg_id_t)(id, SEL);
typedef id (*vb_msg_long_t)(id, SEL, long);
typedef void (*vb_msg_void_t)(id, SEL);

static void vb_generator_fire(void *context)
{
    const char *kind = context;
    bool impact = strcmp(kind, "impact") == 0;
    Class target = objc_getClass(impact ? "UIImpactFeedbackGenerator" : "UISelectionFeedbackGenerator");
    if (target == NULL) {
        vb_say("haptic selftest %s generator class missing\n", kind);
        return;
    }
    /* 自測只做幾次，物件刻意不釋放，避免播放前被回收 */
    id generator = ((vb_msg_id_t)objc_msgSend)((id)target, sel_registerName("alloc"));
    generator = impact ? ((vb_msg_long_t)objc_msgSend)(generator, sel_registerName("initWithStyle:"), 1L)
                       : ((vb_msg_id_t)objc_msgSend)(generator, sel_registerName("init"));
    if (generator == NULL) return;
    ((vb_msg_void_t)objc_msgSend)(generator, sel_registerName("prepare"));
    ((vb_msg_void_t)objc_msgSend)(generator, sel_registerName(impact ? "impactOccurred" : "selectionChanged"));
    vb_say("haptic selftest %s generator fired uptime-ms=%llu pulses=%u\n", kind, (unsigned long long)vb_now_ms(),
        __atomic_load_n(&vb_ui_pulses, __ATOMIC_RELAXED));
}

static void vb_generator_selftest(const char *who)
{
    vb_say("haptic selftest %s: UIImpactFeedbackGenerator now, UISelectionFeedbackGenerator in 1.5 s\n", who);
    dispatch_async_f(dispatch_get_main_queue(), (void *)"impact", vb_generator_fire);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_main_queue(),
        (void *)"selection", vb_generator_fire);
}

/*
 * 2026-11-18 hapticfb3：SpringBoard 內呼叫 LSApplicationWorkspace openApplicationWithBundleID: 回 NO
 * （打開請求經 FrontBoard 回到 SpringBoard 自己）。改在主執行緒用 SpringBoard 的
 * launchApplicationWithIdentifier:suspended:（UIApplication 子類別的私有方法），先檢查有沒有這個方法。
 */
static void vb_launch_settings(void *context)
{
    (void)context;
    Class application = objc_getClass("UIApplication");
    id shared = application ? ((vb_msg_id_t)objc_msgSend)((id)application, sel_registerName("sharedApplication")) : NULL;
    SEL launch = sel_registerName("launchApplicationWithIdentifier:suspended:");
    bool responds = shared && ((bool (*)(id, SEL, SEL))objc_msgSend)(shared, sel_registerName("respondsToSelector:"), launch);
    bool launched = responds && ((bool (*)(id, SEL, id, bool))objc_msgSend)(shared, launch,
        (id)CFSTR("com.apple.Preferences"), false);
    vb_say("haptic selftest launch Preferences springboard=%u responds=%u launched=%u\n", shared != NULL, responds,
        launched);
}

static void vb_open_settings(void)
{
    Class workspace = objc_getClass("LSApplicationWorkspace");
    id shared = workspace ? ((vb_msg_id_t)objc_msgSend)((id)workspace, sel_registerName("defaultWorkspace")) : NULL;
    bool opened = shared && ((bool (*)(id, SEL, id))objc_msgSend)(shared,
        sel_registerName("openApplicationWithBundleID:"), (id)CFSTR("com.apple.Preferences"));
    vb_say("haptic selftest open Preferences workspace=%u opened=%u\n", shared != NULL, opened);
    if (!opened) dispatch_async_f(dispatch_get_main_queue(), NULL, vb_launch_settings);
}

static unsigned vb_adt_u32(CFStringRef key)
{
    vb_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
    unsigned value = 0;
    if (chosen == MACH_PORT_NULL) return 0;
    CFTypeRef data = IORegistryEntryCreateCFProperty(chosen, key, kCFAllocatorDefault, 0);
    if (data && CFGetTypeID(data) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)data) == 4) {
        uint32_t raw = 0;
        CFDataGetBytes((CFDataRef)data, CFRangeMake(0, 4), (UInt8 *)&raw);
        value = raw;
    }
    if (data) CFRelease(data);
    (void)IOObjectRelease(chosen);
    return value;
}

static void *vb_selftest(void *argument)
{
    (void)argument;
    typedef void (*play_t)(uint32_t);
    typedef void (*play_vibe_t)(uint32_t, id, CFDictionaryRef);
    sleep(30);
    play_t play = (play_t)dlsym(RTLD_DEFAULT, "AudioServicesPlaySystemSound");
    play_vibe_t play_vibe = (play_vibe_t)dlsym(RTLD_DEFAULT, "AudioServicesPlaySystemSoundWithVibration");
    vb_say("vibe selftest start play=%d play_vibe=%d\n", play != NULL, play_vibe != NULL);
    if (play) {
        play(4095); /* kSystemSoundID_Vibrate */
        vb_say("vibe selftest 1 AudioServicesPlaySystemSound(4095) sent\n");
        sleep(4);
    }
    if (play_vibe) {
        int values[] = {200, 150, 400};
        CFNumberRef numbers[3];
        for (unsigned index = 0; index < 3; index++)
            numbers[index] = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &values[index]);
        double level = 1.0;
        CFNumberRef intensity = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &level);
        if (numbers[0] && numbers[1] && numbers[2] && intensity) {
            const void *items[] = {kCFBooleanTrue, numbers[0], kCFBooleanFalse, numbers[1], kCFBooleanTrue, numbers[2]};
            CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, items, 6, &kCFTypeArrayCallBacks);
            const void *keys[] = {CFSTR("VibePattern"), CFSTR("Intensity")};
            const void *values2[] = {list, intensity};
            CFDictionaryRef pattern = list ? CFDictionaryCreate(kCFAllocatorDefault, keys, values2, 2,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
            if (pattern) {
                play_vibe(4095, NULL, pattern);
                vb_say("vibe selftest 2 AudioServicesPlaySystemSoundWithVibration(4095, 200/150/400) sent\n");
                CFRelease(pattern);
            }
            if (list) CFRelease(list);
        }
        for (unsigned index = 0; index < 3; index++)
            if (numbers[index]) CFRelease(numbers[index]);
        if (intensity) CFRelease(intensity);
        sleep(4);
    }
    if (play) {
        play(1519); /* Peek：Taptic 的短脈衝系統聲音 */
        vb_say("vibe selftest 3 AudioServicesPlaySystemSound(1519) sent\n");
    }
    sleep(4);
    vb_generator_selftest("springboard");
    sleep(4);
    vb_open_settings();
    return NULL;
}

/* App（Preferences 由 SpringBoard 自測打開）：啟動 8 秒後同樣經 UIFeedbackGenerator 觸發 */
static void *vb_app_selftest(void *argument)
{
    (void)argument;
    sleep(8);
    vb_generator_selftest("app");
    return NULL;
}

/*
 * ---- mediaserverd 探針（fuxi,haptic=3）----
 * MediaExperience 只在「關卡」通過時建立震動器：關卡用 dispatch_once 載入 libVibeSynthEngine 問
 * VibeSynthEngineHasHardwareSupport（＝MobileGestalt DeviceSupportsClosedLoopHaptics），結果存成一個位元組；
 * 不成立再找名為 "vibrator" 的 IOService。建立後的震動器存在一個全域指標。這幾個值沒有匯出，
 * 只能用 iOS 16.7 這份 shared cache 的未滑動位址讀：先用匯出符號算 slide，再比對關卡函式開頭的指令，不符就不讀。
 */
static const uint64_t VB_ME_GATE = 0x195a93ab0ULL;     /* 關卡函式；開頭是 stp x20, x19, [sp, #-0x20]! */
static const uint32_t VB_ME_GATE_WORD = 0xa9be4ff4U;
static const uint64_t VB_ME_ONCE = 0x1dd2ef740ULL;     /* dispatch_once 的 predicate（做完是 -1） */
static const uint64_t VB_ME_HW = 0x1dd2efcf8ULL;       /* HasHardwareSupport 的結果 */
static const uint64_t VB_ME_HANDLE = 0x1dd2efa68ULL;   /* libVibeSynthEngine 的 dlopen handle（不支援時會被清掉） */
static const uint64_t VB_ME_VIBRATOR = 0x1dd2efcb8ULL; /* 建立好的震動器 */
#define VB_ENGINE_PATH "/System/Library/Frameworks/AudioToolbox.framework/libVibeSynthEngine.dylib"

static unsigned vb_native_calls;
static unsigned vb_native_cancels;

static int vb_native_play(CFDictionaryRef pattern, float gain, void *synchronizer, uint32_t flags,
    const void *completion)
{
    if (pattern == NULL || CFGetTypeID(pattern) != CFDictionaryGetTypeID()) return -50;
    unsigned call = __atomic_fetch_add(&vb_native_calls, 1u, __ATOMIC_RELAXED);
    bool queued = vb_vibe_request((id)pattern, gain, flags, 0.0, call, completion);
    if (call < VB_PLAY_LOGS)
        vb_say("vibe native play call=%u queued=%u synchronizer=%u completion=%u\n", call + 1, queued,
            synchronizer != NULL, completion != NULL);
    return queued ? 0 : -108;
}

static int vb_native_cancel(CFDictionaryRef options)
{
    (void)options;
    vb_cancel();
    if (__atomic_fetch_add(&vb_native_cancels, 1u, __ATOMIC_RELAXED) < VB_PLAY_LOGS)
        vb_say("vibe native cancel\n");
    return 0;
}

static void *vb_bridge_thread(void *argument)
{
    (void)argument;
    typedef int (*mutex_t)(void *);
    mutex_t lock = (mutex_t)dlsym(RTLD_DEFAULT, "FigSimpleMutexLock");
    mutex_t unlock = (mutex_t)dlsym(RTLD_DEFAULT, "FigSimpleMutexUnlock");
    void *vibrate = dlsym(RTLD_DEFAULT, "CMSessionMgrVibrateForSystemSoundWithOptions");
    uintptr_t address = vb_strip(vibrate);
    uintptr_t slide = address - (uintptr_t)VB_ME_VIBRATE;
    bool layout = address && (slide & 0x3fff) == 0 &&
        *(volatile const uint32_t *)address == 0xd10303ffU &&
        *(volatile const uint32_t *)((uintptr_t)VB_ME_GATE + slide) == VB_ME_GATE_WORD;
    if (!layout || lock == NULL || unlock == NULL) {
        vb_say("vibe native bridge refused layout=%u mutex=%u/%u\n", layout, lock != NULL, unlock != NULL);
        return NULL;
    }
    for (unsigned attempt = 0; attempt < 90; attempt++) {
        uintptr_t vibrator = *(volatile const uintptr_t *)((uintptr_t)VB_ME_VIBRATOR + slide);
        void *engine = dlopen(VB_ENGINE_PATH, RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
        if (engine && vibrator && *(volatile const uint8_t *)((uintptr_t)VB_ME_HW + slide)) {
            uintptr_t *slots = (uintptr_t *)vibrator;
            void *play = dlsym(engine, "VibeSynthEnginePlay");
            void *cancel = dlsym(engine, "VibeSynthEngineCancelWithOptions");
            int rc = slots[0] ? lock((void *)slots[0]) : -1;
            if (rc == 0) {
                bool expected = play && cancel && slots[6] == (uintptr_t)play && slots[7] == (uintptr_t)cancel;
                if (expected) {
                    slots[6] = (uintptr_t)vb_native_play;
                    slots[7] = (uintptr_t)vb_native_cancel;
                }
                (void)unlock((void *)slots[0]);
                (void)dlclose(engine);
                vb_say("vibe native bridge %s; play/cancel -> kern.fuxivibrate\n", expected ? "installed" : "refused slots");
                return NULL;
            }
        }
        if (engine) (void)dlclose(engine);
        sleep(1);
    }
    vb_say("vibe native bridge timed out waiting for MediaExperience vibrator\n");
    return NULL;
}

static void vb_probe(const char *when, bool call)
{
    typedef unsigned char (*mg_bool_t)(CFStringRef);
    typedef unsigned char (*engine_bool_t)(void);
    typedef int (*engine_init_t)(void);
    typedef int (*vibrate_t)(uint32_t, CFDictionaryRef);
    mg_bool_t mg = (mg_bool_t)dlsym(RTLD_DEFAULT, "MGGetBoolAnswer");
    int closed_loop = mg ? mg(CFSTR("gqDnklGQnpv5ilgh5uHckw")) : -1;
    void *loaded = dlopen(VB_ENGINE_PATH, RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
    void *engine = loaded ? loaded : dlopen(VB_ENGINE_PATH, RTLD_LAZY | RTLD_LOCAL);
    engine_bool_t has_hw = engine ? (engine_bool_t)dlsym(engine, "VibeSynthEngineHasHardwareSupport") : NULL;
    int support = has_hw ? has_hw() : -1;
    void *vibrate_symbol = dlsym(RTLD_DEFAULT, "CMSessionMgrVibrateForSystemSoundWithOptions");
    uintptr_t vibrate_address = vb_strip(vibrate_symbol);
    uintptr_t slide = vibrate_address - (uintptr_t)VB_ME_VIBRATE;
    bool layout = vibrate_address && (slide & 0x3fff) == 0 &&
                  *(volatile const uint32_t *)((uintptr_t)VB_ME_GATE + slide) == VB_ME_GATE_WORD;
    long once = 0;
    unsigned hw_byte = 0;
    uintptr_t handle = 0, vibrator = 0;
    if (layout) {
        once = *(volatile const long *)((uintptr_t)VB_ME_ONCE + slide);
        hw_byte = *(volatile const uint8_t *)((uintptr_t)VB_ME_HW + slide);
        handle = *(volatile const uintptr_t *)((uintptr_t)VB_ME_HANDLE + slide);
        vibrator = *(volatile const uintptr_t *)((uintptr_t)VB_ME_VIBRATOR + slide);
    }
    vb_say("vibe probe %s mg-closed-loop=%d engine-loaded=%d has-hw=%d layout=%d once=%ld hw-byte=%u handle=%d "
           "vibrator=%d\n", when, closed_loop, loaded != NULL, support, layout, once, hw_byte, handle != 0,
           vibrator != 0);
    if (layout && vibrator) {
        /* 震動器的 VibeSynthEngine 函式指標（+0x18 Initialize … +0x38 Cancel）與 +0x40 的狀態 */
        const uintptr_t *slots = (const uintptr_t *)vibrator;
        vb_say("vibe probe %s vibrator init=%d prewarm=%d/%d play=%d cancel=%d state=0x%04x\n", when,
               slots[3] != 0, slots[4] != 0, slots[5] != 0, slots[6] != 0, slots[7] != 0,
               *(const uint16_t *)(vibrator + 0x40));
    } else if (layout && hw_byte && engine) {
        /* 關卡過了卻沒有震動器：多半是 VibeSynthEngineInitialize 失敗，自己問一次它回什麼 */
        engine_init_t initialize = (engine_init_t)dlsym(engine, "VibeSynthEngineInitialize");
        vb_say("vibe probe %s VibeSynthEngineInitialize rc=%d\n", when, initialize ? initialize() : -1);
    }
    if (call && vibrate_symbol) {
        /* 直接走 MediaExperience 的系統聲音震動入口：rc 0 且 playVibePattern 有記錄，問題就在 SSServer 之前 */
        float seconds = 0.2f;
        CFNumberRef duration = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &seconds);
        const void *keys[] = {CFSTR("Duration")};
        const void *values[] = {duration};
        CFDictionaryRef options = duration ? CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
        if (options) {
            int rc = ((vibrate_t)vibrate_symbol)(4095, options);
            vb_say("vibe probe %s CMSessionMgrVibrateForSystemSoundWithOptions(4095, Duration 0.2) rc=%d\n", when, rc);
            CFRelease(options);
        }
        if (duration) CFRelease(duration);
    }
}

static void *vb_probe_thread(void *argument)
{
    (void)argument;
    sleep(10);
    vb_probe("early", false);
    sleep(65); /* SpringBoard 自測在它啟動後 30 秒、約 8 秒做完；這時應該已經過了 */
    vb_probe("late", false);
    sleep(2);
    vb_probe("direct", true);
    sleep(3);
    typedef int (*vibrate_t)(uint32_t, CFDictionaryRef);
    typedef void (*stop_t)(CFDictionaryRef);
    vibrate_t vibrate = (vibrate_t)dlsym(RTLD_DEFAULT, "CMSessionMgrVibrateForSystemSoundWithOptions");
    stop_t stop = (stop_t)dlsym(RTLD_DEFAULT, "CMSessionMgrVibratorStopWithOptions");
    double seconds = 2.0, intensity = 1.0;
    CFNumberRef duration = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &seconds);
    CFNumberRef level = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &intensity);
    const void *keys[] = {CFSTR("OnDuration"), CFSTR("TotalDuration"), CFSTR("Intensity")};
    const void *values[] = {duration, duration, level};
    CFDictionaryRef pattern = duration && level ? CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
    const void *option_keys[] = {CFSTR("VibrationPattern")};
    const void *option_values[] = {pattern};
    CFDictionaryRef options = pattern ? CFDictionaryCreate(kCFAllocatorDefault, option_keys, option_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
    const void *stop_keys[] = {CFSTR("CancelAtLoopEnd")};
    const void *stop_values[] = {kCFBooleanFalse};
    CFDictionaryRef stop_options = CFDictionaryCreate(kCFAllocatorDefault, stop_keys, stop_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (vibrate && stop && options && stop_options) {
        int rc = vibrate(4095, options);
        vb_say("vibe probe cancel play rc=%d requested=2000ms\n", rc);
        usleep(120000);
        stop(stop_options);
        vb_say("vibe probe cancel stop sent after 120ms\n");
    }
    if (stop_options) CFRelease(stop_options);
    if (options) CFRelease(options);
    if (pattern) CFRelease(pattern);
    if (level) CFRelease(level);
    if (duration) CFRelease(duration);
    return NULL;
}

static void vb_spawn(void *(*body)(void *), const char *what)
{
    pthread_t thread;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return;
    if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0 &&
        pthread_create(&thread, &attributes, body, NULL) == 0)
        vb_say("%s\n", what);
    (void)pthread_attr_destroy(&attributes);
}

__attribute__((constructor)) static void fuxi_vibrate_init(void)
{
    const char *name = getprogname();
    if (name == NULL) return;
    const char *path = _dyld_get_image_name(0);
    if (strcmp(name, "mediaserverd") == 0) vb_role = VB_MEDIA;
    else if (strcmp(name, "SpringBoard") == 0) vb_role = VB_SPRINGBOARD;
    else if (strcmp(name, "VoiceMemos") == 0 || strcmp(name, "voicememod") == 0) return;
    /* 設定的執行檔可能從 Cryptex 載入，constructor 裡的 image[0] 路徑不一定能辨認它。 */
    else if (strcmp(name, "Preferences") == 0 || (path && strstr(path, ".app/"))) vb_role = VB_APP;
    else return;
    vb_name = name;
    if (vb_role == VB_SPRINGBOARD && vb_adt_u32(CFSTR("fuxi,bt-probe")) == 1) {
        extern void *fuxi_bluetooth_probe(void *);
        vb_spawn(fuxi_bluetooth_probe, "Bluetooth HCI self-test scheduled");
    }
    if (vb_role == VB_APP) {
        /* 沙盒可能擋 IORegistry：讀得到 fuxi,inject（注入我們的就是它）才算 ADT 可讀，讀不到就照預設開 */
        bool readable = vb_adt_u32(CFSTR("fuxi,inject")) != 0;
        bool native_pcm = readable && vb_adt_u32(CFSTR("fuxi,haptic")) == 9u &&
            vb_adt_u32(CFSTR("fuxi,hal-haptic")) >= 4u;
        unsigned ui = readable ? (native_pcm ? 0u : vb_adt_u32(CFSTR("fuxi,haptic-ui"))) : 1u;
        __atomic_store_n(&vb_native_pcm, native_pcm ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&vb_app_fallback, ui != 0, __ATOMIC_RELAXED);
        vb_say("app haptic fallback=%u adt-readable=%u native-pcm=%u (UIFeedbackGenerator -> system sound %u)\n",
            ui != 0, readable, native_pcm, (unsigned)VB_APP_SOUND);
        if (ui == 0) return;
        _dyld_register_func_for_add_image(vb_image_added);
        if (readable && vb_adt_u32(CFSTR("fuxi,haptic")) == 3)
            vb_spawn(vb_app_selftest, "app haptic selftest scheduled in 8 s (fuxi,haptic=3)");
        return;
    }
    int error = 0;
    if (!vb_ready(&error)) {
        vb_say("vibrate kern.fuxivibrate unavailable errno=%d; haptic hooks skipped\n", error);
        return;
    }
    if (vb_role == VB_SPRINGBOARD) {
        bool native_pcm = vb_adt_u32(CFSTR("fuxi,haptic")) == 9u &&
            vb_adt_u32(CFSTR("fuxi,hal-haptic")) >= 4u;
        __atomic_store_n(&vb_native_pcm, native_pcm ? 1u : 0u, __ATOMIC_RELEASE);
        unsigned ms = native_pcm ? 0u : vb_adt_u32(CFSTR("fuxi,haptic-ui"));
        if (ms) ms = ms < 8 ? 8 : ms > 40 ? 40 : ms;
        __atomic_store_n(&vb_ui_ms, ms, __ATOMIC_RELAXED);
    }
    if (vb_role == VB_MEDIA) __atomic_store_n(&vb_envelope, vb_adt_u32(CFSTR("fuxi,haptic-env")) != 0, __ATOMIC_RELAXED);
    unsigned amplitude = vb_adt_u32(CFSTR("fuxi,haptic-amp"));
    __atomic_store_n(&vb_amp_max, amplitude > 255 ? 255u : amplitude, __ATOMIC_RELAXED);
    vb_say("vibrate kern.fuxivibrate ready; installing haptic hooks ui-fallback-ms=%u native-pcm=%u envelope=%u amplitude-max=%u\n",
        vb_ui_ms, vb_native_pcm, vb_envelope, vb_amp_max);
    _dyld_register_func_for_add_image(vb_image_added);
    if (vb_role == VB_MEDIA) vb_spawn(vb_bridge_thread, "vibe native bridge waiting for MediaExperience vibrator");
    if (vb_adt_u32(CFSTR("fuxi,haptic")) != 3) return;
    if (vb_role == VB_SPRINGBOARD) vb_spawn(vb_selftest, "vibe selftest scheduled in 30 s (fuxi,haptic=3)");
    if (vb_role == VB_MEDIA) vb_spawn(vb_probe_thread, "vibe probe scheduled at 10/75/77 s (fuxi,haptic=3)");
}
