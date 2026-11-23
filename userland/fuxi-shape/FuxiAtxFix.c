/*
 * Fix purple-tinted system blur/tint regions caused by pseudo-twiddled ATX
 * wallpapers on devices without Metal access in UI processes (U-026).
 *
 * References (AGENTS.md section 0):
 *   - Reuses the 20H392 dyld_shared_cache dual-anchor slide verification,
 *     vm_read_overwrite fingerprinting, and __DATA_CONST VM_PROT_COPY vtable
 *     patching pattern established in FuxiVibrate.c (libfuxi-sb.dylib).
 *   - Reuses the ADT /chosen u32 gate, process allow-list, and stderr log
 *     conventions from FuxiVolHud.c and FuxiRtcPersist.c, linking strictly the
 *     4-library allow-list checked by verify-sb-shape.py (libSystem, libobjc,
 *     CoreFoundation, IOKit).
 *
 * Gated behind ADT /chosen u32 "fuxi,atx-fix" (default 0 / absent = disabled):
 *   1 : Intercept ATXReadPlugin::decodeImageData via __ZTV13ATXReadPlugin+0x68
 *       inside SpringBoard and Preferences; when decoding uncompressed ASTC
 *       tagged as pre-twiddled (lowercase 'astc') whose 16-byte HEAD watermark
 *       is all zero, route directly to ASTCTextureImp::decodeRGBXFromLinear
 *       without mutating object state or affecting QuartzCore zero-copy lock.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "FuxiAtxFixLogic.h"

typedef mach_port_t fx_atx_io_object_t;
extern fx_atx_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(fx_atx_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(fx_atx_io_object_t object);

#define ATXFIX_LOG_LIMIT 12u

struct fx_vimage_buffer {
	void *data;
	unsigned long height;
	unsigned long width;
	size_t rowBytes;
};

typedef int (*fx_rp_decode_data_fn)(void *plugin, uint8_t *buf, size_t len);
typedef bool (*fx_tex_decode_linear_fn)(void *tex, void *session, struct fx_vimage_buffer *vbuf, uint32_t alpha,
    uint32_t block_format, uint32_t texel_format);

static const char *atxfix_prog = "unknown";
static int atxfix_adt_state; /* 0 unread, -1 off, 1 on */
static unsigned atxfix_installed;
static unsigned atxfix_refused_said;
static unsigned atxfix_decode_said;
static unsigned atxfix_fail_said;

static uintptr_t atxfix_tex_vptr;
static fx_rp_decode_data_fn atxfix_orig_decode_data;
static fx_tex_decode_linear_fn atxfix_decode_linear;

static void
atxfix_say(const char *format, ...)
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

static bool
atxfix_copy(uint64_t addr, void *out, size_t size)
{
	vm_size_t got = 0;

	return addr != 0u &&
	       vm_read_overwrite(mach_task_self(), (vm_address_t)addr, size, (vm_address_t)out, &got) == KERN_SUCCESS &&
	       got == size;
}

static bool
atxfix_target_process(const char *name)
{
	return name != NULL && (strcmp(name, "SpringBoard") == 0 || strcmp(name, "Preferences") == 0);
}

static bool
atxfix_load_adt(void)
{
	int state = __atomic_load_n(&atxfix_adt_state, __ATOMIC_ACQUIRE);
	fx_atx_io_object_t chosen;
	uint32_t val = 0;
	bool got = false;

	if (state != 0) {
		return state > 0;
	}
	chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	if (chosen != MACH_PORT_NULL) {
		CFTypeRef value = IORegistryEntryCreateCFProperty(chosen, CFSTR("fuxi,atx-fix"), kCFAllocatorDefault, 0);
		(void)IOObjectRelease(chosen);
		if (value != NULL) {
			if (CFGetTypeID(value) == CFDataGetTypeID() &&
			    CFDataGetLength((CFDataRef)value) == (CFIndex)sizeof(val)) {
				memcpy(&val, CFDataGetBytePtr((CFDataRef)value), sizeof(val));
				got = true;
			}
			CFRelease(value);
		}
	}
	if (!got || val == 0u) {
		__atomic_store_n(&atxfix_adt_state, -1, __ATOMIC_RELEASE);
		return false;
	}
	if (val != 1u) {
		atxfix_say("fuxi: %s: fuxi,atx-fix=%u unsupported (expected 0 or 1); unchanged\n", atxfix_prog, val);
		__atomic_store_n(&atxfix_adt_state, -1, __ATOMIC_RELEASE);
		return false;
	}
	__atomic_store_n(&atxfix_adt_state, 1, __ATOMIC_RELEASE);
	return true;
}

static uint32_t
atxfix_read_u32(const uint8_t *base, size_t off)
{
	uint32_t v = 0;
	memcpy(&v, base + off, sizeof(v));
	return v;
}

static uint64_t
atxfix_read_u64(const uint8_t *base, size_t off)
{
	uint64_t v = 0;
	memcpy(&v, base + off, sizeof(v));
	return v;
}

static void *
atxfix_read_ptr(const uint8_t *base, size_t off)
{
	void *p = NULL;
	memcpy(&p, base + off, sizeof(p));
	return p;
}

/*
 * Replacement for __ZTV13ATXReadPlugin + 0x68 (ATXReadPlugin::decodeImageData).
 * All non-matching cases tail-call the original ImageIO method untouched.
 */
static int
atxfix_decode_image_data(void *plugin, uint8_t *buf, size_t len)
{
	const uint8_t *rp = (const uint8_t *)plugin;
	const uint8_t *tex;
	void *session;
	uint32_t width, height, row_bytes;
	struct fuxi_atx_decode_params params;
	struct fx_vimage_buffer vbuf;
	bool ok;

	if (rp == NULL || buf == NULL || len == 0u) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}
	if (atxfix_read_u64(rp, FUXI_ATX_RP_OFF_420F_A) != 0u ||
	    atxfix_read_u64(rp, FUXI_ATX_RP_OFF_420F_B) != 0u ||
	    atxfix_read_u64(rp, FUXI_ATX_RP_OFF_ASTC_CHUNK) == 0u) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}
	if (!fuxi_atx_header_is_zero_watermark(rp + FUXI_ATX_RP_OFF_HEADER)) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}
	tex = (const uint8_t *)atxfix_read_ptr(rp, FUXI_ATX_RP_OFF_TEX);
	if (tex == NULL || (uintptr_t)atxfix_read_ptr(tex, FUXI_ATX_TEX_OFF_VPTR) != atxfix_tex_vptr) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}
	/* Only intercept uncompressed pseudo-pre-twiddled ASTC textures. */
	if (tex[FUXI_ATX_TEX_OFF_LZFSE] != 0u || tex[FUXI_ATX_TEX_OFF_PRETWIDDLED] == 0u) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}
	session = atxfix_read_ptr(rp, FUXI_ATX_RP_OFF_SESSION);
	if (session == NULL) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}
	width = atxfix_read_u32(rp, FUXI_ATX_RP_OFF_WIDTH);
	height = atxfix_read_u32(rp, FUXI_ATX_RP_OFF_HEIGHT);
	row_bytes = atxfix_read_u32(rp, FUXI_ATX_RP_OFF_ROWBYTES);
	if (!fuxi_atx_check_vimage_bounds(width, height, row_bytes, len)) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}
	if (!fuxi_atx_resolve_decode_params(tex[FUXI_ATX_TEX_OFF_BLOCK_W], tex[FUXI_ATX_TEX_OFF_BLOCK_H],
	        rp[FUXI_ATX_RP_OFF_ALPHA_INFO], rp[FUXI_ATX_RP_OFF_SRGB_FLAG] == 1u ? 1u : 0u,
	        tex[FUXI_ATX_TEX_OFF_HDR], &params)) {
		return atxfix_orig_decode_data(plugin, buf, len);
	}

	vbuf.data = buf;
	vbuf.height = (unsigned long)height;
	vbuf.width = (unsigned long)width;
	vbuf.rowBytes = (size_t)row_bytes;

	(void)pthread_mutex_lock((pthread_mutex_t *)(uintptr_t)(tex + FUXI_ATX_TEX_OFF_MUTEX));
	ok = atxfix_decode_linear((void *)(uintptr_t)tex, session, &vbuf, params.alpha, params.block_format,
	    params.texel_format);
	(void)pthread_mutex_unlock((pthread_mutex_t *)(uintptr_t)(tex + FUXI_ATX_TEX_OFF_MUTEX));

	if (!ok) {
		if (__atomic_fetch_add(&atxfix_fail_said, 1u, __ATOMIC_RELAXED) < ATXFIX_LOG_LIMIT) {
			atxfix_say("fuxi: %s: atx-fix decodeRGBXFromLinear failed %ux%u blk=%ux%u srgb=%u\n",
			    atxfix_prog, width, height, tex[FUXI_ATX_TEX_OFF_BLOCK_W],
			    tex[FUXI_ATX_TEX_OFF_BLOCK_H], rp[FUXI_ATX_RP_OFF_SRGB_FLAG] == 1u ? 1u : 0u);
		}
		return FUXI_ATX_DECODE_FAIL;
	}

	if (__atomic_fetch_add(&atxfix_decode_said, 1u, __ATOMIC_RELAXED) < ATXFIX_LOG_LIMIT) {
		uint8_t q1[4] = { 0, 0, 0, 0 };
		uint8_t mid[4] = { 0, 0, 0, 0 };
		uint8_t q3[4] = { 0, 0, 0, 0 };
		(void)fuxi_atx_sample_pixel(buf, width, height, row_bytes, width / 4u, height / 4u, q1);
		(void)fuxi_atx_sample_pixel(buf, width, height, row_bytes, width / 2u, height / 2u, mid);
		(void)fuxi_atx_sample_pixel(buf, width, height, row_bytes, (width * 3u) / 4u, (height * 3u) / 4u, q3);
		atxfix_say("fuxi: %s: atx-fix decoded linear %ux%u blk=%ux%u srgb=%u "
		    "q1=(%u,%u,%u,%u) mid=(%u,%u,%u,%u) q3=(%u,%u,%u,%u)\n",
		    atxfix_prog, width, height, tex[FUXI_ATX_TEX_OFF_BLOCK_W], tex[FUXI_ATX_TEX_OFF_BLOCK_H],
		    rp[FUXI_ATX_RP_OFF_SRGB_FLAG] == 1u ? 1u : 0u,
		    q1[0], q1[1], q1[2], q1[3], mid[0], mid[1], mid[2], mid[3], q3[0], q3[1], q3[2], q3[3]);
	}
	return 0;
}

static bool
atxfix_try_install(void)
{
	void *sym_create;
	void *sym_get_type;
	struct fuxi_atx_layout_snapshot snap;
	uint64_t s;
	enum fuxi_atx_patch_result res;
	uintptr_t slot_addr;
	vm_address_t page;
	kern_return_t kr;

	if (__atomic_load_n(&atxfix_installed, __ATOMIC_ACQUIRE) != 0u) {
		return true;
	}
	sym_create = dlsym(RTLD_DEFAULT, "CGImageSourceCreateWithData");
	sym_get_type = dlsym(RTLD_DEFAULT, "CGImageSourceGetType");
	if (sym_create == NULL || sym_get_type == NULL) {
		return false;
	}

	memset(&snap, 0, sizeof(snap));
	snap.slide_create = (uint64_t)(uintptr_t)sym_create - FUXI_ATX_CGIS_CREATE_WITH_DATA;
	snap.slide_get_type = (uint64_t)(uintptr_t)sym_get_type - FUXI_ATX_CGIS_GET_TYPE;
	snap.replacement_fn = (uint64_t)(uintptr_t)&atxfix_decode_image_data;
	s = snap.slide_create;

	if (snap.slide_create != snap.slide_get_type || (s & 0x3fffULL) != 0u ||
	    !atxfix_copy(FUXI_ATX_RP_SLOT_INIT + s, &snap.rp_slot_init, sizeof(uint64_t)) ||
	    !atxfix_copy(FUXI_ATX_RP_SLOT_SAVE_XPC + s, &snap.rp_slot_save_xpc, sizeof(uint64_t)) ||
	    !atxfix_copy(FUXI_ATX_RP_SLOT_DECODE_DATA + s, &snap.rp_slot_decode_data, sizeof(uint64_t)) ||
	    !atxfix_copy(FUXI_ATX_RP_SLOT_ADD_TYPE_ID + s, &snap.rp_slot_add_type_id, sizeof(uint64_t)) ||
	    !atxfix_copy(FUXI_ATX_TEX_SLOT_LOCK_DATA + s, &snap.tex_slot_lock, sizeof(uint64_t)) ||
	    !atxfix_copy(FUXI_ATX_TEX_SLOT_UNLOCK_DATA + s, &snap.tex_slot_unlock, sizeof(uint64_t)) ||
	    !atxfix_copy(FUXI_ATX_FN_RP_DECODE_DATA + s, &snap.decode_data_insn0, sizeof(uint32_t)) ||
	    !atxfix_copy(FUXI_ATX_FN_RP_DECODE_DATA + s + 4u, &snap.decode_data_insn1, sizeof(uint32_t)) ||
	    !atxfix_copy(FUXI_ATX_FN_DECODE_LINEAR + s, &snap.decode_linear_insn0, sizeof(uint32_t)) ||
	    !atxfix_copy(FUXI_ATX_FN_DECODE_LINEAR + s + 4u, &snap.decode_linear_insn1, sizeof(uint32_t))) {
		if (__atomic_exchange_n(&atxfix_refused_said, 1u, __ATOMIC_RELAXED) == 0u) {
			atxfix_say("fuxi: %s: atx-fix refused layout (anchor/unreadable) c=%#llx g=%#llx\n",
			    atxfix_prog, (unsigned long long)snap.slide_create,
			    (unsigned long long)snap.slide_get_type);
		}
		return true;
	}

	res = fuxi_atx_validate_layout(&snap);
	if (res == FUXI_ATX_ALREADY_PATCHED) {
		__atomic_store_n(&atxfix_installed, 1u, __ATOMIC_RELEASE);
		return true;
	}
	if (res != FUXI_ATX_OK) {
		if (__atomic_exchange_n(&atxfix_refused_said, 1u, __ATOMIC_RELAXED) == 0u) {
			atxfix_say("fuxi: %s: atx-fix refused layout (%s) rp_dec=%#llx insn0=%#x insn1=%#x\n",
			    atxfix_prog, fuxi_atx_patch_result_name(res),
			    (unsigned long long)snap.rp_slot_decode_data, snap.decode_data_insn0,
			    snap.decode_linear_insn0);
		}
		return true;
	}

	if (__atomic_exchange_n(&atxfix_installed, 1u, __ATOMIC_ACQ_REL) != 0u) {
		return true;
	}

	atxfix_tex_vptr = (uintptr_t)(FUXI_ATX_ASTC_TEX_VPTR + s);
	atxfix_orig_decode_data = (fx_rp_decode_data_fn)(uintptr_t)(FUXI_ATX_FN_RP_DECODE_DATA + s);
	atxfix_decode_linear = (fx_tex_decode_linear_fn)(uintptr_t)(FUXI_ATX_FN_DECODE_LINEAR + s);

	slot_addr = (uintptr_t)(FUXI_ATX_RP_SLOT_DECODE_DATA + s);
	page = slot_addr & ~(uintptr_t)(vm_page_size - 1u);
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		atxfix_say("fuxi: %s: atx-fix vm_protect RW|COPY failed kr=%d\n", atxfix_prog, kr);
		return true;
	}
	__atomic_store_n((uintptr_t *)slot_addr, (uintptr_t)&atxfix_decode_image_data, __ATOMIC_RELEASE);
	(void)vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	atxfix_say("fuxi: %s: atx-fix installed slide=%#llx slot=%#lx\n", atxfix_prog,
	    (unsigned long long)s, (unsigned long)slot_addr);
	return true;
}

static void
atxfix_image_added(const struct mach_header *header, intptr_t slide)
{
	(void)header;
	(void)slide;
	if (__atomic_load_n(&atxfix_installed, __ATOMIC_ACQUIRE) == 0u) {
		(void)atxfix_try_install();
	}
}

__attribute__((constructor)) static void
fuxi_atx_fix_init(void)
{
	const char *name = getprogname();

	if (!atxfix_target_process(name)) {
		return;
	}
	atxfix_prog = name;
	if (!atxfix_load_adt()) {
		return;
	}
	if (!atxfix_try_install()) {
		atxfix_say("fuxi: %s: atx-fix ImageIO not loaded yet; waiting for images\n", atxfix_prog);
		_dyld_register_func_for_add_image(atxfix_image_added);
	}
}
