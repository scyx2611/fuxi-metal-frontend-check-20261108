/*
 * Minimal CoreMedia capture XPC server stubs inside mediaserverd (libfuxi-sb.dylib).
 *
 * Why this exists:
 *   sweet-root-tweaks.py sets StartRecorderServer=False in MediaToolbox's
 *   SubServerConfiguration.plist to prevent CMCapture._FigPreloadShaders from crashing
 *   mediaserverd, which skips CMCaptureCore._FigStartCaptureServers. However,
 *   com.apple.mediaserverd.plist still advertises those MachServices to launchd, so
 *   the first synchronous client call (SpringBoard main thread calling
 *   -[AVCaptureProprietaryDefaultsSingleton init] -> _FigCaptureSourceRemoteCopyCaptureSources(8)
 *   when microphone recording starts in Voice Memos or Dictation) blocks forever in
 *   mach_msg2_trap waiting for a service nobody checked in.
 *
 * Design & references:
 *   - Reuses Apple's own CoreMedia._FigXPCServerStart / _FigXPCMessageGetOpCode (resolved via
 *     dlsym so libfuxi-sb.dylib preserves its exact 4-library link set checked by
 *     verify-sb-shape.py). CoreMedia handles '.png' handshake, '.Token', '.ServerCapabilities',
 *     '.ErrorReturn', and client death cleanup natively.
 *   - Gated behind ADT /chosen u32 "fuxi,capsrc-stub" (default off when absent or 0):
 *       bit 0 (1): com.apple.coremedia.capturesource ('csrc' -> empty xpc_array, 'mjds' -> 0)
 *       bit 1 (2): com.apple.coremedia.capturesession (-12782)
 *       bit 2 (4): com.apple.coremedia.capture.deferred.containermanager.xpc (-12782)
 *       bit 3 (8): com.apple.coremedia.flashlight (-12782)
 *   - Camera coexistence: when ADT /chosen enables native camera (fuxi,camera != 0 and
 *     fuxi,camera-dylib != 0), /Developer/libfuxi-camera.dylib starts Apple's real CMCapture
 *     XPC servers (FigCaptureSourceServerStart, FigCaptureSessionServerStart,
 *     FigCaptureDeferredContainerManagerServerStart, FigFlashlightServerStart, plus
 *     viewfinder/memory reporter). Yield FX_CAPSRC_MASK_CAMERA_OWNED so Camera.app receives
 *     real capture sources while Voice Memos/Dictation continue using those native servers.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "FuxiCaptureStubLogic.h"

typedef mach_port_t fx_io_object_t;
extern fx_io_object_t IORegistryEntryFromPath(mach_port_t main_port, const char *path);
extern CFTypeRef IORegistryEntryCreateCFProperty(fx_io_object_t entry, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(fx_io_object_t object);

typedef int32_t (*fx_fig_xpc_msg_handler_t)(void *connection, void *request, void *reply);

typedef struct {
	uint64_t version;
	fx_fig_xpc_msg_handler_t message_handler;
	void *async_handler;
	void *client_death_handler;
	void *client_auth_handler;
} fx_fig_xpc_callbacks_t;

_Static_assert(sizeof(fx_fig_xpc_callbacks_t) == 0x28, "FigXPCServerCallbacks size mismatch");

typedef int32_t (*fx_fig_xpc_server_start_t)(const char *service_name, const fx_fig_xpc_callbacks_t *callbacks,
    CFDictionaryRef options, void **server_out);
typedef int32_t (*fx_fig_xpc_get_opcode_t)(void *message, uint32_t *opcode_out);
typedef void *(*fx_xpc_array_create_t)(void *const *objects, size_t count);
typedef void (*fx_xpc_dict_set_value_t)(void *xdict, const char *key, void *value);
typedef void (*fx_xpc_dict_set_uint64_t)(void *xdict, const char *key, uint64_t value);
typedef int64_t (*fx_xpc_dict_get_int64_t)(void *xdict, const char *key);
typedef void (*fx_xpc_release_t)(void *object);

#define CAPSRC_LOG_LIMIT 16u

static fx_fig_xpc_server_start_t fn_fig_xpc_server_start;
static fx_fig_xpc_get_opcode_t fn_fig_xpc_get_opcode;
static fx_xpc_array_create_t fn_xpc_array_create;
static fx_xpc_dict_set_value_t fn_xpc_dict_set_value;
static fx_xpc_dict_set_uint64_t fn_xpc_dict_set_uint64;
static fx_xpc_dict_get_int64_t fn_xpc_dict_get_int64;
static fx_xpc_release_t fn_xpc_release;

static void *capsrc_servers[FX_CAPSRC_SERVICE_COUNT];
static uint32_t capsrc_mask;
static unsigned capsrc_logged;

static void
capsrc_say(const char *format, ...)
{
	char line[256];
	va_list args;
	int length;

	va_start(args, format);
	length = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (length > (int)sizeof(line) - 1) {
		length = (int)sizeof(line) - 1;
	}
	if (length > 0) {
		(void)write(2, line, (size_t)length);
	}
}

static bool
capsrc_resolve_symbol(const char *symbol_name, void *dst_fn)
{
	void *sym = dlsym(RTLD_DEFAULT, symbol_name);
	if (sym == NULL) {
		return false;
	}
	memcpy(dst_fn, &sym, sizeof(sym));
	return true;
}

static bool
capsrc_resolve_all(void)
{
	return capsrc_resolve_symbol("FigXPCServerStart", &fn_fig_xpc_server_start) &&
	    capsrc_resolve_symbol("FigXPCMessageGetOpCode", &fn_fig_xpc_get_opcode) &&
	    capsrc_resolve_symbol("xpc_array_create", &fn_xpc_array_create) &&
	    capsrc_resolve_symbol("xpc_dictionary_set_value", &fn_xpc_dict_set_value) &&
	    capsrc_resolve_symbol("xpc_dictionary_set_uint64", &fn_xpc_dict_set_uint64) &&
	    capsrc_resolve_symbol("xpc_dictionary_get_int64", &fn_xpc_dict_get_int64) &&
	    capsrc_resolve_symbol("xpc_release", &fn_xpc_release);
}

static int32_t
capsrc_handle_common(uint32_t service_bit, const char *short_name, void *request, void *reply)
{
	uint32_t opcode = 0u;
	int32_t status = fn_fig_xpc_get_opcode(request, &opcode);
	int64_t payload = 0;
	fx_capsrc_action action;

	if (status != 0) {
		return status;
	}
	action = fuxi_capsrc_classify_op(service_bit, opcode);
	if (action == FX_CAPSRC_ACT_EMPTY_ARRAY) {
		payload = fn_xpc_dict_get_int64(request, FX_CAPSRC_KEY_PAYLOAD);
		if (reply != NULL) {
			void *empty_array = fn_xpc_array_create(NULL, 0);
			if (empty_array == NULL) {
				status = FX_CAPSRC_ERR_ALLOC;
			} else {
				fn_xpc_dict_set_value(reply, FX_CAPSRC_KEY_VALUE, empty_array);
				fn_xpc_release(empty_array);
				status = 0;
			}
		} else {
			status = 0;
		}
	} else if (action == FX_CAPSRC_ACT_ZERO_U64) {
		if (reply != NULL) {
			fn_xpc_dict_set_uint64(reply, FX_CAPSRC_KEY_VALUE, 0u);
		}
		status = 0;
	} else {
		status = FX_CAPSRC_ERR_UNSUPPORTED;
	}

	if (__atomic_fetch_add(&capsrc_logged, 1u, __ATOMIC_RELAXED) < CAPSRC_LOG_LIMIT) {
		char fourcc[5];
		fuxi_capsrc_format_fourcc(opcode, fourcc);
		capsrc_say("fuxi: mediaserverd: capsrc-stub svc=%s op=%s(0x%08x) payload=%lld rc=%d\n",
		    short_name, fourcc, (unsigned)opcode, (long long)payload, (int)status);
	}
	return status;
}

static int32_t
capsrc_handle_source(void *connection, void *request, void *reply)
{
	(void)connection;
	return capsrc_handle_common(FX_CAPSRC_BIT_SOURCE, "capturesource", request, reply);
}

static int32_t
capsrc_handle_session(void *connection, void *request, void *reply)
{
	(void)connection;
	return capsrc_handle_common(FX_CAPSRC_BIT_SESSION, "capturesession", request, reply);
}

static int32_t
capsrc_handle_deferred(void *connection, void *request, void *reply)
{
	(void)connection;
	return capsrc_handle_common(FX_CAPSRC_BIT_DEFERRED, "deferred-cm", request, reply);
}

static int32_t
capsrc_handle_flashlight(void *connection, void *request, void *reply)
{
	(void)connection;
	return capsrc_handle_common(FX_CAPSRC_BIT_FLASHLIGHT, "flashlight", request, reply);
}

static const fx_fig_xpc_msg_handler_t capsrc_handlers[FX_CAPSRC_SERVICE_COUNT] = {
	capsrc_handle_source,
	capsrc_handle_session,
	capsrc_handle_deferred,
	capsrc_handle_flashlight,
};

static void
capsrc_sleep_ms(unsigned millis)
{
	struct timespec delay;
	delay.tv_sec = (time_t)(millis / 1000u);
	delay.tv_nsec = (long)((millis % 1000u) * 1000000u);
	(void)nanosleep(&delay, NULL);
}

static void *
capsrc_worker(void *context)
{
	(void)context;
	capsrc_sleep_ms(200u);

	bool resolved = false;
	for (unsigned attempt = 0; attempt < 20u; attempt++) {
		if (capsrc_resolve_all()) {
			resolved = true;
			break;
		}
		capsrc_sleep_ms(100u);
	}
	if (!resolved) {
		capsrc_say("fuxi: mediaserverd: capsrc-stub symbols unavailable; skipped\n");
		return NULL;
	}

	for (size_t idx = 0; idx < FX_CAPSRC_SERVICE_COUNT; idx++) {
		if ((capsrc_mask & fuxi_capsrc_services[idx].bit) == 0u) {
			continue;
		}
		fx_fig_xpc_callbacks_t callbacks;
		memset(&callbacks, 0, sizeof(callbacks));
		callbacks.version = 1u;
		callbacks.message_handler = capsrc_handlers[idx];
		int32_t status = fn_fig_xpc_server_start(
		    fuxi_capsrc_services[idx].mach_service, &callbacks, NULL, &capsrc_servers[idx]);
		capsrc_say("fuxi: mediaserverd: capsrc-stub start %s rc=%d\n",
		    fuxi_capsrc_services[idx].short_name, (int)status);
	}
	return NULL;
}

static bool
capsrc_read_chosen_u32(fx_io_object_t chosen, CFStringRef prop_key, uint32_t *raw_out)
{
	CFTypeRef value;
	uint32_t raw = 0u;
	bool found = false;

	if (chosen == MACH_PORT_NULL || prop_key == NULL) {
		return false;
	}
	value = IORegistryEntryCreateCFProperty(chosen, prop_key, kCFAllocatorDefault, 0);
	if (value == NULL) {
		return false;
	}
	if (CFGetTypeID(value) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)value) == (CFIndex)sizeof(raw)) {
		memcpy(&raw, CFDataGetBytePtr((CFDataRef)value), sizeof(raw));
		found = true;
	}
	CFRelease(value);
	if (found && raw_out != NULL) {
		*raw_out = raw;
	}
	return found;
}

__attribute__((constructor)) static void
fuxi_capsrc_init(void)
{
	const char *prog = getprogname();
	fx_io_object_t chosen;
	uint32_t raw = 0u;
	uint32_t requested_mask = 0u;
	uint32_t camera_mode = 0u;
	uint32_t camera_dylib = 0u;
	uint32_t yielded_mask = 0u;
	pthread_attr_t attr;
	pthread_t thread_id;

	if (prog == NULL || strcmp(prog, "mediaserverd") != 0) {
		return;
	}
	chosen = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen");
	if (chosen == MACH_PORT_NULL) {
		return;
	}
	if (!capsrc_read_chosen_u32(chosen, CFSTR("fuxi,capsrc-stub"), &raw) || raw == 0u) {
		(void)IOObjectRelease(chosen);
		return;
	}
	if (!fuxi_capsrc_decode_adt(raw, &requested_mask)) {
		(void)IOObjectRelease(chosen);
		capsrc_say("fuxi: mediaserverd: fuxi,capsrc-stub=0x%x out of range; disabled\n", (unsigned)raw);
		return;
	}
	(void)capsrc_read_chosen_u32(chosen, CFSTR("fuxi,camera"), &camera_mode);
	(void)capsrc_read_chosen_u32(chosen, CFSTR("fuxi,camera-dylib"), &camera_dylib);
	(void)IOObjectRelease(chosen);

	capsrc_mask = fuxi_capsrc_resolve_mask(requested_mask, camera_mode, camera_dylib, &yielded_mask);
	if (yielded_mask != 0u) {
		capsrc_say("fuxi: mediaserverd: capsrc-stub yielding mask=0x%x to libfuxi-camera (camera=%u dylib=%u) remaining=0x%x\n",
		    (unsigned)yielded_mask, (unsigned)camera_mode, (unsigned)camera_dylib, (unsigned)capsrc_mask);
	}
	if (capsrc_mask == 0u) {
		return;
	}
	if (pthread_attr_init(&attr) != 0) {
		return;
	}
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread_id, &attr, capsrc_worker, NULL) != 0) {
		capsrc_say("fuxi: mediaserverd: capsrc-stub worker spawn failed\n");
	}
	(void)pthread_attr_destroy(&attr);
}
