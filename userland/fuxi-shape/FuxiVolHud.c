/*
 * Xiaomi 13 Volume HUD positioning hook for libfuxi-sb.dylib (SpringBoard).
 *
 * iPhone X (20H392) places the portrait Elastic Volume HUD on the left edge next to iPhone X's
 * left-side volume buttons. Xiaomi 13's physical volume buttons sit on the right edge above power.
 *
 * Gated behind ADT /chosen u32 "fuxi,volhud" (default off when absent or 0):
 *   1        : mirror portrait (axis == 2) SBElasticVolumeViewController HUD centers to right edge
 *              and clamp _leadingLabel/_trailingLabel inside the right edge margin.
 *   100..700 : additionally override -[SBElasticHUDSettings volumeButtonsCenterY] to that pt value.
 *
 * Landscape horizontal HUD (axis == 1) is never altered. Links only libSystem, libobjc,
 * CoreFoundation and IOKit to satisfy verify-sb-shape.py.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "FuxiVolHudLayout.h"

typedef mach_port_t fx_io_object_t;
extern fx_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(fx_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(fx_io_object_t object);

#define VOLHUD_VALUE_CLASS    "SBElasticValueViewController"
#define VOLHUD_VOLUME_CLASS   "SBElasticVolumeViewController"
#define VOLHUD_SETTINGS_CLASS "SBElasticHUDSettings"
#define VOLHUD_SAID_LIMIT     8u

static int volhud_adt_state; /* 0 unread, -1 off, 1 on */
static fx_volhud_config volhud_cfg;
static unsigned volhud_hooked_value;
static unsigned volhud_hooked_settings;
static unsigned volhud_center_said;
static unsigned volhud_y_said;
static unsigned volhud_h_said;

static Class volhud_volume_cls;
static Ivar volhud_leading_ivar;
static Ivar volhud_trailing_ivar;
static IMP volhud_orig_center;
static IMP volhud_orig_labels;
static IMP volhud_orig_center_y;
static IMP volhud_orig_state1_h;
static IMP volhud_orig_state2_h;
static IMP volhud_orig_state3_h;

static SEL sel_is_kind;
static SEL sel_axis;
static SEL sel_view_if_loaded;
static SEL sel_bounds;
static SEL sel_responds_to;
static SEL sel_frame;
static SEL sel_set_frame;

static void
volhud_say(const char *format, ...)
{
	char line[256];
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
volhud_load_adt(void)
{
	int state = __atomic_load_n(&volhud_adt_state, __ATOMIC_ACQUIRE);
	if (state != 0) {
		return state > 0;
	}
	fx_io_object_t chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	uint32_t raw = 0;
	bool got = false;
	if (chosen != MACH_PORT_NULL) {
		CFTypeRef value = IORegistryEntryCreateCFProperty(chosen, CFSTR("fuxi,volhud"), kCFAllocatorDefault, 0);
		(void)IOObjectRelease(chosen);
		if (value != NULL) {
			if (CFGetTypeID(value) == CFDataGetTypeID() &&
			    CFDataGetLength((CFDataRef)value) == (CFIndex)sizeof(raw)) {
				memcpy(&raw, CFDataGetBytePtr((CFDataRef)value), sizeof(raw));
				got = true;
			}
			CFRelease(value);
		}
	}
	if (!got || raw == 0u) {
		__atomic_store_n(&volhud_adt_state, -1, __ATOMIC_RELEASE);
		return false;
	}
	if (!fuxi_volhud_decode_adt(raw, &volhud_cfg)) {
		volhud_say("fuxi: SpringBoard: fuxi,volhud=%u out of range (use 0, 1, 100..700, or Y*1000+H); unchanged\n", raw);
		__atomic_store_n(&volhud_adt_state, -1, __ATOMIC_RELEASE);
		return false;
	}
	__atomic_store_n(&volhud_adt_state, 1, __ATOMIC_RELEASE);
	return true;
}

static bool
volhud_is_volume_controller(id self)
{
	Class cls = __atomic_load_n(&volhud_volume_cls, __ATOMIC_ACQUIRE);
	if (cls == Nil || self == nil) {
		return false;
	}
	return ((bool (*)(id, SEL, Class))objc_msgSend)(self, sel_is_kind, cls);
}

/* -[SBElasticValueViewController centerForState:containerViewSize:bounds:useSpringData:] */
static fx_volhud_point
volhud_center_for_state(id self, SEL selector, long state, fx_volhud_size container_size,
    fx_volhud_rect bounds, bool use_spring)
{
	fx_volhud_point pt = ((fx_volhud_point (*)(id, SEL, long, fx_volhud_size, fx_volhud_rect, bool))
	    volhud_orig_center)(self, selector, state, container_size, bounds, use_spring);
	if (!volhud_is_volume_controller(self)) {
		return pt;
	}
	long axis = ((long (*)(id, SEL))objc_msgSend)(self, sel_axis);
	fx_volhud_point adj = fuxi_volhud_mirror_center(pt, axis, bounds);
	if (adj.x != pt.x && __atomic_fetch_add(&volhud_center_said, 1u, __ATOMIC_RELAXED) < VOLHUD_SAID_LIMIT) {
		volhud_say("fuxi: SpringBoard: volhud state=%ld axis=%ld center (%.1f, %.1f) -> (%.1f, %.1f) w=%.1f\n",
		    state, axis, pt.x, pt.y, adj.x, adj.y, bounds.w);
	}
	return adj;
}

static void
volhud_clamp_ivar_label(id self, Ivar ivar, fx_volhud_rect bounds)
{
	if (ivar == NULL) {
		return;
	}
	id label = object_getIvar(self, ivar);
	if (label == nil) {
		return;
	}
	if (!((bool (*)(id, SEL, SEL))objc_msgSend)(label, sel_responds_to, sel_frame) ||
	    !((bool (*)(id, SEL, SEL))objc_msgSend)(label, sel_responds_to, sel_set_frame)) {
		return;
	}
	fx_volhud_rect frame = ((fx_volhud_rect (*)(id, SEL))objc_msgSend)(label, sel_frame);
	double clamped_x = fuxi_volhud_clamp_label_x(frame.x, frame.w, bounds.x, bounds.w, FX_VOLHUD_LABEL_MARGIN);
	if (clamped_x < frame.x - 0.25 || clamped_x > frame.x + 0.25) {
		frame.x = clamped_x;
		((void (*)(id, SEL, fx_volhud_rect))objc_msgSend)(label, sel_set_frame, frame);
	}
}

/* -[SBElasticValueViewController _updateLabelsForAxis:containerViewSize:state:animated:] */
static void
volhud_update_labels(id self, SEL selector, long axis, fx_volhud_size container_size, long state, bool animated)
{
	((void (*)(id, SEL, long, fx_volhud_size, long, bool))volhud_orig_labels)(
	    self, selector, axis, container_size, state, animated);
	if (axis != FX_VOLHUD_AXIS_VERTICAL || !volhud_is_volume_controller(self)) {
		return;
	}
	id view = ((id (*)(id, SEL))objc_msgSend)(self, sel_view_if_loaded);
	if (view == nil) {
		return;
	}
	fx_volhud_rect bounds = ((fx_volhud_rect (*)(id, SEL))objc_msgSend)(view, sel_bounds);
	volhud_clamp_ivar_label(self, volhud_leading_ivar, bounds);
	volhud_clamp_ivar_label(self, volhud_trailing_ivar, bounds);
}

/* -[SBElasticHUDSettings volumeButtonsCenterY] */
static double
volhud_buttons_center_y(id self, SEL selector)
{
	double stock = ((double (*)(id, SEL))volhud_orig_center_y)(self, selector);
	if (__atomic_exchange_n(&volhud_y_said, 1u, __ATOMIC_RELAXED) == 0u) {
		volhud_say("fuxi: SpringBoard: volhud volumeButtonsCenterY %.1f -> %.1f\n", stock, volhud_cfg.center_y);
	}
	return volhud_cfg.center_y;
}

/* -[SBElasticHUDSettings portraitState{1,2,3}Height] */
static double
volhud_portrait_state1_height(id self, SEL selector)
{
	double stock = ((double (*)(id, SEL))volhud_orig_state1_h)(self, selector);
	double scaled = fuxi_volhud_scale_height(stock, &volhud_cfg);
	if (__atomic_fetch_or(&volhud_h_said, 1u, __ATOMIC_RELAXED) == 0u) {
		volhud_say("fuxi: SpringBoard: volhud portraitState1Height %.1f -> %.1f\n", stock, scaled);
	}
	return scaled;
}

static double
volhud_portrait_state2_height(id self, SEL selector)
{
	double stock = ((double (*)(id, SEL))volhud_orig_state2_h)(self, selector);
	double scaled = fuxi_volhud_scale_height(stock, &volhud_cfg);
	if ((__atomic_fetch_or(&volhud_h_said, 2u, __ATOMIC_RELAXED) & 2u) == 0u) {
		volhud_say("fuxi: SpringBoard: volhud portraitState2Height %.1f -> %.1f\n", stock, scaled);
	}
	return scaled;
}

static double
volhud_portrait_state3_height(id self, SEL selector)
{
	double stock = ((double (*)(id, SEL))volhud_orig_state3_h)(self, selector);
	double scaled = fuxi_volhud_scale_height(stock, &volhud_cfg);
	if ((__atomic_fetch_or(&volhud_h_said, 4u, __ATOMIC_RELAXED) & 4u) == 0u) {
		volhud_say("fuxi: SpringBoard: volhud portraitState3Height %.1f -> %.1f\n", stock, scaled);
	}
	return scaled;
}

static bool
volhud_is_int_arg(char c)
{
	return c == 'q' || c == 'l' || c == 'Q' || c == 'L';
}

static bool
volhud_is_bool_arg(char c)
{
	return c == 'B' || c == 'c';
}

static bool
volhud_check_center_sig(Method m)
{
	char ret[32], a2[16], a3[32], a4[64], a5[16];
	if (m == NULL || method_getNumberOfArguments(m) != 6) {
		return false;
	}
	method_getReturnType(m, ret, sizeof(ret));
	method_getArgumentType(m, 2, a2, sizeof(a2));
	method_getArgumentType(m, 3, a3, sizeof(a3));
	method_getArgumentType(m, 4, a4, sizeof(a4));
	method_getArgumentType(m, 5, a5, sizeof(a5));
	return ret[0] == '{' && volhud_is_int_arg(a2[0]) && a3[0] == '{' && a4[0] == '{' && volhud_is_bool_arg(a5[0]);
}

static bool
volhud_check_labels_sig(Method m)
{
	char ret[16], a2[16], a3[32], a4[16], a5[16];
	if (m == NULL || method_getNumberOfArguments(m) != 6) {
		return false;
	}
	method_getReturnType(m, ret, sizeof(ret));
	method_getArgumentType(m, 2, a2, sizeof(a2));
	method_getArgumentType(m, 3, a3, sizeof(a3));
	method_getArgumentType(m, 4, a4, sizeof(a4));
	method_getArgumentType(m, 5, a5, sizeof(a5));
	return ret[0] == 'v' && volhud_is_int_arg(a2[0]) && a3[0] == '{' && volhud_is_int_arg(a4[0]) &&
	    volhud_is_bool_arg(a5[0]);
}

static bool
volhud_check_double_getter_sig(Method m)
{
	char ret[16];
	if (m == NULL || method_getNumberOfArguments(m) != 2) {
		return false;
	}
	method_getReturnType(m, ret, sizeof(ret));
	return ret[0] == 'd';
}

bool
fuxi_volhud_hook(void)
{
	if (!volhud_load_adt()) {
		return true;
	}
	if (__atomic_load_n(&volhud_hooked_value, __ATOMIC_ACQUIRE) == 0u) {
		Class value_cls = objc_getClass(VOLHUD_VALUE_CLASS);
		Class vol_cls = objc_getClass(VOLHUD_VOLUME_CLASS);
		if (value_cls != Nil && vol_cls != Nil) {
			Method center_m = class_getInstanceMethod(value_cls,
			    sel_registerName("centerForState:containerViewSize:bounds:useSpringData:"));
			Method labels_m = class_getInstanceMethod(value_cls,
			    sel_registerName("_updateLabelsForAxis:containerViewSize:state:animated:"));
			if (volhud_check_center_sig(center_m)) {
				if (__atomic_exchange_n(&volhud_hooked_value, 1u, __ATOMIC_ACQ_REL) == 0u) {
					sel_is_kind = sel_registerName("isKindOfClass:");
					sel_axis = sel_registerName("axis");
					sel_view_if_loaded = sel_registerName("viewIfLoaded");
					sel_bounds = sel_registerName("bounds");
					sel_responds_to = sel_registerName("respondsToSelector:");
					sel_frame = sel_registerName("frame");
					sel_set_frame = sel_registerName("setFrame:");
					volhud_leading_ivar = class_getInstanceVariable(value_cls, "_leadingLabel");
					volhud_trailing_ivar = class_getInstanceVariable(value_cls, "_trailingLabel");
					__atomic_store_n(&volhud_volume_cls, vol_cls, __ATOMIC_RELEASE);
					volhud_orig_center = method_setImplementation(center_m,
					    (IMP)volhud_center_for_state);
					bool labels_ok = false;
					if (volhud_check_labels_sig(labels_m) &&
					    (volhud_leading_ivar != NULL || volhud_trailing_ivar != NULL)) {
						volhud_orig_labels = method_setImplementation(labels_m,
						    (IMP)volhud_update_labels);
						labels_ok = true;
					}
					volhud_say("fuxi: SpringBoard: volhud right-edge hook installed (labels=%u override-y=%u y=%.0f override-h=%u h=%.0f)\n",
					    (unsigned)labels_ok, (unsigned)volhud_cfg.override_y, volhud_cfg.center_y,
					    (unsigned)volhud_cfg.override_height, volhud_cfg.state1_height);
				}
			}
		}
	}
	if ((volhud_cfg.override_y || volhud_cfg.override_height) &&
	    __atomic_load_n(&volhud_hooked_settings, __ATOMIC_ACQUIRE) == 0u) {
		Class settings_cls = objc_getClass(VOLHUD_SETTINGS_CLASS);
		if (settings_cls != Nil) {
			Method y_m = class_getInstanceMethod(settings_cls, sel_registerName("volumeButtonsCenterY"));
			Method h1_m = class_getInstanceMethod(settings_cls, sel_registerName("portraitState1Height"));
			Method h2_m = class_getInstanceMethod(settings_cls, sel_registerName("portraitState2Height"));
			Method h3_m = class_getInstanceMethod(settings_cls, sel_registerName("portraitState3Height"));
			bool y_ok = !volhud_cfg.override_y || volhud_check_double_getter_sig(y_m);
			bool h_ok = !volhud_cfg.override_height ||
			    (volhud_check_double_getter_sig(h1_m) && volhud_check_double_getter_sig(h2_m) &&
			     volhud_check_double_getter_sig(h3_m));
			if (y_ok && h_ok) {
				if (__atomic_exchange_n(&volhud_hooked_settings, 1u, __ATOMIC_ACQ_REL) == 0u) {
					if (volhud_cfg.override_y) {
						volhud_orig_center_y = method_setImplementation(y_m,
						    (IMP)volhud_buttons_center_y);
					}
					if (volhud_cfg.override_height) {
						volhud_orig_state1_h = method_setImplementation(h1_m,
						    (IMP)volhud_portrait_state1_height);
						volhud_orig_state2_h = method_setImplementation(h2_m,
						    (IMP)volhud_portrait_state2_height);
						volhud_orig_state3_h = method_setImplementation(h3_m,
						    (IMP)volhud_portrait_state3_height);
					}
				}
			}
		}
	}
	return __atomic_load_n(&volhud_hooked_value, __ATOMIC_ACQUIRE) != 0u &&
	    (!(volhud_cfg.override_y || volhud_cfg.override_height) ||
	     __atomic_load_n(&volhud_hooked_settings, __ATOMIC_ACQUIRE) != 0u);
}
