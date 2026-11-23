/*
 * bluetoothd os_log 轉錄與相容性修正（fxinj bit 6，ADT fuxi,inject 含 64）。
 *
 * 20H392 bluetoothd 的 OI 日誌只走 os_log：0x1001c28d0 → 0x1001c2684 → _os_log_impl("%{public}s")，
 * 自願重啟 0x100090734／0x100090778 先寫「Bluetooth error - restarting { build=…, reason=%d, context=… }」
 * 再 _exit(1)，console 看不到原因。這裡用 dyld 的 __DATA,__interpose（核心以 DYLD_INSERT_LIBRARIES 注入，
 * patch 0118）攔 bluetoothd 匯入的 _os_log_impl／_os_log_error_impl／_os_log_fault_impl／_os_log_debug_impl、
 * os_log_pack_send_and_compose、os_log_type_enabled、exit／_exit、abort_with_payload，把訊息寫到 fd 2；
 * bluetoothd 的 stderr 是 /dev/null，boot-arg sweetnull 含 bluetoothd 時核心（patch 0048）印成
 * 「|bluetoothd| BTLOG …」。
 *
 *   fuxi,bt-oslog bit 0  default／error／fault（每程序最多 4000 行）
 *                 bit 1  info：os_log_type_enabled 對 info 回 true 並轉錄（與 debug 合計最多 1500 行）
 *                 bit 2  debug：同上
 *
 *   fuxi,bt-fix   bit 0  填補 H4 傳輸表 +0x60（PrepareTerminate）為空函式，避免關藍牙時 pc=0 SIGSEGV
 *                 bit 1  攔截 CFPreferencesCopyAppValue("LE", "com.apple.MobileBluetooth.debug")，回傳
 *                        SupportsMultipleAdvertising=false，讓 ChipMgr／LeBroadcaster 原生走單實例標準
 *                        Legacy BLE 廣播（0x2005／0x2006／0x2008／0x200a），略過 Broadcom VSC 0xfd48
 *
 * 其他程序或 fuxi,bt-oslog=0 時，每個替身只做尾呼叫轉發（原函式看到的返回位址不變）。
 * 緩衝格式解析在 FuxiOsLogFormat.h（主機測試 tools/test-oslog-format.c）。%@ 只展開 CFString，
 * 其他物件印 <obj>，不在日誌路徑呼叫 -description。private 參數印 <private>，位址形字串遮成 xx。
 * 0048 每次 write 最多 256 bytes 一行，長訊息切段，續行前綴 BTLOG+。
 *
 * 參考：dyld 的 interpose tuple 格式（dyld mach-o/dyld-interposing.h 的 DYLD_INTERPOSE，APSL）；
 * 函式原型同 libsystem_trace 的 os/log.h、os/log_private.h（_os_log_*_impl、os_log_pack_send_and_compose）
 * 與 sys/reason.h（abort_with_payload）。ADT 讀法同 FuxiBlueTool.c。
 */
#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "FuxiBluetoothPatch.h"
#include "FuxiOsLogFormat.h"

typedef mach_port_t btlog_io_object_t;
extern btlog_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(btlog_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(btlog_io_object_t object);

/* 原函式：用 asm 標籤直接指名符號，避免和 SDK 標頭（巨集與 availability 標記）衝突 */
typedef void btlog_impl_fn(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size);
extern btlog_impl_fn btlog_real_impl __asm__("__os_log_impl");
extern btlog_impl_fn btlog_real_error __asm__("__os_log_error_impl");
extern btlog_impl_fn btlog_real_fault __asm__("__os_log_fault_impl");
extern btlog_impl_fn btlog_real_debug __asm__("__os_log_debug_impl");
extern bool btlog_real_enabled(void *log, uint8_t type) __asm__("_os_log_type_enabled");
extern char *btlog_real_compose(void *pack, void *log, uint8_t type, char *buf, size_t size)
    __asm__("_os_log_pack_send_and_compose");
extern void btlog_real_exit(int status) __asm__("_exit") __attribute__((noreturn));
extern void btlog_real_quick_exit(int status) __asm__("__exit") __attribute__((noreturn));
extern void btlog_real_abort_payload(uint32_t space, uint64_t code, void *payload, uint32_t payload_size,
    const char *reason, uint64_t flags) __asm__("_abort_with_payload") __attribute__((noreturn));
extern CFPropertyListRef btlog_real_prefs_copy_app_value(CFStringRef key, CFStringRef application_id)
    __asm__("_CFPreferencesCopyAppValue");

#define BTLOG_TYPE_INFO 0x01u
#define BTLOG_TYPE_DEBUG 0x02u
#define BTLOG_TYPE_ERROR 0x10u
#define BTLOG_TYPE_FAULT 0x11u
#define BTLOG_MAIN_LIMIT 4000u
#define BTLOG_DETAIL_LIMIT 1500u
#define BTLOG_TEXT_MAX 900u
#define BTLOG_CHUNK 220u
#define FUXI_BTFIX_H4_PREPARE 1u
#define FUXI_BTFIX_SINGLE_ADV 2u
#define FUXI_BTFIX_ALL (FUXI_BTFIX_H4_PREPARE | FUXI_BTFIX_SINGLE_ADV)

static unsigned btlog_mode;
static unsigned btfix_mode;
static unsigned btlog_main_lines;
static unsigned btlog_detail_lines;
static unsigned btlog_sequence;
static uint64_t btlog_after_ns;
static unsigned btlog_fmt_flags;
static const char *btlog_tag = "BTLOG";
static __thread unsigned btlog_busy;

static unsigned btlog_adt_u32(const char *key)
{
    unsigned value = 0;
    btlog_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
    if (chosen == MACH_PORT_NULL) {
        return 0;
    }
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef data = name ? IORegistryEntryCreateCFProperty(chosen, name, kCFAllocatorDefault, 0) : NULL;
    if (data && CFGetTypeID(data) == CFDataGetTypeID() && CFDataGetLength(data) == (CFIndex)sizeof(uint32_t)) {
        memcpy(&value, CFDataGetBytePtr(data), sizeof(value));
    }
    if (data) {
        CFRelease(data);
    }
    if (name) {
        CFRelease(name);
    }
    IOObjectRelease(chosen);
    return value;
}

static unsigned btlog_active(void)
{
    return __atomic_load_n(&btlog_mode, __ATOMIC_RELAXED);
}

static unsigned btfix_active(void)
{
    return __atomic_load_n(&btfix_mode, __ATOMIC_RELAXED);
}

static void fuxi_h4_prepare_terminate(void)
{
}

static bool btfix_is_exec(const struct mach_header *hdr)
{
    return hdr != NULL && hdr->magic == MH_MAGIC_64 && hdr->filetype == MH_EXECUTE;
}

/*
 * 主程式標頭：先用 _NSGetMachExecuteHeader（dyld 啟動時就設定），不行再掃 dyld 映像表找 MH_EXECUTE。
 * 2026-11-20 02:01 Codex 那趟（native-hw000bt5-complete-1120）只看 _dyld_get_image_header(0)，
 * bluetoothd 三次都回 bad_header，H4 表沒補，關藍牙照樣 pc=0；失敗時這裡把映像數與第 0 個的型別一起印出。
 */
static const struct mach_header_64 *btfix_main_header(const char **how, char *why, size_t why_size)
{
    const struct mach_header *hdr = (const struct mach_header *)_NSGetMachExecuteHeader();
    if (btfix_is_exec(hdr)) {
        *how = "nsget";
        return (const struct mach_header_64 *)hdr;
    }
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header *image = _dyld_get_image_header(i);
        if (btfix_is_exec(image)) {
            *how = i == 0 ? "image0" : "scan";
            return (const struct mach_header_64 *)image;
        }
    }
    const struct mach_header *first = count ? _dyld_get_image_header(0) : NULL;
    snprintf(why, why_size, "no_exec(nsget=%s,n=%u,t0=%d)", hdr == NULL ? "null" : "other", count,
        first == NULL ? -1 : (int)first->filetype);
    return NULL;
}

static const char *btfix_apply_h4(char *buf, size_t buf_size)
{
    const char *how = "none";
    const struct mach_header_64 *hdr64 = btfix_main_header(&how, buf, buf_size);
    if (hdr64 == NULL) {
        return buf;
    }
    /* 滑移量由標頭實際位址減 __TEXT vmaddr 算出，不依賴映像序號 */
    const uint8_t *cursor = (const uint8_t *)(hdr64 + 1);
    const uint8_t *limit = cursor + hdr64->sizeofcmds;
    uint64_t text_vm = 0, text_size = 0, data_vm = 0, data_size = 0;
    bool have_text = false, have_data = false;

    for (uint32_t i = 0; i < hdr64->ncmds; i++) {
        if ((size_t)(limit - cursor) < sizeof(struct load_command)) {
            return "bad_cmds";
        }
        const struct load_command *lc = (const struct load_command *)cursor;
        if (lc->cmdsize < sizeof(struct load_command) || (size_t)(limit - cursor) < lc->cmdsize) {
            return "bad_cmds";
        }
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)cursor;
            if (strncmp(seg->segname, "__TEXT", sizeof(seg->segname)) == 0) {
                text_vm = seg->vmaddr;
                text_size = seg->vmsize;
                have_text = seg->fileoff == 0 && seg->vmsize > 0;
            } else if (strncmp(seg->segname, "__DATA", sizeof(seg->segname)) == 0) {
                data_vm = seg->vmaddr;
                data_size = seg->vmsize;
                have_data = seg->vmsize > 0;
            }
        }
        cursor += lc->cmdsize;
    }
    if (!have_text || !have_data) {
        return "no_segs";
    }
    /* __TEXT 從檔頭（fileoff 0）開始，所以標頭位址就是滑移後的 __TEXT 起點 */
    const uint64_t slide = (uint64_t)(uintptr_t)hdr64 - text_vm;
    const uint64_t text_start = text_vm + slide, data_start = data_vm + slide;
    if (UINT64_MAX - text_start < text_size || UINT64_MAX - data_start < data_size) {
        return "no_segs";
    }
    const uint64_t table_addr = FUXI_BT_H4_STATIC_ADDR + slide;
    const uint64_t replacement = (uint64_t)(uintptr_t)&fuxi_h4_prepare_terminate;
    snprintf(buf, buf_size, "%s/%s", fuxi_bt_h4_patch_result_name(fuxi_bt_patch_h4_table(table_addr, text_start,
        text_start + text_size, data_start, data_start + data_size, replacement)), how);
    return buf;
}

static char btlog_letter(uint8_t type)
{
    switch (type) {
    case 0:
        return 'N';
    case BTLOG_TYPE_INFO:
        return 'I';
    case BTLOG_TYPE_DEBUG:
        return 'D';
    case BTLOG_TYPE_ERROR:
        return 'E';
    case BTLOG_TYPE_FAULT:
        return 'F';
    default:
        return '?';
    }
}

/* 依類型與上限決定要不要印；回 true 時已計入 */
static bool btlog_admit(uint8_t type)
{
    const unsigned mode = btlog_active();
    if (btlog_after_ns != 0 && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < btlog_after_ns) {
        return false;
    }
    if (type == BTLOG_TYPE_INFO || type == BTLOG_TYPE_DEBUG) {
        if (!(mode & (type == BTLOG_TYPE_INFO ? 2u : 4u))) {
            return false;
        }
        return __atomic_add_fetch(&btlog_detail_lines, 1, __ATOMIC_RELAXED) <= BTLOG_DETAIL_LIMIT;
    }
    if (!(mode & 1u)) {
        return false;
    }
    return __atomic_add_fetch(&btlog_main_lines, 1, __ATOMIC_RELAXED) <= BTLOG_MAIN_LIMIT;
}

static void btlog_write(char letter, char *text, size_t length)
{
    char line[256];
    const unsigned sequence = __atomic_add_fetch(&btlog_sequence, 1, __ATOMIC_RELAXED);
    fuxi_oslog_sanitize(text, length);
    size_t offset = 0;
    unsigned part = 0;
    do {
        size_t chunk = length - offset;
        if (chunk > BTLOG_CHUNK) {
            chunk = BTLOG_CHUNK;
        }
        int used = snprintf(line, sizeof(line), "%s%s %c %u %.*s\n", btlog_tag, part ? "+" : "", letter, sequence,
            (int)chunk, text + offset);
        if (used > 0) {
            (void)write(2, line, (size_t)used < sizeof(line) ? (size_t)used : sizeof(line) - 1);
        }
        offset += chunk;
        part++;
    } while (offset < length);
}

static size_t btlog_copy_cfstr(CFStringRef str, char *out, size_t capacity)
{
    if (!str || capacity < 2) {
        return 0;
    }
    CFIndex used = 0;
    CFStringGetBytes(str, CFRangeMake(0, CFStringGetLength(str)), kCFStringEncodingUTF8,
        '?', false, (UInt8 *)out, (CFIndex)capacity - 1, &used);
    if (used < 0) {
        used = 0;
    }
    out[used] = 0;
    return (size_t)used;
}

static size_t btlog_describe(const void *object, char *out, size_t capacity)
{
    CFTypeRef value = object;
    if (!value || capacity < 2) {
        return 0;
    }
    const CFTypeID tid = CFGetTypeID(value);
    if (tid == CFStringGetTypeID()) {
        return btlog_copy_cfstr((CFStringRef)value, out, capacity);
    }
    if (tid == CFErrorGetTypeID()) {
        CFErrorRef err = (CFErrorRef)value;
        char domain[96];
        char desc[160];
        char under_dom[64];
        domain[0] = 0;
        desc[0] = 0;
        under_dom[0] = 0;
        btlog_copy_cfstr(CFErrorGetDomain(err), domain, sizeof(domain));
        CFStringRef cf_desc = CFErrorCopyDescription(err);
        if (cf_desc) {
            btlog_copy_cfstr(cf_desc, desc, sizeof(desc));
            CFRelease(cf_desc);
        }
        long under_code = 0;
        bool has_under = false;
        CFDictionaryRef info = CFErrorCopyUserInfo(err);
        if (info) {
            CFTypeRef under = CFDictionaryGetValue(info, kCFErrorUnderlyingErrorKey);
            if (under && CFGetTypeID(under) == CFErrorGetTypeID()) {
                btlog_copy_cfstr(CFErrorGetDomain((CFErrorRef)under), under_dom, sizeof(under_dom));
                under_code = (long)CFErrorGetCode((CFErrorRef)under);
                has_under = true;
            }
            CFRelease(info);
        }
        int n = has_under
            ? snprintf(out, capacity, "Err(%s:%ld %s [under=%s:%ld])", domain[0] ? domain : "?",
                (long)CFErrorGetCode(err), desc, under_dom[0] ? under_dom : "?", under_code)
            : snprintf(out, capacity, "Err(%s:%ld %s)", domain[0] ? domain : "?",
                (long)CFErrorGetCode(err), desc);
        return n > 0 ? ((size_t)n < capacity ? (size_t)n : capacity - 1) : 0;
    }
    if (tid == CFNumberGetTypeID()) {
        int64_t ival = 0;
        if (CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &ival)) {
            int n = snprintf(out, capacity, "%lld", (long long)ival);
            return n > 0 ? ((size_t)n < capacity ? (size_t)n : capacity - 1) : 0;
        }
    }
    if (tid == CFURLGetTypeID()) {
        return btlog_copy_cfstr(CFURLGetString((CFURLRef)value), out, capacity);
    }
    return 0;
}

__attribute__((noinline)) static void btlog_record(uint8_t type, const char *format, const uint8_t *buf, uint32_t size)
{
    if (btlog_busy || !btlog_admit(type)) {
        return;
    }
    btlog_busy++;
    char text[BTLOG_TEXT_MAX];
    const size_t length = fuxi_oslog_format_flags(text, sizeof(text), format, buf, size, btlog_describe, btlog_fmt_flags);
    btlog_write(btlog_letter(type), text, length);
    btlog_busy--;
}

__attribute__((noinline)) static void btlog_note(char letter, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
__attribute__((noinline)) static void btlog_note(char letter, const char *format, ...)
{
    char text[BTLOG_TEXT_MAX];
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    __builtin_va_end(args);
    if (length > 0) {
        btlog_write(letter, text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
    }
}

static void btlog_impl(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
    if (log == NULL) {
        return;
    }
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_impl(dso, log, type, format, buf, size);
}

static void btlog_error(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
    if (log == NULL) {
        return;
    }
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_error(dso, log, type, format, buf, size);
}

static void btlog_fault(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
    if (log == NULL) {
        return;
    }
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_fault(dso, log, type, format, buf, size);
}

static void btlog_debug(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
    if (log == NULL) {
        return;
    }
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_debug(dso, log, type, format, buf, size);
}

static bool btlog_enabled(void *log, uint8_t type)
{
    const unsigned mode = btlog_active();
    if (__builtin_expect(mode != 0, 0) && log != NULL &&
        ((type == BTLOG_TYPE_INFO && (mode & 2u)) || (type == BTLOG_TYPE_DEBUG && (mode & 4u))) &&
        btlog_real_enabled(log, BTLOG_TYPE_FAULT)) {
        return true;
    }
    return btlog_real_enabled(log, type);
}

static char *btlog_compose(void *pack, void *log, uint8_t type, char *buf, size_t size)
{
    if (__builtin_expect(btlog_active() == 0, 1)) {
        return btlog_real_compose(pack, log, type, buf, size);
    }
    char *text = btlog_real_compose(pack, log, type, buf, size);
    if (text && !btlog_busy && btlog_admit(type)) {
        btlog_note('C', "%c %s", btlog_letter(type), text);
    }
    return text;
}

__attribute__((noreturn)) static void btlog_exit(int status)
{
    if (btlog_active()) {
        btlog_note('X', "exit status=%d", status);
    }
    btlog_real_exit(status);
}

__attribute__((noreturn)) static void btlog_quick_exit(int status)
{
    if (btlog_active()) {
        btlog_note('X', "_exit status=%d", status);
    }
    btlog_real_quick_exit(status);
}

__attribute__((noreturn)) static void btlog_abort_payload(uint32_t space, uint64_t code, void *payload,
    uint32_t payload_size, const char *reason, uint64_t flags)
{
    if (btlog_active()) {
        btlog_note('X', "abort_with_payload namespace=%u code=0x%llx flags=0x%llx reason=%.700s", space,
            (unsigned long long)code, (unsigned long long)flags, reason ? reason : "(null)");
    }
    btlog_real_abort_payload(space, code, payload, payload_size, reason, flags);
}

static CFPropertyListRef btlog_prefs_copy_app_value(CFStringRef key, CFStringRef application_id)
{
    if (__builtin_expect((btfix_active() & FUXI_BTFIX_SINGLE_ADV) == 0, 1)) {
        return btlog_real_prefs_copy_app_value(key, application_id);
    }
    if (key == NULL || application_id == NULL || CFGetTypeID(key) != CFStringGetTypeID() ||
        CFGetTypeID(application_id) != CFStringGetTypeID() || !CFEqual(key, CFSTR("LE")) ||
        !CFEqual(application_id, CFSTR("com.apple.MobileBluetooth.debug"))) {
        return btlog_real_prefs_copy_app_value(key, application_id);
    }
    CFPropertyListRef existing = btlog_real_prefs_copy_app_value(key, application_id);
    CFMutableDictionaryRef dict = NULL;
    if (existing != NULL && CFGetTypeID(existing) == CFDictionaryGetTypeID()) {
        dict = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, (CFDictionaryRef)existing);
    } else {
        dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
    }
    if (dict == NULL) {
        return existing;
    }
    if (existing != NULL) {
        CFRelease(existing);
    }
    CFDictionarySetValue(dict, CFSTR("SupportsMultipleAdvertising"), kCFBooleanFalse);
    return dict;
}

struct btlog_interpose {
    const void *replacement;
    const void *original;
};

#define BTLOG_INTERPOSE(replacement, original)                                                                       \
    __attribute__((used, section("__DATA,__interpose"))) static const struct btlog_interpose                         \
        btlog_tuple_##replacement = { (const void *)&replacement, (const void *)&original }

BTLOG_INTERPOSE(btlog_impl, btlog_real_impl);
BTLOG_INTERPOSE(btlog_error, btlog_real_error);
BTLOG_INTERPOSE(btlog_fault, btlog_real_fault);
BTLOG_INTERPOSE(btlog_debug, btlog_real_debug);
BTLOG_INTERPOSE(btlog_enabled, btlog_real_enabled);
BTLOG_INTERPOSE(btlog_compose, btlog_real_compose);
BTLOG_INTERPOSE(btlog_exit, btlog_real_exit);
BTLOG_INTERPOSE(btlog_quick_exit, btlog_real_quick_exit);
BTLOG_INTERPOSE(btlog_abort_payload, btlog_real_abort_payload);
BTLOG_INTERPOSE(btlog_prefs_copy_app_value, btlog_real_prefs_copy_app_value);

__attribute__((constructor)) static void fuxi_btlog_init(void)
{
    const char *name = getprogname();
    if (name == NULL) {
        return;
    }
    if (strcmp(name, "mediaserverd") == 0) {
        const unsigned raw = btlog_adt_u32("fuxi,media-oslog");
        const unsigned mode = raw & 7u;
        const unsigned delay_s = (raw >> 8) & 0x3ffu;
        if (!mode) {
            return;
        }
        btlog_tag = "MDLOG";
        btlog_after_ns = (uint64_t)delay_s * 1000000000ULL;
        __atomic_store_n(&btlog_mode, mode, __ATOMIC_RELEASE);
        btlog_note('S', "on mode=%u delay_s=%u pid=%d", mode, delay_s, getpid());
        return;
    }
    if (strcmp(name, "VoiceMemos") == 0 || strcmp(name, "voicememod") == 0) {
        const unsigned inject = btlog_adt_u32("fuxi,inject");
        const unsigned raw = inject ? btlog_adt_u32("fuxi,vm-oslog") : 3u;
        const unsigned mode = raw & 7u;
        const unsigned delay_s = (raw >> 8) & 0x3ffu;
        if (!mode) {
            return;
        }
        btlog_tag = (name[0] == 'V') ? "VMLOG" : "VMDLOG";
        btlog_fmt_flags = FUXI_OSLOG_FMT_ALLOW_PRIVATE_DIAG;
        btlog_after_ns = (uint64_t)delay_s * 1000000000ULL;
        __atomic_store_n(&btlog_mode, mode, __ATOMIC_RELEASE);
        btlog_note('S', "on mode=%u delay_s=%u adt=%u pid=%d", mode, delay_s, inject != 0 ? 1u : 0u, getpid());
        return;
    }
    const char *dict_tag = NULL;
    if (strcmp(name, "assistantd") == 0) {
        dict_tag = "ASLOG";
    } else if (strcmp(name, "assistant_service") == 0) {
        dict_tag = "ASSVCLOG";
    } else if (strcmp(name, "corespeechd") == 0) {
        dict_tag = "CSLOG";
    } else if (strcmp(name, "kbd") == 0) {
        dict_tag = "KBDLOG";
    }
    if (dict_tag != NULL) {
        const unsigned raw = btlog_adt_u32("fuxi,dict-oslog");
        const unsigned mode = raw & 7u;
        const unsigned delay_s = (raw >> 8) & 0x3ffu;
        if (!mode) {
            return;
        }
        btlog_tag = dict_tag;
        btlog_fmt_flags = FUXI_OSLOG_FMT_ALLOW_PRIVATE_DIAG;
        btlog_after_ns = (uint64_t)delay_s * 1000000000ULL;
        __atomic_store_n(&btlog_mode, mode, __ATOMIC_RELEASE);
        btlog_note('S', "on mode=%u delay_s=%u pid=%d", mode, delay_s, getpid());
        return;
    }
    if (strcmp(name, "bluetoothd") != 0) {
        return;
    }
    const unsigned mode = btlog_adt_u32("fuxi,bt-oslog") & 7u;
    const unsigned fix = btlog_adt_u32("fuxi,bt-fix") & FUXI_BTFIX_ALL;
    if (!mode && !fix) {
        return;
    }
    if (mode) {
        __atomic_store_n(&btlog_mode, mode, __ATOMIC_RELAXED);
    }
    static char h4_buf[96];
    const char *h4_status = "off";
    if (fix & FUXI_BTFIX_H4_PREPARE) {
        h4_status = btfix_apply_h4(h4_buf, sizeof(h4_buf));
    }
    if (fix) {
        __atomic_store_n(&btfix_mode, fix, __ATOMIC_RELAXED);
    }
    btlog_note('S', "on mode=%u fix=%u h4=%s pid=%d", mode, fix, h4_status, getpid());
}
