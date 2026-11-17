#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "FuxiErrorAudit.h"

static IMP wifi_audit_result_original, wifi_audit_parameters_original;
static unsigned wifi_audit_result_done, wifi_audit_parameters_done;
static unsigned wifi_audit_result_calls, wifi_audit_parameters_calls;
static IMP wifi_audit_match_original, wifi_audit_preflight_original, wifi_audit_profile_original;
static IMP wifi_audit_xpc_original, wifi_audit_filtered_original;
static unsigned wifi_audit_match_calls, wifi_audit_preflight_calls, wifi_audit_profile_calls;
static unsigned wifi_audit_xpc_calls, wifi_audit_filtered_calls;
static IMP wifi_audit_known_original, wifi_audit_native_known_original, wifi_audit_allow_original;
static IMP wifi_audit_sort_original, wifi_audit_transition_original;
static unsigned wifi_audit_known_calls, wifi_audit_native_known_calls, wifi_audit_allow_calls;
static unsigned wifi_audit_sort_calls, wifi_audit_transition_calls;
static IMP wifi_audit_perform_original, wifi_audit_associate_original;
static unsigned wifi_audit_perform_calls, wifi_audit_associate_calls;

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
    if (expected == 'b') return *value == 'B' || *value == 'c';
    return *value == expected;
}

static id wifi_audit_object(id object, const char *name)
{
    if (object == NULL) return NULL;
    SEL selector = sel_registerName(name);
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    if (method == NULL || method_getNumberOfArguments(method) != 2 || !wifi_audit_type(method, 0, '@')) return NULL;
    return ((id (*)(id, SEL))method_getImplementation(method))(object, selector);
}

static intptr_t wifi_audit_integer(id object, const char *name)
{
    if (object == NULL) return -1;
    SEL selector = sel_registerName(name);
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    char type[32];
    if (method == NULL || method_getNumberOfArguments(method) != 2) return -1;
    method_getReturnType(method, type, sizeof(type));
    const char *value = type;
    while (*value && strchr("rnNoORV", *value)) value++;
    IMP implementation = method_getImplementation(method);
    switch (*value) {
        case 'c': return ((int8_t (*)(id, SEL))implementation)(object, selector);
        case 'C': return ((uint8_t (*)(id, SEL))implementation)(object, selector);
        case 's': return ((int16_t (*)(id, SEL))implementation)(object, selector);
        case 'S': return ((uint16_t (*)(id, SEL))implementation)(object, selector);
        case 'i': return ((int32_t (*)(id, SEL))implementation)(object, selector);
        case 'I': return ((uint32_t (*)(id, SEL))implementation)(object, selector);
        case 'l': case 'q': return ((intptr_t (*)(id, SEL))implementation)(object, selector);
        case 'L': case 'Q': return (intptr_t)((uintptr_t (*)(id, SEL))implementation)(object, selector);
        case 'B': return ((bool (*)(id, SEL))implementation)(object, selector);
        default: return -1;
    }
}

static uint32_t wifi_audit_ssid(id object)
{
    id ssid = wifi_audit_object(object, "SSID");
    intptr_t length = wifi_audit_integer(ssid, "length");
    if (length <= 0 || length > 32) return 0;
    SEL selector = sel_registerName("bytes");
    Method method = class_getInstanceMethod(object_getClass(ssid), selector);
    if (method == NULL || method_getNumberOfArguments(method) != 2 || !wifi_audit_type(method, 0, '^')) return 0;
    const uint8_t *bytes = ((const uint8_t *(*)(id, SEL))method_getImplementation(method))(ssid, selector);
    if (bytes == NULL) return 0;
    uint32_t identifier = UINT32_C(2166136261);
    for (intptr_t index = 0; index < length; index++)
        identifier = (identifier ^ bytes[index]) * UINT32_C(16777619);
    return identifier;
}

static void wifi_audit_network(const char *stage, unsigned call, unsigned index, id object)
{
    char line[480];
    int length = snprintf(line, sizeof(line),
        "fuxi: wifid: %s call=%u index=%u ssid-id=%08x age-ms=%ld rssi=%ld security=%ld "
        "security-mask=%ld effective-mask=%ld wpa2=%ld psk=%ld autojoin-off=%ld disallowed=%ld fils=%ld\n",
        stage, call, index, wifi_audit_ssid(object),
        (long)wifi_audit_integer(object, "age"), (long)wifi_audit_integer(object, "RSSI"),
        (long)wifi_audit_integer(object, "strongestSupportedSecurityType"),
        (long)wifi_audit_integer(object, "supportedSecurityTypes"),
        (long)wifi_audit_integer(object, "effectiveSupportedSecurityTypes"),
        (long)wifi_audit_integer(object, "isWPA2"), (long)wifi_audit_integer(object, "isPSK"),
        (long)wifi_audit_integer(object, "isAutoJoinDisabled"),
        (long)wifi_audit_integer(object, "isAssociationDisallowed"),
        (long)wifi_audit_integer(object, "isFILSDiscoveryFrame"));
    if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
}

static void wifi_audit_networks(const char *stage, unsigned call, id objects)
{
    intptr_t count = wifi_audit_integer(objects, "count");
    if (count <= 0) return;
    SEL selector = sel_registerName("objectAtIndex:");
    Method method = class_getInstanceMethod(object_getClass(objects), selector);
    if (method == NULL || method_getNumberOfArguments(method) != 3 ||
        !wifi_audit_type(method, 0, '@') || !wifi_audit_type(method, 2, 'Q')) return;
    for (intptr_t index = 0; index < count && index < 12; index++) {
        id object = ((id (*)(id, SEL, uintptr_t))method_getImplementation(method))(objects, selector, (uintptr_t)index);
        wifi_audit_network(stage, call, (unsigned)index, object);
    }
}

static id wifi_audit_result(id self, SEL selector, id record, id properties)
{
    id result = ((id (*)(id, SEL, id, id))wifi_audit_result_original)(self, selector, record, properties);
    unsigned call = __atomic_fetch_add(&wifi_audit_result_calls, 1u, __ATOMIC_RELAXED);
    if (call < 96) {
        char line[480];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: scan audit call=%u created=%u ssid-id=%08x age-ms=%ld rssi=%ld ap-mode=%ld ess=%ld "
            "security=%ld wpa2=%ld psk=%ld disallowed=%ld\n",
            call + 1, result != NULL, result ? wifi_audit_ssid(result) : 0,
            (long)wifi_audit_integer(result, "age"), (long)wifi_audit_integer(result, "RSSI"),
            (long)wifi_audit_integer(result, "APMode"), (long)wifi_audit_integer(result, "isESS"),
            (long)wifi_audit_integer(result, "strongestSupportedSecurityType"),
            (long)wifi_audit_integer(result, "isWPA2"), (long)wifi_audit_integer(result, "isPSK"),
            (long)wifi_audit_integer(result, "isAssociationDisallowed"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    return result;
}

static bool wifi_audit_match(id self, SEL selector, id results, bool preassociation, id context, id *error)
{
    unsigned call = __atomic_fetch_add(&wifi_audit_match_calls, 1u, __ATOMIC_RELAXED);
    if (call < 12) {
        id known = wifi_audit_object(context, "knownNetworks");
        char line[320];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: candidate audit call=%u results=%ld known=%ld minimum-rssi=%ld preassociation=%u\n",
            call + 1, (long)wifi_audit_integer(results, "count"), (long)wifi_audit_integer(known, "count"),
            (long)wifi_audit_integer(context, "minRSSI"), preassociation);
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        wifi_audit_networks("candidate scan", call + 1, results);
        wifi_audit_networks("candidate known", call + 1, wifi_audit_object(known, "array"));
    }
    bool result = ((bool (*)(id, SEL, id, bool, id, id *))wifi_audit_match_original)(
        self, selector, results, preassociation, context, error);
    if (call < 12) {
        char line[160];
        int length = snprintf(line, sizeof(line), "fuxi: wifid: candidate return call=%u joined=%u error=%ld\n",
            call + 1, result, (long)wifi_audit_integer(!result && error ? *error : NULL, "code"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    return result;
}

static bool wifi_audit_preflight(id self, SEL selector, id scan)
{
    bool result = ((bool (*)(id, SEL, id))wifi_audit_preflight_original)(self, selector, scan);
    unsigned call = __atomic_fetch_add(&wifi_audit_preflight_calls, 1u, __ATOMIC_RELAXED);
    if (call < 96) {
        char line[160];
        int length = snprintf(line, sizeof(line), "fuxi: wifid: preflight call=%u ssid-id=%08x allowed=%u\n",
            call + 1, wifi_audit_ssid(scan), result);
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    return result;
}

static id wifi_audit_profile(id self, SEL selector, id profile)
{
    id result = ((id (*)(id, SEL, id))wifi_audit_profile_original)(self, selector, profile);
    unsigned call = __atomic_fetch_add(&wifi_audit_profile_calls, 1u, __ATOMIC_RELAXED);
    if (call < 32) {
        wifi_audit_network("matched scan", call + 1, 0, result);
        wifi_audit_network("matched profile", call + 1, 0, profile);
    }
    return result;
}

static id wifi_audit_xpc(id self, SEL selector, id profiles, id scans)
{
    id result = ((id (*)(id, SEL, id, id))wifi_audit_xpc_original)(self, selector, profiles, scans);
    unsigned call = __atomic_fetch_add(&wifi_audit_xpc_calls, 1u, __ATOMIC_RELAXED);
    if (call < 8) {
        char line[160];
        int length = snprintf(line, sizeof(line), "fuxi: wifid: profile match call=%u known=%ld scans=%ld output=%ld\n",
            call + 1, (long)wifi_audit_integer(profiles, "count"), (long)wifi_audit_integer(scans, "count"),
            (long)wifi_audit_integer(result, "count"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        wifi_audit_networks("profile input", call + 1, profiles);
    }
    return result;
}

static id wifi_audit_filtered(id self, SEL selector, id properties)
{
    id result = ((id (*)(id, SEL, id))wifi_audit_filtered_original)(self, selector, properties);
    unsigned call = __atomic_fetch_add(&wifi_audit_filtered_calls, 1u, __ATOMIC_RELAXED);
    if (call < 96) wifi_audit_network("filtered scan", call + 1, 0, result);
    return result;
}

static bool wifi_audit_known_call(IMP original, const char *stage, unsigned *calls, id self, SEL selector,
    id profile, intptr_t trigger, id transition, bool *deferred, id *error)
{
    bool result = ((bool (*)(id, SEL, id, intptr_t, id, bool *, id *))original)(
        self, selector, profile, trigger, transition, deferred, error);
    unsigned call = __atomic_fetch_add(calls, 1u, __ATOMIC_RELAXED);
    if (call < 48) {
        id failure = !result && error ? *error : NULL;
        char line[400];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: %s call=%u ssid-id=%08x trigger=%ld transition=%u allowed=%u deferred=%d "
            "error-present=%u error=%ld autojoin-off=%ld captive=%ld was-captive=%ld\n", stage, call + 1,
            wifi_audit_ssid(profile), (long)trigger, transition != NULL, result,
            deferred ? (int)*deferred : -1, failure != NULL, (long)wifi_audit_integer(failure, "code"),
            (long)wifi_audit_integer(profile, "isAutoJoinDisabled"), (long)wifi_audit_integer(profile, "isCaptive"),
            (long)wifi_audit_integer(profile, "wasCaptive"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    return result;
}

static bool wifi_audit_known(id self, SEL selector, id profile, intptr_t trigger, id transition, bool *deferred,
    id *error)
{
    return wifi_audit_known_call(wifi_audit_known_original, "known decision", &wifi_audit_known_calls,
        self, selector, profile, trigger, transition, deferred, error);
}

static bool wifi_audit_native_known(id self, SEL selector, id profile, intptr_t trigger, id transition,
    bool *deferred, id *error)
{
    return wifi_audit_known_call(wifi_audit_native_known_original, "native known decision",
        &wifi_audit_native_known_calls, self, selector, profile, trigger, transition, deferred, error);
}

static bool wifi_audit_allow(id self, SEL selector, id scan, intptr_t trigger, bool *deferred, id *error)
{
    bool result = ((bool (*)(id, SEL, id, intptr_t, bool *, id *))wifi_audit_allow_original)(
        self, selector, scan, trigger, deferred, error);
    unsigned call = __atomic_fetch_add(&wifi_audit_allow_calls, 1u, __ATOMIC_RELAXED);
    if (call < 32) {
        id failure = !result && error ? *error : NULL;
        char line[240];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: join decision call=%u ssid-id=%08x trigger=%ld allowed=%u deferred=%d "
            "error-present=%u error=%ld\n", call + 1, wifi_audit_ssid(scan), (long)trigger, result,
            deferred ? (int)*deferred : -1, failure != NULL, (long)wifi_audit_integer(failure, "code"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        FuxiErrorAuditEntry entries[FUXI_ERROR_AUDIT_DEPTH];
        unsigned count = fuxi_error_audit((CFTypeRef)failure, entries);
        for (unsigned depth = 0; depth < count; depth++) {
            length = snprintf(line, sizeof(line),
                "fuxi: wifid: join error call=%u depth=%u domain-id=%08x code=%ld reason=%s\n",
                call + 1, depth, entries[depth].domain, (long)entries[depth].code, entries[depth].reason);
            if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        }
    }
    return result;
}

/* 加入失敗的錯誤鏈：只輸出 domain 雜湊、數字碼與固定分類，不輸出 userInfo 任意字串 */
static void wifi_audit_error_chain(const char *stage, unsigned call, id failure)
{
    FuxiErrorAuditEntry entries[FUXI_ERROR_AUDIT_DEPTH];
    unsigned count = fuxi_error_audit((CFTypeRef)failure, entries);
    for (unsigned depth = 0; depth < count; depth++) {
        char line[240];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: %s error call=%u depth=%u domain-id=%08x code=%ld reason=%s\n",
            stage, call, depth, entries[depth].domain, (long)entries[depth].code, entries[depth].reason);
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
}

/*
 * CWFAutoJoinManager -__performJoinWithNetwork:context:error:
 * 2026-11-17 反組譯（CoreWiFi 20H392）：失敗只有 code 6（associateToNetworkHandler 為 nil）或
 * code 57（__calloutToAssociateWithParameters:error: 失敗，底層錯誤在 userInfo）；失敗後網路 ID
 * 進 _failedToJoinKnownNetworkIDs，同輪 __allowJoinCandidate 才回「Already failed to auto-join」。
 */
static bool wifi_audit_perform_join(id self, SEL selector, id network, id context, id *error)
{
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    bool result = ((bool (*)(id, SEL, id, id, id *))wifi_audit_perform_original)(
        self, selector, network, context, error);
    uint64_t elapsed = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1000000u;
    unsigned call = __atomic_fetch_add(&wifi_audit_perform_calls, 1u, __ATOMIC_RELAXED);
    if (call < 24) {
        id failure = !result && error ? *error : NULL;
        char line[280];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: perform join call=%u ssid-id=%08x context=%u handler=%u result=%u elapsed-ms=%llu "
            "error-present=%u error=%ld\n", call + 1, wifi_audit_ssid(network), context != NULL,
            wifi_audit_object(self, "associateToNetworkHandler") != NULL, result,
            (unsigned long long)elapsed, failure != NULL, (long)wifi_audit_integer(failure, "code"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        wifi_audit_error_chain("perform join", call + 1, failure);
    }
    return result;
}

/* CWFAutoJoinManager -__calloutToAssociateWithParameters:error:（呼叫 wifid 的 associate handler） */
static bool wifi_audit_associate(id self, SEL selector, id parameters, id *error)
{
    uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    bool result = ((bool (*)(id, SEL, id, id *))wifi_audit_associate_original)(self, selector, parameters, error);
    uint64_t elapsed = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started) / 1000000u;
    unsigned call = __atomic_fetch_add(&wifi_audit_associate_calls, 1u, __ATOMIC_RELAXED);
    if (call < 24) {
        id failure = !result && error ? *error : NULL;
        id scan = wifi_audit_object(parameters, "scanResult");
        char line[280];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: associate callout call=%u ssid-id=%08x scan=%u remember=%ld force-bssid=%ld "
            "result=%u elapsed-ms=%llu error-present=%u error=%ld\n", call + 1, wifi_audit_ssid(scan), scan != NULL,
            (long)wifi_audit_integer(parameters, "rememberUponSuccessfulAssociation"),
            (long)wifi_audit_integer(parameters, "forceBSSID"), result, (unsigned long long)elapsed,
            failure != NULL, (long)wifi_audit_integer(failure, "code"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        wifi_audit_error_chain("associate callout", call + 1, failure);
    }
    return result;
}

static void wifi_audit_sort(id self, SEL selector, id scans, id context)
{
    ((void (*)(id, SEL, id, id))wifi_audit_sort_original)(self, selector, scans, context);
    unsigned call = __atomic_fetch_add(&wifi_audit_sort_calls, 1u, __ATOMIC_RELAXED);
    if (call < 12) {
        char line[160];
        int length = snprintf(line, sizeof(line), "fuxi: wifid: sorted candidates call=%u count=%ld\n",
            call + 1, (long)wifi_audit_integer(scans, "count"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        wifi_audit_networks("sorted candidate", call + 1, scans);
    }
}

static bool wifi_audit_transition(id self, SEL selector, id scan, id previous, intptr_t trigger)
{
    bool result = ((bool (*)(id, SEL, id, id, intptr_t))wifi_audit_transition_original)(
        self, selector, scan, previous, trigger);
    unsigned call = __atomic_fetch_add(&wifi_audit_transition_calls, 1u, __ATOMIC_RELAXED);
    if (call < 48) {
        id channel = wifi_audit_object(scan, "channel");
        char line[320];
        int length = snprintf(line, sizeof(line),
            "fuxi: wifid: transition decision call=%u ssid-id=%08x previous=%u previous-id=%08x "
            "trigger=%ld allowed=%u channel=%ld channel-flags=%ld six-ghz=%ld\n", call + 1, wifi_audit_ssid(scan),
            previous != NULL, wifi_audit_ssid(previous), (long)trigger, result,
            (long)wifi_audit_integer(channel, "channel"), (long)wifi_audit_integer(channel, "flags"),
            (long)wifi_audit_integer(channel, "is6GHz"));
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    return result;
}

typedef struct {
    const char *target, *selector, *types;
    IMP replacement, *original;
    unsigned done;
} wifi_audit_hook;

/* 依 types 字串（回傳型別＋各引數；b＝BOOL）核對 ABI，相符才換實作；每個項目只試一次。 */
static void wifi_audit_install_table(const char *label, wifi_audit_hook *hooks, unsigned total)
{
    for (unsigned index = 0; index < total; index++) {
        if (__atomic_load_n(&hooks[index].done, __ATOMIC_ACQUIRE)) continue;
        Class target = objc_getClass(hooks[index].target);
        Method method = target ? class_getInstanceMethod(target, sel_registerName(hooks[index].selector)) : NULL;
        if (method == NULL || __atomic_exchange_n(&hooks[index].done, 1u, __ATOMIC_ACQ_REL)) continue;
        size_t count = strlen(hooks[index].types);
        bool usable = method_getNumberOfArguments(method) == count + 1 &&
            wifi_audit_type(method, 0, hooks[index].types[0]);
        for (unsigned argument = 1; usable && argument < count; argument++)
            usable = wifi_audit_type(method, argument + 1, hooks[index].types[argument]);
        char line[320];
        int length = snprintf(line, sizeof(line), "fuxi: %s hook %s %s %s\n", label, hooks[index].target,
            hooks[index].selector, usable ? "installed" : "refused ABI");
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        if (!usable) continue;
        *hooks[index].original = method_getImplementation(method);
        (void)method_setImplementation(method, hooks[index].replacement);
    }
}

static void wifi_audit_install_candidates(void)
{
    static wifi_audit_hook hooks[] = {
        {"CWFAutoJoinManager", "__matchAndJoinScanResults:allowPreAssociationScan:context:error:", "b@b@^",
            (IMP)wifi_audit_match, &wifi_audit_match_original, 0},
        {"CWFAutoJoinManager", "__preflightMatchKnownNetworksForScanResult:", "b@",
            (IMP)wifi_audit_preflight, &wifi_audit_preflight_original, 0},
        {"CWFScanResult", "scanResultWithMatchingKnownNetworkProfile:", "@@",
            (IMP)wifi_audit_profile, &wifi_audit_profile_original, 0},
        {"CWFXPCConnection", "__matchedScanResultsWithKnownNetworkProfiles:scanResults:", "@@@",
            (IMP)wifi_audit_xpc, &wifi_audit_xpc_original, 0},
        {"CWFScanResult", "filteredScanResultWithProperties:", "@@",
            (IMP)wifi_audit_filtered, &wifi_audit_filtered_original, 0},
        {"CWFAutoJoinManager", "__allowKnownNetwork:trigger:allowForSeamlessSSIDTransition:defer:error:", "b@q@^^",
            (IMP)wifi_audit_known, &wifi_audit_known_original, 0},
        {"CWFAutoJoinManager", "__calloutToAllowKnownNetwork:trigger:allowForSeamlessSSIDTransition:defer:error:",
            "b@q@^^", (IMP)wifi_audit_native_known, &wifi_audit_native_known_original, 0},
        {"CWFAutoJoinManager", "__allowJoinCandidate:trigger:defer:error:", "b@q^^",
            (IMP)wifi_audit_allow, &wifi_audit_allow_original, 0},
        {"CWFAutoJoinManager", "__sortJoinCandidates:context:", "v@@",
            (IMP)wifi_audit_sort, &wifi_audit_sort_original, 0},
        {"CWFAutoJoinManager", "__candidateSupportsSeamlessSSIDTransition:fromNetwork:trigger:", "b@@q",
            (IMP)wifi_audit_transition, &wifi_audit_transition_original, 0},
        {"CWFAutoJoinManager", "__performJoinWithNetwork:context:error:", "b@@^",
            (IMP)wifi_audit_perform_join, &wifi_audit_perform_original, 0},
        {"CWFAutoJoinManager", "__calloutToAssociateWithParameters:error:", "b@^",
            (IMP)wifi_audit_associate, &wifi_audit_associate_original, 0}
    };
    wifi_audit_install_table("wifid: candidate", hooks, sizeof(hooks) / sizeof(hooks[0]));
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
    wifi_audit_install_candidates();
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

/*
 * SpringBoard 狀態欄 Wi-Fi 圖示（ADT /chosen fuxi,wifi-sb 非 0 才裝；唯讀，原實作照呼叫、回傳值不改）。
 * 20H392 反組譯：-[SBSystemStatusWifiDataProvider _updateWifiActive] 的 block 以
 * SBWiFiManager isPowered && isPrimaryInterface && isAssociated 決定 wifiActive，_updateData 再把
 * wifiActive 與 signalStrengthBars 發佈給狀態欄；isPrimaryInterface 是 NWSystemPathMonitor isWiFiPrimary
 * （primaryEvaluator.path usesInterfaceType:wifi）。核心 en0 已是 IFNET_SUBFAMILY_WIFI
 * （ksrc/xnu/fuxi-wifi-ios：原生與合成後端共用 link.wireless=true），所以要實測是哪一項為假。
 * 每個值只在改變時記一行（各最多 16 行），RSSI 回報最多 24 行；不記 SSID、BSSID 或任何網路名稱。
 */
enum { SB_WIFI_CHANGES = 16, SB_WIFI_RSSI_LOGS = 24 };
enum { SB_POWERED, SB_PRIMARY, SB_ASSOCIATED, SB_ENABLED, SB_BARS, SB_RSSI, SB_ACTIVE, SB_VALUES };
static const char *const sb_wifi_names[SB_VALUES] = {
    "isPowered", "isPrimaryInterface", "isAssociated", "wiFiEnabled", "signalStrengthBars", "signalStrengthRSSI",
    "provider-setWifiActive"
};
static IMP sb_wifi_original[SB_VALUES], sb_wifi_rssi_original, sb_wifi_primary_original, sb_wifi_link_original;
static long long sb_wifi_last[SB_VALUES];
static unsigned sb_wifi_changes[SB_VALUES], sb_wifi_rssi_calls, sb_wifi_primary_calls, sb_wifi_link_calls;
static unsigned sb_wifi_seen[SB_VALUES];

static unsigned long long sb_wifi_uptime_ms(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull;
}

static void sb_wifi_value(unsigned which, long long value)
{
    bool first = !__atomic_exchange_n(&sb_wifi_seen[which], 1u, __ATOMIC_ACQ_REL);
    long long previous = __atomic_exchange_n(&sb_wifi_last[which], value, __ATOMIC_ACQ_REL);
    if (!first && previous == value) return;
    unsigned change = __atomic_fetch_add(&sb_wifi_changes[which], 1u, __ATOMIC_RELAXED);
    if (change >= SB_WIFI_CHANGES) return;
    char line[200], before[24] = "none";
    if (!first) (void)snprintf(before, sizeof(before), "%lld", previous);
    int length = snprintf(line, sizeof(line), "fuxi: sb-wifi: %s change=%u uptime-ms=%llu value=%lld previous=%s\n",
        sb_wifi_names[which], change + 1, sb_wifi_uptime_ms(), value, before);
    if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
}

#define SB_WIFI_BOOL_GETTER(function, which) \
    static bool function(id self, SEL selector) \
    { \
        bool result = ((bool (*)(id, SEL))sb_wifi_original[which])(self, selector); \
        sb_wifi_value(which, result); \
        return result; \
    }
#define SB_WIFI_INT_GETTER(function, which) \
    static int function(id self, SEL selector) \
    { \
        int result = ((int (*)(id, SEL))sb_wifi_original[which])(self, selector); \
        sb_wifi_value(which, result); \
        return result; \
    }
SB_WIFI_BOOL_GETTER(sb_wifi_powered, SB_POWERED)
SB_WIFI_BOOL_GETTER(sb_wifi_primary, SB_PRIMARY)
SB_WIFI_BOOL_GETTER(sb_wifi_associated, SB_ASSOCIATED)
SB_WIFI_BOOL_GETTER(sb_wifi_enabled, SB_ENABLED)
SB_WIFI_INT_GETTER(sb_wifi_bars, SB_BARS)
SB_WIFI_INT_GETTER(sb_wifi_rssi_value, SB_RSSI)

static void sb_wifi_set_active(id self, SEL selector, bool active)
{
    sb_wifi_value(SB_ACTIVE, active);
    ((void (*)(id, SEL, bool))sb_wifi_original[SB_ACTIVE])(self, selector, active);
}

static void sb_wifi_rssi(id self, SEL selector, int raw, float scaled)
{
    unsigned call = __atomic_fetch_add(&sb_wifi_rssi_calls, 1u, __ATOMIC_RELAXED);
    if (call < SB_WIFI_RSSI_LOGS) {
        char line[200];
        int length = snprintf(line, sizeof(line), "fuxi: sb-wifi: rssi update call=%u uptime-ms=%llu raw=%d scaled=%.3f\n",
            call + 1, sb_wifi_uptime_ms(), raw, (double)scaled);
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
    }
    ((void (*)(id, SEL, int, float))sb_wifi_rssi_original)(self, selector, raw, scaled);
}

static void sb_wifi_event(const char *name, unsigned *calls)
{
    unsigned call = __atomic_fetch_add(calls, 1u, __ATOMIC_RELAXED);
    if (call >= SB_WIFI_CHANGES) return;
    char line[160];
    int length = snprintf(line, sizeof(line), "fuxi: sb-wifi: %s call=%u uptime-ms=%llu\n", name, call + 1,
        sb_wifi_uptime_ms());
    if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
}

static void sb_wifi_primary_changed(id self, SEL selector, id notification)
{
    sb_wifi_event("primaryInterfaceChanged", &sb_wifi_primary_calls);
    ((void (*)(id, SEL, id))sb_wifi_primary_original)(self, selector, notification);
}

static void sb_wifi_link_changed(id self, SEL selector)
{
    sb_wifi_event("linkDidChange", &sb_wifi_link_calls);
    ((void (*)(id, SEL))sb_wifi_link_original)(self, selector);
}

static void sb_wifi_image(const struct mach_header *header, intptr_t slide)
{
    (void)header;
    (void)slide;
    static wifi_audit_hook hooks[] = {
        {"SBWiFiManager", "isPowered", "b", (IMP)sb_wifi_powered, &sb_wifi_original[SB_POWERED], 0},
        {"SBWiFiManager", "isPrimaryInterface", "b", (IMP)sb_wifi_primary, &sb_wifi_original[SB_PRIMARY], 0},
        {"SBWiFiManager", "isAssociated", "b", (IMP)sb_wifi_associated, &sb_wifi_original[SB_ASSOCIATED], 0},
        {"SBWiFiManager", "wiFiEnabled", "b", (IMP)sb_wifi_enabled, &sb_wifi_original[SB_ENABLED], 0},
        {"SBWiFiManager", "signalStrengthBars", "i", (IMP)sb_wifi_bars, &sb_wifi_original[SB_BARS], 0},
        {"SBWiFiManager", "signalStrengthRSSI", "i", (IMP)sb_wifi_rssi_value, &sb_wifi_original[SB_RSSI], 0},
        {"SBWiFiManager", "_updateSignalStrengthFromRawRSSI:andScaledRSSI:", "vif", (IMP)sb_wifi_rssi,
            &sb_wifi_rssi_original, 0},
        {"SBWiFiManager", "_primaryInterfaceChanged:", "v@", (IMP)sb_wifi_primary_changed,
            &sb_wifi_primary_original, 0},
        {"SBWiFiManager", "_linkDidChange", "v", (IMP)sb_wifi_link_changed, &sb_wifi_link_original, 0},
        {"SBSystemStatusWifiDataProvider", "setWifiActive:", "vb", (IMP)sb_wifi_set_active,
            &sb_wifi_original[SB_ACTIVE], 0}
    };
    wifi_audit_install_table("sb-wifi:", hooks, sizeof(hooks) / sizeof(hooks[0]));
}

typedef mach_port_t wifi_audit_io_object_t;
extern wifi_audit_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(wifi_audit_io_object_t entry, CFStringRef key,
    CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(wifi_audit_io_object_t object);

static unsigned wifi_audit_adt_u32(CFStringRef key)
{
    wifi_audit_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
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

__attribute__((constructor)) static void fuxi_wifi_audit_init(void)
{
    const char *name = getprogname();
    if (name == NULL) return;
    if (strcmp(name, "SpringBoard") == 0) {
        if (wifi_audit_adt_u32(CFSTR("fuxi,wifi-sb")) == 0) return;
        wifi_audit_say("fuxi: sb-wifi: status audit enabled (fuxi,wifi-sb)\n");
        _dyld_register_func_for_add_image(sb_wifi_image);
        return;
    }
    if (strcmp(name, "wifid") != 0) return;
    _dyld_register_func_for_add_image(wifi_audit_image);
}
