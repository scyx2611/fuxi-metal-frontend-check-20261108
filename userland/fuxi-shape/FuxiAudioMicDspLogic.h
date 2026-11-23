/*
 * Pure C11 path rewrite logic for overriding VirtualAudio D22/D221 microphone
 * DSP strips (tested under ASan/UBSan in tools/test-audio-mic-dsp.c).
 *
 * References (AGENTS.md section 0):
 *   - Reuses ADT /chosen u32 gating and DYLD_INSERT_LIBRARIES interposition
 *     conventions from FuxiBluetoothLog.c (no Foundation/AudioToolbox links).
 *   - Target strip verified on local iOS 16.7.16 root:
 *     /Library/Audio/Tunings/D22/VAD/d22_bottom_mic_measurement.austrip
 *     (flat EQ + 30 Hz wind HPF + fixed +18 dB, without AUAGCV2/AUMeisterStueck).
 */
#ifndef FUXI_AUDIO_MIC_DSP_LOGIC_H
#define FUXI_AUDIO_MIC_DSP_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUXI_MIC_DSP_BOTTOM_MEAS 1u
#define FUXI_MIC_DSP_BEAM_MEAS   2u
#define FUXI_MIC_DSP_ALL         (FUXI_MIC_DSP_BOTTOM_MEAS | FUXI_MIC_DSP_BEAM_MEAS)
#define FUXI_MIC_DSP_MAP_SLOTS   16u
#define FUXI_MIC_DSP_STEM_MAX    64u

#define FUXI_MIC_DSP_FOURCC_APPL 0x6170706cu
#define FUXI_MIC_DSP_FOURCC_AUFX 0x61756678u
#define FUXI_MIC_DSP_FOURCC_AGC2 0x61676332u
#define FUXI_MIC_DSP_FOURCC_MSTK 0x6d73746bu
#define FUXI_MIC_DSP_FOURCC_CFRK 0x6366726bu
#define FUXI_MIC_DSP_FOURCC_BEM2 0x62656d32u

static inline bool
fuxi_mic_dsp_rewrite_path_ex(unsigned mode, const char *path, char *out, size_t out_size,
    char *stem_out, size_t stem_size)
{
	static const char *const prefixes[] = {
		"/Library/Audio/Tunings/D22/VAD/",
		"/Library/Audio/Tunings/D221/VAD/",
	};
	static const char dst_name[] = "d22_bottom_mic_measurement.austrip";
	static const char ext[] = ".austrip";

	if (mode == 0u || path == NULL || out == NULL || out_size == 0u ||
	    (stem_out == NULL && stem_size != 0u) || (stem_out != NULL && stem_size == 0u)) {
		return false;
	}
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t plen = strlen(prefixes[i]);
		if (strncmp(path, prefixes[i], plen) != 0) {
			continue;
		}
		const char *base = path + plen;
		bool match = false;
		if ((mode & FUXI_MIC_DSP_BOTTOM_MEAS) != 0u &&
		    strcmp(base, "d22_bottom_mic_general.austrip") == 0) {
			match = true;
		} else if ((mode & FUXI_MIC_DSP_BEAM_MEAS) != 0u &&
		    strcmp(base, "d22_beam_mic_voice_recognition.austrip") == 0) {
			match = true;
		}
		if (!match) {
			return false;
		}
		size_t blen = strlen(base);
		size_t ext_len = sizeof(ext) - 1u;
		size_t slen = (blen >= ext_len && strcmp(base + (blen - ext_len), ext) == 0)
		    ? (blen - ext_len) : blen;
		if (stem_out != NULL && slen >= stem_size) {
			return false;
		}
		int n = snprintf(out, out_size, "%s%s", prefixes[i], dst_name);
		if (n <= 0 || (size_t)n >= out_size) {
			return false;
		}
		if (stem_out != NULL) {
			memcpy(stem_out, base, slen);
			stem_out[slen] = '\0';
		}
		return true;
	}
	return false;
}

static inline bool
fuxi_mic_dsp_rewrite_path(unsigned mode, const char *path, char *out, size_t out_size)
{
	return fuxi_mic_dsp_rewrite_path_ex(mode, path, out, out_size, NULL, 0u);
}

struct fuxi_mic_dsp_slot {
	const void *key;
	char stem[FUXI_MIC_DSP_STEM_MAX];
	bool used;
};

struct fuxi_mic_dsp_map {
	struct fuxi_mic_dsp_slot slots[FUXI_MIC_DSP_MAP_SLOTS];
	size_t next_evict;
};

static inline bool
fuxi_mic_dsp_map_put(struct fuxi_mic_dsp_map *map, const void *key, const char *stem)
{
	if (map == NULL || key == NULL || stem == NULL || stem[0] == '\0') {
		return false;
	}
	size_t len = strlen(stem);
	if (len >= FUXI_MIC_DSP_STEM_MAX) {
		return false;
	}
	for (size_t i = 0; i < FUXI_MIC_DSP_MAP_SLOTS; i++) {
		if (map->slots[i].used && map->slots[i].key == key) {
			memcpy(map->slots[i].stem, stem, len + 1u);
			return true;
		}
	}
	for (size_t i = 0; i < FUXI_MIC_DSP_MAP_SLOTS; i++) {
		if (!map->slots[i].used) {
			map->slots[i].used = true;
			map->slots[i].key = key;
			memcpy(map->slots[i].stem, stem, len + 1u);
			return true;
		}
	}
	size_t idx = map->next_evict % FUXI_MIC_DSP_MAP_SLOTS;
	map->next_evict = (idx + 1u) % FUXI_MIC_DSP_MAP_SLOTS;
	map->slots[idx].used = true;
	map->slots[idx].key = key;
	memcpy(map->slots[idx].stem, stem, len + 1u);
	return true;
}

static inline bool
fuxi_mic_dsp_map_find(const struct fuxi_mic_dsp_map *map, const void *key, char *stem_out, size_t stem_size)
{
	if (map == NULL || key == NULL || stem_out == NULL || stem_size == 0u) {
		return false;
	}
	for (size_t i = 0; i < FUXI_MIC_DSP_MAP_SLOTS; i++) {
		const struct fuxi_mic_dsp_slot *slot = &map->slots[i];
		if (slot->used && slot->key == key) {
			size_t len = strlen(slot->stem);
			if (len >= stem_size) {
				return false;
			}
			memcpy(stem_out, slot->stem, len + 1u);
			return true;
		}
	}
	return false;
}

static inline bool
fuxi_mic_dsp_map_take(struct fuxi_mic_dsp_map *map, const void *key, char *stem_out, size_t stem_size)
{
	if (map == NULL || key == NULL || (stem_out != NULL && stem_size == 0u)) {
		return false;
	}
	for (size_t i = 0; i < FUXI_MIC_DSP_MAP_SLOTS; i++) {
		struct fuxi_mic_dsp_slot *slot = &map->slots[i];
		if (!slot->used || slot->key != key) {
			continue;
		}
		if (stem_out != NULL) {
			size_t len = strlen(slot->stem);
			if (len >= stem_size) {
				return false;
			}
			memcpy(stem_out, slot->stem, len + 1u);
		}
		slot->used = false;
		slot->key = NULL;
		slot->stem[0] = '\0';
		return true;
	}
	return false;
}

static inline size_t
fuxi_mic_dsp_bypass_subtypes(const char *stem, uint32_t *out, size_t max_count)
{
	if (stem == NULL || out == NULL || max_count == 0u) {
		return 0u;
	}
	if (strcmp(stem, "d22_bottom_mic_general") == 0) {
		static const uint32_t subs[] = {
			FUXI_MIC_DSP_FOURCC_AGC2,
			FUXI_MIC_DSP_FOURCC_MSTK,
			FUXI_MIC_DSP_FOURCC_CFRK,
		};
		size_t n = sizeof(subs) / sizeof(subs[0]);
		if (n > max_count) {
			n = max_count;
		}
		memcpy(out, subs, n * sizeof(subs[0]));
		return n;
	}
	if (strcmp(stem, "d22_beam_mic_voice_recognition") == 0) {
		out[0] = FUXI_MIC_DSP_FOURCC_BEM2;
		return 1u;
	}
	return 0u;
}

#endif
