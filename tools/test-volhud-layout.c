#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "FuxiVolHudLayout.h"

static bool
near(double a, double b)
{
	return fabs(a - b) < 1e-6;
}

int
main(void)
{
	fx_volhud_config cfg;

	/* Default off and out-of-range values */
	assert(!fuxi_volhud_decode_adt(0u, &cfg) && !cfg.enabled && !cfg.override_y);
	assert(!fuxi_volhud_decode_adt(2u, &cfg) && !cfg.enabled);
	assert(!fuxi_volhud_decode_adt(99u, &cfg) && !cfg.enabled);
	assert(!fuxi_volhud_decode_adt(701u, &cfg) && !cfg.enabled);
	assert(!fuxi_volhud_decode_adt(0xffffffffu, &cfg) && !cfg.enabled);
	assert(!fuxi_volhud_decode_adt(1u, NULL));

	/* Right edge only (keep stock volumeButtonsCenterY) */
	assert(fuxi_volhud_decode_adt(1u, &cfg) && cfg.enabled && !cfg.override_y && near(cfg.center_y, 0.0));

	/* Right edge + explicit centerY override */
	assert(fuxi_volhud_decode_adt(100u, &cfg) && cfg.enabled && cfg.override_y && !cfg.override_height && near(cfg.center_y, 100.0));
	assert(fuxi_volhud_decode_adt(255u, &cfg) && cfg.enabled && cfg.override_y && !cfg.override_height && near(cfg.center_y, 255.0));
	assert(fuxi_volhud_decode_adt(700u, &cfg) && cfg.enabled && cfg.override_y && !cfg.override_height && near(cfg.center_y, 700.0));
	assert(near(fuxi_volhud_scale_height(148.0, &cfg), 148.0));

	/* Packed Y*1000+H (Xiaomi 13 rocker alignment: Y=238 pt, State1 H=128 pt) */
	assert(fuxi_volhud_decode_adt(238128u, &cfg) && cfg.enabled && cfg.override_y && cfg.override_height &&
	    near(cfg.center_y, 238.0) && near(cfg.state1_height, 128.0));
	assert(near(fuxi_volhud_scale_height(148.0, &cfg), 128.0));
	assert(near(fuxi_volhud_scale_height(134.0, &cfg), 134.0 * 128.0 / 148.0));
	assert(near(fuxi_volhud_scale_height(170.0, &cfg), 170.0 * 128.0 / 148.0));
	assert(near(fuxi_volhud_scale_height(0.0, &cfg), 0.0));
	assert(fuxi_volhud_decode_adt(100080u, &cfg) && near(cfg.center_y, 100.0) && near(cfg.state1_height, 80.0));
	assert(fuxi_volhud_decode_adt(700300u, &cfg) && near(cfg.center_y, 700.0) && near(cfg.state1_height, 300.0));
	assert(!fuxi_volhud_decode_adt(99128u, &cfg) && !cfg.enabled);
	assert(!fuxi_volhud_decode_adt(701128u, &cfg) && !cfg.enabled);
	assert(!fuxi_volhud_decode_adt(238079u, &cfg) && !cfg.enabled);
	assert(!fuxi_volhud_decode_adt(238301u, &cfg) && !cfg.enabled);

	/* Portrait mirroring across 375 x 833.33 pt canvas (states 0..3) */
	fx_volhud_rect portrait = {0.0, 0.0, 375.0, 833.33};
	fx_volhud_point offscreen = fuxi_volhud_mirror_center((fx_volhud_point){-16.0, 255.0}, FX_VOLHUD_AXIS_VERTICAL, portrait);
	assert(near(offscreen.x, 391.0) && near(offscreen.y, 255.0));

	fx_volhud_point collapsed = fuxi_volhud_mirror_center((fx_volhud_point){15.5, 255.0}, FX_VOLHUD_AXIS_VERTICAL, portrait);
	assert(near(collapsed.x, 359.5) && near(collapsed.y, 255.0));

	fx_volhud_point expanded = fuxi_volhud_mirror_center((fx_volhud_point){36.0, 255.0}, FX_VOLHUD_AXIS_VERTICAL, portrait);
	assert(near(expanded.x, 339.0) && near(expanded.y, 255.0));

	/* Non-zero bounds origin mirrors around bounds midpoint */
	fx_volhud_rect shifted = {10.0, 0.0, 375.0, 812.0};
	fx_volhud_point mirrored_shifted = fuxi_volhud_mirror_center((fx_volhud_point){25.0, 200.0}, FX_VOLHUD_AXIS_VERTICAL, shifted);
	assert(near(mirrored_shifted.x, 370.0) && near(mirrored_shifted.y, 200.0));

	/* Landscape horizontal HUD (axis == 1) and invalid bounds must stay untouched */
	fx_volhud_rect landscape = {0.0, 0.0, 833.33, 375.0};
	fx_volhud_point horiz = fuxi_volhud_mirror_center((fx_volhud_point){416.665, 24.0}, FX_VOLHUD_AXIS_HORIZONTAL, landscape);
	assert(near(horiz.x, 416.665) && near(horiz.y, 24.0));

	fx_volhud_rect empty = {0.0, 0.0, 0.0, 812.0};
	fx_volhud_point noop = fuxi_volhud_mirror_center((fx_volhud_point){15.5, 255.0}, FX_VOLHUD_AXIS_VERTICAL, empty);
	assert(near(noop.x, 15.5) && near(noop.y, 255.0));

	/* Label clamping on right edge (margin 6.0 pt) */
	assert(near(fuxi_volhud_clamp_label_x(340.0, 80.0, 0.0, 375.0, FX_VOLHUD_LABEL_MARGIN), 289.0));
	assert(near(fuxi_volhud_clamp_label_x(260.0, 80.0, 0.0, 375.0, FX_VOLHUD_LABEL_MARGIN), 260.0));
	assert(near(fuxi_volhud_clamp_label_x(2.0, 80.0, 0.0, 375.0, FX_VOLHUD_LABEL_MARGIN), 6.0));
	/* Label wider than container stays pinned at left margin */
	assert(near(fuxi_volhud_clamp_label_x(100.0, 400.0, 0.0, 375.0, FX_VOLHUD_LABEL_MARGIN), 6.0));
	/* Invalid widths leave x unchanged */
	assert(near(fuxi_volhud_clamp_label_x(340.0, 0.0, 0.0, 375.0, FX_VOLHUD_LABEL_MARGIN), 340.0));
	assert(near(fuxi_volhud_clamp_label_x(340.0, 80.0, 0.0, 0.0, FX_VOLHUD_LABEL_MARGIN), 340.0));

	puts("PASS: FuxiVolHudLayout ADT decode, portrait right-edge mirror, landscape noop, and label clamp");
	return 0;
}
