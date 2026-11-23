/* Host ASan/UBSan tests for FuxiAudioMicDspLogic.h path rewriting. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "FuxiAudioMicDspLogic.h"

int main(void)
{
	const char *d22_bottom = "/Library/Audio/Tunings/D22/VAD/d22_bottom_mic_general.austrip";
	const char *d22_meas = "/Library/Audio/Tunings/D22/VAD/d22_bottom_mic_measurement.austrip";
	const char *d22_beam = "/Library/Audio/Tunings/D22/VAD/d22_beam_mic_voice_recognition.austrip";
	const char *d221_bottom = "/Library/Audio/Tunings/D221/VAD/d22_bottom_mic_general.austrip";
	const char *d221_meas = "/Library/Audio/Tunings/D221/VAD/d22_bottom_mic_measurement.austrip";
	const char *d221_beam = "/Library/Audio/Tunings/D221/VAD/d22_beam_mic_voice_recognition.austrip";
	char out[256];

	assert(!fuxi_mic_dsp_rewrite_path(0u, d22_bottom, out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(0u, d22_beam, out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, NULL, out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d22_bottom, NULL, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d22_bottom, out, 0u));

	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_BOTTOM_MEAS, d22_bottom, out, sizeof(out)));
	assert(strcmp(out, d22_meas) == 0);
	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_BOTTOM_MEAS, d221_bottom, out, sizeof(out)));
	assert(strcmp(out, d221_meas) == 0);
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_BOTTOM_MEAS, d22_beam, out, sizeof(out)));

	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_BEAM_MEAS, d22_beam, out, sizeof(out)));
	assert(strcmp(out, d22_meas) == 0);
	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_BEAM_MEAS, d221_beam, out, sizeof(out)));
	assert(strcmp(out, d221_meas) == 0);
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_BEAM_MEAS, d22_bottom, out, sizeof(out)));

	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d22_bottom, out, sizeof(out)));
	assert(strcmp(out, d22_meas) == 0);
	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d22_beam, out, sizeof(out)));
	assert(strcmp(out, d22_meas) == 0);
	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d221_bottom, out, sizeof(out)));
	assert(strcmp(out, d221_meas) == 0);
	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d221_beam, out, sizeof(out)));
	assert(strcmp(out, d221_meas) == 0);

	/* Speaker, actuator, target strip, and non-VAD/other dirs must remain untouched. */
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d22_meas, out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL,
	    "/Library/Audio/Tunings/D22/VAD/d22_speaker_general.austrip", out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL,
	    "/Library/Audio/Tunings/D22/VAD/d22_actuator_general.austrip", out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL,
	    "/Library/Audio/Tunings/D22/AU/d22_bottom_mic_general.austrip", out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL,
	    "/Library/Audio/Tunings/D22/VAD/sub/d22_bottom_mic_general.austrip", out, sizeof(out)));
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL,
	    "/var/tmp/d22_bottom_mic_general.austrip", out, sizeof(out)));

	/* Exact boundary check: length(d22_meas) == 65 bytes + NUL == 66. */
	assert(!fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d22_bottom, out, strlen(d22_meas)));
	assert(fuxi_mic_dsp_rewrite_path(FUXI_MIC_DSP_ALL, d22_bottom, out, strlen(d22_meas) + 1u));
	assert(strcmp(out, d22_meas) == 0);

	/* Extended rewrite returning stripped stem + guardrails. */
	char stem[FUXI_MIC_DSP_STEM_MAX];
	assert(fuxi_mic_dsp_rewrite_path_ex(FUXI_MIC_DSP_BOTTOM_MEAS, d22_bottom, out, sizeof(out), stem, sizeof(stem)));
	assert(strcmp(out, d22_meas) == 0);
	assert(strcmp(stem, "d22_bottom_mic_general") == 0);
	assert(fuxi_mic_dsp_rewrite_path_ex(FUXI_MIC_DSP_BEAM_MEAS, d221_beam, out, sizeof(out), stem, sizeof(stem)));
	assert(strcmp(out, d221_meas) == 0);
	assert(strcmp(stem, "d22_beam_mic_voice_recognition") == 0);
	assert(!fuxi_mic_dsp_rewrite_path_ex(FUXI_MIC_DSP_ALL, d22_bottom, out, sizeof(out), NULL, 8u));
	assert(!fuxi_mic_dsp_rewrite_path_ex(FUXI_MIC_DSP_ALL, d22_bottom, out, sizeof(out), stem, 0u));
	assert(!fuxi_mic_dsp_rewrite_path_ex(FUXI_MIC_DSP_ALL, d22_bottom, out, sizeof(out), stem, strlen("d22_bottom_mic_general")));
	assert(fuxi_mic_dsp_rewrite_path_ex(FUXI_MIC_DSP_ALL, d22_bottom, out, sizeof(out), stem, strlen("d22_bottom_mic_general") + 1u));

	/* Pure-C11 16-slot key -> stem map tests. */
	struct fuxi_mic_dsp_map map;
	memset(&map, 0, sizeof(map));
	int keys[20];
	assert(!fuxi_mic_dsp_map_put(NULL, &keys[0], "a"));
	assert(!fuxi_mic_dsp_map_put(&map, NULL, "a"));
	assert(!fuxi_mic_dsp_map_put(&map, &keys[0], ""));
	assert(fuxi_mic_dsp_map_put(&map, &keys[0], "d22_bottom_mic_general"));
	assert(fuxi_mic_dsp_map_find(&map, &keys[0], stem, sizeof(stem)));
	assert(strcmp(stem, "d22_bottom_mic_general") == 0);
	/* Updating same key overwrites existing slot without duplicating. */
	assert(fuxi_mic_dsp_map_put(&map, &keys[0], "d22_beam_mic_voice_recognition"));
	assert(fuxi_mic_dsp_map_find(&map, &keys[0], stem, sizeof(stem)));
	assert(strcmp(stem, "d22_beam_mic_voice_recognition") == 0);
	/* Too-small output buffer preserves slot on take. */
	assert(!fuxi_mic_dsp_map_take(&map, &keys[0], stem, 4u));
	assert(fuxi_mic_dsp_map_find(&map, &keys[0], stem, sizeof(stem)));
	assert(fuxi_mic_dsp_map_take(&map, &keys[0], stem, sizeof(stem)));
	assert(strcmp(stem, "d22_beam_mic_voice_recognition") == 0);
	assert(!fuxi_mic_dsp_map_find(&map, &keys[0], stem, sizeof(stem)));
	/* Fill 16 slots then insert 17th: evicts slot 0 round-robin. */
	for (size_t i = 0; i < FUXI_MIC_DSP_MAP_SLOTS; i++) {
		char s[16];
		snprintf(s, sizeof(s), "stem_%zu", i);
		assert(fuxi_mic_dsp_map_put(&map, &keys[i], s));
	}
	assert(fuxi_mic_dsp_map_put(&map, &keys[16], "stem_16"));
	assert(!fuxi_mic_dsp_map_find(&map, &keys[0], stem, sizeof(stem)));
	assert(fuxi_mic_dsp_map_find(&map, &keys[1], stem, sizeof(stem)));
	assert(strcmp(stem, "stem_1") == 0);
	assert(fuxi_mic_dsp_map_find(&map, &keys[16], stem, sizeof(stem)));
	assert(strcmp(stem, "stem_16") == 0);

	/* Bypass subtype lookup for stripped effects + boundary checks. */
	uint32_t subs[4];
	memset(subs, 0, sizeof(subs));
	assert(fuxi_mic_dsp_bypass_subtypes(NULL, subs, 4u) == 0u);
	assert(fuxi_mic_dsp_bypass_subtypes("d22_bottom_mic_general", NULL, 4u) == 0u);
	assert(fuxi_mic_dsp_bypass_subtypes("d22_bottom_mic_general", subs, 0u) == 0u);
	assert(fuxi_mic_dsp_bypass_subtypes("d22_speaker_general", subs, 4u) == 0u);
	assert(fuxi_mic_dsp_bypass_subtypes("d22_bottom_mic_general", subs, 2u) == 2u);
	assert(subs[0] == FUXI_MIC_DSP_FOURCC_AGC2 && subs[1] == FUXI_MIC_DSP_FOURCC_MSTK);
	assert(fuxi_mic_dsp_bypass_subtypes("d22_bottom_mic_general", subs, 4u) == 3u);
	assert(subs[0] == FUXI_MIC_DSP_FOURCC_AGC2 && subs[1] == FUXI_MIC_DSP_FOURCC_MSTK &&
	    subs[2] == FUXI_MIC_DSP_FOURCC_CFRK);
	assert(fuxi_mic_dsp_bypass_subtypes("d22_beam_mic_voice_recognition", subs, 4u) == 1u);
	assert(subs[0] == FUXI_MIC_DSP_FOURCC_BEM2);

	puts("PASS: audio mic DSP path rewrite tests ok");
	return 0;
}
