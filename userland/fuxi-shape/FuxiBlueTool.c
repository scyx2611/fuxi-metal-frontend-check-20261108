/*
 * BlueTool 替身（fxinj bit 5，ADT fuxi,inject 含 32）：核心把 libfuxi-sb.dylib 注入 /usr/sbin/BlueTool，
 * 這個建構子在 BlueTool 的 main 之前接手 launchd 的 com.apple.BlueTool Mach 服務並停在 dispatch_main，
 * 原廠只認 Broadcom（HCD 下載、ACIPC／UART 裝置）的啟動流程不會執行。WCN7850 由核心 fuxi,bt=5 上電並
 * 下載韌體，這裡只回報結果；HCI、配對與設定檔仍由 bluetoothd 經 UART.SK 處理。
 *
 * 協定改寫自 UniXNU QcaBluetooth userland/BlueTool.c（BSD-2-Clause-Patent，daimile2 and contributors），
 * sysctl kern.qbt_power_on／qbt_boot／qbt_firmware 由 ksrc xnu/fuxi-bluetooth/fuxi_bt_sk.c 提供。
 * 對照 20H392：BlueTool 0x100041e28 讀 kMsgId／kMsgArgs／script；bluetoothd 0x100272034 送出 script，
 * 回覆函式 0x10027249c 把 result=0 當成功、10／13 當「找不到平台／晶片組合」、其餘當失敗，並讀 firmware
 * 字串與 manufacturer。
 */
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

/* Theos iPhoneOS16.5 SDK 沒有 xpc／IOKit 標頭；符號由 libSystem.B.tbd 重新匯出的 libxpc 與 IOKit.tbd 提供。
 * 型別依 libxpc 公開標頭的 C（非 Objective-C）定義，listener 旗標名稱與值同 libxpc。 */
typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t object);
extern const struct _xpc_type_s _xpc_type_dictionary;
extern const struct _xpc_type_s _xpc_type_connection;
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *string);
extern xpc_connection_t xpc_dictionary_get_remote_connection(xpc_object_t dictionary);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
extern void xpc_connection_set_target_queue(xpc_connection_t connection, dispatch_queue_t queue);
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_release(xpc_object_t object);
/* iOS SDK 把這個函式標成不可用，這裡用別名直接連到同一個符號。 */
extern xpc_connection_t fuxi_bt_xpc_listener(const char *, dispatch_queue_t, uint64_t)
    __asm__("_xpc_connection_create_mach_service");

/* IOKit.framework 在 iPhoneOS SDK 沒有標頭：同 FuxiShape.c 只宣告用到的三個呼叫 */
typedef mach_port_t bluetool_io_object_t;
extern bluetool_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(bluetool_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(bluetool_io_object_t object);

#define FUXI_BLUETOOL_INJECT 32u
#define FUXI_BLUETOOL_QUALCOMM 29 /* Bluetooth SIG company identifier */

static unsigned bluetool_requests;

static unsigned bluetool_adt_u32(const char *key)
{
    unsigned value = 0;
    bluetool_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
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

/* 核心在控制器還在初始化時最多等 30 秒才回答。 */
static int bluetool_sysctl(const char *name)
{
    int result = EIO;
    size_t length = sizeof(result);
    if (sysctlbyname(name, &result, &length, NULL, 0) != 0) {
        return errno;
    }
    return length == sizeof(result) ? result : EIO;
}

static void bluetool_handle(xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
        return;
    }
    xpc_object_t reply = xpc_dictionary_create_reply(message);
    if (!reply) {
        return;
    }
    const char *id = xpc_dictionary_get_string(message, "kMsgId");
    const char *script = NULL;
    int error = ENOTSUP;
    if (id && strcmp(id, "RunBuiltinScript") == 0) {
        xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
        if (args && xpc_get_type(args) == XPC_TYPE_DICTIONARY) {
            script = xpc_dictionary_get_string(args, "script");
        }
        if (script && (strcmp(script, "boot") == 0 || strcmp(script, "init") == 0)) {
            error = bluetool_sysctl("kern.qbt_power_on");
        } else if (script && strcmp(script, "deepsleep") == 0) {
            /* 控制器已經停了也算睡眠成功 */
            error = bluetool_sysctl("kern.qbt_boot");
            if (error == ENETDOWN || error == EPERM) {
                error = 0;
            }
        } else {
            error = EINVAL;
        }
    }
    xpc_dictionary_set_int64(reply, "result", error ? 1 : 0);
    char firmware[64] = {0};
    if (!error) {
        size_t length = sizeof(firmware) - 1;
        (void)sysctlbyname("kern.qbt_firmware", firmware, &length, NULL, 0);
        xpc_dictionary_set_int64(reply, "manufacturer", FUXI_BLUETOOL_QUALCOMM);
        xpc_dictionary_set_string(reply, "firmware", firmware);
    }
    if (id && strcmp(id, "RunCommand") == 0) {
        xpc_dictionary_set_string(reply, "output", "Unsupported controller command");
    }
    unsigned count = __atomic_add_fetch(&bluetool_requests, 1, __ATOMIC_RELAXED);
    if (count <= 32) {
        dprintf(2, "fuxi: BlueTool request=%u id=%s script=%s error=%d result=%d firmware=%s\n", count,
            id ? id : "missing", script ? script : "-", error, error ? 1 : 0, firmware[0] ? firmware : "-");
    }
    xpc_connection_t remote = xpc_dictionary_get_remote_connection(message);
    if (remote) {
        xpc_connection_send_message(remote, reply);
    }
    xpc_release(reply);
}

__attribute__((constructor)) static void fuxi_bluetool_init(void)
{
    const char *name = getprogname();
    if (name == NULL || strcmp(name, "BlueTool") != 0) {
        return;
    }
    unsigned inject = bluetool_adt_u32("fuxi,inject");
    if (!(inject & FUXI_BLUETOOL_INJECT)) {
        dprintf(2, "fuxi: BlueTool adapter off (fuxi,inject=%u); original BlueTool runs\n", inject);
        return;
    }
    dispatch_queue_t queue = dispatch_queue_create("fuxi.bluetool", DISPATCH_QUEUE_SERIAL);
    xpc_connection_t listener = fuxi_bt_xpc_listener("com.apple.BlueTool", queue, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) {
        dprintf(2, "fuxi: BlueTool adapter listener unavailable; original BlueTool runs\n");
        return;
    }
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
            return;
        }
        xpc_connection_set_target_queue(peer, queue);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
            bluetool_handle(event);
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);
    dprintf(2, "fuxi: BlueTool adapter listening com.apple.BlueTool qbt_boot=%d (original main skipped)\n",
        bluetool_sysctl("kern.qbt_boot"));
    /* 不回到 dyld：原廠 main 不執行，程序由 launchd 管理生命週期。 */
    dispatch_main();
}
