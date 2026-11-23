/*
 * Override VirtualAudio D22/D221 microphone DSP strips in mediaserverd to bypass
 * iPhone X AUAGCV2 noise pumping and NBandEQ coloration behind ADT "fuxi,audio-mic-dsp".
 *
 * References (AGENTS.md section 0):
 *   - Reuses ADT /chosen u32 property lookup and dyld __DATA,__interpose patterns from
 *     FuxiBluetoothLog.c (links only libSystem, CoreFoundation, IOKit, libobjc).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FuxiAudioMicDspLogic.h"

typedef mach_port_t fx_micdsp_io_object_t;
extern fx_micdsp_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(fx_micdsp_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(fx_micdsp_io_object_t object);

extern int micdsp_real_open(const char *path, int oflag, ...) __asm__("_open");
extern int micdsp_real_openat(int dirfd, const char *path, int oflag, ...) __asm__("_openat");
extern int micdsp_real_stat(const char *path, struct stat *st) __asm__("_stat");
extern int micdsp_real_access(const char *path, int amode) __asm__("_access");
extern CFURLRef micdsp_real_cfurl_path(CFAllocatorRef allocator, CFStringRef filePath,
    CFURLPathStyle pathStyle, Boolean isDirectory) __asm__("_CFURLCreateWithFileSystemPath");
extern CFURLRef micdsp_real_cfurl_fsrep(CFAllocatorRef allocator, const UInt8 *buffer,
    CFIndex bufLen, Boolean isDirectory) __asm__("_CFURLCreateFromFileSystemRepresentation");
extern CFReadStreamRef micdsp_real_readstream_file(CFAllocatorRef alloc, CFURLRef fileURL)
    __asm__("_CFReadStreamCreateWithFile");
extern CFPropertyListRef micdsp_real_plist_stream(CFAllocatorRef allocator, CFReadStreamRef stream,
    CFIndex streamLength, CFOptionFlags options, CFPropertyListFormat *format, CFErrorRef *error)
    __asm__("_CFPropertyListCreateWithStream");
extern CFPropertyListRef micdsp_real_plist_data(CFAllocatorRef allocator, CFDataRef data,
    CFOptionFlags options, CFPropertyListFormat *format, CFErrorRef *error)
    __asm__("_CFPropertyListCreateWithData");

#define MICDSP_LOG_LIMIT 16u

static unsigned micdsp_mode;
static unsigned micdsp_log_count;
static unsigned micdsp_patch_log_count;
static pthread_mutex_t micdsp_map_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fuxi_mic_dsp_map micdsp_url_map;
static struct fuxi_mic_dsp_map micdsp_stream_map;
static __thread char micdsp_tls_stem[FUXI_MIC_DSP_STEM_MAX];

static unsigned
micdsp_adt_u32(const char *key)
{
	unsigned value = 0;
	fx_micdsp_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	if (chosen == MACH_PORT_NULL) {
		return 0;
	}
	CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
	CFTypeRef data = name != NULL ? IORegistryEntryCreateCFProperty(chosen, name, kCFAllocatorDefault, 0) : NULL;
	if (data != NULL && CFGetTypeID(data) == CFDataGetTypeID() && CFDataGetLength(data) == (CFIndex)sizeof(uint32_t)) {
		memcpy(&value, CFDataGetBytePtr(data), sizeof(value));
	}
	if (data != NULL) {
		CFRelease(data);
	}
	if (name != NULL) {
		CFRelease(name);
	}
	IOObjectRelease(chosen);
	return value;
}

static unsigned
micdsp_active(void)
{
	return __atomic_load_n(&micdsp_mode, __ATOMIC_RELAXED);
}

static void
micdsp_note(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
micdsp_note(const char *format, ...)
{
	char line[320];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	if (n > 0) {
		(void)write(2, line, (size_t)n);
	}
}

static void
micdsp_log_rewrite(const char *api, const char *orig, const char *rewritten)
{
	unsigned seen = __atomic_fetch_add(&micdsp_log_count, 1u, __ATOMIC_RELAXED);
	if (seen < MICDSP_LOG_LIMIT) {
		micdsp_note("MDLOG-DSP: %s %s -> %s\n", api, orig, rewritten);
	}
}

static void
micdsp_record_url(CFURLRef url, const char *stem)
{
	if (url == NULL || stem == NULL || stem[0] == '\0') {
		return;
	}
	pthread_mutex_lock(&micdsp_map_lock);
	(void)fuxi_mic_dsp_map_put(&micdsp_url_map, url, stem);
	pthread_mutex_unlock(&micdsp_map_lock);
}

static CFDictionaryRef
micdsp_make_bypass_effect(uint32_t subtype)
{
	int32_t mfg_val = (int32_t)FUXI_MIC_DSP_FOURCC_APPL;
	int32_t type_val = (int32_t)FUXI_MIC_DSP_FOURCC_AUFX;
	int32_t sub_val = (int32_t)subtype;
	int32_t one_val = 1;
	CFNumberRef mfg = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &mfg_val);
	CFNumberRef type = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &type_val);
	CFNumberRef sub = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &sub_val);
	CFNumberRef bypass = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &one_val);
	CFMutableDictionaryRef unit = CFDictionaryCreateMutable(kCFAllocatorDefault, 3,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef effect = CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	bool ok = (mfg != NULL && type != NULL && sub != NULL && bypass != NULL && unit != NULL && effect != NULL);
	if (ok) {
		CFDictionarySetValue(unit, CFSTR("manufacturer"), mfg);
		CFDictionarySetValue(unit, CFSTR("type"), type);
		CFDictionarySetValue(unit, CFSTR("subtype"), sub);
		CFDictionarySetValue(effect, CFSTR("unit"), unit);
		CFDictionarySetValue(effect, CFSTR("bypass"), bypass);
	}
	if (mfg != NULL) CFRelease(mfg);
	if (type != NULL) CFRelease(type);
	if (sub != NULL) CFRelease(sub);
	if (bypass != NULL) CFRelease(bypass);
	if (unit != NULL) CFRelease(unit);
	if (!ok && effect != NULL) {
		CFRelease(effect);
		return NULL;
	}
	return effect;
}

static size_t
micdsp_append_bypass_effects(CFMutableDictionaryRef patched, const uint32_t *subtypes, size_t count)
{
	if (patched == NULL || subtypes == NULL || count == 0u) {
		return 0u;
	}
	CFTypeRef strips_ref = CFDictionaryGetValue(patched, CFSTR("strips"));
	if (strips_ref == NULL || CFGetTypeID(strips_ref) != CFArrayGetTypeID() ||
	    CFArrayGetCount((CFArrayRef)strips_ref) < 1) {
		return 0u;
	}
	CFTypeRef strip0_ref = CFArrayGetValueAtIndex((CFArrayRef)strips_ref, 0);
	if (strip0_ref == NULL || CFGetTypeID(strip0_ref) != CFDictionaryGetTypeID()) {
		return 0u;
	}
	CFTypeRef effects_ref = CFDictionaryGetValue((CFDictionaryRef)strip0_ref, CFSTR("effects"));
	if (effects_ref == NULL || CFGetTypeID(effects_ref) != CFArrayGetTypeID()) {
		return 0u;
	}
	CFMutableArrayRef strips = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, (CFArrayRef)strips_ref);
	CFMutableDictionaryRef strip0 = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, (CFDictionaryRef)strip0_ref);
	CFMutableArrayRef effects = CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, (CFArrayRef)effects_ref);
	size_t added = 0u;
	if (strips != NULL && strip0 != NULL && effects != NULL) {
		for (size_t i = 0; i < count; i++) {
			CFDictionaryRef eff = micdsp_make_bypass_effect(subtypes[i]);
			if (eff != NULL) {
				CFArrayAppendValue(effects, eff);
				CFRelease(eff);
				added++;
			}
		}
		if (added > 0u) {
			CFDictionarySetValue(strip0, CFSTR("effects"), effects);
			CFArraySetValueAtIndex(strips, 0, strip0);
			CFDictionarySetValue(patched, CFSTR("strips"), strips);
		}
	}
	if (effects != NULL) CFRelease(effects);
	if (strip0 != NULL) CFRelease(strip0);
	if (strips != NULL) CFRelease(strips);
	return added;
}

static CFPropertyListRef
micdsp_patch_plist_name(CFPropertyListRef plist, const char *stem, bool *patched_out)
{
	if (patched_out != NULL) {
		*patched_out = false;
	}
	if (plist == NULL || stem == NULL || stem[0] == '\0' || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		return plist;
	}
	CFDictionaryRef dict = (CFDictionaryRef)plist;
	CFTypeRef cur_name = CFDictionaryGetValue(dict, CFSTR("name"));
	if (cur_name == NULL || CFGetTypeID(cur_name) != CFStringGetTypeID() ||
	    !CFEqual(cur_name, CFSTR("d22_bottom_mic_measurement"))) {
		return plist;
	}
	CFStringRef next_name = CFStringCreateWithCString(kCFAllocatorDefault, stem, kCFStringEncodingUTF8);
	CFMutableDictionaryRef patched = next_name != NULL ? CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, dict) : NULL;
	if (next_name == NULL || patched == NULL) {
		if (next_name != NULL) {
			CFRelease(next_name);
		}
		if (patched != NULL) {
			CFRelease(patched);
		}
		return plist;
	}
	CFDictionarySetValue(patched, CFSTR("name"), next_name);
	CFTypeRef cur_disp = CFDictionaryGetValue(dict, CFSTR("displayname"));
	if (cur_disp != NULL && CFGetTypeID(cur_disp) == CFStringGetTypeID() &&
	    CFEqual(cur_disp, CFSTR("d22_bottom_mic_measurement"))) {
		CFDictionarySetValue(patched, CFSTR("displayname"), next_name);
	}
	uint32_t bypass_subs[4];
	size_t bypass_cnt = fuxi_mic_dsp_bypass_subtypes(stem, bypass_subs, sizeof(bypass_subs) / sizeof(bypass_subs[0]));
	size_t bypass_added = micdsp_append_bypass_effects(patched, bypass_subs, bypass_cnt);
	CFRelease(next_name);
	CFRelease(plist);
	if (patched_out != NULL) {
		*patched_out = true;
	}
	unsigned seen = __atomic_fetch_add(&micdsp_patch_log_count, 1u, __ATOMIC_RELAXED);
	if (seen < MICDSP_LOG_LIMIT) {
		micdsp_note("MDLOG-DSP: patched strip name -> %s (+%zu bypass)\n", stem, bypass_added);
	}
	return patched;
}

static int
micdsp_open(const char *path, int oflag, ...)
{
	unsigned mode = micdsp_active();
	char rewritten[256];
	char stem[FUXI_MIC_DSP_STEM_MAX];
	const char *target = path;
	bool matched = false;

	if (__builtin_expect(mode != 0u, 0) &&
	    fuxi_mic_dsp_rewrite_path_ex(mode, path, rewritten, sizeof(rewritten), stem, sizeof(stem))) {
		micdsp_log_rewrite("open", path, rewritten);
		target = rewritten;
		matched = true;
	}
	int fd;
	if ((oflag & O_CREAT) != 0) {
		va_list ap;
		va_start(ap, oflag);
		int fmode = va_arg(ap, int);
		va_end(ap);
		fd = micdsp_real_open(target, oflag, fmode);
	} else {
		fd = micdsp_real_open(target, oflag);
	}
	if (matched && fd >= 0) {
		snprintf(micdsp_tls_stem, sizeof(micdsp_tls_stem), "%s", stem);
	}
	return fd;
}

static int
micdsp_openat(int dirfd, const char *path, int oflag, ...)
{
	unsigned mode = micdsp_active();
	char rewritten[256];
	char stem[FUXI_MIC_DSP_STEM_MAX];
	const char *target = path;
	bool matched = false;

	if (__builtin_expect(mode != 0u, 0) &&
	    fuxi_mic_dsp_rewrite_path_ex(mode, path, rewritten, sizeof(rewritten), stem, sizeof(stem))) {
		micdsp_log_rewrite("openat", path, rewritten);
		target = rewritten;
		matched = true;
	}
	int fd;
	if ((oflag & O_CREAT) != 0) {
		va_list ap;
		va_start(ap, oflag);
		int fmode = va_arg(ap, int);
		va_end(ap);
		fd = micdsp_real_openat(dirfd, target, oflag, fmode);
	} else {
		fd = micdsp_real_openat(dirfd, target, oflag);
	}
	if (matched && fd >= 0) {
		snprintf(micdsp_tls_stem, sizeof(micdsp_tls_stem), "%s", stem);
	}
	return fd;
}

static int
micdsp_stat(const char *path, struct stat *st)
{
	unsigned mode = micdsp_active();
	char rewritten[256];

	if (__builtin_expect(mode != 0u, 0) && fuxi_mic_dsp_rewrite_path(mode, path, rewritten, sizeof(rewritten))) {
		micdsp_log_rewrite("stat", path, rewritten);
		return micdsp_real_stat(rewritten, st);
	}
	return micdsp_real_stat(path, st);
}

static int
micdsp_access(const char *path, int amode)
{
	unsigned mode = micdsp_active();
	char rewritten[256];

	if (__builtin_expect(mode != 0u, 0) && fuxi_mic_dsp_rewrite_path(mode, path, rewritten, sizeof(rewritten))) {
		micdsp_log_rewrite("access", path, rewritten);
		return micdsp_real_access(rewritten, amode);
	}
	return micdsp_real_access(path, amode);
}

static CFURLRef
micdsp_cfurl_path(CFAllocatorRef allocator, CFStringRef filePath, CFURLPathStyle pathStyle, Boolean isDirectory)
{
	unsigned mode = micdsp_active();

	if (__builtin_expect(mode != 0u, 0) && filePath != NULL) {
		char orig[256], rewritten[256], stem[FUXI_MIC_DSP_STEM_MAX];
		if (CFStringGetCString(filePath, orig, (CFIndex)sizeof(orig), kCFStringEncodingUTF8) &&
		    fuxi_mic_dsp_rewrite_path_ex(mode, orig, rewritten, sizeof(rewritten), stem, sizeof(stem))) {
			CFStringRef next = CFStringCreateWithCString(kCFAllocatorDefault, rewritten, kCFStringEncodingUTF8);
			if (next != NULL) {
				micdsp_log_rewrite("CFURLPath", orig, rewritten);
				CFURLRef url = micdsp_real_cfurl_path(allocator, next, pathStyle, isDirectory);
				CFRelease(next);
				micdsp_record_url(url, stem);
				return url;
			}
		}
	}
	return micdsp_real_cfurl_path(allocator, filePath, pathStyle, isDirectory);
}

static CFURLRef
micdsp_cfurl_fsrep(CFAllocatorRef allocator, const UInt8 *buffer, CFIndex bufLen, Boolean isDirectory)
{
	unsigned mode = micdsp_active();

	if (__builtin_expect(mode != 0u, 0) && buffer != NULL && bufLen > 0) {
		char orig[256], rewritten[256], stem[FUXI_MIC_DSP_STEM_MAX];
		if ((size_t)bufLen < sizeof(orig)) {
			memcpy(orig, buffer, (size_t)bufLen);
			orig[bufLen] = '\0';
			if (fuxi_mic_dsp_rewrite_path_ex(mode, orig, rewritten, sizeof(rewritten), stem, sizeof(stem))) {
				micdsp_log_rewrite("CFURLFSRep", orig, rewritten);
				CFURLRef url = micdsp_real_cfurl_fsrep(allocator, (const UInt8 *)rewritten,
				    (CFIndex)strlen(rewritten), isDirectory);
				micdsp_record_url(url, stem);
				return url;
			}
		}
	}
	return micdsp_real_cfurl_fsrep(allocator, buffer, bufLen, isDirectory);
}

static CFReadStreamRef
micdsp_readstream_file(CFAllocatorRef alloc, CFURLRef fileURL)
{
	CFReadStreamRef stream = micdsp_real_readstream_file(alloc, fileURL);
	unsigned mode = micdsp_active();

	if (__builtin_expect(mode != 0u, 0) && fileURL != NULL && stream != NULL) {
		UInt8 pathbuf[256];
		if (CFURLGetFileSystemRepresentation(fileURL, true, pathbuf, (CFIndex)sizeof(pathbuf)) &&
		    strstr((const char *)pathbuf, "d22_bottom_mic_measurement.austrip") != NULL) {
			char stem[FUXI_MIC_DSP_STEM_MAX];
			pthread_mutex_lock(&micdsp_map_lock);
			bool hit = fuxi_mic_dsp_map_take(&micdsp_url_map, fileURL, stem, sizeof(stem));
			if (hit) {
				(void)fuxi_mic_dsp_map_put(&micdsp_stream_map, stream, stem);
			}
			pthread_mutex_unlock(&micdsp_map_lock);
		}
	}
	return stream;
}

static CFPropertyListRef
micdsp_plist_stream(CFAllocatorRef allocator, CFReadStreamRef stream, CFIndex streamLength,
    CFOptionFlags options, CFPropertyListFormat *format, CFErrorRef *error)
{
	unsigned mode = micdsp_active();
	char stem[FUXI_MIC_DSP_STEM_MAX];
	bool has_stem = false;

	if (__builtin_expect(mode != 0u, 0) && stream != NULL) {
		pthread_mutex_lock(&micdsp_map_lock);
		has_stem = fuxi_mic_dsp_map_take(&micdsp_stream_map, stream, stem, sizeof(stem));
		pthread_mutex_unlock(&micdsp_map_lock);
	}
	CFPropertyListRef plist = micdsp_real_plist_stream(allocator, stream, streamLength, options, format, error);
	if (has_stem && plist != NULL) {
		plist = micdsp_patch_plist_name(plist, stem, NULL);
	}
	return plist;
}

static CFPropertyListRef
micdsp_plist_data(CFAllocatorRef allocator, CFDataRef data, CFOptionFlags options,
    CFPropertyListFormat *format, CFErrorRef *error)
{
	CFPropertyListRef plist = micdsp_real_plist_data(allocator, data, options, format, error);
	unsigned mode = micdsp_active();

	if (__builtin_expect(mode != 0u, 0) && micdsp_tls_stem[0] != '\0' && plist != NULL) {
		bool patched = false;
		plist = micdsp_patch_plist_name(plist, micdsp_tls_stem, &patched);
		if (patched) {
			micdsp_tls_stem[0] = '\0';
		}
	}
	return plist;
}

struct micdsp_interpose {
	const void *replacement;
	const void *original;
};

#define MICDSP_INTERPOSE(replacement, original)                                                                     \
	__attribute__((used, section("__DATA,__interpose"))) static const struct micdsp_interpose                        \
	    micdsp_tuple_##replacement = { (const void *)&replacement, (const void *)&original }

MICDSP_INTERPOSE(micdsp_open, micdsp_real_open);
MICDSP_INTERPOSE(micdsp_openat, micdsp_real_openat);
MICDSP_INTERPOSE(micdsp_stat, micdsp_real_stat);
MICDSP_INTERPOSE(micdsp_access, micdsp_real_access);
MICDSP_INTERPOSE(micdsp_cfurl_path, micdsp_real_cfurl_path);
MICDSP_INTERPOSE(micdsp_cfurl_fsrep, micdsp_real_cfurl_fsrep);
MICDSP_INTERPOSE(micdsp_readstream_file, micdsp_real_readstream_file);
MICDSP_INTERPOSE(micdsp_plist_stream, micdsp_real_plist_stream);
MICDSP_INTERPOSE(micdsp_plist_data, micdsp_real_plist_data);

__attribute__((constructor)) static void
fuxi_micdsp_init(void)
{
	const char *name = getprogname();
	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	unsigned mode = micdsp_adt_u32("fuxi,audio-mic-dsp") & FUXI_MIC_DSP_ALL;
	if (mode == 0u) {
		return;
	}
	__atomic_store_n(&micdsp_mode, mode, __ATOMIC_RELEASE);
	micdsp_note("MDLOG-DSP: on mode=%u pid=%d\n", mode, getpid());
}
