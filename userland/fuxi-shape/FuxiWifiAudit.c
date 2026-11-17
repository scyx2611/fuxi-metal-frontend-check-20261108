#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static IMP wifi_audit_result_original, wifi_audit_parameters_original;
static unsigned wifi_audit_result_done, wifi_audit_parameters_done;
static unsigned wifi_audit_result_calls, wifi_audit_parameters_calls;

static void wifi_audit_say(const char *line)
{
    (void)write(2, line, strlen(line));
}

static bool wifi_audit_type(Method method, unsigned argument, char expected)
{
    char type[64];
    if (argument == 0) method_getReturnType(method, type, sizeof(type));
    else method_getArgumentType(method, argument, type, sizeof(type));
    const char *value = type;
    while (*value && strchr("rnNoORV", *value)) value++;
    return *value == expected;
}

static intptr_t wifi_audit_integer(id object, const char *name)
{
    if (object == NULL) return -1;
    SEL selector = sel_registerName(name);
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    char type[32];
    if (method == NULL || method_getNumberOfArguments(method) != 2) return -1;
    method_getReturnType(method, type, sizeof(type));
    if (type[0] == '\0' || strchr("cCiIsSlLqQB", type[0]) == NULL) return -1;
    return ((intptr_t (*)(id, SEL))method_getImplementation(method))(object, selector);
}

static uint32_t wifi_audit_ssid(id object)
{
    SEL selector = sel_registerName("SSID");
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    if (method == NULL || method_getNumberOfArguments(method) != 2 || !wifi_audit_type(method, 0, '@')) return 0;
    id ssid = ((id (*)(id, SEL))method_getImplementation(method))(object, selector);
    intptr_t length = wifi_audit_integer(ssid, "length");
    if (length <= 0 || length > 32) return 0;
    selector = sel_registerName("bytes");
    method = class_getInstanceMethod(object_getClass(ssid), selector);
    if (method == NULL || method_getNumberOfArguments(method) != 2 || !wifi_audit_type(method, 0, '^')) return 0;
    const uint8_t *bytes = ((const uint8_t *(*)(id, SEL))method_getImplementation(method))(ssid, selector);
    if (bytes == NULL) return 0;
    uint32_t identifier = UINT32_C(2166136261);
    for (intptr_t index = 0; index < length; index++)
        identifier = (identifier ^ bytes[index]) * UINT32_C(16777619);
    return identifier;
}

static id wifi_audit_result(id self, SEL selector, id record, id properties)
{
    id result = ((id (*)(id, SEL, id, id))wifi_audit_result_original)(self, selector, record, properties);
    unsigned call = __atomic_fetch_add(&wifi_audit_result_calls, 1u, __ATOMIC_RELAXED);
    if (call < 96) {
        char line[320];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: scan audit call=%u created=%u ssid-id=%08x age-ms=%ld rssi=%ld ap-mode=%ld ess=%ld\n",
            call + 1, result != NULL, result ? wifi_audit_ssid(result) : 0,
            (long)wifi_audit_integer(result, "age"), (long)wifi_audit_integer(result, "RSSI"),
            (long)wifi_audit_integer(result, "APMode"), (long)wifi_audit_integer(result, "isESS"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    return result;
}

static id wifi_audit_parameters(id self, SEL selector)
{
    id result = ((id (*)(id, SEL))wifi_audit_parameters_original)(self, selector);
    unsigned call = __atomic_fetch_add(&wifi_audit_parameters_calls, 1u, __ATOMIC_RELAXED);
    if (call < 24) {
        char line[320];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: scan filter call=%u maximum-age=%ld minimum-rssi=%ld bss-type=%ld hidden=%ld "
            "include-present=%u include-count=%ld\n", call + 1,
            (long)wifi_audit_integer(self, "maximumAge"), (long)wifi_audit_integer(self, "minimumRSSI"),
            (long)wifi_audit_integer(self, "BSSType"), (long)wifi_audit_integer(self, "includeHiddenNetworks"),
            result != NULL, (long)wifi_audit_integer(result, "count"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    return result;
}

static void wifi_audit_image(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    if (!__atomic_load_n(&wifi_audit_result_done, __ATOMIC_ACQUIRE)) {
        Class target = objc_getClass("CWFScanResult");
        Method method = target ? class_getInstanceMethod(target,
            sel_registerName("initWithScanRecord:includeProperties:")) : NULL;
        if (method && !__atomic_exchange_n(&wifi_audit_result_done, 1u, __ATOMIC_ACQ_REL)) {
            if (method_getNumberOfArguments(method) == 4 && wifi_audit_type(method, 0, '@') &&
                wifi_audit_type(method, 2, '@') && wifi_audit_type(method, 3, '@')) {
                wifi_audit_result_original = method_getImplementation(method);
                (void)method_setImplementation(method, (IMP)wifi_audit_result);
                wifi_audit_say("fuxi: wifid: scan result audit installed\n");
            } else {
                wifi_audit_say("fuxi: wifid: scan result audit refused ABI\n");
            }
        }
    }
    if (!__atomic_load_n(&wifi_audit_parameters_done, __ATOMIC_ACQUIRE)) {
        Class target = objc_getClass("CWFScanParameters");
        Method method = target ? class_getInstanceMethod(target, sel_registerName("includeScanResults")) : NULL;
        if (method && !__atomic_exchange_n(&wifi_audit_parameters_done, 1u, __ATOMIC_ACQ_REL)) {
            if (method_getNumberOfArguments(method) == 2 && wifi_audit_type(method, 0, '@')) {
                wifi_audit_parameters_original = method_getImplementation(method);
                (void)method_setImplementation(method, (IMP)wifi_audit_parameters);
                wifi_audit_say("fuxi: wifid: scan parameter audit installed\n");
            } else {
                wifi_audit_say("fuxi: wifid: scan parameter audit refused ABI\n");
            }
        }
    }
}

__attribute__((constructor)) static void fuxi_wifi_audit_init(void)
{
    const char *name = getprogname();
    if (name == NULL || strcmp(name, "wifid") != 0) return;
    _dyld_register_func_for_add_image(wifi_audit_image);
}
