/*
 * bluetoothd os_log 轉錄（fxinj bit 6，ADT fuxi,inject 含 64；/chosen u32 fuxi,bt-oslog 非 0 才動作）。
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
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define BTLOG_TYPE_INFO 0x01u
#define BTLOG_TYPE_DEBUG 0x02u
#define BTLOG_TYPE_ERROR 0x10u
#define BTLOG_TYPE_FAULT 0x11u
#define BTLOG_MAIN_LIMIT 4000u
#define BTLOG_DETAIL_LIMIT 1500u
#define BTLOG_TEXT_MAX 900u
#define BTLOG_CHUNK 220u

static unsigned btlog_mode;
static unsigned btlog_main_lines;
static unsigned btlog_detail_lines;
static unsigned btlog_sequence;
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
        int used = snprintf(line, sizeof(line), "BTLOG%s %c %u %.*s\n", part ? "+" : "", letter, sequence, (int)chunk,
            text + offset);
        if (used > 0) {
            (void)write(2, line, (size_t)used < sizeof(line) ? (size_t)used : sizeof(line) - 1);
        }
        offset += chunk;
        part++;
    } while (offset < length);
}

static size_t btlog_describe(const void *object, char *out, size_t capacity)
{
    CFTypeRef value = object;
    if (capacity < 2 || CFGetTypeID(value) != CFStringGetTypeID()) {
        return 0;
    }
    CFIndex used = 0;
    CFStringGetBytes((CFStringRef)value, CFRangeMake(0, CFStringGetLength((CFStringRef)value)), kCFStringEncodingUTF8,
        '?', false, (UInt8 *)out, (CFIndex)capacity - 1, &used);
    return used > 0 ? (size_t)used : 0;
}

__attribute__((noinline)) static void btlog_record(uint8_t type, const char *format, const uint8_t *buf, uint32_t size)
{
    if (btlog_busy || !btlog_admit(type)) {
        return;
    }
    btlog_busy++;
    char text[BTLOG_TEXT_MAX];
    const size_t length = fuxi_oslog_format(text, sizeof(text), format, buf, size, btlog_describe);
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
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_impl(dso, log, type, format, buf, size);
}

static void btlog_error(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_error(dso, log, type, format, buf, size);
}

static void btlog_fault(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_fault(dso, log, type, format, buf, size);
}

static void btlog_debug(void *dso, void *log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
    if (__builtin_expect(btlog_active() != 0, 0)) {
        btlog_record(type, format, buf, size);
    }
    btlog_real_debug(dso, log, type, format, buf, size);
}

static bool btlog_enabled(void *log, uint8_t type)
{
    const unsigned mode = btlog_active();
    if (__builtin_expect(mode != 0, 0) &&
        ((type == BTLOG_TYPE_INFO && (mode & 2u)) || (type == BTLOG_TYPE_DEBUG && (mode & 4u)))) {
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

__attribute__((constructor)) static void fuxi_btlog_init(void)
{
    const char *name = getprogname();
    if (name == NULL || strcmp(name, "bluetoothd") != 0) {
        return;
    }
    const unsigned mode = btlog_adt_u32("fuxi,bt-oslog") & 7u;
    if (!mode) {
        return;
    }
    __atomic_store_n(&btlog_mode, mode, __ATOMIC_RELAXED);
    btlog_note('S', "on mode=%u pid=%d", mode, getpid());
}
