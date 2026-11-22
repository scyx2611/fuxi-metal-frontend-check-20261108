/*
 * Pure-C geometry and ADT decoding for Xiaomi 13 right-edge Volume HUD adaptation.
 *
 * Reference: SpringBoard 20H392 disassembly of
 *   -[SBElasticValueViewController centerForState:containerViewSize:bounds:useSpringData:],
 *   -[SBElasticValueViewController _updateLabelsForAxis:containerViewSize:state:animated:],
 *   -[SBElasticHUDSettings volumeButtonsCenterY].
 *
 * ADT /chosen u32 "fuxi,volhud" (default off when absent or 0):
 *   0        : disabled (stock iPhone X left-edge Volume HUD)
 *   1        : right edge in portrait (axis == 2), keep stock volumeButtonsCenterY
 *   100..700 : right edge in portrait (axis == 2), override volumeButtonsCenterY to N pt
 *   Y*1000+H : Y in 100..700, H in 80..300 — right edge, override volumeButtonsCenterY to Y pt
 *              and scale portraitState{1,2,3}Height proportionally so State1 equals H pt (stock 148 pt)
 *   other    : ignored (treated as disabled)
 */
#ifndef FUXI_VOLHUD_LAYOUT_H
#define FUXI_VOLHUD_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

enum {
	FX_VOLHUD_AXIS_HORIZONTAL = 1,
	FX_VOLHUD_AXIS_VERTICAL = 2,
	FX_VOLHUD_Y_MIN = 100u,
	FX_VOLHUD_Y_MAX = 700u,
	FX_VOLHUD_H_MIN = 80u,
	FX_VOLHUD_H_MAX = 300u,
	FX_VOLHUD_PACK_BASE = 1000u,
};

#define FX_VOLHUD_LABEL_MARGIN 6.0
#define FX_VOLHUD_STOCK_STATE1_H 148.0

typedef struct {
	double x, y;
} fx_volhud_point;

typedef struct {
	double w, h;
} fx_volhud_size;

typedef struct {
	double x, y, w, h;
} fx_volhud_rect;

typedef struct {
	bool enabled;
	bool override_y;
	bool override_height;
	double center_y;
	double state1_height;
} fx_volhud_config;

static inline bool
fuxi_volhud_decode_adt(uint32_t raw, fx_volhud_config *out)
{
	if (out == NULL) {
		return false;
	}
	out->enabled = false;
	out->override_y = false;
	out->override_height = false;
	out->center_y = 0.0;
	out->state1_height = 0.0;
	if (raw == 1u) {
		out->enabled = true;
		return true;
	}
	if (raw >= FX_VOLHUD_Y_MIN && raw <= FX_VOLHUD_Y_MAX) {
		out->enabled = true;
		out->override_y = true;
		out->center_y = (double)raw;
		return true;
	}
	if (raw >= FX_VOLHUD_Y_MIN * FX_VOLHUD_PACK_BASE) {
		uint32_t center_y = raw / FX_VOLHUD_PACK_BASE;
		uint32_t height = raw % FX_VOLHUD_PACK_BASE;
		if (center_y >= FX_VOLHUD_Y_MIN && center_y <= FX_VOLHUD_Y_MAX &&
		    height >= FX_VOLHUD_H_MIN && height <= FX_VOLHUD_H_MAX) {
			out->enabled = true;
			out->override_y = true;
			out->override_height = true;
			out->center_y = (double)center_y;
			out->state1_height = (double)height;
			return true;
		}
	}
	return false;
}

static inline double
fuxi_volhud_scale_height(double stock_height, const fx_volhud_config *cfg)
{
	if (cfg == NULL || !cfg->override_height || stock_height <= 0.0) {
		return stock_height;
	}
	return stock_height * (cfg->state1_height / FX_VOLHUD_STOCK_STATE1_H);
}

static inline fx_volhud_point
fuxi_volhud_mirror_center(fx_volhud_point center, long axis, fx_volhud_rect bounds)
{
	if (axis == FX_VOLHUD_AXIS_VERTICAL && bounds.w > 0.0) {
		center.x = 2.0 * bounds.x + bounds.w - center.x;
	}
	return center;
}

static inline double
fuxi_volhud_clamp_label_x(double x, double width, double bounds_x, double bounds_w, double margin)
{
	if (bounds_w <= 0.0 || width <= 0.0) {
		return x;
	}
	double max_x = bounds_x + bounds_w - margin - width;
	double min_x = bounds_x + margin;
	if (x > max_x) {
		x = max_x;
	}
	if (x < min_x) {
		x = min_x;
	}
	return x;
}

#endif /* FUXI_VOLHUD_LAYOUT_H */
