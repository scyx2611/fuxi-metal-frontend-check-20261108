/*
 * libfuxi-sb.dylib (fuxi-shape): the status bar follows Fuxi's centred punch hole instead of the iPhone X notch.
 *
 * Fuxi runs 20H392 as an iPhone X. _UIStatusBarVisualProvider_FixedSplit lays the
 * two status-bar clusters out around its -notchRect:
 *   ears = (375 - notch width) / 2 points, each cluster centred in its ear;
 *   baseline = notch height - baselineBottomInset.
 * The X notch is (249, 0, 627, 90) in the 1125-pixel space (209 x 30 points), far wider than Fuxi's hole,
 * so the time and the icons sit near the corners and above the hole's centre.
 *
 * This library changes the provider's rectangle to a narrower and taller one on the same axis
 * (default 300 x 98 pixels). UIScreen._exclusionArea stays original: UIKit uses its exact dimensions
 * to select the provider and shows its diagnostic provider for an unrecognized cut-out.
 * -notchSize reads -notchRect, so both size and layout receive the same adjusted geometry.
 *
 * The kernel inserts the library into SpringBoard with ADT /chosen u32 "fuxi,inject"=2 (or boot-arg fxinj=2,
 * xnu patch 0118): launchd drops DYLD_ variables from com.apple jobs. ADT /chosen u32 "fuxi,sbshape" = W * 1000 + H
 * (pixels, 1125 space; 30..627 x 30..150) sets the size, "fuxi,sbshape"=0 leaves SpringBoard alone; the daily
 * loader's boot-args field is full, so both are ADT properties (IODeviceTree:/chosen). Lines go to stderr
 * (the kernel log while SpringBoard is in sweetnull).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* IOKit.framework ships without headers in the iPhoneOS SDK: the three calls used here */
typedef mach_port_t fx_io_object_t;
extern fx_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(fx_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(fx_io_object_t object);

#define SHAPE_CLASS     "_UIStatusBarVisualProvider_FixedSplit"
#define SHAPE_SELECTOR  "notchRect"
#define SHAPE_LIB       "/usr/local/lib/fuxi/libfuxi-sb.dylib"
#define SHAPE_SAID      8               /* the first replacements are logged */

/* CGRect: four doubles, a homogeneous aggregate passed and returned in d0-d3 on arm64 */
typedef struct {
	double x, y, w, h;
} fx_rect;

/* the iPhone X family's notch, 1125-pixel space */
static const fx_rect x_notch = { 249.0, 0.0, 627.0, 90.0 };

static double shape_w = 300.0, shape_h = 98.0;
static unsigned shape_said;
static int hooked;                      /* 0 not yet, 1 replaced */
static bool shape_disabled;
static IMP original_shape;
extern bool fuxi_flashlight_hook(void);
extern bool fuxi_volhud_hook(void);

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0) {
		(void)write(2, line, (size_t)n);
	}
}

static bool
fx_near(double a, double b)
{
	return a > b - 0.5 && a < b + 0.5;
}

/* -[_UIStatusBarVisualProvider_FixedSplit notchRect] */
static fx_rect
shape_for_provider(id self, SEL selector)
{
	fx_rect r = ((fx_rect (*)(id, SEL))original_shape)(self, selector), n;
	double unit_scale = 1.0;

	if (fx_near(r.x, x_notch.x / 3.0) && fx_near(r.y, x_notch.y / 3.0) &&
	    fx_near(r.w, x_notch.w / 3.0) && fx_near(r.h, x_notch.h / 3.0)) {
		unit_scale = 1.0 / 3.0;
	}
	if (!fx_near(r.x, x_notch.x * unit_scale) || !fx_near(r.y, x_notch.y * unit_scale) ||
	    !fx_near(r.w, x_notch.w * unit_scale) || !fx_near(r.h, x_notch.h * unit_scale)) {
		if (__atomic_fetch_add(&shape_said, 1, __ATOMIC_RELAXED) < SHAPE_SAID) {
			say("fuxi: SpringBoard: provider notch (%.1f, %.1f, %.1f, %.1f) is not the X notch; unchanged\n",
			    r.x, r.y, r.w, r.h);
		}
		return r;
	}
	n.x = r.x + (r.w - shape_w * unit_scale) / 2.0;
	n.y = r.y;
	n.w = shape_w * unit_scale;
	n.h = shape_h * unit_scale;
	if (__atomic_fetch_add(&shape_said, 1, __ATOMIC_RELAXED) < SHAPE_SAID) {
		say("fuxi: SpringBoard: provider notch (%.1f, %.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f, %.1f)\n",
		    r.x, r.y, r.w, r.h, n.x, n.y, n.w, n.h);
	}
	return n;
}

/*
 * ADT /chosen u32 "fuxi,sbshape" (IODeviceTree:/chosen, a 4-byte CFData): 0 when absent, 1 when 0 (off),
 * 2 when W * 1000 + H, read into *w, *h. A value outside 30..627 x 30..150 counts as absent.
 */
static int
adt_shape(double *w, double *h)
{
	fx_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	CFTypeRef value;
	uint32_t v = 0;
	bool got = false;
	unsigned a, b;

	if (chosen == MACH_PORT_NULL) {
		return 0;
	}
	value = IORegistryEntryCreateCFProperty(chosen, CFSTR("fuxi,sbshape"), kCFAllocatorDefault, 0);
	(void)IOObjectRelease(chosen);
	if (value == NULL) {
		return 0;
	}
	if (CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)value) == (CFIndex)sizeof(v)) {
		memcpy(&v, CFDataGetBytePtr((CFDataRef)value), sizeof(v));
		got = true;
	}
	CFRelease(value);
	if (!got) {
		return 0;
	}
	if (v == 0) {
		return 1;
	}
	a = v / 1000;
	b = v % 1000;
	if (a < 30 || a > 627 || b < 30 || b > 150) {
		say("fuxi: SpringBoard: fuxi,sbshape=%u out of range; default size\n", v);
		return 0;
	}
	*w = a;
	*h = b;
	return 2;
}

/* Replace the instance method once UIKit has registered the provider. True when done. */
static bool
try_hook(void)
{
	Class cls;
	Method method;

	if (__atomic_load_n(&hooked, __ATOMIC_ACQUIRE) != 0) {
		return true;
	}
	cls = objc_getClass(SHAPE_CLASS);
	if (cls == NULL) {
		return false;
	}
	method = class_getInstanceMethod(cls, sel_registerName(SHAPE_SELECTOR));
	if (method == NULL) {
		return false;
	}
	if (__atomic_exchange_n(&hooked, 1, __ATOMIC_ACQ_REL) != 0) {
		return true;
	}
	original_shape = method_setImplementation(method, (IMP)shape_for_provider);
	say("fuxi: SpringBoard: status-bar provider %.0f x %.0f px; display cut-out preserved\n", shape_w, shape_h);
	return true;
}

static void
image_added(const struct mach_header *header, intptr_t slide)
{
	(void)header;
	(void)slide;
	if (!shape_disabled) {
		(void)try_hook();
	}
	(void)fuxi_flashlight_hook();
	(void)fuxi_volhud_hook();
}

__attribute__((constructor)) static void
fuxi_shape_init(void)
{
	const char *name = getprogname();
	const char *inserted = getenv("DYLD_INSERT_LIBRARIES");
	int arg;

	/* children of SpringBoard do not inherit the library */
	if (inserted != NULL && strcmp(inserted, SHAPE_LIB) == 0) {
		(void)unsetenv("DYLD_INSERT_LIBRARIES");
	}
	if (name == NULL || strcmp(name, "SpringBoard") != 0) {
		return;
	}
	arg = adt_shape(&shape_w, &shape_h);
	if (arg == 1) {
		shape_disabled = true;
		say("fuxi: SpringBoard: fuxi,sbshape=0, status-bar cut-out unchanged\n");
	} else if (!try_hook()) {
		say("fuxi: SpringBoard: -[%s %s] not there yet; waiting for images\n", SHAPE_CLASS, SHAPE_SELECTOR);
	}
	(void)fuxi_flashlight_hook();
	(void)fuxi_volhud_hook();
	_dyld_register_func_for_add_image(image_added);
}
