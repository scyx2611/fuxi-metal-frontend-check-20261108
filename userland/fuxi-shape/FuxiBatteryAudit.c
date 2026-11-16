#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef mach_port_t fa_io_object_t;
extern fa_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(fa_io_object_t entry, CFStringRef key,
    CFAllocatorRef allocator, uint32_t options);
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern fa_io_object_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IORegistryEntryCreateCFProperties(fa_io_object_t entry,
    CFMutableDictionaryRef *properties, CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(fa_io_object_t object);

enum { FA_SAMPLE_LIMIT = 120, FA_CHANGE_LIMIT = 32 };
enum fa_kind { FA_NUMBER, FA_BOOLEAN, FA_DICTIONARY };
enum fa_state { FA_MISSING, FA_VALID, FA_TYPE_ERROR, FA_NON_INTEGER, FA_NUMBER_ERROR, FA_PARENT_ERROR };
enum fa_result { FA_MATCH_ERROR, FA_SERVICE_MISSING, FA_READ_ERROR, FA_DICTIONARY_ERROR, FA_OK };

static const struct {
    const char *label;
    CFStringRef parent, key;
    enum fa_kind kind;
    bool age;
} fa_fields[] = {
    {"CurrentCapacity", NULL, CFSTR("CurrentCapacity"), FA_NUMBER, false},
    {"MaxCapacity", NULL, CFSTR("MaxCapacity"), FA_NUMBER, false},
    {"AppleRawCurrentCapacity", NULL, CFSTR("AppleRawCurrentCapacity"), FA_NUMBER, false},
    {"AppleRawMaxCapacity", NULL, CFSTR("AppleRawMaxCapacity"), FA_NUMBER, false},
    {"ExternalConnected", NULL, CFSTR("ExternalConnected"), FA_BOOLEAN, false},
    {"AppleRawExternalConnected", NULL, CFSTR("AppleRawExternalConnected"), FA_BOOLEAN, false},
    {"IsCharging", NULL, CFSTR("IsCharging"), FA_BOOLEAN, false},
    {"Voltage", NULL, CFSTR("Voltage"), FA_NUMBER, false},
    {"Amperage", NULL, CFSTR("Amperage"), FA_NUMBER, false},
    {"AdapterDetails", NULL, CFSTR("AdapterDetails"), FA_DICTIONARY, false},
    {"AdapterDetails.FamilyCode", CFSTR("AdapterDetails"), CFSTR("FamilyCode"), FA_NUMBER, false},
    {"AdapterDetails.AdapterVoltage", CFSTR("AdapterDetails"), CFSTR("AdapterVoltage"), FA_NUMBER, false},
    {"AdapterDetails.Current", CFSTR("AdapterDetails"), CFSTR("Current"), FA_NUMBER, false},
    {"AdapterDetails.Watts", CFSTR("AdapterDetails"), CFSTR("Watts"), FA_NUMBER, false},
    {"LegacyBatteryInfo", NULL, CFSTR("LegacyBatteryInfo"), FA_DICTIONARY, false},
    {"LegacyBatteryInfo.Current", CFSTR("LegacyBatteryInfo"), CFSTR("Current"), FA_NUMBER, false},
    {"LegacyBatteryInfo.Capacity", CFSTR("LegacyBatteryInfo"), CFSTR("Capacity"), FA_NUMBER, false},
    {"LegacyBatteryInfo.Voltage", CFSTR("LegacyBatteryInfo"), CFSTR("Voltage"), FA_NUMBER, false},
    {"LegacyBatteryInfo.Amperage", CFSTR("LegacyBatteryInfo"), CFSTR("Amperage"), FA_NUMBER, false},
    {"LegacyBatteryInfo.CycleCount", CFSTR("LegacyBatteryInfo"), CFSTR("Cycle Count"), FA_NUMBER, false},
    {"LegacyBatteryInfo.Flags", CFSTR("LegacyBatteryInfo"), CFSTR("Flags"), FA_NUMBER, false},
    {"FuxiBatterySOCValid", NULL, CFSTR("FuxiBatterySOCValid"), FA_BOOLEAN, false},
    {"FuxiBatteryExternalKnown", NULL, CFSTR("FuxiBatteryExternalKnown"), FA_BOOLEAN, false},
    {"FuxiBatteryStatusKnown", NULL, CFSTR("FuxiBatteryStatusKnown"), FA_BOOLEAN, false},
    {"FuxiBatteryAdapterValid", NULL, CFSTR("FuxiBatteryAdapterValid"), FA_BOOLEAN, false},
    {"FuxiBatteryFreshFields", NULL, CFSTR("FuxiBatteryFreshFields"), FA_NUMBER, false},
    {"FuxiBatteryMaxAgeMs", NULL, CFSTR("FuxiBatteryMaxAgeMs"), FA_NUMBER, true},
    {"FuxiBatterySOCAgeMs", NULL, CFSTR("FuxiBatterySOCAgeMs"), FA_NUMBER, true},
    {"FuxiBatteryExternalAgeMs", NULL, CFSTR("FuxiBatteryExternalAgeMs"), FA_NUMBER, true},
    {"FuxiBatteryStatusAgeMs", NULL, CFSTR("FuxiBatteryStatusAgeMs"), FA_NUMBER, true},
    {"FuxiBatteryGeneration", NULL, CFSTR("FuxiBatteryGeneration"), FA_NUMBER, false},
    {"FuxiBatteryChannelUp", NULL, CFSTR("FuxiBatteryChannelUp"), FA_BOOLEAN, false}
};

struct fa_value {
    enum fa_state state;
    CFTypeID type;
    int64_t number;
};

struct fa_snapshot {
    enum fa_result result;
    bool read_attempted;
    kern_return_t read_result;
    struct fa_value values[sizeof(fa_fields) / sizeof(fa_fields[0])];
};

static mach_timebase_info_data_t fa_timebase;

static void fa_say(const char *format, ...)
{
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length > 0 && (size_t)length < sizeof(line)) (void)write(2, line, (size_t)length);
}

static bool fa_enabled(void)
{
    fa_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
    if (chosen == MACH_PORT_NULL) return false;
    CFTypeRef value = IORegistryEntryCreateCFProperty(chosen, CFSTR("fuxi,batteryaudit"), kCFAllocatorDefault, 0);
    (void)IOObjectRelease(chosen);
    if (value == NULL) return false;
    uint32_t flag = 0;
    bool enabled = false;
    if (CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)value) == (CFIndex)sizeof(flag)) {
        const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)value);
        if (bytes != NULL) {
            memcpy(&flag, bytes, sizeof(flag));
            enabled = flag == 1;
        }
    }
    CFRelease(value);
    return enabled;
}

static bool fa_now_ms(uint64_t *now_ms)
{
    const uint64_t ticks = mach_absolute_time();
    const uint64_t whole = ticks / fa_timebase.denom;
    const uint64_t fraction = ticks % fa_timebase.denom * (uint64_t)fa_timebase.numer / fa_timebase.denom;
    if (whole > (UINT64_MAX - fraction) / fa_timebase.numer) return false;
    *now_ms = (whole * fa_timebase.numer + fraction) / 1000000u;
    return true;
}

static struct fa_value fa_read_value(CFDictionaryRef properties, size_t index)
{
    struct fa_value observed = {0};
    CFDictionaryRef dictionary = properties;
    if (fa_fields[index].parent != NULL) {
        CFTypeRef parent = CFDictionaryGetValue(properties, fa_fields[index].parent);
        if (parent == NULL) return observed;
        if (CFGetTypeID(parent) != CFDictionaryGetTypeID()) {
            observed.state = FA_PARENT_ERROR;
            observed.type = CFGetTypeID(parent);
            return observed;
        }
        dictionary = (CFDictionaryRef)parent;
    }
    CFTypeRef value = CFDictionaryGetValue(dictionary, fa_fields[index].key);
    if (value == NULL) return observed;
    observed.type = CFGetTypeID(value);
    const CFTypeID expected = fa_fields[index].kind == FA_BOOLEAN ? CFBooleanGetTypeID() :
        fa_fields[index].kind == FA_DICTIONARY ? CFDictionaryGetTypeID() : CFNumberGetTypeID();
    if (observed.type != expected) {
        observed.state = FA_TYPE_ERROR;
    } else if (fa_fields[index].kind == FA_BOOLEAN) {
        observed.number = CFBooleanGetValue((CFBooleanRef)value) ? 1 : 0;
        observed.state = FA_VALID;
    } else if (fa_fields[index].kind == FA_DICTIONARY) {
        observed.number = CFDictionaryGetCount((CFDictionaryRef)value);
        observed.state = FA_VALID;
    } else if (CFNumberIsFloatType((CFNumberRef)value)) {
        observed.state = FA_NON_INTEGER;
    } else {
        observed.state = CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &observed.number) ?
            FA_VALID : FA_NUMBER_ERROR;
    }
    return observed;
}

static struct fa_snapshot fa_collect(void)
{
    struct fa_snapshot snapshot = {0};
    CFMutableDictionaryRef matching = IOServiceMatching("IOPMPowerSource");
    if (matching == NULL) return snapshot;
    fa_io_object_t service = IOServiceGetMatchingService(MACH_PORT_NULL, matching);
    if (service == MACH_PORT_NULL) {
        snapshot.result = FA_SERVICE_MISSING;
        return snapshot;
    }
    CFMutableDictionaryRef properties = NULL;
    snapshot.read_attempted = true;
    snapshot.read_result = IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0);
    (void)IOObjectRelease(service);
    if (snapshot.read_result != KERN_SUCCESS) {
        snapshot.result = FA_READ_ERROR;
    } else if (properties == NULL || CFGetTypeID(properties) != CFDictionaryGetTypeID()) {
        snapshot.result = FA_DICTIONARY_ERROR;
    } else {
        snapshot.result = FA_OK;
        for (size_t index = 0; index < sizeof(fa_fields) / sizeof(fa_fields[0]); index++)
            snapshot.values[index] = fa_read_value(properties, index);
    }
    if (properties != NULL) CFRelease(properties);
    return snapshot;
}

static bool fa_changed(const struct fa_snapshot *previous, const struct fa_snapshot *current)
{
    if (previous->result != current->result || previous->read_attempted != current->read_attempted ||
        (current->read_attempted && previous->read_result != current->read_result)) return true;
    for (size_t index = 0; index < sizeof(fa_fields) / sizeof(fa_fields[0]); index++) {
        const struct fa_value *before = &previous->values[index], *after = &current->values[index];
        if (before->state != after->state || before->type != after->type ||
            (!fa_fields[index].age && after->state == FA_VALID && before->number != after->number)) return true;
    }
    return false;
}

static bool fa_append(char *line, size_t size, size_t *used, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line + *used, size - *used, format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= size - *used) return false;
    *used += (size_t)length;
    return true;
}

static bool fa_log(const struct fa_snapshot *snapshot, unsigned sample, uint64_t begin_ms, uint64_t now_ms)
{
    static const char *const results[] = {"match-error", "service-missing", "read-error", "dictionary-error", "ok"};
    static const char *const unknown[] = {"unknown", "", "unknown(type)", "unknown(non-integer)",
        "unknown(number-conversion)", "unknown(parent-type)"};
    char line[4096];
    size_t used = 0;
    if (!fa_append(line, sizeof(line), &used,
        "fuxi: SpringBoard: battery-audit sample=%u begin_ms=%" PRIu64 " now_ms=%" PRIu64
        " clock=mach_absolute_time_ms result=%s coherence=unestablished publication_sequence=unknown rc=",
        sample, begin_ms, now_ms, results[snapshot->result])) return false;
    if (snapshot->read_attempted) {
        if (!fa_append(line, sizeof(line), &used, "%d", snapshot->read_result)) return false;
    } else if (!fa_append(line, sizeof(line), &used, "unknown")) return false;
    for (size_t index = 0; index < sizeof(fa_fields) / sizeof(fa_fields[0]); index++) {
        const struct fa_value *value = &snapshot->values[index];
        if (!fa_append(line, sizeof(line), &used, " %s=", fa_fields[index].label)) return false;
        if (value->state != FA_VALID) {
            if (!fa_append(line, sizeof(line), &used, "%s", unknown[value->state])) return false;
        } else if (fa_fields[index].kind == FA_BOOLEAN) {
            if (!fa_append(line, sizeof(line), &used, "%s", value->number ? "true" : "false")) return false;
        } else if (fa_fields[index].kind == FA_DICTIONARY) {
            if (!fa_append(line, sizeof(line), &used, "dictionary(%" PRId64 ")", value->number)) return false;
        } else if (!fa_append(line, sizeof(line), &used, "%" PRId64, value->number)) return false;
    }
    if (!fa_append(line, sizeof(line), &used, "\n")) return false;
    (void)write(2, line, used);
    return true;
}

static void *fa_worker(void *context)
{
    (void)context;
    struct fa_snapshot previous = {0};
    unsigned samples = 0, changes = 0, lines = 0;
    const char *reason = "sample-limit";
    fa_say("fuxi: SpringBoard: battery-audit enabled samples<=120 interval_ms=2000 change_lines<=32"
        " clock=mach_absolute_time_ms publication_sequence=unknown coherence=unestablished\n");
    while (samples < FA_SAMPLE_LIMIT) {
        uint64_t begin_ms, now_ms;
        if (!fa_now_ms(&begin_ms)) { reason = "clock-error"; break; }
        struct fa_snapshot current = fa_collect();
        samples++;
        if (!fa_now_ms(&now_ms) || now_ms < begin_ms) { reason = "clock-error"; break; }
        if (samples == 1 || fa_changed(&previous, &current)) {
            changes++;
            if (lines < FA_CHANGE_LIMIT) {
                if (!fa_log(&current, samples, begin_ms, now_ms)) { reason = "log-size-error"; break; }
                lines++;
            }
        }
        previous = current;
        if (samples == FA_SAMPLE_LIMIT) break;
        const struct timespec delay = {2, 0};
        if (nanosleep(&delay, NULL) != 0) { reason = "sleep-error"; break; }
    }
    fa_say("fuxi: SpringBoard: battery-audit done samples=%u changes=%u change_lines=%u reason=%s"
        " coherence=unestablished\n", samples, changes, lines, reason);
    return NULL;
}

__attribute__((constructor)) static void fuxi_battery_audit_init(void)
{
    const char *name = getprogname();
    if (name == NULL || strcmp(name, "SpringBoard") != 0 || !fa_enabled()) return;
    if (mach_timebase_info(&fa_timebase) != KERN_SUCCESS || !fa_timebase.numer || !fa_timebase.denom) {
        fa_say("fuxi: SpringBoard: battery-audit rejected clock-unavailable\n");
        return;
    }
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result != 0) {
        fa_say("fuxi: SpringBoard: battery-audit rejected pthread-attr rc=%d\n", result);
        return;
    }
    result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (result == 0) result = pthread_create(&thread, &attributes, fa_worker, NULL);
    (void)pthread_attr_destroy(&attributes);
    if (result != 0) fa_say("fuxi: SpringBoard: battery-audit rejected pthread-create rc=%d\n", result);
}
