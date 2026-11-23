/*
 * Host ASan/UBSan test for FuxiAtxFixLogic.h (U-026 ATX linear decode fallback).
 *
 * Includes exact 84-byte HEAD payloads extracted from:
 *   1. RuntimeSnapshot-home.atx (pseudo-twiddled lowercase 'astc', zero watermark)
 *   2. AvatarKit alien-camera.atx (genuine pre-twiddled ATX, 0x55 watermark)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FuxiAtxFixLogic.h"

static int failures;

#define EXPECT(cond)                                                                                                 \
	do {                                                                                                         \
		if (!(cond)) {                                                                                       \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                              \
			failures++;                                                                                  \
		}                                                                                                    \
	} while (0)

static void
make_valid_layout(struct fuxi_atx_layout_snapshot *snap, uint64_t slide)
{
	snap->slide_create = slide;
	snap->slide_get_type = slide;
	snap->replacement_fn = 0x104012340ULL;
	snap->rp_slot_init = FUXI_ATX_FN_RP_INIT + slide;
	snap->rp_slot_save_xpc = FUXI_ATX_FN_RP_SAVE_XPC + slide;
	snap->rp_slot_decode_data = FUXI_ATX_FN_RP_DECODE_DATA + slide;
	snap->rp_slot_add_type_id = FUXI_ATX_FN_RP_ADD_TYPE_ID + slide;
	snap->tex_slot_lock = FUXI_ATX_FN_TEX_LOCK_DATA + slide;
	snap->tex_slot_unlock = FUXI_ATX_FN_TEX_UNLOCK_DATA + slide;
	snap->decode_data_insn0 = FUXI_ATX_DECODE_DATA_INSN0;
	snap->decode_data_insn1 = FUXI_ATX_DECODE_DATA_INSN1;
	snap->decode_linear_insn0 = FUXI_ATX_DECODE_LINEAR_INSN0;
	snap->decode_linear_insn1 = FUXI_ATX_DECODE_LINEAR_INSN1;
}

static void
test_watermark_classification(void)
{
	/* Real RuntimeSnapshot-home.atx HEAD (1125x2436, 4x4 ASTC, zero watermark) */
	static const uint8_t runtime_home_head[FUXI_ATX_HEAD_SIZE] = {
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xb0, 0x93, 0x00, 0x00, 0x08, 0x19, 0x00, 0x00,
		0x65, 0x04, 0x00, 0x00, 0x84, 0x09, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	};
	/* Real AvatarKit alien-camera.atx HEAD (428x428, 4x4 ASTC, 0x55 watermark) */
	static const uint8_t avatarkit_head[FUXI_ATX_HEAD_SIZE] = {
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xb0, 0x93, 0x00, 0x00, 0x08, 0x19, 0x00, 0x00,
		0xac, 0x01, 0x00, 0x00, 0xac, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
		0x55, 0x55, 0x55, 0x55, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	};
	uint8_t modified[FUXI_ATX_HEAD_SIZE];

	EXPECT(fuxi_atx_header_is_zero_watermark(runtime_home_head));
	EXPECT(!fuxi_atx_header_is_zero_watermark(avatarkit_head));
	EXPECT(!fuxi_atx_header_is_zero_watermark(NULL));

	/* Bad version -> reject */
	memcpy(modified, runtime_home_head, sizeof(modified));
	modified[0] = 2u;
	EXPECT(!fuxi_atx_header_is_zero_watermark(modified));

	/* Any single non-zero watermark byte -> reject */
	for (size_t i = 0; i < FUXI_ATX_WATERMARK_SIZE; i++) {
		memcpy(modified, runtime_home_head, sizeof(modified));
		modified[FUXI_ATX_WATERMARK_OFF + i] = 0x01u;
		EXPECT(!fuxi_atx_header_is_zero_watermark(modified));
	}
}

static void
test_decode_params(void)
{
	struct fuxi_atx_decode_params p;

	EXPECT(fuxi_atx_resolve_decode_params(4u, 4u, 1u, 1u, 0u, &p));
	EXPECT(p.alpha == 2u && p.block_format == 1u && p.texel_format == 6u);

	EXPECT(fuxi_atx_resolve_decode_params(4u, 4u, 0u, 0u, 0u, &p));
	EXPECT(p.alpha == 1u && p.block_format == 1u && p.texel_format == 5u);

	EXPECT(fuxi_atx_resolve_decode_params(8u, 8u, 2u, 1u, 0u, &p));
	EXPECT(p.alpha == 2u && p.block_format == 8u && p.texel_format == 6u);

	EXPECT(fuxi_atx_resolve_decode_params(12u, 12u, 5u, 0u, 1u, &p));
	EXPECT(p.alpha == 0u && p.block_format == 0x1eu && p.texel_format == 10u);

	EXPECT(!fuxi_atx_resolve_decode_params(0u, 0u, 1u, 1u, 0u, &p));
	EXPECT(!fuxi_atx_resolve_decode_params(4u, 5u, 1u, 1u, 0u, &p));
	EXPECT(!fuxi_atx_resolve_decode_params(4u, 4u, 1u, 1u, 0u, NULL));
}

static void
test_bounds_and_sampling(void)
{
	uint8_t img[16 * 8]; /* max rowBytes 16, height 8 */
	uint8_t px[4];

	EXPECT(fuxi_atx_check_vimage_bounds(1125u, 2436u, 4500u, (size_t)1125u * 4u * 2436u));
	EXPECT(fuxi_atx_check_vimage_bounds(1125u, 2500u, 4544u, (size_t)4544u * 2500u));
	EXPECT(!fuxi_atx_check_vimage_bounds(0u, 100u, 400u, 40000u));
	EXPECT(!fuxi_atx_check_vimage_bounds(100u, 0u, 400u, 40000u));
	EXPECT(!fuxi_atx_check_vimage_bounds(100u, 10u, 396u, 4000u));
	EXPECT(!fuxi_atx_check_vimage_bounds(100u, 10u, 400u, 3999u));
	EXPECT(!fuxi_atx_check_vimage_bounds(0x40000000u, 1u, 4u, 64u));

	memset(img, 0, sizeof(img));
	img[2u * 16u + 3u * 4u + 0u] = 15u;
	img[2u * 16u + 3u * 4u + 1u] = 138u;
	img[2u * 16u + 3u * 4u + 2u] = 203u;
	img[2u * 16u + 3u * 4u + 3u] = 255u;

	EXPECT(fuxi_atx_sample_pixel(img, 4u, 8u, 16u, 3u, 2u, px));
	EXPECT(px[0] == 15u && px[1] == 138u && px[2] == 203u && px[3] == 255u);
	EXPECT(!fuxi_atx_sample_pixel(img, 4u, 8u, 16u, 4u, 2u, px));
	EXPECT(!fuxi_atx_sample_pixel(img, 4u, 8u, 16u, 0u, 8u, px));
	EXPECT(!fuxi_atx_sample_pixel(NULL, 4u, 8u, 16u, 0u, 0u, px));
}

static void
test_layout_validation(void)
{
	struct fuxi_atx_layout_snapshot s;
	const uint64_t good_slide = 0x1234000ULL;

	make_valid_layout(&s, good_slide);
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_OK);

	s.rp_slot_decode_data = s.replacement_fn;
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_ALREADY_PATCHED);

	make_valid_layout(&s, good_slide);
	s.slide_get_type += 0x4000ULL;
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_SLIDE_MISMATCH);

	make_valid_layout(&s, good_slide + 0x1000ULL);
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_BAD_SLIDE);

	make_valid_layout(&s, good_slide);
	s.rp_slot_init ^= 8ULL;
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_BAD_RP_VTABLE);

	make_valid_layout(&s, good_slide);
	s.rp_slot_decode_data ^= 4ULL;
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_BAD_RP_VTABLE);

	make_valid_layout(&s, good_slide);
	s.tex_slot_unlock = 0ULL;
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_BAD_TEX_VTABLE);

	make_valid_layout(&s, good_slide);
	s.decode_linear_insn1 ^= 1u;
	EXPECT(fuxi_atx_validate_layout(&s) == FUXI_ATX_BAD_INSN);

	EXPECT(strcmp(fuxi_atx_patch_result_name(FUXI_ATX_OK), "ok") == 0);
	EXPECT(strcmp(fuxi_atx_patch_result_name(FUXI_ATX_BAD_INSN), "bad_insn") == 0);
}

int
main(void)
{
	test_watermark_classification();
	test_decode_params();
	test_bounds_and_sampling();
	test_layout_validation();
	if (failures != 0) {
		fprintf(stderr, "ATX_FIX_TESTS FAILED (%d)\n", failures);
		return 1;
	}
	printf("ATX_FIX_TESTS OK\n");
	return 0;
}
