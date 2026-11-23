/*
 * Pure decision and dispatch helpers for FuxiCaptureStub (host-testable under ASan/UBSan).
 *
 * Reference: iOS 16.7 (20H392) dyld_shared_cache disassembly of
 *   CMCapture._FigCaptureSourceServerStart (0x1a21b7138)
 *   CMCapture._FigCaptureSourceRemoteCopyCaptureSources (0x1a1f805f8)
 *   CMCapture._FigCaptureSourceRemoteMaxStillImageJPEGDataSize (0x1a2030544)
 *   CoreMedia._FigXPCServerStart (0x1916ab6a0)
 *   CoreMedia._FigXPCMessageGetOpCode (0x1916a83c0)
 * and sweet-root-tweaks.py (StartRecorderServer=False in SubServerConfiguration.plist).
 */
#ifndef FUXI_CAPTURE_STUB_LOGIC_H
#define FUXI_CAPTURE_STUB_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FX_CAPSRC_BIT_SOURCE     (1u << 0)
#define FX_CAPSRC_BIT_SESSION    (1u << 1)
#define FX_CAPSRC_BIT_DEFERRED   (1u << 2)
#define FX_CAPSRC_BIT_FLASHLIGHT (1u << 3)
#define FX_CAPSRC_MASK_DEFAULT   (FX_CAPSRC_BIT_SOURCE | FX_CAPSRC_BIT_SESSION | FX_CAPSRC_BIT_DEFERRED)
#define FX_CAPSRC_MASK_ALL       (FX_CAPSRC_BIT_SOURCE | FX_CAPSRC_BIT_SESSION | FX_CAPSRC_BIT_DEFERRED | FX_CAPSRC_BIT_FLASHLIGHT)
/*
 * Services started natively by /Developer/libfuxi-camera.dylib
 * (cam-1122/userland/fuxi-camera/inject_media.c:fuxi_start_capture_servers,
 * matching CMCaptureCore._FigStartCaptureServers minus FigPreloadShaders):
 *   - FigCaptureSourceServerStart                   -> com.apple.coremedia.capturesource
 *   - FigCaptureSessionServerStart                  -> com.apple.coremedia.capturesession
 *   - FigCaptureDeferredContainerManagerServerStart -> com.apple.coremedia.capture.deferred.containermanager.xpc
 *   - FigFlashlightServerStart                      -> com.apple.coremedia.flashlight
 */
#define FX_CAPSRC_MASK_CAMERA_OWNED \
	(FX_CAPSRC_BIT_SOURCE | FX_CAPSRC_BIT_SESSION | FX_CAPSRC_BIT_DEFERRED | FX_CAPSRC_BIT_FLASHLIGHT)

#define FX_CAPSRC_OP_CSRC        0x63737263u /* 'csrc' */
#define FX_CAPSRC_OP_MJDS        0x6d6a6473u /* 'mjds' */

#define FX_CAPSRC_ERR_UNSUPPORTED (-12782)   /* -0x31ee: kFigCaptureError_Unsupported */
#define FX_CAPSRC_ERR_ALLOC       (-12786)   /* -0x31f2: allocation failure */

#define FX_CAPSRC_KEY_PAYLOAD    ".payload"
#define FX_CAPSRC_KEY_VALUE      ".Value"

typedef struct {
	uint32_t bit;
	const char *mach_service;
	const char *short_name;
} fx_capsrc_service_desc;

static const fx_capsrc_service_desc fuxi_capsrc_services[] = {
	{FX_CAPSRC_BIT_SOURCE, "com.apple.coremedia.capturesource", "capturesource"},
	{FX_CAPSRC_BIT_SESSION, "com.apple.coremedia.capturesession", "capturesession"},
	{FX_CAPSRC_BIT_DEFERRED, "com.apple.coremedia.capture.deferred.containermanager.xpc", "deferred-cm"},
	{FX_CAPSRC_BIT_FLASHLIGHT, "com.apple.coremedia.flashlight", "flashlight"},
};

#define FX_CAPSRC_SERVICE_COUNT (sizeof(fuxi_capsrc_services) / sizeof(fuxi_capsrc_services[0]))

typedef enum {
	FX_CAPSRC_ACT_UNSUPPORTED = 0,
	FX_CAPSRC_ACT_EMPTY_ARRAY = 1,
	FX_CAPSRC_ACT_ZERO_U64 = 2,
} fx_capsrc_action;

static inline bool
fuxi_capsrc_decode_adt(uint32_t raw, uint32_t *mask_out)
{
	if (mask_out == NULL) {
		return false;
	}
	*mask_out = 0u;
	if (raw == 0u || (raw & ~FX_CAPSRC_MASK_ALL) != 0u) {
		return false;
	}
	*mask_out = raw;
	return true;
}

static inline bool
fuxi_capsrc_camera_owns_servers(uint32_t camera_mode, uint32_t camera_dylib)
{
	return camera_mode != 0u && camera_dylib != 0u;
}

static inline uint32_t
fuxi_capsrc_resolve_mask(uint32_t requested_mask, uint32_t camera_mode, uint32_t camera_dylib,
    uint32_t *yielded_mask_out)
{
	uint32_t effective = requested_mask & FX_CAPSRC_MASK_ALL;
	uint32_t yielded = 0u;

	if (fuxi_capsrc_camera_owns_servers(camera_mode, camera_dylib)) {
		yielded = effective & FX_CAPSRC_MASK_CAMERA_OWNED;
		effective &= ~FX_CAPSRC_MASK_CAMERA_OWNED;
	}
	if (yielded_mask_out != NULL) {
		*yielded_mask_out = yielded;
	}
	return effective;
}

static inline fx_capsrc_action
fuxi_capsrc_classify_op(uint32_t service_bit, uint32_t opcode)
{
	if (service_bit == FX_CAPSRC_BIT_SOURCE) {
		if (opcode == FX_CAPSRC_OP_CSRC) {
			return FX_CAPSRC_ACT_EMPTY_ARRAY;
		}
		if (opcode == FX_CAPSRC_OP_MJDS) {
			return FX_CAPSRC_ACT_ZERO_U64;
		}
	}
	return FX_CAPSRC_ACT_UNSUPPORTED;
}

static inline void
fuxi_capsrc_format_fourcc(uint32_t opcode, char out[5])
{
	if (out == NULL) {
		return;
	}
	for (size_t idx = 0; idx < 4; idx++) {
		unsigned byte_val = (opcode >> (24u - idx * 8u)) & 0xffu;
		out[idx] = (byte_val >= 0x20u && byte_val <= 0x7eu) ? (char)byte_val : '?';
	}
	out[4] = '\0';
}

#endif
