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
static IMP wifi_audit_match_original, wifi_audit_preflight_original, wifi_audit_profile_original;
static IMP wifi_audit_xpc_original, wifi_audit_filtered_original;
static unsigned wifi_audit_match_calls, wifi_audit_preflight_calls, wifi_audit_profile_calls;
static unsigned wifi_audit_xpc_calls, wifi_audit_filtered_calls;
static IMP wifi_audit_known_original, wifi_audit_native_known_original, wifi_audit_allow_original;
static IMP wifi_audit_sort_original, wifi_audit_transition_original;
static unsigned wifi_audit_known_calls, wifi_audit_native_known_calls, wifi_audit_allow_calls;
static unsigned wifi_audit_sort_calls, wifi_audit_transition_calls;

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

static void wifi_audit_install_candidates(void)
{
    static struct {
        const char *target, *selector, *types;
        IMP replacement, *original;
        unsigned done;
    } hooks[] = {
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
            (IMP)wifi_audit_transition, &wifi_audit_transition_original, 0}
    };
    for (unsigned index = 0; index < sizeof(hooks) / sizeof(hooks[0]); index++) {
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
        int length = snprintf(line, sizeof(line), "fuxi: wifid: candidate hook %s %s\n",
            hooks[index].selector, usable ? "installed" : "refused ABI");
        if (length > 0 && length < (int)sizeof(line)) (void)write(2, line, (size_t)length);
        if (!usable) continue;
        *hooks[index].original = method_getImplementation(method);
        (void)method_setImplementation(method, hooks[index].replacement);
    }
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

__attribute__((constructor)) static void fuxi_wifi_audit_init(void)
{
    const char *name = getprogname();
    if (name == NULL || strcmp(name, "wifid") != 0) return;
    _dyld_register_func_for_add_image(wifi_audit_image);
}
