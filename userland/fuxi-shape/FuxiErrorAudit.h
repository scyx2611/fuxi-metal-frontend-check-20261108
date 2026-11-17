#ifndef FUXI_ERROR_AUDIT_H
#define FUXI_ERROR_AUDIT_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <string.h>

enum { FUXI_ERROR_AUDIT_DEPTH = 3 };

typedef struct {
    CFIndex code;
    uint32_t domain;
    const char *reason;
} FuxiErrorAuditEntry;

static inline uint32_t fuxi_error_domain(CFStringRef domain)
{
    char text[128];
    if (domain == NULL || !CFStringGetCString(domain, text, sizeof(text), kCFStringEncodingUTF8)) return 0;
    uint32_t identifier = UINT32_C(2166136261);
    for (size_t index = 0; text[index]; index++)
        identifier = (identifier ^ (uint8_t)text[index]) * UINT32_C(16777619);
    return identifier;
}

static inline const char *fuxi_error_reason(CFDictionaryRef info)
{
    const CFStringRef keys[] = {
        kCFErrorLocalizedDescriptionKey, kCFErrorLocalizedFailureReasonKey, kCFErrorDescriptionKey
    };
    for (unsigned index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
        CFTypeRef value = CFDictionaryGetValue(info, keys[index]);
        if (value == NULL || CFGetTypeID(value) != CFStringGetTypeID()) continue;
        if (CFEqual(value, CFSTR("Join candidate not suitable for auto-join"))) return "native-suitability";
        if (CFEqual(value, CFSTR("Below minimum RSSI threshold"))) return "minimum-rssi";
    }
    return "unclassified";
}

static inline unsigned fuxi_error_audit(CFTypeRef error, FuxiErrorAuditEntry entries[FUXI_ERROR_AUDIT_DEPTH])
{
    CFTypeRef current = error ? CFRetain(error) : NULL;
    unsigned count = 0;
    while (current && count < FUXI_ERROR_AUDIT_DEPTH && CFGetTypeID(current) == CFErrorGetTypeID()) {
        CFErrorRef failure = (CFErrorRef)current;
        CFDictionaryRef info = CFErrorCopyUserInfo(failure);
        entries[count].code = CFErrorGetCode(failure);
        entries[count].domain = fuxi_error_domain(CFErrorGetDomain(failure));
        entries[count].reason = info ? fuxi_error_reason(info) : "unclassified";
        count++;
        CFTypeRef next = info ? CFDictionaryGetValue(info, kCFErrorUnderlyingErrorKey) : NULL;
        if (next) CFRetain(next);
        if (info) CFRelease(info);
        CFRelease(current);
        current = next;
    }
    if (current) CFRelease(current);
    return count;
}

#endif
