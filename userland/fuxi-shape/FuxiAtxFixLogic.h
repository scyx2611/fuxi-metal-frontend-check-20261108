/*
 * Pure C logic & 20H392 dyld_shared_cache fingerprint checks for FuxiAtxFix.c
 * (host-tested with ASan/UBSan in tools/test-atx-fix.c).
 *
 * Root cause summary (verified on local 20H392 dyld_shared_cache_arm64):
 *   1. Poster extensions (CollectionsPoster, PosterBoard, etc.) have Metal
 *      denied by kernel gate -> ImageIO ATXWritePlugin::writeASTCData emits
 *      linear ASTC 4x4 data with lowercase 'astc' (pre-twiddled marker) and
 *      all-zero 16-byte HEAD watermark because ASTCTwiddler::twiddledData()
 *      returns NULL and skips writing uppercase 'ASTC'.
 *   2. SpringBoard's QuartzCore lock path (textureDataLockData) forwards that
 *      payload via createTwiddledDataFromTwiddled to backboardd/FuxiNativeMetal,
 *      which treats foreign-linear ASTC as row-major linear (home wallpaper OK).
 *   3. SpringBoard's CPU decode path (PaperBoardUI PBUIBackdropCompositor ->
 *      CGContextDrawImage -> ATXReadPlugin::decodeImageData -> decodeASTCtoRGBX)
 *      routes lowercase 'astc' to decodeRGBXFromTwiddled -> twiddledDataSize()==0
 *      -> malloc(0) -> copyFromTextureData no-op -> libate decompresses zero
 *      bytes into ASTC error color magenta RGB=(255,0,255) -> purple tint across
 *      Reachability, InCallService, Safari Tab Overview, and App Switcher.
 *
 * Fix: Intercept ATXReadPlugin::decodeImageData via a single 8-byte COW vtable
 * slot in __DATA_CONST (__ZTV13ATXReadPlugin + 0x68) behind ADT fuxi,atx-fix=1,
 * and route zero-watermark pseudo-twiddled ASTC reads directly to ImageIO's
 * native ASTCTextureImp::decodeRGBXFromLinear under tex+0xa8 mutex protection.
 */
#ifndef FUXI_ATX_FIX_LOGIC_H
#define FUXI_ATX_FIX_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 20H392 dyld_shared_cache_arm64 unslid static addresses */
#define FUXI_ATX_CGIS_CREATE_WITH_DATA     0x18bab51f4ULL
#define FUXI_ATX_CGIS_GET_TYPE             0x18bae5ee4ULL

#define FUXI_ATX_READ_PLUGIN_VTABLE        0x1d5f459f0ULL
#define FUXI_ATX_RP_SLOT_INIT              0x1d5f45a40ULL /* +0x50: ATXReadPlugin::initialize */
#define FUXI_ATX_RP_SLOT_SAVE_XPC          0x1d5f45a50ULL /* +0x60: ATXReadPlugin::saveDataToXPCObject */
#define FUXI_ATX_RP_SLOT_DECODE_DATA       0x1d5f45a58ULL /* +0x68: ATXReadPlugin::decodeImageData */
#define FUXI_ATX_RP_SLOT_ADD_TYPE_ID       0x1d5f45a60ULL /* +0x70: IIOReadPlugin::addImageTypeIdentifier */

#define FUXI_ATX_FN_RP_INIT                0x18bafe390ULL
#define FUXI_ATX_FN_RP_SAVE_XPC            0x18bc0e4c8ULL
#define FUXI_ATX_FN_RP_DECODE_DATA         0x18bc0e59cULL
#define FUXI_ATX_FN_RP_ADD_TYPE_ID         0x18bafa058ULL

#define FUXI_ATX_ASTC_TEX_VTABLE           0x1d5f46ec0ULL
#define FUXI_ATX_ASTC_TEX_VPTR             0x1d5f46ed0ULL /* __ZTV14ASTCTextureImp + 0x10 */
#define FUXI_ATX_TEX_SLOT_LOCK_DATA        0x1d5f46ef8ULL /* +0x38: ASTCTextureImp::textureDataLockData */
#define FUXI_ATX_TEX_SLOT_UNLOCK_DATA      0x1d5f46f00ULL /* +0x40: ASTCTextureImp::textureDataUnlockData */

#define FUXI_ATX_FN_TEX_LOCK_DATA          0x18bab6cc8ULL
#define FUXI_ATX_FN_TEX_UNLOCK_DATA        0x18bab7abcULL
#define FUXI_ATX_FN_DECODE_LINEAR          0x18bc2cc48ULL

/* First two instruction words of target ImageIO routines on 20H392 */
#define FUXI_ATX_DECODE_DATA_INSN0         0xf9411808u /* ldr x8, [x0, #0x230] */
#define FUXI_ATX_DECODE_DATA_INSN1         0xb5000068u /* cbnz x8, +0xc */
#define FUXI_ATX_DECODE_LINEAR_INSN0       0xd10343ffu /* sub sp, sp, #0xd0 */
#define FUXI_ATX_DECODE_LINEAR_INSN1       0xa9076ffcu /* stp x28, x27, [sp, #0x70] */

/* Field offsets inside ATXReadPlugin & ASTCTextureImp (20H392) */
#define FUXI_ATX_RP_OFF_SESSION            0x018u
#define FUXI_ATX_RP_OFF_WIDTH              0x0fcu
#define FUXI_ATX_RP_OFF_HEIGHT             0x100u
#define FUXI_ATX_RP_OFF_ROWBYTES           0x124u
#define FUXI_ATX_RP_OFF_ALPHA_INFO         0x12eu
#define FUXI_ATX_RP_OFF_SRGB_FLAG          0x184u
#define FUXI_ATX_RP_OFF_TEX                0x1c0u
#define FUXI_ATX_RP_OFF_HEADER             0x1d0u
#define FUXI_ATX_RP_OFF_WATERMARK          0x20cu
#define FUXI_ATX_RP_OFF_ASTC_CHUNK         0x228u
#define FUXI_ATX_RP_OFF_420F_A             0x230u
#define FUXI_ATX_RP_OFF_420F_B             0x248u

#define FUXI_ATX_TEX_OFF_VPTR              0x000u
#define FUXI_ATX_TEX_OFF_BLOCK_W           0x01cu
#define FUXI_ATX_TEX_OFF_BLOCK_H           0x020u
#define FUXI_ATX_TEX_OFF_LZFSE             0x028u
#define FUXI_ATX_TEX_OFF_PRETWIDDLED       0x029u
#define FUXI_ATX_TEX_OFF_HDR               0x02au
#define FUXI_ATX_TEX_OFF_MUTEX             0x0a8u

#define FUXI_ATX_HEAD_SIZE                 84u
#define FUXI_ATX_WATERMARK_OFF             60u
#define FUXI_ATX_WATERMARK_SIZE            16u
#define FUXI_ATX_DECODE_FAIL               (-4)

struct fuxi_atx_decode_params {
	uint32_t alpha;
	uint32_t block_format;
	uint32_t texel_format;
};

struct fuxi_atx_layout_snapshot {
	uint64_t slide_create;
	uint64_t slide_get_type;
	uint64_t replacement_fn;
	uint64_t rp_slot_init;
	uint64_t rp_slot_save_xpc;
	uint64_t rp_slot_decode_data;
	uint64_t rp_slot_add_type_id;
	uint64_t tex_slot_lock;
	uint64_t tex_slot_unlock;
	uint32_t decode_data_insn0;
	uint32_t decode_data_insn1;
	uint32_t decode_linear_insn0;
	uint32_t decode_linear_insn1;
};

enum fuxi_atx_patch_result {
	FUXI_ATX_OK = 0,
	FUXI_ATX_ALREADY_PATCHED,
	FUXI_ATX_BAD_SLIDE,
	FUXI_ATX_SLIDE_MISMATCH,
	FUXI_ATX_BAD_RP_VTABLE,
	FUXI_ATX_BAD_TEX_VTABLE,
	FUXI_ATX_BAD_INSN,
};

static inline const char *
fuxi_atx_patch_result_name(enum fuxi_atx_patch_result res)
{
	switch (res) {
	case FUXI_ATX_OK:
		return "ok";
	case FUXI_ATX_ALREADY_PATCHED:
		return "already_patched";
	case FUXI_ATX_BAD_SLIDE:
		return "bad_slide";
	case FUXI_ATX_SLIDE_MISMATCH:
		return "slide_mismatch";
	case FUXI_ATX_BAD_RP_VTABLE:
		return "bad_rp_vtable";
	case FUXI_ATX_BAD_TEX_VTABLE:
		return "bad_tex_vtable";
	case FUXI_ATX_BAD_INSN:
		return "bad_insn";
	default:
		return "unknown";
	}
}

/*
 * Verify 84-byte ATX HEAD payload has version==1 and an all-zero 16-byte
 * twiddle watermark at bytes 60..75 (ATXHeader offset 0x3c = +0x20c - +0x1d0).
 */
static inline bool
fuxi_atx_header_is_zero_watermark(const uint8_t *hdr84)
{
	uint32_t version = 0;

	if (hdr84 == NULL) {
		return false;
	}
	memcpy(&version, hdr84, sizeof(version));
	if (version != 1u) {
		return false;
	}
	for (size_t i = 0; i < FUXI_ATX_WATERMARK_SIZE; i++) {
		if (hdr84[FUXI_ATX_WATERMARK_OFF + i] != 0u) {
			return false;
		}
	}
	return true;
}

/*
 * Exact mirror of ImageIO ASTCTextureImp::BlockFormatForASTCBlockSize (0x18bb1fdfc).
 * Returns 0 for unsupported block dimensions.
 */
static inline uint32_t
fuxi_atx_block_format(uint8_t bw, uint8_t bh)
{
	const unsigned code = ((unsigned)bw << 4) | (unsigned)bh;

	if (bw > 15u || bh > 15u) {
		return 0u;
	}
	switch (code) {
	case 0x44u: return 1u;
	case 0x54u: return 2u;
	case 0x55u: return 3u;
	case 0x65u: return 4u;
	case 0x66u: return 5u;
	case 0x85u: return 6u;
	case 0x86u: return 7u;
	case 0x88u: return 8u;
	case 0xa5u: return 9u;
	case 0xa6u: return 10u;
	case 0xa8u: return 11u;
	case 0xaau: return 12u;
	case 0xcau: return 13u;
	case 0xccu: return 14u;
	default:    return 0u;
	}
}

/*
 * Derive at_alpha_t, at_block_format_t, and at_texel_format_t identically to
 * ASTCTextureImp::decodeASTCtoRGBX (0x18bafcf70) + HDRBlockFormat (0x18bc2c9a8).
 */
static inline bool
fuxi_atx_resolve_decode_params(uint8_t bw, uint8_t bh, uint8_t alpha_info, uint8_t srgb_flag, uint8_t is_hdr,
    struct fuxi_atx_decode_params *out)
{
	static const uint32_t alpha_lut[3] = { 1u, 2u, 2u };
	uint32_t blk;
	uint32_t texel;

	if (out == NULL) {
		return false;
	}
	blk = fuxi_atx_block_format(bw, bh);
	if (blk == 0u) {
		return false;
	}
	texel = (srgb_flag != 0u) ? 6u : 5u;
	if (is_hdr != 0u) {
		/* ASTCTextureImp::HDRBlockFormat shifts 1..14 -> +0x10 */
		blk += 0x10u;
		texel = 10u;
	}
	out->alpha = (alpha_info <= 2u) ? alpha_lut[alpha_info] : 0u;
	out->block_format = blk;
	out->texel_format = texel;
	return true;
}

/* Bounds check for caller-supplied RGBA output buffer backing vImage_Buffer. */
static inline bool
fuxi_atx_check_vimage_bounds(uint32_t width, uint32_t height, uint32_t row_bytes, size_t buf_len)
{
	if (width == 0u || height == 0u) {
		return false;
	}
	if ((uint64_t)width > UINT32_MAX / 4u) {
		return false;
	}
	if (row_bytes < width * 4u) {
		return false;
	}
	if ((size_t)height > buf_len / (size_t)row_bytes) {
		return false;
	}
	return true;
}

/*
 * Safely read 4-byte pixel at (x, y) if inside validated vImage buffer.
 */
static inline bool
fuxi_atx_sample_pixel(const uint8_t *buf, uint32_t width, uint32_t height, uint32_t row_bytes, uint32_t x, uint32_t y,
    uint8_t rgba[4])
{
	size_t off;

	if (buf == NULL || rgba == NULL || x >= width || y >= height) {
		return false;
	}
	if (row_bytes < width * 4u) {
		return false;
	}
	off = (size_t)y * (size_t)row_bytes + (size_t)x * 4u;
	memcpy(rgba, buf + off, 4u);
	return true;
}

/* Validate 20H392 ImageIO shared-cache slide, vtable slots, and entry insns. */
static inline enum fuxi_atx_patch_result
fuxi_atx_validate_layout(const struct fuxi_atx_layout_snapshot *snap)
{
	uint64_t s;

	if (snap == NULL || snap->replacement_fn == 0u) {
		return FUXI_ATX_BAD_SLIDE;
	}
	if (snap->slide_create != snap->slide_get_type) {
		return FUXI_ATX_SLIDE_MISMATCH;
	}
	s = snap->slide_create;
	if ((s & 0x3fffULL) != 0u) {
		return FUXI_ATX_BAD_SLIDE;
	}
	if (snap->rp_slot_init != FUXI_ATX_FN_RP_INIT + s ||
	    snap->rp_slot_save_xpc != FUXI_ATX_FN_RP_SAVE_XPC + s ||
	    snap->rp_slot_add_type_id != FUXI_ATX_FN_RP_ADD_TYPE_ID + s) {
		return FUXI_ATX_BAD_RP_VTABLE;
	}
	if (snap->tex_slot_lock != FUXI_ATX_FN_TEX_LOCK_DATA + s ||
	    snap->tex_slot_unlock != FUXI_ATX_FN_TEX_UNLOCK_DATA + s) {
		return FUXI_ATX_BAD_TEX_VTABLE;
	}
	if (snap->decode_data_insn0 != FUXI_ATX_DECODE_DATA_INSN0 ||
	    snap->decode_data_insn1 != FUXI_ATX_DECODE_DATA_INSN1 ||
	    snap->decode_linear_insn0 != FUXI_ATX_DECODE_LINEAR_INSN0 ||
	    snap->decode_linear_insn1 != FUXI_ATX_DECODE_LINEAR_INSN1) {
		return FUXI_ATX_BAD_INSN;
	}
	if (snap->rp_slot_decode_data == snap->replacement_fn) {
		return FUXI_ATX_ALREADY_PATCHED;
	}
	if (snap->rp_slot_decode_data != FUXI_ATX_FN_RP_DECODE_DATA + s) {
		return FUXI_ATX_BAD_RP_VTABLE;
	}
	return FUXI_ATX_OK;
}

#endif
