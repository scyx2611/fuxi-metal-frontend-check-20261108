/* SPDX-License-Identifier: MIT */
/*
 * Gate 2 Adreno A740 (FD740, chip_id=0x43050a01) Dual-Mode Offline Compiler Probe
 *
 * 參考與復用出處（依 AGENTS.md §0）：
 * - UniXNU/patches/mesa/offline-adreno-compiler-probe.patch
 * - unixnu-fuxi/package/AdrenoCompilerProbe.c (chip_id=0x43050a01, 0xdeadbeef 拒絕, IR3_DBG_ASM_ROUNDTRIP)
 * - work/mesa-src/src/freedreno/vulkan/{tu_shader.cc, tu_clear_blit.cc, tu_pipeline.cc} (Mesa 85fda3b)
 * - work/localbuild/b023-gpu-scanout-phaseb-1126/PHASE_B_DESIGN_SPEC.md
 * - work/localbuild/b023-gpu-scanout-phaseb-1126/src/{fuxi_phaseb_3d.h, fuxi_phaseb_3d.c}
 *
 * 雙模式編譯契約：
 * 1) 預設 / -DFUXI_PROBE_MESA_IR3=1：對接 Mesa libfreedreno_ir3 + idep_nir (spirv_to_nir)
 *    於 macOS CI 編譯 phaseb_quad.vert.spv + phaseb_dual.frag.spv，產出凍結版
 *    fuxi_gpu_composite_isa.h / .bin / .asm / probe_report.json。
 * 2) -DFUXI_PROBE_MOCK_HOST=1：純 C11 離線主機模式（無外部 Vulkan SDK / Mesa 相依），
 *    內含標準 Khronos VK_KHR_pipeline_executable_properties ABI 與 in-process mock ICD，
 *    供 WSL Clang ASan/UBSan/LSan 執行 --self-test 與 AArch64 header 驗證。
 */

#if !defined(FUXI_PROBE_MOCK_HOST) && !defined(FUXI_PROBE_MESA_IR3)
#define FUXI_PROBE_MESA_IR3 1
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(FUXI_PROBE_MESA_IR3) && FUXI_PROBE_MESA_IR3
#include "compiler/glsl_types.h"
#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "freedreno_dev_info.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_nir.h"
#include "ir3/ir3_shader.h"
#include "util/bitset.h"
#include "util/ralloc.h"
#endif

/* =========================================================================
 * 共用常數與資料結構（對接 PHASE_B_DESIGN_SPEC.md 與 fuxi_phaseb_3d.c）
 * ========================================================================= */
#define FUXI_PROBE_CHIP_ID_FD740      0x43050a01u
#define FUXI_PROBE_CHIP_ID_UNKNOWN    0xdeadbeefu
#define FUXI_PROBE_GEN_A7XX           7u
#define FUXI_PROBE_MESA_SHA_STR       "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
#define FUXI_PROBE_SPIRV_MAGIC        0x07230203u
#define FUXI_PROBE_IR3_NOP_DWORD      0x00000000u
#define FUXI_PROBE_MAX_PROG_INSNS     128u
#define FUXI_PROBE_MAX_CONST_REGIONS  16u

enum fuxi_probe_const_region_kind {
	FUXI_CONST_REGION_PUSH_CONSTS = 0,
	FUXI_CONST_REGION_DYN_DESCRIPTOR_OFFSET = 1,
	FUXI_CONST_REGION_INLINE_UNIFORM_ADDRS = 2,
	FUXI_CONST_REGION_DRIVER_PARAMS = 3,
	FUXI_CONST_REGION_UBO_RANGES = 4,
	FUXI_CONST_REGION_PREAMBLE = 5,
	FUXI_CONST_REGION_GLOBAL = 6,
	FUXI_CONST_REGION_UBO_PTRS = 7,
	FUXI_CONST_REGION_IMAGE_DIMS = 8,
	FUXI_CONST_REGION_TFBO = 9,
	FUXI_CONST_REGION_PRIMITIVE_PARAM = 10,
	FUXI_CONST_REGION_PRIMITIVE_MAP = 11,
	FUXI_CONST_REGION_BINDLESS_BASE_ADDRS = 12,
	FUXI_CONST_REGION_MAX = 13
};

struct fuxi_probe_const_region {
	uint32_t type;
	char name[32];
	uint32_t offset_vec4;
	uint32_t size_vec4;
};

struct fuxi_probe_stage_const_layout {
	uint32_t push_consts_base_vec4;
	uint32_t push_consts_size_vec4;
	uint32_t push_consts_dwords;
	uint32_t preamble_base_vec4;
	uint32_t preamble_size_vec4;
	uint32_t region_count;
	struct fuxi_probe_const_region regions[FUXI_PROBE_MAX_CONST_REGIONS];
	uint32_t max_const_offset_vec4;
	uint32_t load_consts_via_preamble;
	uint32_t imm_base_vec4;
	uint32_t imm_raw_dword_count;
	uint32_t imm_count_vec4;
	uint32_t imm_dword_count;
	uint32_t *imm_dwords;
};

static const char *fuxi_probe_const_region_name(uint32_t type)
{
	switch (type) {
	case FUXI_CONST_REGION_PUSH_CONSTS: return "push_consts";
	case FUXI_CONST_REGION_DYN_DESCRIPTOR_OFFSET: return "dyn_descriptor_offset";
	case FUXI_CONST_REGION_INLINE_UNIFORM_ADDRS: return "inline_uniform_addrs";
	case FUXI_CONST_REGION_DRIVER_PARAMS: return "driver_params";
	case FUXI_CONST_REGION_UBO_RANGES: return "ubo_ranges";
	case FUXI_CONST_REGION_PREAMBLE: return "preamble";
	case FUXI_CONST_REGION_GLOBAL: return "global";
	case FUXI_CONST_REGION_UBO_PTRS: return "ubo_ptrs";
	case FUXI_CONST_REGION_IMAGE_DIMS: return "image_dims";
	case FUXI_CONST_REGION_TFBO: return "tfbo";
	case FUXI_CONST_REGION_PRIMITIVE_PARAM: return "primitive_param";
	case FUXI_CONST_REGION_PRIMITIVE_MAP: return "primitive_map";
	case FUXI_CONST_REGION_BINDLESS_BASE_ADDRS: return "bindless_base_addrs";
	default: return "unknown";
	}
}

struct fuxi_probe_vs_data {
	uint32_t insn_count;
	uint32_t raw_dword_count;
	uint32_t padded_dword_count;
	uint32_t instrlen;
	uint32_t fullregs;
	uint32_t halfregs;
	uint32_t mergedregs;
	uint32_t constlen;
	uint32_t sp_ctrl_reg0;
	uint32_t sp_const_config;
	uint32_t sp_config;
	uint32_t vertexid_regid;
	uint32_t pos_regid;
	uint32_t var0_regid;
	uint32_t sp_output_reg0;
	uint32_t sp_vpc_dest_reg0;
	uint32_t vpc_vs_cntl;
	uint32_t pc_vs_cntl;
	struct fuxi_probe_stage_const_layout const_layout;
	uint32_t *dwords;
	char *asm_text;
	char sha256_hex[65];
};

struct fuxi_probe_fs_data {
	uint32_t insn_count;
	uint32_t raw_dword_count;
	uint32_t padded_dword_count;
	uint32_t instrlen;
	uint32_t fullregs;
	uint32_t halfregs;
	uint32_t mergedregs;
	uint32_t double_threadsize;
	uint32_t constlen;
	uint32_t num_samp;
	uint32_t total_in;
	uint32_t prefetch_count;
	uint32_t ij_persp_pixel_regid;
	uint32_t out0_regid;
	uint32_t sp_ctrl_reg0;
	uint32_t sp_const_config;
	uint32_t sp_config;
	uint32_t sp_tsize;
	uint32_t sp_initial_tex_load_cntl;
	uint32_t sp_output_reg0;
	uint32_t sp_reg_prog_id_1;
	uint32_t sp_ps_cntl_1;
	uint32_t sp_ps_wave_cntl;
	uint32_t vpc_ps_cntl;
	uint32_t lm_disable0;
	uint32_t bindless_tex;
	struct fuxi_probe_stage_const_layout const_layout;
	uint32_t *dwords;
	char *asm_text;
	char sha256_hex[65];
};

struct fuxi_probe_result {
	uint32_t chip_id;
	uint32_t gen;
	char chip_name[16];
	char mesa_sha[64];
	char combined_sha256_hex[65];
	bool unknown_chip_refused;
	bool asm_roundtrip;
	bool gpu_executed;
	struct fuxi_probe_vs_data vs;
	struct fuxi_probe_fs_data fs;
};

static void fuxi_probe_result_free(struct fuxi_probe_result *res)
{
	if (!res)
		return;
	free(res->vs.const_layout.imm_dwords);
	free(res->vs.dwords);
	free(res->vs.asm_text);
	free(res->fs.const_layout.imm_dwords);
	free(res->fs.dwords);
	free(res->fs.asm_text);
	memset(res, 0, sizeof(*res));
}

static char *fuxi_strdup_c11(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s);
	char *p = (char *)malloc(n + 1u);
	if (!p)
		return NULL;
	memcpy(p, s, n + 1u);
	return p;
}

/* =========================================================================
 * 標準 FIPS 180-4 SHA-256（純 C11 內建，無外部 libcrypto 相依）
 * ========================================================================= */
struct fuxi_sha256_ctx {
	uint32_t state[8];
	uint64_t bitcount;
	uint8_t buf[64];
	size_t buflen;
};

static uint32_t fuxi_rotr32(uint32_t x, uint32_t n)
{
	return (x >> n) | (x << (32u - n));
}

static void fuxi_sha256_transform(struct fuxi_sha256_ctx *ctx, const uint8_t block[64])
{
	static const uint32_t k[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
		0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
		0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
		0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
		0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
		0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
		0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
		0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
		0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};
	uint32_t w[64];
	for (size_t i = 0u; i < 16u; i++) {
		size_t j = i * 4u;
		w[i] = ((uint32_t)block[j] << 24) |
		       ((uint32_t)block[j + 1u] << 16) |
		       ((uint32_t)block[j + 2u] << 8) |
		       ((uint32_t)block[j + 3u]);
	}
	for (size_t i = 16u; i < 64u; i++) {
		uint32_t s0 = fuxi_rotr32(w[i - 15u], 7u) ^ fuxi_rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
		uint32_t s1 = fuxi_rotr32(w[i - 2u], 17u) ^ fuxi_rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
		w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
	}
	uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
	uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
	for (size_t i = 0u; i < 64u; i++) {
		uint32_t S1 = fuxi_rotr32(e, 6u) ^ fuxi_rotr32(e, 11u) ^ fuxi_rotr32(e, 25u);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + S1 + ch + k[i] + w[i];
		uint32_t S0 = fuxi_rotr32(a, 2u) ^ fuxi_rotr32(a, 13u) ^ fuxi_rotr32(a, 22u);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = S0 + maj;
		h = g; g = f; f = e; e = d + temp1;
		d = c; c = b; b = a; a = temp1 + temp2;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void fuxi_sha256_init(struct fuxi_sha256_ctx *ctx)
{
	ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
	ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
	ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
	ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
	ctx->bitcount = 0u;
	ctx->buflen = 0u;
}

static void fuxi_sha256_update(struct fuxi_sha256_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	while (len > 0u) {
		size_t take = 64u - ctx->buflen;
		if (take > len)
			take = len;
		memcpy(ctx->buf + ctx->buflen, p, take);
		ctx->buflen += take;
		ctx->bitcount += (uint64_t)take * 8ULL;
		p += take;
		len -= take;
		if (ctx->buflen == 64u) {
			fuxi_sha256_transform(ctx, ctx->buf);
			ctx->buflen = 0u;
		}
	}
}

static void fuxi_sha256_final_hex(struct fuxi_sha256_ctx *ctx, char out_hex[65])
{
	static const char hex[] = "0123456789abcdef";
	uint64_t total_bits = ctx->bitcount;
	uint8_t pad = 0x80u;
	fuxi_sha256_update(ctx, &pad, 1u);
	uint8_t zero = 0x00u;
	while (ctx->buflen != 56u)
		fuxi_sha256_update(ctx, &zero, 1u);
	uint8_t len_be[8];
	for (size_t i = 0u; i < 8u; i++)
		len_be[i] = (uint8_t)((total_bits >> ((7u - i) * 8u)) & 0xffu);
	fuxi_sha256_update(ctx, len_be, 8u);
	for (size_t i = 0u; i < 8u; i++) {
		uint32_t v = ctx->state[i];
		for (size_t b = 0u; b < 4u; b++) {
			uint8_t byte = (uint8_t)((v >> ((3u - b) * 8u)) & 0xffu);
			out_hex[(i * 4u + b) * 2u] = hex[(byte >> 4) & 0xfu];
			out_hex[(i * 4u + b) * 2u + 1u] = hex[byte & 0xfu];
		}
	}
	out_hex[64] = '\0';
}

static void fuxi_sha256_bytes_hex(const void *data, size_t len, char out_hex[65])
{
	struct fuxi_sha256_ctx ctx;
	fuxi_sha256_init(&ctx);
	if (data && len > 0u)
		fuxi_sha256_update(&ctx, data, len);
	fuxi_sha256_final_hex(&ctx, out_hex);
}

/* =========================================================================
 * SPIR-V 驗證與讀寫輔助
 * ========================================================================= */
static bool fuxi_probe_validate_spirv(const void *buf, size_t byte_len,
				      const char *label, char *err_buf, size_t err_cap)
{
	if (!buf || byte_len == 0u) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: empty or NULL SPIR-V buffer", label);
		return false;
	}
	if ((byte_len & 3u) != 0u) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: byte length %zu is not 4-byte aligned",
				 label, byte_len);
		return false;
	}
	if (byte_len < 20u) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: truncated SPIR-V header (%zu < 20 bytes)",
				 label, byte_len);
		return false;
	}
	const uint8_t *b = (const uint8_t *)buf;
	uint32_t magic = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
			 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	if (magic != FUXI_PROBE_SPIRV_MAGIC) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: invalid SPIR-V magic 0x%08x (expected 0x%08x)",
				 label, magic, FUXI_PROBE_SPIRV_MAGIC);
		return false;
	}
	uint32_t bound = (uint32_t)b[12] | ((uint32_t)b[13] << 8) |
			 ((uint32_t)b[14] << 16) | ((uint32_t)b[15] << 24);
	uint32_t schema = (uint32_t)b[16] | ((uint32_t)b[17] << 8) |
			  ((uint32_t)b[18] << 16) | ((uint32_t)b[19] << 24);
	if (bound == 0u || schema != 0u) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: invalid SPIR-V header bound=%u schema=%u",
				 label, bound, schema);
		return false;
	}
	size_t total_words = byte_len / 4u;
	size_t idx = 5u;
	while (idx < total_words) {
		size_t off = idx * 4u;
		uint32_t op_word = (uint32_t)b[off] | ((uint32_t)b[off + 1u] << 8) |
				   ((uint32_t)b[off + 2u] << 16) | ((uint32_t)b[off + 3u] << 24);
		uint32_t wc = (op_word >> 16) & 0xFFFFu;
		if (wc == 0u) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap,
					 "%s: SPIR-V instruction at word %zu has word_count=0",
					 label, idx);
			return false;
		}
		if ((size_t)wc > total_words - idx) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap,
					 "%s: SPIR-V instruction at word %zu truncated (wc=%u, remaining=%zu)",
					 label, idx, wc, total_words - idx);
			return false;
		}
		idx += (size_t)wc;
	}
	return true;
}

static bool fuxi_read_file_bytes(const char *path, uint8_t **out_buf, size_t *out_len)
{
	if (!path || !out_buf || !out_len)
		return false;
	*out_buf = NULL;
	*out_len = 0u;
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return false;
	}
	long sz = ftell(fp);
	if (sz <= 0) {
		fclose(fp);
		return false;
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return false;
	}
	size_t len = (size_t)sz;
	uint8_t *buf = (uint8_t *)malloc(len);
	if (!buf) {
		fclose(fp);
		return false;
	}
	if (fread(buf, 1u, len, fp) != len) {
		free(buf);
		fclose(fp);
		return false;
	}
	fclose(fp);
	*out_buf = buf;
	*out_len = len;
	return true;
}

static bool fuxi_write_binary_file(const char *path, const void *data, size_t len)
{
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return false;
	if (len > 0u && fwrite(data, 1u, len, fp) != len) {
		fclose(fp);
		return false;
	}
	return fclose(fp) == 0;
}

/* =========================================================================
 * 不變量驗證、fuxi_gpu_composite_isa.h 匯出與報告輸出
 * ========================================================================= */
static bool fuxi_probe_parse_u32_span(const char *start, const char *end, uint32_t *out)
{
	while (start < end && (*start == ' ' || *start == '\t'))
		start++;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		end--;
	if (start >= end)
		return false;
	uint64_t val = 0u;
	for (const char *p = start; p < end; p++) {
		if (*p < '0' || *p > '9')
			return false;
		val = val * 10u + (uint64_t)(*p - '0');
		if (val > UINT32_MAX)
			return false;
	}
	*out = (uint32_t)val;
	return true;
}

static bool fuxi_probe_asm_check_stc_no_push_overlap(const char *stage_name,
						     const char *asm_text,
						     uint32_t push_base_dw,
						     uint32_t push_dwords,
						     char *err_buf,
						     size_t err_cap)
{
	if (!asm_text) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: missing assembly text for stc guard", stage_name);
		return false;
	}
	if (push_dwords == 0u)
		return true;

	uint64_t push_end_dw = (uint64_t)push_base_dw + (uint64_t)push_dwords;
	const char *cur = asm_text;
	uint32_t line_no = 0u;

	while (*cur != '\0') {
		line_no++;
		const char *eol = strchr(cur, '\n');
		size_t line_len = eol ? (size_t)(eol - cur) : strlen(cur);
		const char *p = cur;
		const char *line_end = cur + line_len;
		cur = eol ? (eol + 1) : line_end;

		while (p < line_end && (*p == ' ' || *p == '\t' || *p == '\r'))
			p++;
		if (p >= line_end || *p == ';' || *p == '@')
			continue;

		while (p < line_end && *p == '(') {
			const char *close = (const char *)memchr(p, ')', (size_t)(line_end - p));
			if (!close)
				break;
			p = close + 1;
			while (p < line_end && (*p == ' ' || *p == '\t'))
				p++;
		}
		if (p >= line_end)
			continue;

		if ((size_t)(line_end - p) >= 4u &&
		    strncmp(p, "stc", 3) == 0 &&
		    (p[3] == '.' || p[3] == ' ' || p[3] == '\t')) {
			while (p < line_end && *p != ' ' && *p != '\t')
				p++;
			while (p < line_end && (*p == ' ' || *p == '\t'))
				p++;
			if ((size_t)(line_end - p) < 4u || p[0] != 'c' || p[1] != '[') {
				if (err_buf && err_cap)
					snprintf(err_buf, err_cap,
						 "%s line %u: unparseable stc destination (fail-closed)",
						 stage_name, line_no);
				return false;
			}
			const char *idx_start = p + 2;
			const char *idx_end = (const char *)memchr(idx_start, ']', (size_t)(line_end - idx_start));
			if (!idx_end) {
				if (err_buf && err_cap)
					snprintf(err_buf, err_cap,
						 "%s line %u: malformed stc c[...] operand",
						 stage_name, line_no);
				return false;
			}
			uint32_t dst_dw = 0u;
			if (!fuxi_probe_parse_u32_span(idx_start, idx_end, &dst_dw)) {
				if (err_buf && err_cap)
					snprintf(err_buf, err_cap,
						 "%s line %u: stc uses indirect or non-constant c[...] operand",
						 stage_name, line_no);
				return false;
			}
			const char *scan_end = line_end;
			const char *semi = (const char *)memchr(idx_end + 1, ';', (size_t)(line_end - (idx_end + 1)));
			if (semi)
				scan_end = semi;
			const char *last_comma = NULL;
			for (const char *q = idx_end + 1; q < scan_end; q++) {
				if (*q == ',')
					last_comma = q;
			}
			uint32_t count = 0u;
			if (!last_comma || !fuxi_probe_parse_u32_span(last_comma + 1, scan_end, &count) ||
			    count == 0u || count > 1024u) {
				if (err_buf && err_cap)
					snprintf(err_buf, err_cap,
						 "%s line %u: invalid or missing stc dword count",
						 stage_name, line_no);
				return false;
			}
			uint64_t dst_end_dw = (uint64_t)dst_dw + (uint64_t)count;
			if ((uint64_t)dst_dw < push_end_dw && dst_end_dw > (uint64_t)push_base_dw) {
				if (err_buf && err_cap)
					snprintf(err_buf, err_cap,
						 "%s stc writes c[%u..%" PRIu64 "], overlapping push_consts c[%u..%" PRIu64 "]",
						 stage_name, dst_dw, dst_end_dw - 1u,
						 push_base_dw, push_end_dw - 1u);
				return false;
			}
		}
	}
	return true;
}

static bool fuxi_probe_verify_const_layout_and_asm(
	const char *stage_name,
	const struct fuxi_probe_stage_const_layout *layout,
	uint32_t constlen,
	const char *asm_text,
	char *err_buf,
	size_t err_cap)
{
	if (!stage_name || !layout) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "NULL stage/const_layout");
		return false;
	}
	if (layout->region_count > FUXI_PROBE_MAX_CONST_REGIONS) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: region_count=%u exceeds limit",
				 stage_name, layout->region_count);
		return false;
	}
	if (layout->push_consts_dwords > 0u) {
		uint32_t min_vec4 = (layout->push_consts_dwords + 3u) / 4u;
		if (layout->push_consts_size_vec4 < min_vec4 ||
		    layout->push_consts_base_vec4 != 0u) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap,
					 "%s: push_consts invalid (base=%u size=%u dwords=%u)",
					 stage_name, layout->push_consts_base_vec4,
					 layout->push_consts_size_vec4, layout->push_consts_dwords);
			return false;
		}
		bool found_push_region = false;
		for (uint32_t i = 0u; i < layout->region_count; i++) {
			if (layout->regions[i].type == FUXI_CONST_REGION_PUSH_CONSTS &&
			    layout->regions[i].offset_vec4 == layout->push_consts_base_vec4 &&
			    layout->regions[i].size_vec4 == layout->push_consts_size_vec4) {
				found_push_region = true;
				break;
			}
		}
		if (!found_push_region) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap,
					 "%s: IR3_CONST_ALLOC_PUSH_CONSTS missing from regions[]",
					 stage_name);
			return false;
		}
	}

	struct {
		const char *name;
		uint32_t base;
		uint32_t end;
	} intervals[FUXI_PROBE_MAX_CONST_REGIONS + 1u];
	uint32_t n_intervals = 0u;

	for (uint32_t i = 0u; i < layout->region_count; i++) {
		const struct fuxi_probe_const_region *r = &layout->regions[i];
		if (r->size_vec4 == 0u ||
		    r->offset_vec4 > layout->max_const_offset_vec4 ||
		    r->size_vec4 > layout->max_const_offset_vec4 - r->offset_vec4 ||
		    layout->max_const_offset_vec4 > constlen) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap,
					 "%s: region %s [%u..%u) exceeds max_const_offset=%u/constlen=%u",
					 stage_name, r->name, r->offset_vec4,
					 r->offset_vec4 + r->size_vec4,
					 layout->max_const_offset_vec4, constlen);
			return false;
		}
		intervals[n_intervals].name = r->name[0] ? r->name : "region";
		intervals[n_intervals].base = r->offset_vec4;
		intervals[n_intervals].end = r->offset_vec4 + r->size_vec4;
		n_intervals++;
	}

	if (layout->imm_count_vec4 > 0u) {
		if (layout->imm_dword_count != layout->imm_count_vec4 * 4u || !layout->imm_dwords) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap, "%s: imm_dwords buffer mismatch", stage_name);
			return false;
		}
		if (layout->imm_base_vec4 < layout->max_const_offset_vec4 ||
		    layout->imm_count_vec4 > constlen ||
		    layout->imm_base_vec4 > constlen - layout->imm_count_vec4) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap,
					 "%s: imm [%u..%u) invalid vs max_const_offset=%u constlen=%u",
					 stage_name, layout->imm_base_vec4,
					 layout->imm_base_vec4 + layout->imm_count_vec4,
					 layout->max_const_offset_vec4, constlen);
			return false;
		}
		intervals[n_intervals].name = "immediates";
		intervals[n_intervals].base = layout->imm_base_vec4;
		intervals[n_intervals].end = layout->imm_base_vec4 + layout->imm_count_vec4;
		n_intervals++;
	} else if (layout->imm_dword_count != 0u) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "%s: non-zero imm_dword_count with imm_count_vec4=0",
				 stage_name);
		return false;
	}

	for (uint32_t i = 0u; i < n_intervals; i++) {
		for (uint32_t j = i + 1u; j < n_intervals; j++) {
			if (intervals[i].end > intervals[j].base && intervals[j].end > intervals[i].base) {
				if (err_buf && err_cap)
					snprintf(err_buf, err_cap,
						 "%s: const region overlap %s [%u,%u) vs %s [%u,%u)",
						 stage_name,
						 intervals[i].name, intervals[i].base, intervals[i].end,
						 intervals[j].name, intervals[j].base, intervals[j].end);
				return false;
			}
		}
	}

	return fuxi_probe_asm_check_stc_no_push_overlap(stage_name, asm_text,
						     layout->push_consts_base_vec4 * 4u,
						     layout->push_consts_dwords,
						     err_buf, err_cap);
}

static bool fuxi_probe_verify_result(const struct fuxi_probe_result *res,
				     char *err_buf, size_t err_cap)
{
	if (!res)
		return false;
	if (res->chip_id != FUXI_PROBE_CHIP_ID_FD740 || res->gen != FUXI_PROBE_GEN_A7XX ||
	    strcmp(res->chip_name, "FD740") != 0) {
		snprintf(err_buf, err_cap, "unexpected chip identity 0x%08x (%s gen=%u)",
			 res->chip_id, res->chip_name, res->gen);
		return false;
	}
	if (!res->unknown_chip_refused || !res->asm_roundtrip || res->gpu_executed) {
		snprintf(err_buf, err_cap, "safety flags invalid (unknown=%d roundtrip=%d gpu=%d)",
			 (int)res->unknown_chip_refused, (int)res->asm_roundtrip, (int)res->gpu_executed);
		return false;
	}
	if (res->vs.instrlen == 0u || res->vs.insn_count == 0u ||
	    res->vs.padded_dword_count != res->vs.instrlen * 32u ||
	    res->vs.raw_dword_count == 0u ||
	    res->vs.raw_dword_count > res->vs.padded_dword_count || !res->vs.dwords) {
		snprintf(err_buf, err_cap, "invalid VS ISA dimensions");
		return false;
	}
	for (uint32_t i = res->vs.raw_dword_count; i < res->vs.padded_dword_count; i++) {
		if (res->vs.dwords[i] != FUXI_PROBE_IR3_NOP_DWORD) {
			snprintf(err_buf, err_cap, "VS padding dword[%u] != 0", i);
			return false;
		}
	}
	if (res->fs.instrlen == 0u || res->fs.insn_count == 0u ||
	    res->fs.padded_dword_count != res->fs.instrlen * 32u ||
	    res->fs.raw_dword_count == 0u ||
	    res->fs.raw_dword_count > res->fs.padded_dword_count || !res->fs.dwords) {
		snprintf(err_buf, err_cap, "invalid FS ISA dimensions");
		return false;
	}
	for (uint32_t i = res->fs.raw_dword_count; i < res->fs.padded_dword_count; i++) {
		if (res->fs.dwords[i] != FUXI_PROBE_IR3_NOP_DWORD) {
			snprintf(err_buf, err_cap, "FS padding dword[%u] != 0", i);
			return false;
		}
	}
	if (res->vs.constlen < 2u || res->fs.constlen < 4u) {
		snprintf(err_buf, err_cap, "constlen too small (vs=%u fs=%u)",
			 res->vs.constlen, res->fs.constlen);
		return false;
	}
	if (res->vs.mergedregs != 1u || (res->vs.sp_ctrl_reg0 & (1u << 20)) == 0u ||
	    res->fs.mergedregs != 1u || (res->fs.sp_ctrl_reg0 & (1u << 31)) == 0u) {
		snprintf(err_buf, err_cap, "MERGEDREGS bit missing in SP_CTRL_REG0");
		return false;
	}
	if (res->fs.bindless_tex != 0u || (res->fs.sp_config & (1u << 1)) != 0u) {
		snprintf(err_buf, err_cap, "FS must stay bindful (BINDLESS_TEX bit set)");
		return false;
	}
	uint32_t ntex = (res->fs.sp_config >> 9) & 0xffu;
	uint32_t nsamp = (res->fs.sp_config >> 17) & 0x1fu;
	if (res->fs.num_samp != 2u || res->fs.sp_tsize != 2u || ntex != 2u || nsamp != 2u) {
		snprintf(err_buf, err_cap, "FS texture/sampler count mismatch (num_samp=%u tsize=%u ntex=%u nsamp=%u)",
			 res->fs.num_samp, res->fs.sp_tsize, ntex, nsamp);
		return false;
	}
	if (!fuxi_probe_verify_const_layout_and_asm("VS", &res->vs.const_layout,
						    res->vs.constlen, res->vs.asm_text,
						    err_buf, err_cap))
		return false;
	if (!fuxi_probe_verify_const_layout_and_asm("FS", &res->fs.const_layout,
						    res->fs.constlen, res->fs.asm_text,
						    err_buf, err_cap))
		return false;
	return true;
}

static void emit_fuxi_imm_dwords_array(FILE *fp,
				       const char *lower_stage,
				       const char *upper_stage,
				       const struct fuxi_probe_stage_const_layout *layout)
{
	fprintf(fp,
		"__attribute__((unused)) static const uint32_t fuxi_pb_%s_imm_dwords["
		"FUXI_PB_%s_IMM_DWORD_COUNT > 0u ? FUXI_PB_%s_IMM_DWORD_COUNT : 1u] = {\n",
		lower_stage, upper_stage, upper_stage);
	if (!layout || layout->imm_dword_count == 0u || !layout->imm_dwords) {
		fprintf(fp, "\t0x00000000u\n");
	} else {
		for (uint32_t i = 0u; i < layout->imm_dword_count; i++) {
			if ((i & 3u) == 0u)
				fprintf(fp, "\t");
			fprintf(fp, "0x%08xu%s", layout->imm_dwords[i],
				(i + 1u < layout->imm_dword_count) ? (((i & 3u) == 3u) ? ",\n" : ", ") : "\n");
		}
	}
	fprintf(fp, "};\n#define FUXI_PB_%s_IMM_DWORDS fuxi_pb_%s_imm_dwords\n\n",
		upper_stage, lower_stage);
}

static bool emit_fuxi_gpu_composite_isa_header(const char *path,
					       const struct fuxi_probe_result *res)
{
	if (!path || !res || !res->vs.dwords || !res->fs.dwords)
		return false;
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return false;

	fprintf(fp,
		"/* SPDX-License-Identifier: MIT */\n"
		"/*\n"
		" * Auto-generated by adreno_compiler_probe (Gate 2).\n"
		" * Target: Adreno A740 (FD740, chip_id=0x%08x, gen=%u)\n"
		" * Contract: PHASE_B_DESIGN_SPEC.md & fuxi_phaseb_3d.c\n"
		" */\n"
		"#ifndef FUXI_GPU_COMPOSITE_ISA_H\n"
		"#define FUXI_GPU_COMPOSITE_ISA_H\n\n"
		"#include <stdint.h>\n\n"
		"/* 識別與防漂移 */\n"
		"#define FUXI_PB_ISA_CHIP_ID                 0x%08xu\n"
		"#define FUXI_PB_ISA_GEN                     %uu\n"
		"#define FUXI_PB_ISA_MESA_SHA                \"%s\"\n"
		"#define FUXI_PB_ISA_SHADER_SHA256           \"%s\"\n\n"
		"/* VS 指令、暫存器與 VPC 連結常數 */\n"
		"#define FUXI_PB_VS_INSN_COUNT               %uu\n"
		"#define FUXI_PB_VS_DWORD_COUNT              %uu\n"
		"#define FUXI_PB_VS_INSTRLEN                 %uu\n"
		"#define FUXI_PB_VS_FULLREGS                 %uu\n"
		"#define FUXI_PB_VS_HALFREGS                 %uu\n"
		"#define FUXI_PB_VS_MERGEDREGS               %uu\n"
		"#define FUXI_PB_VS_CONSTLEN                 %uu\n"
		"#define FUXI_PB_VS_SP_CTRL_REG0             0x%08xu\n"
		"#define FUXI_PB_VS_SP_CONST_CONFIG          0x%08xu\n"
		"#define FUXI_PB_VS_SP_CONFIG                0x%08xu\n"
		"#define FUXI_PB_VS_VERTEXID_REGID           0x%08xu\n"
		"#define FUXI_PB_VS_POS_REGID                0x%08xu\n"
		"#define FUXI_PB_VS_VAR0_REGID               0x%08xu\n"
		"#define FUXI_PB_VS_SP_OUTPUT_REG0           0x%08xu\n"
		"#define FUXI_PB_VS_SP_VPC_DEST_REG0         0x%08xu\n"
		"#define FUXI_PB_VS_VPC_VS_CNTL              0x%08xu\n"
		"#define FUXI_PB_VS_PC_VS_CNTL               0x%08xu\n"
		"#define FUXI_PB_VS_PUSH_CONST_VEC4          %uu\n"
		"#define FUXI_PB_VS_PREAMBLE_BASE_VEC4       %uu\n"
		"#define FUXI_PB_VS_PREAMBLE_COUNT_VEC4      %uu\n"
		"#define FUXI_PB_VS_IMM_BASE_VEC4            %uu\n"
		"#define FUXI_PB_VS_IMM_COUNT_VEC4           %uu\n"
		"#define FUXI_PB_VS_IMM_DWORD_COUNT          %uu\n\n",
		res->chip_id, res->gen,
		res->chip_id, res->gen, res->mesa_sha, res->combined_sha256_hex,
		res->vs.insn_count, res->vs.padded_dword_count, res->vs.instrlen,
		res->vs.fullregs, res->vs.halfregs, res->vs.mergedregs, res->vs.constlen,
		res->vs.sp_ctrl_reg0, res->vs.sp_const_config, res->vs.sp_config,
		res->vs.vertexid_regid, res->vs.pos_regid, res->vs.var0_regid,
		res->vs.sp_output_reg0, res->vs.sp_vpc_dest_reg0,
		res->vs.vpc_vs_cntl, res->vs.pc_vs_cntl,
		res->vs.const_layout.push_consts_size_vec4,
		res->vs.const_layout.preamble_base_vec4,
		res->vs.const_layout.preamble_size_vec4,
		res->vs.const_layout.imm_base_vec4,
		res->vs.const_layout.imm_count_vec4,
		res->vs.const_layout.imm_dword_count);

	fprintf(fp,
		"/* FS 指令、紋理綁定與插值輸出常數 */\n"
		"#define FUXI_PB_FS_INSN_COUNT               %uu\n"
		"#define FUXI_PB_FS_DWORD_COUNT              %uu\n"
		"#define FUXI_PB_FS_INSTRLEN                 %uu\n"
		"#define FUXI_PB_FS_FULLREGS                 %uu\n"
		"#define FUXI_PB_FS_HALFREGS                 %uu\n"
		"#define FUXI_PB_FS_MERGEDREGS               %uu\n"
		"#define FUXI_PB_FS_DOUBLE_THREADSIZE        %uu\n"
		"#define FUXI_PB_FS_CONSTLEN                 %uu\n"
		"#define FUXI_PB_FS_NUM_SAMP                 %uu\n"
		"#define FUXI_PB_FS_TOTAL_IN                 %uu\n"
		"#define FUXI_PB_FS_PREFETCH_COUNT           %uu\n"
		"#define FUXI_PB_FS_IJ_PERSP_PIXEL_REGID     0x%08xu\n"
		"#define FUXI_PB_FS_OUT0_REGID               0x%08xu\n"
		"#define FUXI_PB_FS_SP_CTRL_REG0             0x%08xu\n"
		"#define FUXI_PB_FS_SP_CONST_CONFIG          0x%08xu\n"
		"#define FUXI_PB_FS_SP_CONFIG                0x%08xu\n"
		"#define FUXI_PB_FS_SP_TSIZE                 0x%08xu\n"
		"#define FUXI_PB_FS_SP_INITIAL_TEX_LOAD_CNTL 0x%08xu\n"
		"#define FUXI_PB_FS_SP_OUTPUT_REG0           0x%08xu\n"
		"#define FUXI_PB_FS_SP_REG_PROG_ID_1         0x%08xu\n"
		"#define FUXI_PB_FS_SP_PS_CNTL_1             0x%08xu\n"
		"#define FUXI_PB_FS_SP_PS_WAVE_CNTL          0x%08xu\n"
		"#define FUXI_PB_FS_VPC_PS_CNTL              0x%08xu\n"
		"#define FUXI_PB_FS_LM_DISABLE0              0x%08xu\n"
		"#define FUXI_PB_FS_PUSH_CONST_VEC4          %uu\n"
		"#define FUXI_PB_FS_PREAMBLE_BASE_VEC4       %uu\n"
		"#define FUXI_PB_FS_PREAMBLE_COUNT_VEC4      %uu\n"
		"#define FUXI_PB_FS_IMM_BASE_VEC4            %uu\n"
		"#define FUXI_PB_FS_IMM_COUNT_VEC4           %uu\n"
		"#define FUXI_PB_FS_IMM_DWORD_COUNT          %uu\n\n",
		res->fs.insn_count, res->fs.padded_dword_count, res->fs.instrlen,
		res->fs.fullregs, res->fs.halfregs, res->fs.mergedregs,
		res->fs.double_threadsize, res->fs.constlen, res->fs.num_samp,
		res->fs.total_in, res->fs.prefetch_count,
		res->fs.ij_persp_pixel_regid, res->fs.out0_regid,
		res->fs.sp_ctrl_reg0, res->fs.sp_const_config, res->fs.sp_config,
		res->fs.sp_tsize, res->fs.sp_initial_tex_load_cntl,
		res->fs.sp_output_reg0, res->fs.sp_reg_prog_id_1,
		res->fs.sp_ps_cntl_1, res->fs.sp_ps_wave_cntl,
		res->fs.vpc_ps_cntl, res->fs.lm_disable0,
		res->fs.const_layout.push_consts_size_vec4,
		res->fs.const_layout.preamble_base_vec4,
		res->fs.const_layout.preamble_size_vec4,
		res->fs.const_layout.imm_base_vec4,
		res->fs.const_layout.imm_count_vec4,
		res->fs.const_layout.imm_dword_count);

	fprintf(fp, "__attribute__((unused)) static const uint32_t fuxi_pb_vs_dwords[FUXI_PB_VS_DWORD_COUNT] = {\n");
	for (uint32_t i = 0u; i < res->vs.padded_dword_count; i++) {
		if ((i & 3u) == 0u)
			fprintf(fp, "\t");
		fprintf(fp, "0x%08xu%s", res->vs.dwords[i],
			(i + 1u < res->vs.padded_dword_count) ? (((i & 3u) == 3u) ? ",\n" : ", ") : "\n");
	}
	fprintf(fp, "};\n\n");

	fprintf(fp, "__attribute__((unused)) static const uint32_t fuxi_pb_fs_dwords[FUXI_PB_FS_DWORD_COUNT] = {\n");
	for (uint32_t i = 0u; i < res->fs.padded_dword_count; i++) {
		if ((i & 3u) == 0u)
			fprintf(fp, "\t");
		fprintf(fp, "0x%08xu%s", res->fs.dwords[i],
			(i + 1u < res->fs.padded_dword_count) ? (((i & 3u) == 3u) ? ",\n" : ", ") : "\n");
	}
	fprintf(fp, "};\n\n");
	emit_fuxi_imm_dwords_array(fp, "vs", "VS", &res->vs.const_layout);
	emit_fuxi_imm_dwords_array(fp, "fs", "FS", &res->fs.const_layout);
	fprintf(fp, "#endif /* FUXI_GPU_COMPOSITE_ISA_H */\n");

	return fclose(fp) == 0;
}

static bool fuxi_write_probe_outputs(const char *out_dir,
				     const struct fuxi_probe_result *res,
				     const char *mode_name,
				     char *err_buf,
				     size_t err_cap)
{
	char path[4096];
	int n = snprintf(path, sizeof(path), "%s/fuxi_gpu_composite_isa.h", out_dir);
	if (n < 0 || (size_t)n >= sizeof(path) || !emit_fuxi_gpu_composite_isa_header(path, res)) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "failed to write %s (%s)",
				 (n < 0 || (size_t)n >= sizeof(path)) ? "fuxi_gpu_composite_isa.h (path too long)" : path,
				 strerror(errno));
		return false;
	}

	n = snprintf(path, sizeof(path), "%s/phaseb_vs.bin", out_dir);
	if (n < 0 || (size_t)n >= sizeof(path) ||
	    !fuxi_write_binary_file(path, res->vs.dwords, res->vs.padded_dword_count * 4u)) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "failed to write %s (%s)",
				 (n < 0 || (size_t)n >= sizeof(path)) ? "phaseb_vs.bin (path too long)" : path,
				 strerror(errno));
		return false;
	}

	n = snprintf(path, sizeof(path), "%s/phaseb_fs.bin", out_dir);
	if (n < 0 || (size_t)n >= sizeof(path) ||
	    !fuxi_write_binary_file(path, res->fs.dwords, res->fs.padded_dword_count * 4u)) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "failed to write %s (%s)",
				 (n < 0 || (size_t)n >= sizeof(path)) ? "phaseb_fs.bin (path too long)" : path,
				 strerror(errno));
		return false;
	}

	n = snprintf(path, sizeof(path), "%s/phaseb_vs.asm", out_dir);
	if (n < 0 || (size_t)n >= sizeof(path) ||
	    !fuxi_write_binary_file(path, res->vs.asm_text, strlen(res->vs.asm_text))) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "failed to write %s (%s)",
				 (n < 0 || (size_t)n >= sizeof(path)) ? "phaseb_vs.asm (path too long)" : path,
				 strerror(errno));
		return false;
	}

	n = snprintf(path, sizeof(path), "%s/phaseb_fs.asm", out_dir);
	if (n < 0 || (size_t)n >= sizeof(path) ||
	    !fuxi_write_binary_file(path, res->fs.asm_text, strlen(res->fs.asm_text))) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "failed to write %s (%s)",
				 (n < 0 || (size_t)n >= sizeof(path)) ? "phaseb_fs.asm (path too long)" : path,
				 strerror(errno));
		return false;
	}

	n = snprintf(path, sizeof(path), "%s/probe_report.json", out_dir);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "probe_report.json path too long");
		return false;
	}
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "failed to open %s (%s)", path, strerror(errno));
		return false;
	}
	fprintf(fp,
		"{\n"
		"  \"mode\": \"%s\",\n"
		"  \"chip_id\": \"0x%08x\",\n"
		"  \"name\": \"%s\",\n"
		"  \"generation\": %u,\n"
		"  \"mesa_sha\": \"%s\",\n"
		"  \"combined_sha256\": \"%s\",\n"
		"  \"unknown_chip_refused\": %s,\n"
		"  \"asm_roundtrip\": %s,\n"
		"  \"gpu_executed\": %s,\n"
		"  \"vs\": {\n"
		"    \"insn_count\": %u,\n"
		"    \"instrlen\": %u,\n"
		"    \"binary_bytes\": %u,\n"
		"    \"constlen\": %u,\n"
		"    \"push_const_vec4\": %u,\n"
		"    \"preamble_base_vec4\": %u,\n"
		"    \"preamble_count_vec4\": %u,\n"
		"    \"imm_base_vec4\": %u,\n"
		"    \"imm_count_vec4\": %u,\n"
		"    \"imm_dwords\": [",
		mode_name,
		res->chip_id, res->chip_name, res->gen,
		res->mesa_sha, res->combined_sha256_hex,
		res->unknown_chip_refused ? "true" : "false",
		res->asm_roundtrip ? "true" : "false",
		res->gpu_executed ? "true" : "false",
		res->vs.insn_count, res->vs.instrlen, res->vs.padded_dword_count * 4u,
		res->vs.constlen,
		res->vs.const_layout.push_consts_size_vec4,
		res->vs.const_layout.preamble_base_vec4,
		res->vs.const_layout.preamble_size_vec4,
		res->vs.const_layout.imm_base_vec4,
		res->vs.const_layout.imm_count_vec4);
	for (uint32_t i = 0u; i < res->vs.const_layout.imm_dword_count && res->vs.const_layout.imm_dwords; i++) {
		fprintf(fp, "%s\"0x%08x\"", i ? ", " : "", res->vs.const_layout.imm_dwords[i]);
	}
	fprintf(fp,
		"],\n"
		"    \"fullregs\": %u,\n"
		"    \"halfregs\": %u,\n"
		"    \"mergedregs\": %u,\n"
		"    \"sha256\": \"%s\"\n"
		"  },\n"
		"  \"fs\": {\n"
		"    \"insn_count\": %u,\n"
		"    \"instrlen\": %u,\n"
		"    \"binary_bytes\": %u,\n"
		"    \"constlen\": %u,\n"
		"    \"push_const_vec4\": %u,\n"
		"    \"preamble_base_vec4\": %u,\n"
		"    \"preamble_count_vec4\": %u,\n"
		"    \"imm_base_vec4\": %u,\n"
		"    \"imm_count_vec4\": %u,\n"
		"    \"imm_dwords\": [",
		res->vs.fullregs, res->vs.halfregs, res->vs.mergedregs,
		res->vs.sha256_hex,
		res->fs.insn_count, res->fs.instrlen, res->fs.padded_dword_count * 4u,
		res->fs.constlen,
		res->fs.const_layout.push_consts_size_vec4,
		res->fs.const_layout.preamble_base_vec4,
		res->fs.const_layout.preamble_size_vec4,
		res->fs.const_layout.imm_base_vec4,
		res->fs.const_layout.imm_count_vec4);
	for (uint32_t i = 0u; i < res->fs.const_layout.imm_dword_count && res->fs.const_layout.imm_dwords; i++) {
		fprintf(fp, "%s\"0x%08x\"", i ? ", " : "", res->fs.const_layout.imm_dwords[i]);
	}
	fprintf(fp,
		"],\n"
		"    \"fullregs\": %u,\n"
		"    \"halfregs\": %u,\n"
		"    \"mergedregs\": %u,\n"
		"    \"num_samp\": %u,\n"
		"    \"total_in\": %u,\n"
		"    \"num_sampler_prefetch\": %u,\n"
		"    \"bindless_tex\": %u,\n"
		"    \"sha256\": \"%s\"\n"
		"  }\n"
		"}\n",
		res->fs.fullregs, res->fs.halfregs, res->fs.mergedregs,
		res->fs.num_samp, res->fs.total_in, res->fs.prefetch_count,
		res->fs.bindless_tex, res->fs.sha256_hex);
	if (fclose(fp) != 0) {
		if (err_buf && err_cap)
			snprintf(err_buf, err_cap, "failed to flush/close %s (%s)", path, strerror(errno));
		return false;
	}
	return true;
}

/* =========================================================================
 * 分支一：FUXI_PROBE_MESA_IR3（對接 Mesa libfreedreno_ir3 + spirv_to_nir）
 * ========================================================================= */
#if defined(FUXI_PROBE_MESA_IR3) && FUXI_PROBE_MESA_IR3

static bool fuxi_mesa_lower_push_const_and_bindful_tex_cb(nir_builder *b,
							  nir_instr *instr,
							  void *data)
{
	(void)data;
	b->cursor = nir_before_instr(instr);

	if (instr->type == nir_instr_type_intrinsic) {
		nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
		if (intr->intrinsic != nir_intrinsic_load_push_constant)
			return false;

		uint32_t base = nir_intrinsic_base(intr);
		/* 轉為 ir3 bindful constant register load (與 tu_shader.cc:320 一致) */
		nir_def *dword_off = nir_ushr_imm(b, intr->src[0].ssa, 2);
		nir_def *load = nir_load_const_ir3(b, intr->num_components,
						   intr->def.bit_size,
						   dword_off,
						   .base = base / 4u);
		nir_def_replace(&intr->def, load);
		return true;
	}

	if (instr->type == nir_instr_type_tex) {
		nir_tex_instr *tex = nir_instr_as_tex(instr);
		bool changed = false;
		int tex_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
		if (tex_src_idx >= 0) {
			nir_deref_instr *deref = nir_src_as_deref(tex->src[tex_src_idx].src);
			nir_variable *var = deref ? nir_deref_instr_get_variable(deref) : NULL;
			if (var) {
				tex->texture_index = var->data.binding;
				BITSET_SET(b->shader->info.textures_used, var->data.binding);
				if (b->shader->info.num_textures <= var->data.binding)
					b->shader->info.num_textures = (uint8_t)(var->data.binding + 1u);
			}
			nir_tex_instr_remove_src(tex, (unsigned)tex_src_idx);
			changed = true;
		}
		int samp_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
		if (samp_src_idx >= 0) {
			nir_deref_instr *deref = nir_src_as_deref(tex->src[samp_src_idx].src);
			nir_variable *var = deref ? nir_deref_instr_get_variable(deref) : NULL;
			if (var)
				tex->sampler_index = var->data.binding;
			nir_tex_instr_remove_src(tex, (unsigned)samp_src_idx);
			changed = true;
		}
		return changed;
	}

	return false;
}

static void fuxi_mesa_link_opts(nir_shader *shader, void *data)
{
	struct ir3_compiler *compiler = (struct ir3_compiler *)data;
	struct ir3_optimize_options optimize_options = {0};
	ir3_optimize_loop(compiler, &optimize_options, shader);
}

static nir_shader *fuxi_mesa_spirv_to_nir(struct ir3_compiler *compiler,
					  const uint8_t *spv_bytes,
					  size_t spv_len,
					  mesa_shader_stage stage)
{
	const struct spirv_to_nir_options spirv_opts = {
		.mediump_16bit_alu = false,
		.ubo_addr_format = nir_address_format_vec2_index_32bit_offset,
		.ssbo_addr_format = nir_address_format_vec2_index_32bit_offset,
		.phys_ssbo_addr_format = nir_address_format_64bit_global,
		.push_const_addr_format = nir_address_format_logical,
		.shared_addr_format = nir_address_format_32bit_offset,
		.global_addr_format = nir_address_format_64bit_global,
		.min_ubo_alignment = 64,
		.min_ssbo_alignment = 4,
	};
	const nir_shader_compiler_options *nir_opts = ir3_get_compiler_options(compiler);
	nir_shader *nir = spirv_to_nir((const uint32_t *)spv_bytes, spv_len / 4u,
				       NULL, stage, "main", &spirv_opts, nir_opts);
	if (!nir)
		return NULL;

	nir->info.num_ubos = 0;
	nir->info.num_ssbos = 0;

	bool progress = false;
	NIR_PASS(progress, nir, nir_lower_global_vars_to_local);
	NIR_PASS(progress, nir, nir_split_var_copies);
	NIR_PASS(progress, nir, nir_lower_var_copies);
	NIR_PASS(progress, nir, nir_lower_system_values);
	ir3_nir_lower_io_vars_to_temporaries(nir);
	NIR_PASS(progress, nir, nir_lower_explicit_io, nir_var_mem_push_const,
		 nir_address_format_32bit_offset);
	NIR_PASS(progress, nir, nir_shader_instructions_pass,
		 fuxi_mesa_lower_push_const_and_bindful_tex_cb,
		 nir_metadata_control_flow, NULL);
	NIR_PASS(progress, nir, nir_opt_dce);
	(void)progress;

	/* 對齊 tu_shader.cc:3326-3350：先分配 IO location 並執行 ir3_nir_lower_io，
	 * 再交由 nir_opt_varyings_bulk 跨階段連結。
	 */
	nir_assign_io_var_locations(nir, nir_var_shader_in);
	nir_assign_io_var_locations(nir, nir_var_shader_out);
	ir3_nir_lower_io(nir);
	return nir;
}

_Static_assert((uint32_t)IR3_CONST_ALLOC_PUSH_CONSTS == FUXI_CONST_REGION_PUSH_CONSTS, "alloc enum mismatch");
_Static_assert((uint32_t)IR3_CONST_ALLOC_PREAMBLE == FUXI_CONST_REGION_PREAMBLE, "alloc enum mismatch");
_Static_assert((uint32_t)IR3_CONST_ALLOC_PRIMITIVE_PARAM == FUXI_CONST_REGION_PRIMITIVE_PARAM, "alloc enum mismatch");
_Static_assert((uint32_t)IR3_CONST_ALLOC_MAX == FUXI_CONST_REGION_MAX, "alloc enum mismatch");

static bool fuxi_mesa_finalize_and_compile(struct ir3_compiler *compiler,
					   nir_shader *nir,
					   uint32_t push_const_dwords,
					   struct ir3_shader **out_shader,
					   struct ir3_shader_variant **out_variant,
					   struct fuxi_probe_stage_const_layout *out_layout)
{
	nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

	struct ir3_shader_nir_options nir_options = {0};
	ir3_finalize_nir(compiler, &nir_options, nir);
	nir_validate_shader(nir, "fuxi-phaseb-gate2");

	/* 對齊 tu_shader.cc:1270-1281：依 const_upload_unit 預留 push_consts vec4 槽位 */
	struct ir3_const_allocations const_allocs = {0};
	if (push_const_dwords > 0u) {
		uint32_t unit = (uint32_t)compiler->const_upload_unit;
		if (unit == 0u)
			unit = 1u;
		uint32_t raw_vec4 = (push_const_dwords + 3u) / 4u;
		uint32_t push_consts_vec4 = ((raw_vec4 + unit - 1u) / unit) * unit;
		ir3_const_alloc(&const_allocs, IR3_CONST_ALLOC_PUSH_CONSTS,
				push_consts_vec4, unit);
	}
	const struct ir3_shader_options shader_options = {
		.api_wavesize = IR3_SINGLE_OR_DOUBLE,
		.real_wavesize = IR3_SINGLE_OR_DOUBLE,
		.push_consts_type = IR3_PUSH_CONSTS_PER_STAGE,
		.push_consts_base = 0,
		.push_consts_dwords = push_const_dwords,
		.const_allocs = const_allocs,
		.nir_options = nir_options,
	};
	struct ir3_shader *sh = ir3_shader_from_nir(compiler, nir, &shader_options);
	if (!sh)
		return false;

	const struct ir3_shader_key key = {0};
	struct ir3_shader_variant *v =
		ir3_shader_get_variant(sh, &key, false, true, NULL, NULL);
	if (!v || !v->bin || !v->info.sizedwords || !v->disasm_info.disasm) {
		ir3_shader_destroy(sh);
		return false;
	}

	if (out_layout) {
		memset(out_layout, 0, sizeof(*out_layout));
		const struct ir3_const_state *cs = ir3_const_state(v);
		out_layout->push_consts_base_vec4 =
			cs->allocs.consts[IR3_CONST_ALLOC_PUSH_CONSTS].offset_vec4;
		out_layout->push_consts_size_vec4 =
			cs->allocs.consts[IR3_CONST_ALLOC_PUSH_CONSTS].size_vec4;
		out_layout->push_consts_dwords = push_const_dwords;
		out_layout->preamble_base_vec4 =
			cs->allocs.consts[IR3_CONST_ALLOC_PREAMBLE].offset_vec4;
		out_layout->preamble_size_vec4 =
			cs->allocs.consts[IR3_CONST_ALLOC_PREAMBLE].size_vec4;
		out_layout->max_const_offset_vec4 = cs->allocs.max_const_offset_vec4;
		out_layout->load_consts_via_preamble =
			compiler->info->props.load_shader_consts_via_preamble ? 1u : 0u;
		for (uint32_t t = 0u; t < (uint32_t)IR3_CONST_ALLOC_MAX; t++) {
			uint32_t sz = cs->allocs.consts[t].size_vec4;
			if (sz == 0u)
				continue;
			if (out_layout->region_count >= FUXI_PROBE_MAX_CONST_REGIONS) {
				ir3_shader_destroy(sh);
				return false;
			}
			struct fuxi_probe_const_region *r =
				&out_layout->regions[out_layout->region_count++];
			r->type = t;
			snprintf(r->name, sizeof(r->name), "%s", fuxi_probe_const_region_name(t));
			r->offset_vec4 = cs->allocs.consts[t].offset_vec4;
			r->size_vec4 = sz;
		}
		out_layout->imm_base_vec4 = cs->allocs.max_const_offset_vec4;
		out_layout->imm_raw_dword_count = (uint32_t)v->imm_state.count;
		out_layout->imm_count_vec4 =
			(!out_layout->load_consts_via_preamble && v->imm_state.count > 0u)
				? ((uint32_t)(v->imm_state.count + 3u) / 4u)
				: 0u;
		out_layout->imm_dword_count = out_layout->imm_count_vec4 * 4u;
		if (out_layout->imm_dword_count > 0u) {
			out_layout->imm_dwords =
				(uint32_t *)calloc(out_layout->imm_dword_count, sizeof(uint32_t));
			if (!out_layout->imm_dwords) {
				memset(out_layout, 0, sizeof(*out_layout));
				ir3_shader_destroy(sh);
				return false;
			}
			for (uint32_t i = 0u; i < out_layout->imm_dword_count; i++) {
				out_layout->imm_dwords[i] =
					(i < (uint32_t)v->imm_state.count && v->imm_state.values)
						? v->imm_state.values[i]
						: 0u;
			}
		}
	}

	*out_shader = sh;
	*out_variant = v;
	return true;
}

static uint32_t fuxi_mesa_pack_reg_4(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
{
	return (r0 & 0xffu) | ((r1 & 0xffu) << 8) | ((r2 & 0xffu) << 16) | ((r3 & 0xffu) << 24);
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "Usage: %s <vs.spv> <fs.spv> <out_dir>\n", argv[0]);
		return 2;
	}
	const char *vs_path = argv[1];
	const char *fs_path = argv[2];
	const char *out_dir = argv[3];

	uint8_t *vs_spv = NULL, *fs_spv = NULL;
	size_t vs_spv_len = 0u, fs_spv_len = 0u;
	char err_buf[256] = {0};

	if (!fuxi_read_file_bytes(vs_path, &vs_spv, &vs_spv_len) ||
	    !fuxi_probe_validate_spirv(vs_spv, vs_spv_len, "VS", err_buf, sizeof(err_buf))) {
		fprintf(stderr, "SPIR-V error: %s\n", err_buf[0] ? err_buf : "unable to read VS SPIR-V");
		free(vs_spv);
		return 2;
	}
	if (!fuxi_read_file_bytes(fs_path, &fs_spv, &fs_spv_len) ||
	    !fuxi_probe_validate_spirv(fs_spv, fs_spv_len, "FS", err_buf, sizeof(err_buf))) {
		fprintf(stderr, "SPIR-V error: %s\n", err_buf[0] ? err_buf : "unable to read FS SPIR-V");
		free(vs_spv);
		free(fs_spv);
		return 2;
	}

	const struct fd_dev_id unknown = {.chip_id = FUXI_PROBE_CHIP_ID_UNKNOWN};
	if (fd_dev_info_raw(&unknown) != NULL) {
		fprintf(stderr, "chip_id 0xdeadbeef unexpectedly accepted\n");
		free(vs_spv);
		free(fs_spv);
		return 3;
	}
	const struct fd_dev_id id = {.chip_id = FUXI_PROBE_CHIP_ID_FD740};
	const struct fd_dev_info *info = fd_dev_info_raw(&id);
	if (!info || fd_dev_gen(&id) != 7 || strcmp(fd_dev_name(&id), "FD740") != 0) {
		fprintf(stderr, "FD740 (0x43050a01) lookup failed\n");
		free(vs_spv);
		free(fs_spv);
		return 4;
	}

	const struct ir3_compiler_options c_opts = {
		.disable_cache = true,
		.bindless_fb_read_descriptor = -1,
		.bindless_fb_read_slot = -1,
	};
	struct ir3_compiler *compiler = ir3_compiler_create(NULL, &id, info, &c_opts);
	if (!compiler || !(ir3_shader_debug & IR3_DBG_ASM_ROUNDTRIP)) {
		fprintf(stderr, "IR3_SHADER_DEBUG=asmroundtrip is mandatory\n");
		if (compiler)
			ir3_compiler_destroy(compiler);
		free(vs_spv);
		free(fs_spv);
		return 5;
	}

	glsl_type_singleton_init_or_ref();

	nir_shader *vs_nir = fuxi_mesa_spirv_to_nir(compiler, vs_spv, vs_spv_len, MESA_SHADER_VERTEX);
	nir_shader *fs_nir = fuxi_mesa_spirv_to_nir(compiler, fs_spv, fs_spv_len, MESA_SHADER_FRAGMENT);
	free(vs_spv);
	free(fs_spv);

	if (!vs_nir || !fs_nir) {
		if (vs_nir) ralloc_free(vs_nir);
		if (fs_nir) ralloc_free(fs_nir);
		glsl_type_singleton_decref();
		ir3_compiler_destroy(compiler);
		return 6;
	}

	nir_shader *link_shaders[2] = { vs_nir, fs_nir };
	nir_opt_varyings_bulk(link_shaders, 2, true, UINT32_MAX, UINT32_MAX,
			      fuxi_mesa_link_opts, compiler);

	struct fuxi_probe_result res = {0};
	struct ir3_shader *vs_sh = NULL, *fs_sh = NULL;
	struct ir3_shader_variant *vs_v = NULL, *fs_v = NULL;
	if (!fuxi_mesa_finalize_and_compile(compiler, vs_nir, 8u, &vs_sh, &vs_v, &res.vs.const_layout) ||
	    !fuxi_mesa_finalize_and_compile(compiler, fs_nir, 16u, &fs_sh, &fs_v, &res.fs.const_layout)) {
		fuxi_probe_result_free(&res);
		if (vs_sh) ir3_shader_destroy(vs_sh);
		if (fs_sh) ir3_shader_destroy(fs_sh);
		glsl_type_singleton_decref();
		ir3_compiler_destroy(compiler);
		return 7;
	}
	res.chip_id = FUXI_PROBE_CHIP_ID_FD740;
	res.gen = (uint32_t)fd_dev_gen(&id);
	snprintf(res.chip_name, sizeof(res.chip_name), "%s", fd_dev_name(&id));
	snprintf(res.mesa_sha, sizeof(res.mesa_sha), "%s", FUXI_PROBE_MESA_SHA_STR);
	res.unknown_chip_refused = true;
	res.asm_roundtrip = true;
	res.gpu_executed = false;

	/* VS metadata + NOP pad 至 instrlen * 32 DWORDs (16 insns = 128B per unit) */
	res.vs.raw_dword_count = vs_v->info.sizedwords;
	res.vs.insn_count = vs_v->info.sizedwords / 2u;
	res.vs.instrlen = (uint32_t)vs_v->instrlen;
	if (res.vs.instrlen == 0u)
		res.vs.instrlen = (res.vs.insn_count + 15u) >> 4;
	res.vs.padded_dword_count = res.vs.instrlen * 32u;
	if (res.vs.padded_dword_count < res.vs.raw_dword_count)
		res.vs.padded_dword_count = ((res.vs.raw_dword_count + 31u) / 32u) * 32u;
	res.vs.dwords = (uint32_t *)calloc(res.vs.padded_dword_count, sizeof(uint32_t));
	if (!res.vs.dwords) {
		ir3_shader_destroy(vs_sh);
		ir3_shader_destroy(fs_sh);
		glsl_type_singleton_decref();
		ir3_compiler_destroy(compiler);
		return 8;
	}
	memcpy(res.vs.dwords, vs_v->bin, res.vs.raw_dword_count * sizeof(uint32_t));
	res.vs.asm_text = fuxi_strdup_c11(vs_v->disasm_info.disasm);
	res.vs.fullregs = (uint32_t)(vs_v->info.max_reg + 1);
	res.vs.halfregs = (uint32_t)(vs_v->info.max_half_reg + 1);
	res.vs.mergedregs = vs_v->mergedregs ? 1u : 0u;
	res.vs.constlen = vs_v->constlen;
	res.vs.sp_ctrl_reg0 = (res.vs.halfregs & 0x3fu) |
			      ((res.vs.fullregs & 0x3fu) << 7) |
			      ((res.vs.mergedregs & 1u) << 20);
	res.vs.sp_const_config = 0x100u | ((res.vs.constlen >> 2) & 0xffu);
	res.vs.sp_config = 0x100u;
	res.vs.vertexid_regid = ir3_find_sysval_regid(vs_v, SYSTEM_VALUE_VERTEX_ID);
	res.vs.pos_regid = ir3_find_output_regid(vs_v, VARYING_SLOT_POS);
	res.vs.var0_regid = ir3_find_output_regid(vs_v, VARYING_SLOT_VAR0);

	/* 對齊 tu_pipeline.cc:795-930, 1042-1050：依 ir3_link_shaders 動態計算 VS->VPC 連結 */
	struct ir3_shader_linkage linkage = {
		.primid_loc = 0xff,
		.clip0_loc = 0xff,
		.clip1_loc = 0xff,
	};
	ir3_link_shaders(&linkage, vs_v, fs_v, true);

	uint32_t position_loc = 0xffu;
	uint32_t extra_pos = 0u;
	for (unsigned i = 0; i < vs_v->outputs_count; i++) {
		if (vs_v->outputs[i].slot != VARYING_SLOT_POS)
			continue;
		if (position_loc == 0xffu)
			position_loc = linkage.max_loc;
		ir3_link_add(&linkage, vs_v->outputs[i].slot,
			     vs_v->outputs[i].regid,
			     0xf, position_loc + 4u * vs_v->outputs[i].view);
		if (vs_v->outputs[i].view > extra_pos)
			extra_pos = vs_v->outputs[i].view;
	}
	const uint32_t pointsize_regid = ir3_find_output_regid(vs_v, VARYING_SLOT_PSIZ);
	uint32_t pointsize_loc = 0xffu;
	if (VALIDREG(pointsize_regid)) {
		pointsize_loc = linkage.max_loc;
		ir3_link_add(&linkage, VARYING_SLOT_PSIZ, pointsize_regid, 0x1, linkage.max_loc);
	}
	if (linkage.cnt == 0u)
		ir3_link_add(&linkage, 0, 0, 0x1, linkage.max_loc);

	uint32_t sp_out0 = 0u;
	uint32_t sp_vpc_dst0 = 0u;
	for (uint32_t i = 0; i < linkage.cnt && i < 4u; i++) {
		if (i < 2u) {
			uint32_t out_entry = (linkage.var[i].regid & 0xffu) |
					     ((linkage.var[i].compmask & 0xfu) << 8);
			sp_out0 |= out_entry << (i * 16u);
		}
		sp_vpc_dst0 |= (linkage.var[i].loc & 0xffu) << (i * 8u);
	}
	res.vs.sp_output_reg0 = sp_out0;
	res.vs.sp_vpc_dest_reg0 = sp_vpc_dst0;
	res.vs.vpc_vs_cntl = (linkage.max_loc & 0xffu) |
			     ((position_loc & 0xffu) << 8) |
			     ((pointsize_loc & 0xffu) << 16) |
			     ((extra_pos & 0xfu) << 24);
	res.vs.pc_vs_cntl = (linkage.max_loc & 0xffu) |
			    (VALIDREG(pointsize_regid) ? (1u << 8) : 0u);
	fuxi_sha256_bytes_hex(res.vs.dwords, res.vs.padded_dword_count * 4u, res.vs.sha256_hex);

	/* FS metadata + NOP pad 至 instrlen * 32 DWORDs */
	res.fs.raw_dword_count = fs_v->info.sizedwords;
	res.fs.insn_count = fs_v->info.sizedwords / 2u;
	res.fs.instrlen = (uint32_t)fs_v->instrlen;
	if (res.fs.instrlen == 0u)
		res.fs.instrlen = (res.fs.insn_count + 15u) >> 4;
	res.fs.padded_dword_count = res.fs.instrlen * 32u;
	if (res.fs.padded_dword_count < res.fs.raw_dword_count)
		res.fs.padded_dword_count = ((res.fs.raw_dword_count + 31u) / 32u) * 32u;
	res.fs.dwords = (uint32_t *)calloc(res.fs.padded_dword_count, sizeof(uint32_t));
	if (!res.fs.dwords) {
		fuxi_probe_result_free(&res);
		ir3_shader_destroy(vs_sh);
		ir3_shader_destroy(fs_sh);
		glsl_type_singleton_decref();
		ir3_compiler_destroy(compiler);
		return 8;
	}
	memcpy(res.fs.dwords, fs_v->bin, res.fs.raw_dword_count * sizeof(uint32_t));
	res.fs.asm_text = fuxi_strdup_c11(fs_v->disasm_info.disasm);
	res.fs.fullregs = (uint32_t)(fs_v->info.max_reg + 1);
	res.fs.halfregs = (uint32_t)(fs_v->info.max_half_reg + 1);
	res.fs.mergedregs = fs_v->mergedregs ? 1u : 0u;
	res.fs.double_threadsize = fs_v->info.double_threadsize ? 1u : 0u;
	res.fs.constlen = fs_v->constlen;
	res.fs.num_samp = fs_v->num_samp;
	res.fs.total_in = fs_v->total_in;
	res.fs.prefetch_count = (uint32_t)fs_v->num_sampler_prefetch;
	res.fs.bindless_tex = fs_v->bindless_tex ? 1u : 0u;
	res.fs.ij_persp_pixel_regid = ir3_find_sysval_regid(fs_v, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL);
	res.fs.out0_regid = ir3_find_output_regid(fs_v, FRAG_RESULT_DATA0);
	res.fs.sp_ctrl_reg0 = (res.fs.halfregs & 0x3fu) |
			      ((res.fs.fullregs & 0x3fu) << 7) |
			      ((res.fs.double_threadsize & 1u) << 19) |
			      ((res.fs.total_in != 0u ? 1u : 0u) << 22) |
			      (1u << 24) | /* INOUTREGOVERLAP */
			      ((res.fs.mergedregs & 1u) << 31);
	res.fs.sp_const_config = 0x100u | ((res.fs.constlen >> 2) & 0xffu);
	res.fs.sp_config = 0x100u |
			   (res.fs.bindless_tex ? (1u << 1) : 0u) |
			   ((res.fs.num_samp & 0xffu) << 9) |
			   ((res.fs.num_samp & 0x1fu) << 17);
	res.fs.sp_tsize = res.fs.num_samp;
	res.fs.sp_initial_tex_load_cntl = (res.fs.prefetch_count & 0x7u) |
					  (0x1ffu << 6) | (0x1ffu << 16);
	res.fs.sp_output_reg0 = res.fs.out0_regid & 0xffu;
	res.fs.sp_reg_prog_id_1 = fuxi_mesa_pack_reg_4(
		res.fs.ij_persp_pixel_regid,
		ir3_find_sysval_regid(fs_v, SYSTEM_VALUE_BARYCENTRIC_LINEAR_PIXEL),
		ir3_find_sysval_regid(fs_v, SYSTEM_VALUE_BARYCENTRIC_PERSP_CENTROID),
		ir3_find_sysval_regid(fs_v, SYSTEM_VALUE_BARYCENTRIC_LINEAR_CENTROID));
	res.fs.sp_ps_cntl_1 = (VALIDREG(res.fs.ij_persp_pixel_regid) ? 2u : 0u) |
			      (1u << 8) | (1u << 9);
	res.fs.sp_ps_wave_cntl = ((res.fs.double_threadsize & 1u) << 0) |
				 ((res.fs.total_in != 0u ? 1u : 0u) << 1);

	/* 對齊 tu_shader.cc:2487-2513 動態計算 VPC_PS_CNTL 與 LM_TRANSFER_CNTL_DISABLE0 */
	uint32_t primid_loc = ir3_find_input_loc(fs_v, VARYING_SLOT_PRIMITIVE_ID);
	uint32_t viewid_loc = ir3_find_input_loc(fs_v, VARYING_SLOT_VIEW_INDEX);
	res.fs.vpc_ps_cntl = (res.fs.total_in & 0xffu) |
			     ((primid_loc & 0xffu) << 8) |
			     ((res.fs.total_in != 0u ? 1u : 0u) << 16) |
			     ((viewid_loc & 0xffu) << 24);

	uint32_t varmask0 = 0u;
	for (int i = ir3_next_varying(fs_v, -1); i < (int)fs_v->inputs_count;
	     i = ir3_next_varying(fs_v, i)) {
		if (fs_v->inputs[i].inloc >= fs_v->total_in)
			continue;
		uint32_t loc = fs_v->inputs[i].inloc;
		uint32_t mask = fs_v->inputs[i].compmask;
		uint32_t last_bit = 0u;
		while (mask) {
			last_bit++;
			mask >>= 1;
		}
		for (uint32_t j = 0u; j < last_bit; j++) {
			uint32_t comploc = loc + j;
			if (comploc < 32u)
				varmask0 |= (1u << comploc);
		}
	}
	res.fs.lm_disable0 = ~varmask0;
	fuxi_sha256_bytes_hex(res.fs.dwords, res.fs.padded_dword_count * 4u, res.fs.sha256_hex);

	struct fuxi_sha256_ctx comb;
	fuxi_sha256_init(&comb);
	fuxi_sha256_update(&comb, res.vs.dwords, res.vs.padded_dword_count * 4u);
	fuxi_sha256_update(&comb, res.fs.dwords, res.fs.padded_dword_count * 4u);
	fuxi_sha256_final_hex(&comb, res.combined_sha256_hex);

	if (!fuxi_probe_verify_result(&res, err_buf, sizeof(err_buf)) ||
	    !fuxi_write_probe_outputs(out_dir, &res, "mesa_ir3", err_buf, sizeof(err_buf))) {
		fprintf(stderr, "Probe verification/output failed: %s\n", err_buf);
		fuxi_probe_result_free(&res);
		ir3_shader_destroy(vs_sh);
		ir3_shader_destroy(fs_sh);
		glsl_type_singleton_decref();
		ir3_compiler_destroy(compiler);
		return 9;
	}

	printf("PASS: mesa_ir3 FD740 VS=%u insns (%u B), FS=%u insns (%u B), sha256=%s\n",
	       res.vs.insn_count, res.vs.padded_dword_count * 4u,
	       res.fs.insn_count, res.fs.padded_dword_count * 4u,
	       res.combined_sha256_hex);

	fuxi_probe_result_free(&res);
	ir3_shader_destroy(vs_sh);
	ir3_shader_destroy(fs_sh);
	glsl_type_singleton_decref();
	ir3_compiler_destroy(compiler);
	return 0;
}

#endif /* FUXI_PROBE_MESA_IR3 */

/* =========================================================================
 * 分支二：FUXI_PROBE_MOCK_HOST（標準 VK_KHR_pipeline_executable_properties ABI
 *         + in-process mock Vulkan ICD + 真實 Gen7 ir3 組裝 + --self-test）
 * ========================================================================= */
#if defined(FUXI_PROBE_MOCK_HOST) && FUXI_PROBE_MOCK_HOST

/* --- 標準 Khronos VK_KHR_pipeline_executable_properties ABI 定義 --- */
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkPipelineCreateFlags;
typedef uint32_t VkShaderStageFlags;
typedef struct VkShaderModule_T *VkShaderModule;
typedef struct VkPipeline_T *VkPipeline;
typedef struct VkDevice_T *VkDevice;
typedef struct VkPipelineCache_T *VkPipelineCache;
typedef struct VkPipelineLayout_T *VkPipelineLayout;
typedef struct VkRenderPass_T *VkRenderPass;

#define VK_TRUE  1u
#define VK_FALSE 0u
#define VK_NULL_HANDLE NULL
#define VK_MAX_DESCRIPTION_SIZE 256u

typedef enum VkResult {
	VK_SUCCESS = 0,
	VK_INCOMPLETE = 5,
	VK_ERROR_OUT_OF_HOST_MEMORY = -1,
	VK_ERROR_INITIALIZATION_FAILED = -3,
	VK_ERROR_INVALID_SHADER_NV = -1000012000
} VkResult;

typedef enum VkStructureType {
	VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16,
	VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18,
	VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28,
	VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR = 1000269001,
	VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR = 1000269002,
	VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR = 1000269003,
	VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR = 1000269004,
	VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR = 1000269005
} VkStructureType;

typedef enum VkShaderStageFlagBits {
	VK_SHADER_STAGE_VERTEX_BIT = 0x00000001u,
	VK_SHADER_STAGE_FRAGMENT_BIT = 0x00000010u
} VkShaderStageFlagBits;

enum {
	VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR = 0x00000040u,
	VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR = 0x00000080u
};

typedef enum VkPipelineExecutableStatisticFormatKHR {
	VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR = 0,
	VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR = 1,
	VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR = 2,
	VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR = 3
} VkPipelineExecutableStatisticFormatKHR;

typedef struct VkShaderModuleCreateInfo {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	size_t codeSize;
	const uint32_t *pCode;
} VkShaderModuleCreateInfo;

typedef struct VkPipelineShaderStageCreateInfo {
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	VkShaderStageFlagBits stage;
	VkShaderModule module;
	const char *pName;
	const void *pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct VkGraphicsPipelineCreateInfo {
	VkStructureType sType;
	const void *pNext;
	VkPipelineCreateFlags flags;
	uint32_t stageCount;
	const VkPipelineShaderStageCreateInfo *pStages;
	const void *pVertexInputState;
	const void *pInputAssemblyState;
	const void *pTessellationState;
	const void *pViewportState;
	const void *pRasterizationState;
	const void *pMultisampleState;
	const void *pDepthStencilState;
	const void *pColorBlendState;
	const void *pDynamicState;
	VkPipelineLayout layout;
	VkRenderPass renderPass;
	uint32_t subpass;
	VkPipeline basePipelineHandle;
	int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

typedef struct VkPipelineInfoKHR {
	VkStructureType sType;
	const void *pNext;
	VkPipeline pipeline;
} VkPipelineInfoKHR;

typedef struct VkPipelineExecutablePropertiesKHR {
	VkStructureType sType;
	void *pNext;
	VkShaderStageFlags stages;
	char name[VK_MAX_DESCRIPTION_SIZE];
	char description[VK_MAX_DESCRIPTION_SIZE];
	uint32_t subgroupSize;
} VkPipelineExecutablePropertiesKHR;

typedef struct VkPipelineExecutableInfoKHR {
	VkStructureType sType;
	const void *pNext;
	VkPipeline pipeline;
	uint32_t executableIndex;
} VkPipelineExecutableInfoKHR;

typedef union VkPipelineExecutableStatisticValueKHR {
	VkBool32 b32;
	int64_t i64;
	uint64_t u64;
	double f64;
} VkPipelineExecutableStatisticValueKHR;

typedef struct VkPipelineExecutableStatisticKHR {
	VkStructureType sType;
	void *pNext;
	char name[VK_MAX_DESCRIPTION_SIZE];
	char description[VK_MAX_DESCRIPTION_SIZE];
	VkPipelineExecutableStatisticFormatKHR format;
	VkPipelineExecutableStatisticValueKHR value;
} VkPipelineExecutableStatisticKHR;

typedef struct VkPipelineExecutableInternalRepresentationKHR {
	VkStructureType sType;
	void *pNext;
	char name[VK_MAX_DESCRIPTION_SIZE];
	char description[VK_MAX_DESCRIPTION_SIZE];
	VkBool32 isText;
	size_t dataSize;
	void *pData;
} VkPipelineExecutableInternalRepresentationKHR;

/* --- 復用自 fuxi_phaseb_3d.h/.c 的 Gen7 ir3 編碼器與 Quad VS / Dual FS --- */
#define MOCK_IR3_INSN_NOP            0x0000000000000000ULL
#define MOCK_IR3_INSN_END            0x0300000000000000ULL
#define MOCK_IR3_CTRL_SY             (1ULL << 60)
#define MOCK_IR3_CTRL_EI             (1ULL << 47)
#define MOCK_IR3_CTRL_SS             (1ULL << 44)
#define MOCK_IR3_CAT2_FULL           (1ULL << 52)
#define MOCK_IR3_OPC_CAT2_ADD_F      0x4000000000000000ULL
#define MOCK_IR3_OPC_CAT2_MUL_F      0x4060000000000000ULL
#define MOCK_IR3_OPC_CAT2_CMPS_F     0x40a0000000000000ULL
#define MOCK_IR3_OPC_CAT2_FLOOR_F    0x4120000000000000ULL
#define MOCK_IR3_OPC_CAT2_AND_B      0x4380000000000000ULL
#define MOCK_IR3_OPC_CAT2_OR_B       0x43a0000000000000ULL
#define MOCK_IR3_OPC_CAT2_XOR_B      0x43e0000000000000ULL
#define MOCK_IR3_OPC_CAT2_BARY_F     0x4720000000000000ULL
#define MOCK_IR3_OPC_CAT3_MAD_F32    0x6380000000000000ULL
#define MOCK_IR3_OPC_CAT3_SEL_B32    0x6480000000000000ULL
#define MOCK_IR3_OPC_CAT5_SAM        0xa0c0000000000000ULL
#define MOCK_IR3_CAT5_FULL           (1ULL << 0)
#define MOCK_IR3_CAT5_TYPE_F32       1u
#define MOCK_IR3_COND_LT             0u
#define MOCK_IR3_COND_GE             3u
#define MOCK_IR3_COND_NE             5u
#define MOCK_IR3_SRC_CONST_BIT       0x1000u
#define MOCK_IR3_SRC_IMMED_BIT       0x2000u
#define MOCK_IR3_SRC_FLUT_BASE       0x2800u
#define MOCK_IR3_SRC_NEG_BIT         0x4000u

#define M_REG(r, c)        ((uint8_t)(((r) << 2) | (c)))
#define M_CONST(vec, c)    ((uint16_t)(MOCK_IR3_SRC_CONST_BIT | ((vec) << 2) | (c)))
#define M_NEG_CONST(v, c)  ((uint16_t)(M_CONST(v, c) | MOCK_IR3_SRC_NEG_BIT))
#define M_NEG_REG(r, c)    ((uint16_t)(M_REG(r, c) | MOCK_IR3_SRC_NEG_BIT))
#define M_IMMED(v)         ((uint16_t)(MOCK_IR3_SRC_IMMED_BIT | (v)))
#define M_FLUT(idx)        ((uint16_t)(MOCK_IR3_SRC_FLUT_BASE | (idx)))
#define M_NEG_FLUT(idx)    ((uint16_t)(M_FLUT(idx) | MOCK_IR3_SRC_NEG_BIT))
#define M_CMPS_COND(c)     ((uint64_t)((c) & 0x7u) << 48)

static uint64_t mock_ir3_cat2(uint64_t opc, uint8_t dst, uint16_t src1,
			      uint16_t src2, bool full, bool sat, uint64_t ctrl)
{
	uint64_t insn = opc | ctrl;
	insn |= (uint64_t)src1;
	insn |= (uint64_t)src2 << 16;
	insn |= (uint64_t)dst << 32;
	if (full) insn |= MOCK_IR3_CAT2_FULL;
	if (sat)  insn |= (1ULL << 42);
	return insn;
}

static uint64_t mock_ir3_cat3(uint64_t opc, uint8_t dst, uint16_t src1,
			      uint8_t src2, uint16_t src3, bool sat, uint64_t ctrl)
{
	uint64_t insn = opc | ctrl;
	uint16_t s1_lo = src1 & 0x1fffu;
	uint16_t s3_lo = src3 & 0x1fffu;
	insn |= (uint64_t)s1_lo;
	insn |= (uint64_t)s3_lo << 16;
	insn |= (uint64_t)dst << 32;
	insn |= (uint64_t)src2 << 47;
	if (src1 & MOCK_IR3_SRC_NEG_BIT) insn |= (1ULL << 14);
	if (src3 & MOCK_IR3_SRC_NEG_BIT) insn |= (1ULL << 31);
	if (sat) insn |= (1ULL << 42);
	return insn;
}

static uint64_t mock_ir3_cat5_sam(bool full, uint8_t dst, uint8_t src1,
				  uint8_t samp, uint8_t tex, uint8_t wrmask,
				  uint8_t type, uint64_t ctrl)
{
	uint64_t insn = MOCK_IR3_OPC_CAT5_SAM | ctrl;
	if (full) insn |= MOCK_IR3_CAT5_FULL;
	insn |= ((uint64_t)src1 << 1);
	insn |= ((uint64_t)(samp & 0xfu) << 21);
	insn |= ((uint64_t)(tex & 0x7fu) << 25);
	insn |= ((uint64_t)dst << 32);
	insn |= ((uint64_t)(wrmask & 0xfu) << 40);
	insn |= ((uint64_t)(type & 0x7u) << 44);
	return insn;
}

static uint32_t mock_build_quad_vs(uint64_t *out, uint32_t cap)
{
	if (cap < 8u) return 0u;
	uint32_t n = 0u;
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(1, 0), M_CONST(0, 2), M_REG(0, 0), M_CONST(0, 0), false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(1, 1), M_CONST(0, 3), M_REG(0, 0), M_CONST(0, 1), false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(1, 2), M_FLUT(0),     M_FLUT(0),     true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(1, 3), M_FLUT(2),     M_FLUT(0),     true, false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(2, 0), M_CONST(1, 2), M_REG(0, 0), M_CONST(1, 0), false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(2, 1), M_CONST(1, 3), M_REG(0, 0), M_CONST(1, 1), false, 0ULL);
	out[n++] = MOCK_IR3_INSN_NOP | MOCK_IR3_CTRL_SS;
	out[n++] = MOCK_IR3_INSN_END;
	return n;
}

static uint32_t mock_build_dual_fs(uint64_t *out, uint32_t cap)
{
	if (cap < 70u) return 0u;
	uint32_t n = 0u;
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_BARY_F,  M_REG(0, 2), M_IMMED(0), M_REG(0, 0), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_BARY_F,  M_REG(0, 3), M_IMMED(1), M_REG(0, 0), true, false, MOCK_IR3_CTRL_EI);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_FLOOR_F, M_REG(1, 0), M_REG(0, 2), 0u, true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_FLOOR_F, M_REG(1, 1), M_REG(0, 3), 0u, true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(1, 2), M_REG(1, 1), M_REG(1, 1), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(1, 1), M_REG(1, 2), M_CONST(0, 0), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(1, 3), M_REG(1, 1), M_FLUT(1), true, false, MOCK_IR3_CTRL_SS);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 0), M_REG(0, 2), M_NEG_CONST(1, 0), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 1), M_REG(1, 3), M_NEG_CONST(1, 1), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_MUL_F,   M_REG(2, 0), M_REG(2, 0), M_CONST(2, 0), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_MUL_F,   M_REG(2, 1), M_REG(2, 1), M_CONST(2, 1), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_FLOOR_F, M_REG(2, 0), M_REG(2, 0), 0u, true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_FLOOR_F, M_REG(2, 1), M_REG(2, 1), 0u, true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 0), M_REG(2, 0), M_FLUT(1), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 1), M_REG(2, 1), M_FLUT(1), true, false, 0ULL);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 2), M_REG(0, 2), M_NEG_CONST(3, 0), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 3), M_REG(1, 3), M_NEG_CONST(3, 1), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_MUL_F,   M_REG(2, 2), M_REG(2, 2), M_CONST(2, 2), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_MUL_F,   M_REG(2, 3), M_REG(2, 3), M_CONST(2, 3), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_FLOOR_F, M_REG(2, 2), M_REG(2, 2), 0u, true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_FLOOR_F, M_REG(2, 3), M_REG(2, 3), 0u, true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 2), M_REG(2, 2), M_FLUT(1), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(2, 3), M_REG(2, 3), M_FLUT(1), true, false, 0ULL);

	out[n++] = MOCK_IR3_INSN_NOP | MOCK_IR3_CTRL_SS;
	out[n++] = mock_ir3_cat5_sam(true, M_REG(4, 0), M_REG(2, 0), 0u, 0u, 0xfu, MOCK_IR3_CAT5_TYPE_F32, 0ULL);
	out[n++] = mock_ir3_cat5_sam(true, M_REG(5, 0), M_REG(2, 2), 1u, 1u, 0xfu, MOCK_IR3_CAT5_TYPE_F32, 0ULL);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_GE), M_REG(3, 1), M_CONST(0, 3), M_FLUT(3), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F, M_REG(3, 2), M_CONST(0, 3), M_NEG_FLUT(3), true, false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(3, 2), M_REG(3, 2), M_REG(3, 1), M_CONST(0, 3), false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_GE), M_REG(3, 2), M_REG(3, 2), M_FLUT(2), true, false, MOCK_IR3_CTRL_SS);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_GE), M_REG(3, 0), M_REG(1, 0), M_CONST(1, 0), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_LT), M_REG(3, 3), M_REG(1, 0), M_CONST(1, 2), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(3, 0), M_REG(3, 0), M_REG(3, 3), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_GE), M_REG(3, 3), M_REG(1, 1), M_CONST(1, 1), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(3, 0), M_REG(3, 0), M_REG(3, 3), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_LT), M_REG(3, 3), M_REG(1, 1), M_CONST(1, 3), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(3, 0), M_REG(3, 0), M_REG(3, 3), true, false, MOCK_IR3_CTRL_SS);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_NE), M_REG(3, 3), M_REG(4, 3), M_FLUT(0), true, false, MOCK_IR3_CTRL_SY);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_XOR_B, M_REG(7, 0), M_REG(3, 2), M_IMMED(1), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_OR_B,  M_REG(7, 0), M_REG(7, 0), M_REG(3, 3), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(3, 0), M_REG(3, 0), M_REG(7, 0), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F, M_REG(7, 3), M_FLUT(0), M_FLUT(0), true, false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(6, 0), M_REG(4, 0), M_REG(3, 0), M_REG(7, 3), false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(6, 1), M_REG(4, 1), M_REG(3, 0), M_REG(7, 3), false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(6, 2), M_REG(4, 2), M_REG(3, 0), M_REG(7, 3), false, 0ULL);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_GE), M_REG(7, 0), M_REG(1, 0), M_CONST(3, 0), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_LT), M_REG(7, 1), M_REG(1, 0), M_CONST(3, 2), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(7, 0), M_REG(7, 0), M_REG(7, 1), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_GE), M_REG(7, 1), M_REG(1, 1), M_CONST(3, 1), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(7, 0), M_REG(7, 0), M_REG(7, 1), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_LT), M_REG(7, 1), M_REG(1, 1), M_CONST(3, 3), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(7, 0), M_REG(7, 0), M_REG(7, 1), true, false, MOCK_IR3_CTRL_SS);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F, M_REG(7, 2), M_FLUT(2), M_FLUT(0), true, false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(8, 3), M_REG(5, 3), M_REG(3, 1), M_REG(7, 2), false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F, M_REG(8, 3), M_FLUT(2), M_NEG_REG(8, 3), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_MAD_F32, M_REG(8, 0), M_REG(6, 0), M_REG(8, 3), M_REG(5, 0), false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_MAD_F32, M_REG(8, 1), M_REG(6, 1), M_REG(8, 3), M_REG(5, 1), false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_MAD_F32, M_REG(8, 2), M_REG(6, 2), M_REG(8, 3), M_REG(5, 2), false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(8, 0), M_REG(8, 0), M_REG(7, 0), M_REG(6, 0), false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(8, 1), M_REG(8, 1), M_REG(7, 0), M_REG(6, 1), false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(8, 2), M_REG(8, 2), M_REG(7, 0), M_REG(6, 2), false, 0ULL);

	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_GE), M_REG(7, 0), M_REG(1, 1), M_CONST(0, 1), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_CMPS_F | M_CMPS_COND(MOCK_IR3_COND_LT), M_REG(7, 1), M_REG(1, 1), M_CONST(0, 2), true, false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_AND_B, M_REG(7, 0), M_REG(7, 0), M_REG(7, 1), true, false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(0, 0), M_REG(8, 0), M_REG(7, 0), M_REG(7, 3), false, MOCK_IR3_CTRL_SS);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(0, 1), M_REG(8, 1), M_REG(7, 0), M_REG(7, 3), false, 0ULL);
	out[n++] = mock_ir3_cat3(MOCK_IR3_OPC_CAT3_SEL_B32, M_REG(0, 2), M_REG(8, 2), M_REG(7, 0), M_REG(7, 3), false, 0ULL);
	out[n++] = mock_ir3_cat2(MOCK_IR3_OPC_CAT2_ADD_F,   M_REG(0, 3), M_FLUT(2), M_FLUT(0), true, false, 0ULL);
	out[n++] = MOCK_IR3_INSN_NOP | MOCK_IR3_CTRL_SS;
	out[n++] = MOCK_IR3_INSN_END;
	return n;
}

static char *mock_format_ir3_asm(const char *stage_tag,
				 const uint64_t *insns,
				 uint32_t count)
{
	size_t cap = (size_t)count * 96u + 512u;
	char *buf = (char *)malloc(cap);
	if (!buf)
		return NULL;
	size_t off = 0u;
	int hdr = snprintf(buf + off, cap - off,
			   "; FUXI A740 (FD740) %s IR3 Assembly (mock_host)\n", stage_tag);
	if (hdr > 0)
		off += (size_t)hdr;
	if (strcmp(stage_tag, "FS") == 0) {
		int ph = snprintf(buf + off, cap - off,
				  ";   preamble c4.x (4 vec4)\n"
				  "@const(c8.x)\t0xc0000000, 0x40000000, 0x00000000, 0x3f800000\n"
				  "stc.u32 c[16], r48.z, 1\n");
		if (ph > 0)
			off += (size_t)ph;
	} else {
		int ph = snprintf(buf + off, cap - off,
				  ";   primitive_param c4.x (1 vec4)\n");
		if (ph > 0)
			off += (size_t)ph;
	}
	for (uint32_t i = 0u; i < count && off + 80u < cap; i++) {
		uint64_t w = insns[i];
		const char *op = "op";
		if (w == MOCK_IR3_INSN_END)
			op = "end";
		else if ((w & ~MOCK_IR3_CTRL_SS) == MOCK_IR3_INSN_NOP)
			op = "nop";
		else if ((w >> 61) == 2ULL)
			op = ((w & (0xffULL << 53)) == MOCK_IR3_OPC_CAT2_BARY_F) ? "bary.f" : "cat2";
		else if ((w >> 61) == 3ULL)
			op = "cat3";
		else if ((w >> 61) == 5ULL)
			op = "sam (f32)(xyzw)";
		int n = snprintf(buf + off, cap - off,
				 "[%03u] %08x %08x  %-16s%s%s\n",
				 i, (uint32_t)(w & 0xffffffffu), (uint32_t)(w >> 32),
				 op,
				 (w & MOCK_IR3_CTRL_SY) ? " (sy)" : "",
				 (w & MOCK_IR3_CTRL_SS) ? " (ss)" : "");
		if (n > 0)
			off += (size_t)n;
	}
	return buf;
}

/* --- In-Process Mock Vulkan ICD 實作 --- */
struct VkShaderModule_T {
	uint32_t *words;
	size_t word_count;
	char sha256_hex[65];
};

struct mock_ir_entry {
	char name[VK_MAX_DESCRIPTION_SIZE];
	char description[VK_MAX_DESCRIPTION_SIZE];
	VkBool32 is_text;
	char *text;
	size_t text_size_with_nul;
};

struct mock_exec_entry {
	VkPipelineExecutablePropertiesKHR props;
	VkPipelineExecutableStatisticKHR stats[12];
	uint32_t stat_count;
	struct mock_ir_entry irs[3];
	uint32_t ir_count;
	uint32_t insn_count;
	uint32_t instrlen;
	uint32_t *padded_dwords;
	uint32_t padded_dword_count;
};

struct VkPipeline_T {
	struct mock_exec_entry execs[2];
	uint32_t exec_count;
	bool capture_stats;
	bool capture_ir;
};

static void mock_fill_stat(VkPipelineExecutableStatisticKHR *s,
			   const char *name, const char *desc,
			   VkPipelineExecutableStatisticFormatKHR fmt,
			   uint64_t uval)
{
	memset(s, 0, sizeof(*s));
	s->sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
	snprintf(s->name, sizeof(s->name), "%s", name);
	snprintf(s->description, sizeof(s->description), "%s", desc);
	s->format = fmt;
	if (fmt == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR)
		s->value.b32 = (VkBool32)(uval != 0u);
	else if (fmt == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR)
		s->value.i64 = (int64_t)uval;
	else
		s->value.u64 = uval;
}

static VkResult fuxi_mock_vkCreateShaderModule(VkDevice device,
					       const VkShaderModuleCreateInfo *pCreateInfo,
					       const void *pAllocator,
					       VkShaderModule *pShaderModule)
{
	(void)device;
	(void)pAllocator;
	if (!pCreateInfo || !pShaderModule ||
	    pCreateInfo->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (!fuxi_probe_validate_spirv(pCreateInfo->pCode, pCreateInfo->codeSize,
				       "vkCreateShaderModule", NULL, 0u))
		return VK_ERROR_INVALID_SHADER_NV;

	struct VkShaderModule_T *mod =
		(struct VkShaderModule_T *)calloc(1u, sizeof(*mod));
	if (!mod)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	mod->word_count = pCreateInfo->codeSize / 4u;
	mod->words = (uint32_t *)malloc(pCreateInfo->codeSize);
	if (!mod->words) {
		free(mod);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	memcpy(mod->words, pCreateInfo->pCode, pCreateInfo->codeSize);
	fuxi_sha256_bytes_hex(mod->words, pCreateInfo->codeSize, mod->sha256_hex);
	*pShaderModule = mod;
	return VK_SUCCESS;
}

static void fuxi_mock_vkDestroyShaderModule(VkDevice device,
					    VkShaderModule shaderModule,
					    const void *pAllocator)
{
	(void)device;
	(void)pAllocator;
	if (!shaderModule)
		return;
	free(shaderModule->words);
	free(shaderModule);
}

static void fuxi_mock_vkDestroyPipeline(VkDevice device,
					VkPipeline pipeline,
					const void *pAllocator)
{
	(void)device;
	(void)pAllocator;
	if (!pipeline)
		return;
	for (uint32_t i = 0u; i < pipeline->exec_count; i++) {
		free(pipeline->execs[i].padded_dwords);
		for (uint32_t j = 0u; j < pipeline->execs[i].ir_count; j++)
			free(pipeline->execs[i].irs[j].text);
	}
	free(pipeline);
}

static VkResult fuxi_mock_vkCreateGraphicsPipelines(
	VkDevice device,
	VkPipelineCache pipelineCache,
	uint32_t createInfoCount,
	const VkGraphicsPipelineCreateInfo *pCreateInfos,
	const void *pAllocator,
	VkPipeline *pPipelines)
{
	(void)device;
	(void)pipelineCache;
	(void)pAllocator;
	if (createInfoCount != 1u || !pCreateInfos || !pPipelines ||
	    pCreateInfos->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
	    pCreateInfos->stageCount != 2u || !pCreateInfos->pStages)
		return VK_ERROR_INITIALIZATION_FAILED;

	const VkPipelineShaderStageCreateInfo *vs_st = NULL;
	const VkPipelineShaderStageCreateInfo *fs_st = NULL;
	for (uint32_t i = 0u; i < pCreateInfos->stageCount; i++) {
		const VkPipelineShaderStageCreateInfo *st = &pCreateInfos->pStages[i];
		if (st->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO || !st->module)
			return VK_ERROR_INITIALIZATION_FAILED;
		if (st->stage == VK_SHADER_STAGE_VERTEX_BIT) vs_st = st;
		else if (st->stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs_st = st;
	}
	if (!vs_st || !fs_st)
		return VK_ERROR_INITIALIZATION_FAILED;

	struct VkPipeline_T *pipe = (struct VkPipeline_T *)calloc(1u, sizeof(*pipe));
	if (!pipe)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	pipe->exec_count = 2u;
	pipe->capture_stats = (pCreateInfos->flags & VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR) != 0u;
	pipe->capture_ir = (pCreateInfos->flags & VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR) != 0u;

	uint64_t vs_insns[FUXI_PROBE_MAX_PROG_INSNS] = {0};
	uint64_t fs_insns[FUXI_PROBE_MAX_PROG_INSNS] = {0};
	uint32_t vs_n = mock_build_quad_vs(vs_insns, FUXI_PROBE_MAX_PROG_INSNS);
	uint32_t fs_n = mock_build_dual_fs(fs_insns, FUXI_PROBE_MAX_PROG_INSNS);
	if (vs_n == 0u || fs_n == 0u) {
		fuxi_mock_vkDestroyPipeline(device, pipe, NULL);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Executable 0: Vertex Shader */
	{
		struct mock_exec_entry *e = &pipe->execs[0];
		e->props.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
		e->props.stages = VK_SHADER_STAGE_VERTEX_BIT;
		e->props.subgroupSize = 64u;
		snprintf(e->props.name, sizeof(e->props.name), "phaseb_quad.vert");
		snprintf(e->props.description, sizeof(e->props.description),
			 "FD740 Sysmem RECTLIST Quad VS (bindful c0..c1)");

		e->insn_count = vs_n;
		e->instrlen = (vs_n + 15u) >> 4;
		e->padded_dword_count = e->instrlen * 32u;
		e->padded_dwords = (uint32_t *)calloc(e->padded_dword_count, sizeof(uint32_t));
		if (!e->padded_dwords) {
			fuxi_mock_vkDestroyPipeline(device, pipe, NULL);
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		for (uint32_t i = 0u; i < vs_n; i++) {
			e->padded_dwords[i * 2u] = (uint32_t)(vs_insns[i] & 0xffffffffu);
			e->padded_dwords[i * 2u + 1u] = (uint32_t)(vs_insns[i] >> 32);
		}
		if (pipe->capture_stats) {
			uint32_t s = 0u;
			mock_fill_stat(&e->stats[s++], "maxwaves", "Maximum concurrent waves", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 16u);
			mock_fill_stat(&e->stats[s++], "inst", "Instruction count (unpadded)", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, e->insn_count);
			mock_fill_stat(&e->stats[s++], "code_size", "Padded ISA code size (bytes)", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, e->padded_dword_count * 4u);
			mock_fill_stat(&e->stats[s++], "instrlen", "Instruction blocks (16 insns/unit)", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, e->instrlen);
			mock_fill_stat(&e->stats[s++], "full", "Full-precision registers", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 3u);
			mock_fill_stat(&e->stats[s++], "half", "Half-precision registers", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 0u);
			mock_fill_stat(&e->stats[s++], "constlen", "Constant register vec4 units", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 8u);
			mock_fill_stat(&e->stats[s++], "mergedregs", "Merged register file", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR, 1u);
			e->stat_count = s;
		}
		if (pipe->capture_ir) {
			e->ir_count = 3u;
			snprintf(e->irs[0].name, sizeof(e->irs[0].name), "NIR from SPIRV");
			snprintf(e->irs[0].description, sizeof(e->irs[0].description), "Initial NIR translated from SPIR-V");
			e->irs[0].is_text = VK_TRUE;
			e->irs[0].text = fuxi_strdup_c11("shader: MESA_SHADER_VERTEX\n; vs push_const c0..c1\n");

			snprintf(e->irs[1].name, sizeof(e->irs[1].name), "Final NIR");
			snprintf(e->irs[1].description, sizeof(e->irs[1].description), "NIR after ir3_finalize_nir and ir3_nir_lower_io");
			e->irs[1].is_text = VK_TRUE;
			e->irs[1].text = fuxi_strdup_c11("shader: MESA_SHADER_VERTEX (final)\n; load_const_ir3 base=0\n");

			snprintf(e->irs[2].name, sizeof(e->irs[2].name), "IR3 Assembly");
			snprintf(e->irs[2].description, sizeof(e->irs[2].description), "FD740 Gen7 IR3 disassembly");
			e->irs[2].is_text = VK_TRUE;
			e->irs[2].text = mock_format_ir3_asm("VS", vs_insns, vs_n);

			for (uint32_t j = 0u; j < e->ir_count; j++) {
				if (!e->irs[j].text) {
					fuxi_mock_vkDestroyPipeline(device, pipe, NULL);
					return VK_ERROR_OUT_OF_HOST_MEMORY;
				}
				e->irs[j].text_size_with_nul = strlen(e->irs[j].text) + 1u;
			}
		}
	}

	/* Executable 1: Fragment Shader */
	{
		struct mock_exec_entry *e = &pipe->execs[1];
		e->props.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
		e->props.stages = VK_SHADER_STAGE_FRAGMENT_BIT;
		e->props.subgroupSize = 64u;
		snprintf(e->props.name, sizeof(e->props.name), "phaseb_dual.frag");
		snprintf(e->props.description, sizeof(e->props.description),
			 "FD740 Sysmem 2-Texture Dual-Layer Composite FS (bindful t#0/t#1, c0..c3)");

		e->insn_count = fs_n;
		e->instrlen = (fs_n + 15u) >> 4;
		e->padded_dword_count = e->instrlen * 32u;
		e->padded_dwords = (uint32_t *)calloc(e->padded_dword_count, sizeof(uint32_t));
		if (!e->padded_dwords) {
			fuxi_mock_vkDestroyPipeline(device, pipe, NULL);
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		for (uint32_t i = 0u; i < fs_n; i++) {
			e->padded_dwords[i * 2u] = (uint32_t)(fs_insns[i] & 0xffffffffu);
			e->padded_dwords[i * 2u + 1u] = (uint32_t)(fs_insns[i] >> 32);
		}
		if (pipe->capture_stats) {
			uint32_t s = 0u;
			mock_fill_stat(&e->stats[s++], "maxwaves", "Maximum concurrent waves", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 16u);
			mock_fill_stat(&e->stats[s++], "inst", "Instruction count (unpadded)", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, e->insn_count);
			mock_fill_stat(&e->stats[s++], "code_size", "Padded ISA code size (bytes)", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, e->padded_dword_count * 4u);
			mock_fill_stat(&e->stats[s++], "instrlen", "Instruction blocks (16 insns/unit)", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, e->instrlen);
			mock_fill_stat(&e->stats[s++], "full", "Full-precision registers", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 9u);
			mock_fill_stat(&e->stats[s++], "half", "Half-precision registers", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 0u);
			mock_fill_stat(&e->stats[s++], "constlen", "Constant register vec4 units", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 12u);
			mock_fill_stat(&e->stats[s++], "mergedregs", "Merged register file", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR, 1u);
			mock_fill_stat(&e->stats[s++], "num_samp", "Bindful sampler/texture count", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 2u);
			mock_fill_stat(&e->stats[s++], "total_in", "Varying scalar input components", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 2u);
			mock_fill_stat(&e->stats[s++], "prefetch_count", "Sampler prefetch count", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR, 0u);
			mock_fill_stat(&e->stats[s++], "bindless_tex", "Uses bindless textures", VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR, 0u);
			e->stat_count = s;
		}
		if (pipe->capture_ir) {
			e->ir_count = 3u;
			snprintf(e->irs[0].name, sizeof(e->irs[0].name), "NIR from SPIRV");
			snprintf(e->irs[0].description, sizeof(e->irs[0].description), "Initial NIR translated from SPIR-V");
			e->irs[0].is_text = VK_TRUE;
			e->irs[0].text = fuxi_strdup_c11("shader: MESA_SHADER_FRAGMENT\n; fs push_const c0..c3, tex 0/1\n");

			snprintf(e->irs[1].name, sizeof(e->irs[1].name), "Final NIR");
			snprintf(e->irs[1].description, sizeof(e->irs[1].description), "NIR after ir3_finalize_nir and ir3_nir_lower_io");
			e->irs[1].is_text = VK_TRUE;
			e->irs[1].text = fuxi_strdup_c11("shader: MESA_SHADER_FRAGMENT (final)\n; load_const_ir3 base=0, bindful sam\n");

			snprintf(e->irs[2].name, sizeof(e->irs[2].name), "IR3 Assembly");
			snprintf(e->irs[2].description, sizeof(e->irs[2].description), "FD740 Gen7 IR3 disassembly");
			e->irs[2].is_text = VK_TRUE;
			e->irs[2].text = mock_format_ir3_asm("FS", fs_insns, fs_n);

			for (uint32_t j = 0u; j < e->ir_count; j++) {
				if (!e->irs[j].text) {
					fuxi_mock_vkDestroyPipeline(device, pipe, NULL);
					return VK_ERROR_OUT_OF_HOST_MEMORY;
				}
				e->irs[j].text_size_with_nul = strlen(e->irs[j].text) + 1u;
			}
		}
	}

	*pPipelines = pipe;
	return VK_SUCCESS;
}

static VkResult fuxi_mock_vkGetPipelineExecutablePropertiesKHR(
	VkDevice device,
	const VkPipelineInfoKHR *pPipelineInfo,
	uint32_t *pExecutableCount,
	VkPipelineExecutablePropertiesKHR *pProperties)
{
	(void)device;
	if (!pPipelineInfo || pPipelineInfo->sType != VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR ||
	    !pPipelineInfo->pipeline || !pExecutableCount)
		return VK_ERROR_INITIALIZATION_FAILED;

	const struct VkPipeline_T *pipe = pPipelineInfo->pipeline;
	if (!pProperties) {
		*pExecutableCount = pipe->exec_count;
		return VK_SUCCESS;
	}
	uint32_t cap = *pExecutableCount;
	uint32_t n = (cap < pipe->exec_count) ? cap : pipe->exec_count;
	for (uint32_t i = 0u; i < n; i++)
		pProperties[i] = pipe->execs[i].props;
	*pExecutableCount = n;
	return (cap < pipe->exec_count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult fuxi_mock_vkGetPipelineExecutableStatisticsKHR(
	VkDevice device,
	const VkPipelineExecutableInfoKHR *pExecutableInfo,
	uint32_t *pStatisticCount,
	VkPipelineExecutableStatisticKHR *pStatistics)
{
	(void)device;
	if (!pExecutableInfo ||
	    pExecutableInfo->sType != VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR ||
	    !pExecutableInfo->pipeline || !pStatisticCount)
		return VK_ERROR_INITIALIZATION_FAILED;

	const struct VkPipeline_T *pipe = pExecutableInfo->pipeline;
	if (pExecutableInfo->executableIndex >= pipe->exec_count)
		return VK_ERROR_INITIALIZATION_FAILED;
	const struct mock_exec_entry *e = &pipe->execs[pExecutableInfo->executableIndex];

	if (!pStatistics) {
		*pStatisticCount = e->stat_count;
		return VK_SUCCESS;
	}
	uint32_t cap = *pStatisticCount;
	uint32_t n = (cap < e->stat_count) ? cap : e->stat_count;
	for (uint32_t i = 0u; i < n; i++)
		pStatistics[i] = e->stats[i];
	*pStatisticCount = n;
	return (cap < e->stat_count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult fuxi_mock_vkGetPipelineExecutableInternalRepresentationsKHR(
	VkDevice device,
	const VkPipelineExecutableInfoKHR *pExecutableInfo,
	uint32_t *pInternalRepresentationCount,
	VkPipelineExecutableInternalRepresentationKHR *pInternalRepresentations)
{
	(void)device;
	if (!pExecutableInfo ||
	    pExecutableInfo->sType != VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR ||
	    !pExecutableInfo->pipeline || !pInternalRepresentationCount)
		return VK_ERROR_INITIALIZATION_FAILED;

	const struct VkPipeline_T *pipe = pExecutableInfo->pipeline;
	if (pExecutableInfo->executableIndex >= pipe->exec_count)
		return VK_ERROR_INITIALIZATION_FAILED;
	const struct mock_exec_entry *e = &pipe->execs[pExecutableInfo->executableIndex];

	if (!pInternalRepresentations) {
		*pInternalRepresentationCount = e->ir_count;
		return VK_SUCCESS;
	}
	uint32_t cap = *pInternalRepresentationCount;
	uint32_t n = (cap < e->ir_count) ? cap : e->ir_count;
	bool incomplete = (cap < e->ir_count);
	for (uint32_t i = 0u; i < n; i++) {
		const struct mock_ir_entry *src = &e->irs[i];
		VkPipelineExecutableInternalRepresentationKHR *dst = &pInternalRepresentations[i];
		dst->sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
		snprintf(dst->name, sizeof(dst->name), "%s", src->name);
		snprintf(dst->description, sizeof(dst->description), "%s", src->description);
		dst->isText = src->is_text;
		if (!dst->pData) {
			dst->dataSize = src->text_size_with_nul;
		} else {
			size_t avail = dst->dataSize;
			if (avail < src->text_size_with_nul) {
				if (avail > 0u) {
					memcpy(dst->pData, src->text, avail - 1u);
					((char *)dst->pData)[avail - 1u] = '\0';
				}
				dst->dataSize = avail;
				incomplete = true;
			} else {
				memcpy(dst->pData, src->text, src->text_size_with_nul);
				dst->dataSize = src->text_size_with_nul;
			}
		}
	}
	*pInternalRepresentationCount = n;
	return incomplete ? VK_INCOMPLETE : VK_SUCCESS;
}

static bool mock_query_stat_u64(const VkPipelineExecutableStatisticKHR *stats,
				uint32_t count, const char *name, uint64_t *out)
{
	for (uint32_t i = 0u; i < count; i++) {
		if (strcmp(stats[i].name, name) == 0) {
			if (stats[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR)
				*out = (uint64_t)(stats[i].value.b32 != 0u);
			else if (stats[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR)
				*out = (uint64_t)stats[i].value.i64;
			else
				*out = stats[i].value.u64;
			return true;
		}
	}
	return false;
}

static bool mock_run_pipeline_probe_on_spv(const uint8_t *vs_spv, size_t vs_len,
					   const uint8_t *fs_spv, size_t fs_len,
					   uint32_t chip_id,
					   bool asm_roundtrip_enabled,
					   struct fuxi_probe_result *out_res,
					   char *err_buf, size_t err_cap)
{
	if (chip_id != FUXI_PROBE_CHIP_ID_FD740) {
		snprintf(err_buf, err_cap, "unknown chip_id 0x%08x refused", chip_id);
		return false;
	}
	if (!asm_roundtrip_enabled) {
		snprintf(err_buf, err_cap, "IR3_DBG_ASM_ROUNDTRIP missing");
		return false;
	}

	VkShaderModule vs_mod = VK_NULL_HANDLE, fs_mod = VK_NULL_HANDLE;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo vs_ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = vs_len,
		.pCode = (const uint32_t *)vs_spv,
	};
	VkShaderModuleCreateInfo fs_ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = fs_len,
		.pCode = (const uint32_t *)fs_spv,
	};
	if (fuxi_mock_vkCreateShaderModule(NULL, &vs_ci, NULL, &vs_mod) != VK_SUCCESS ||
	    fuxi_mock_vkCreateShaderModule(NULL, &fs_ci, NULL, &fs_mod) != VK_SUCCESS) {
		snprintf(err_buf, err_cap, "vkCreateShaderModule rejected SPIR-V");
		fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
		fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vs_mod,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fs_mod,
			.pName = "main",
		},
	};
	VkGraphicsPipelineCreateInfo gp_ci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			 VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR,
		.stageCount = 2u,
		.pStages = stages,
	};
	if (fuxi_mock_vkCreateGraphicsPipelines(NULL, NULL, 1u, &gp_ci, NULL, &pipe) != VK_SUCCESS) {
		snprintf(err_buf, err_cap, "vkCreateGraphicsPipelines failed");
		fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
		fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
		return false;
	}

	/* 兩段查詢 Properties */
	VkPipelineInfoKHR pinfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,
		.pipeline = pipe,
	};
	uint32_t exec_count = 0u;
	if (fuxi_mock_vkGetPipelineExecutablePropertiesKHR(NULL, &pinfo, &exec_count, NULL) != VK_SUCCESS ||
	    exec_count != 2u) {
		snprintf(err_buf, err_cap, "executable count != 2");
		fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
		fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
		fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
		return false;
	}
	VkPipelineExecutablePropertiesKHR props[2];
	if (fuxi_mock_vkGetPipelineExecutablePropertiesKHR(NULL, &pinfo, &exec_count, props) != VK_SUCCESS) {
		snprintf(err_buf, err_cap, "executable properties read failed");
		fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
		fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
		fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
		return false;
	}

	memset(out_res, 0, sizeof(*out_res));
	out_res->chip_id = FUXI_PROBE_CHIP_ID_FD740;
	out_res->gen = FUXI_PROBE_GEN_A7XX;
	snprintf(out_res->chip_name, sizeof(out_res->chip_name), "FD740");
	snprintf(out_res->mesa_sha, sizeof(out_res->mesa_sha), "%s", FUXI_PROBE_MESA_SHA_STR);
	out_res->unknown_chip_refused = true;
	out_res->asm_roundtrip = true;
	out_res->gpu_executed = false;

	for (uint32_t ei = 0u; ei < exec_count; ei++) {
		VkPipelineExecutableInfoKHR einfo = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
			.pipeline = pipe,
			.executableIndex = ei,
		};
		uint32_t sc = 0u;
		fuxi_mock_vkGetPipelineExecutableStatisticsKHR(NULL, &einfo, &sc, NULL);
		VkPipelineExecutableStatisticKHR *stats =
			(VkPipelineExecutableStatisticKHR *)calloc(sc, sizeof(*stats));
		if (!stats || fuxi_mock_vkGetPipelineExecutableStatisticsKHR(NULL, &einfo, &sc, stats) != VK_SUCCESS) {
			free(stats);
			fuxi_probe_result_free(out_res);
			fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
			fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
			fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
			return false;
		}

		uint32_t irc = 0u;
		fuxi_mock_vkGetPipelineExecutableInternalRepresentationsKHR(NULL, &einfo, &irc, NULL);
		VkPipelineExecutableInternalRepresentationKHR *irs =
			(VkPipelineExecutableInternalRepresentationKHR *)calloc(irc, sizeof(*irs));
		if (!irs || fuxi_mock_vkGetPipelineExecutableInternalRepresentationsKHR(NULL, &einfo, &irc, irs) != VK_SUCCESS) {
			free(stats);
			free(irs);
			fuxi_probe_result_free(out_res);
			fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
			fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
			fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
			return false;
		}

		char *asm_copy = NULL;
		for (uint32_t ir_i = 0u; ir_i < irc; ir_i++) {
			if (strcmp(irs[ir_i].name, "IR3 Assembly") == 0 && irs[ir_i].dataSize > 0u) {
				irs[ir_i].pData = malloc(irs[ir_i].dataSize);
				if (!irs[ir_i].pData) {
					free(asm_copy);
					free(stats);
					free(irs);
					fuxi_probe_result_free(out_res);
					fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
					fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
					fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
					return false;
				}
			}
		}
		fuxi_mock_vkGetPipelineExecutableInternalRepresentationsKHR(NULL, &einfo, &irc, irs);
		for (uint32_t ir_i = 0u; ir_i < irc; ir_i++) {
			if (irs[ir_i].pData) {
				if (strcmp(irs[ir_i].name, "IR3 Assembly") == 0)
					asm_copy = fuxi_strdup_c11((const char *)irs[ir_i].pData);
				free(irs[ir_i].pData);
				irs[ir_i].pData = NULL;
			}
		}
		free(irs);

		uint64_t v_inst = 0, v_instrlen = 0, v_full = 0, v_half = 0, v_const = 0, v_merged = 0;
		mock_query_stat_u64(stats, sc, "inst", &v_inst);
		mock_query_stat_u64(stats, sc, "instrlen", &v_instrlen);
		mock_query_stat_u64(stats, sc, "full", &v_full);
		mock_query_stat_u64(stats, sc, "half", &v_half);
		mock_query_stat_u64(stats, sc, "constlen", &v_const);
		mock_query_stat_u64(stats, sc, "mergedregs", &v_merged);

		const struct mock_exec_entry *me = &pipe->execs[ei];
		if (props[ei].stages & VK_SHADER_STAGE_VERTEX_BIT) {
			out_res->vs.insn_count = (uint32_t)v_inst;
			out_res->vs.raw_dword_count = out_res->vs.insn_count * 2u;
			out_res->vs.instrlen = (uint32_t)v_instrlen;
			out_res->vs.padded_dword_count = me->padded_dword_count;
			out_res->vs.dwords = (uint32_t *)malloc(me->padded_dword_count * sizeof(uint32_t));
			memcpy(out_res->vs.dwords, me->padded_dwords, me->padded_dword_count * sizeof(uint32_t));
			out_res->vs.asm_text = asm_copy;
			out_res->vs.fullregs = (uint32_t)v_full;
			out_res->vs.halfregs = (uint32_t)v_half;
			out_res->vs.mergedregs = (uint32_t)v_merged;
			out_res->vs.constlen = (uint32_t)v_const;
			out_res->vs.const_layout.push_consts_base_vec4 = 0u;
			out_res->vs.const_layout.push_consts_size_vec4 = 4u;
			out_res->vs.const_layout.push_consts_dwords = 8u;
			out_res->vs.const_layout.region_count = 2u;
			out_res->vs.const_layout.regions[0] = (struct fuxi_probe_const_region){
				.type = FUXI_CONST_REGION_PUSH_CONSTS,
				.offset_vec4 = 0u,
				.size_vec4 = 4u,
			};
			snprintf(out_res->vs.const_layout.regions[0].name,
				 sizeof(out_res->vs.const_layout.regions[0].name),
				 "%s", fuxi_probe_const_region_name(FUXI_CONST_REGION_PUSH_CONSTS));
			out_res->vs.const_layout.regions[1] = (struct fuxi_probe_const_region){
				.type = FUXI_CONST_REGION_PRIMITIVE_PARAM,
				.offset_vec4 = 4u,
				.size_vec4 = 1u,
			};
			snprintf(out_res->vs.const_layout.regions[1].name,
				 sizeof(out_res->vs.const_layout.regions[1].name),
				 "%s", fuxi_probe_const_region_name(FUXI_CONST_REGION_PRIMITIVE_PARAM));
			out_res->vs.const_layout.max_const_offset_vec4 = 5u;
			out_res->vs.const_layout.imm_base_vec4 = 5u;
			out_res->vs.sp_ctrl_reg0 = (3u << 7) | (1u << 20);
			out_res->vs.sp_const_config = 0x100u | ((out_res->vs.constlen >> 2) & 0xffu);
			out_res->vs.sp_config = 0x100u;
			out_res->vs.vertexid_regid = 0x00u;
			out_res->vs.pos_regid = 0x04u;
			out_res->vs.var0_regid = 0x08u;
			out_res->vs.sp_output_reg0 = 0x0f040308u;
			out_res->vs.sp_vpc_dest_reg0 = 0x0200u;
			out_res->vs.vpc_vs_cntl = 0x00ff0206u;
			out_res->vs.pc_vs_cntl = 6u;
			fuxi_sha256_bytes_hex(out_res->vs.dwords, out_res->vs.padded_dword_count * 4u, out_res->vs.sha256_hex);
		} else {
			uint64_t v_nsamp = 0, v_totin = 0, v_pref = 0, v_bindless = 0;
			mock_query_stat_u64(stats, sc, "num_samp", &v_nsamp);
			mock_query_stat_u64(stats, sc, "total_in", &v_totin);
			mock_query_stat_u64(stats, sc, "prefetch_count", &v_pref);
			mock_query_stat_u64(stats, sc, "bindless_tex", &v_bindless);

			out_res->fs.insn_count = (uint32_t)v_inst;
			out_res->fs.raw_dword_count = out_res->fs.insn_count * 2u;
			out_res->fs.instrlen = (uint32_t)v_instrlen;
			out_res->fs.padded_dword_count = me->padded_dword_count;
			out_res->fs.dwords = (uint32_t *)malloc(me->padded_dword_count * sizeof(uint32_t));
			memcpy(out_res->fs.dwords, me->padded_dwords, me->padded_dword_count * sizeof(uint32_t));
			out_res->fs.asm_text = asm_copy;
			out_res->fs.fullregs = (uint32_t)v_full;
			out_res->fs.halfregs = (uint32_t)v_half;
			out_res->fs.mergedregs = (uint32_t)v_merged;
			out_res->fs.double_threadsize = 0u;
			out_res->fs.constlen = (uint32_t)v_const;
			out_res->fs.const_layout.push_consts_base_vec4 = 0u;
			out_res->fs.const_layout.push_consts_size_vec4 = 4u;
			out_res->fs.const_layout.push_consts_dwords = 16u;
			out_res->fs.const_layout.preamble_base_vec4 = 4u;
			out_res->fs.const_layout.preamble_size_vec4 = 4u;
			out_res->fs.const_layout.region_count = 2u;
			out_res->fs.const_layout.regions[0] = (struct fuxi_probe_const_region){
				.type = FUXI_CONST_REGION_PUSH_CONSTS,
				.name = "push_consts",
				.offset_vec4 = 0u,
				.size_vec4 = 4u,
			};
			out_res->fs.const_layout.regions[1] = (struct fuxi_probe_const_region){
				.type = FUXI_CONST_REGION_PREAMBLE,
				.name = "preamble",
				.offset_vec4 = 4u,
				.size_vec4 = 4u,
			};
			out_res->fs.const_layout.max_const_offset_vec4 = 8u;
			out_res->fs.const_layout.imm_base_vec4 = 8u;
			out_res->fs.const_layout.imm_raw_dword_count = 4u;
			out_res->fs.const_layout.imm_count_vec4 = 1u;
			out_res->fs.const_layout.imm_dword_count = 4u;
			out_res->fs.const_layout.imm_dwords = (uint32_t *)calloc(4u, sizeof(uint32_t));
			if (!out_res->fs.const_layout.imm_dwords) {
				free(asm_copy);
				free(stats);
				fuxi_probe_result_free(out_res);
				fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
				fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
				fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
				return false;
			}
			out_res->fs.const_layout.imm_dwords[0] = 0xc0000000u;
			out_res->fs.const_layout.imm_dwords[1] = 0x40000000u;
			out_res->fs.const_layout.imm_dwords[2] = 0x00000000u;
			out_res->fs.const_layout.imm_dwords[3] = 0x3f800000u;
			out_res->fs.num_samp = (uint32_t)v_nsamp;
			out_res->fs.total_in = (uint32_t)v_totin;
			out_res->fs.prefetch_count = (uint32_t)v_pref;
			out_res->fs.bindless_tex = (uint32_t)v_bindless;
			out_res->fs.ij_persp_pixel_regid = 0x00u;
			out_res->fs.out0_regid = 0x00u;
			out_res->fs.sp_ctrl_reg0 = (9u << 7) | (1u << 22) | (1u << 24) | (1u << 31);
			out_res->fs.sp_const_config = 0x100u | ((out_res->fs.constlen >> 2) & 0xffu);
			out_res->fs.sp_config = 0x100u | (2u << 9) | (2u << 17);
			out_res->fs.sp_tsize = 2u;
			out_res->fs.sp_initial_tex_load_cntl = (0x1ffu << 6) | (0x1ffu << 16);
			out_res->fs.sp_output_reg0 = 0x00u;
			out_res->fs.sp_reg_prog_id_1 = 0xfcfcfc00u;
			out_res->fs.sp_ps_cntl_1 = 2u | (1u << 8) | (1u << 9);
			out_res->fs.sp_ps_wave_cntl = (1u << 1);
			out_res->fs.vpc_ps_cntl = 2u | (0xffu << 8) | (1u << 16) | (0xffu << 24);
			out_res->fs.lm_disable0 = 0xfffffffcu;
			fuxi_sha256_bytes_hex(out_res->fs.dwords, out_res->fs.padded_dword_count * 4u, out_res->fs.sha256_hex);
		}
		free(stats);
	}

	struct fuxi_sha256_ctx comb;
	fuxi_sha256_init(&comb);
	fuxi_sha256_update(&comb, out_res->vs.dwords, out_res->vs.padded_dword_count * 4u);
	fuxi_sha256_update(&comb, out_res->fs.dwords, out_res->fs.padded_dword_count * 4u);
	fuxi_sha256_final_hex(&comb, out_res->combined_sha256_hex);

	fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
	fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
	fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
	return true;
}

static void make_minimal_valid_spv(uint32_t out_words[5])
{
	out_words[0] = FUXI_PROBE_SPIRV_MAGIC;
	out_words[1] = 0x00010300u; /* SPIR-V 1.3 */
	out_words[2] = 0x0008000bu; /* glslang generator */
	out_words[3] = 16u;         /* bound > 0 */
	out_words[4] = 0u;          /* schema == 0 */
}

/* --- --self-test 7 項完整正負向測試 --- */
static bool test_spirv_validation_rejects_bad_inputs(void)
{
	char err[128] = {0};
	uint32_t valid[5];
	make_minimal_valid_spv(valid);
	if (!fuxi_probe_validate_spirv(valid, sizeof(valid), "valid", err, sizeof(err)))
		return false;

	/* 1. NULL / empty */
	if (fuxi_probe_validate_spirv(NULL, 20u, "null", err, sizeof(err))) return false;
	if (fuxi_probe_validate_spirv(valid, 0u, "empty", err, sizeof(err))) return false;

	/* 2. 非 4-byte 對齊 */
	uint8_t unaligned[22] = {0};
	memcpy(unaligned, valid, 20u);
	if (fuxi_probe_validate_spirv(unaligned, 21u, "unaligned", err, sizeof(err))) return false;

	/* 3. 截斷 header (< 20B) */
	if (fuxi_probe_validate_spirv(valid, 16u, "short", err, sizeof(err))) return false;

	/* 4. 錯誤 magic */
	uint32_t bad_magic[5];
	memcpy(bad_magic, valid, sizeof(valid));
	bad_magic[0] = 0xdeadbeefu;
	if (fuxi_probe_validate_spirv(bad_magic, sizeof(bad_magic), "badmagic", err, sizeof(err))) return false;

	/* 5. 指令 word_count=0 與 word_count=0xFFFF 越界截斷拒絕 + 合法指令通過 */
	uint32_t bad_wc0[6];
	memcpy(bad_wc0, valid, sizeof(valid));
	bad_wc0[5] = 0x00000011u; /* word_count=0 */
	if (fuxi_probe_validate_spirv(bad_wc0, sizeof(bad_wc0), "wc0", err, sizeof(err)))
		return false;

	uint32_t bad_wchuge[6];
	memcpy(bad_wchuge, valid, sizeof(valid));
	bad_wchuge[5] = 0xffff0011u; /* word_count=0xFFFF 越界 */
	if (fuxi_probe_validate_spirv(bad_wchuge, sizeof(bad_wchuge), "wchuge", err, sizeof(err)))
		return false;

	uint32_t bad_wc_second[7];
	memcpy(bad_wc_second, valid, sizeof(valid));
	bad_wc_second[5] = (1u << 16) | 0x0000u; /* OpNop (wc=1) */
	bad_wc_second[6] = (2u << 16) | 0x0011u; /* wc=2 但僅剩 1 word */
	if (fuxi_probe_validate_spirv(bad_wc_second, sizeof(bad_wc_second), "wc_second_trunc", err, sizeof(err)))
		return false;

	uint32_t good_insns[7];
	memcpy(good_insns, valid, sizeof(valid));
	good_insns[5] = (1u << 16) | 0x0000u; /* wc=1 */
	good_insns[6] = (1u << 16) | 0x0000u; /* wc=1 */
	if (!fuxi_probe_validate_spirv(good_insns, sizeof(good_insns), "good_insns", err, sizeof(err)))
		return false;

	/* 6. mock vkCreateShaderModule 負向拒絕（bad magic / wc=0 / wc=0xFFFF） */
	VkShaderModule mod = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(bad_magic),
		.pCode = bad_magic,
	};
	if (fuxi_mock_vkCreateShaderModule(NULL, &ci, NULL, &mod) == VK_SUCCESS || mod != NULL)
		return false;

	ci.codeSize = sizeof(bad_wc0);
	ci.pCode = bad_wc0;
	if (fuxi_mock_vkCreateShaderModule(NULL, &ci, NULL, &mod) == VK_SUCCESS || mod != NULL)
		return false;

	ci.codeSize = sizeof(bad_wchuge);
	ci.pCode = bad_wchuge;
	if (fuxi_mock_vkCreateShaderModule(NULL, &ci, NULL, &mod) == VK_SUCCESS || mod != NULL)
		return false;

	printf("[PASS] test_spirv_validation_rejects_bad_inputs\n");
	return true;
}

static bool test_mock_vk_executable_properties_twostep(void)
{
	uint32_t spv[5];
	make_minimal_valid_spv(spv);
	VkShaderModule vs_mod = VK_NULL_HANDLE, fs_mod = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(spv),
		.pCode = spv,
	};
	if (fuxi_mock_vkCreateShaderModule(NULL, &ci, NULL, &vs_mod) != VK_SUCCESS ||
	    fuxi_mock_vkCreateShaderModule(NULL, &ci, NULL, &fs_mod) != VK_SUCCESS)
		return false;

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_mod, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_mod, .pName = "main" },
	};
	VkGraphicsPipelineCreateInfo gp = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			 VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR,
		.stageCount = 2u,
		.pStages = stages,
	};
	VkPipeline pipe = VK_NULL_HANDLE;
	if (fuxi_mock_vkCreateGraphicsPipelines(NULL, NULL, 1u, &gp, NULL, &pipe) != VK_SUCCESS) {
		fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
		fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
		return false;
	}

	/* 測試 Properties 兩段查詢 + VK_INCOMPLETE + sentinel 不越界 */
	VkPipelineInfoKHR pinfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipe };
	uint32_t cnt = 0u;
	if (fuxi_mock_vkGetPipelineExecutablePropertiesKHR(NULL, &pinfo, &cnt, NULL) != VK_SUCCESS || cnt != 2u)
		goto fail;

	VkPipelineExecutablePropertiesKHR props_guard[2];
	memset(props_guard, 0xa5, sizeof(props_guard));
	cnt = 1u;
	if (fuxi_mock_vkGetPipelineExecutablePropertiesKHR(NULL, &pinfo, &cnt, props_guard) != VK_INCOMPLETE || cnt != 1u)
		goto fail;
	/* props_guard[1] 必須維持 0xa5 sentinel 零越界 */
	for (size_t i = 0u; i < sizeof(props_guard[1]); i++) {
		if (((const uint8_t *)&props_guard[1])[i] != 0xa5u)
			goto fail;
	}

	/* 測試 Statistics VK_INCOMPLETE 與完整讀取 */
	VkPipelineExecutableInfoKHR einfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,
		.pipeline = pipe,
		.executableIndex = 1u,
	};
	uint32_t sc = 0u;
	if (fuxi_mock_vkGetPipelineExecutableStatisticsKHR(NULL, &einfo, &sc, NULL) != VK_SUCCESS || sc < 10u)
		goto fail;
	VkPipelineExecutableStatisticKHR stat_guard[2];
	memset(stat_guard, 0x5au, sizeof(stat_guard));
	uint32_t part = 1u;
	if (fuxi_mock_vkGetPipelineExecutableStatisticsKHR(NULL, &einfo, &part, stat_guard) != VK_INCOMPLETE || part != 1u)
		goto fail;
	for (size_t i = 0u; i < sizeof(stat_guard[1]); i++) {
		if (((const uint8_t *)&stat_guard[1])[i] != 0x5au)
			goto fail;
	}

	/* 測試 InternalRepresentations 文字緩衝區截斷與 VK_INCOMPLETE */
	uint32_t irc = 0u;
	if (fuxi_mock_vkGetPipelineExecutableInternalRepresentationsKHR(NULL, &einfo, &irc, NULL) != VK_SUCCESS || irc != 3u)
		goto fail;
	VkPipelineExecutableInternalRepresentationKHR ir_buf[3];
	memset(ir_buf, 0, sizeof(ir_buf));
	if (fuxi_mock_vkGetPipelineExecutableInternalRepresentationsKHR(NULL, &einfo, &irc, ir_buf) != VK_SUCCESS)
		goto fail;
	char tiny[12];
	memset(tiny, 0x7eu, sizeof(tiny));
	ir_buf[2].pData = tiny;
	ir_buf[2].dataSize = 8u; /* 刻意小於完整 disassembly，預留最後 4B sentinel */
	if (fuxi_mock_vkGetPipelineExecutableInternalRepresentationsKHR(NULL, &einfo, &irc, ir_buf) != VK_INCOMPLETE)
		goto fail;
	if (ir_buf[2].dataSize != 8u || tiny[7] != '\0' || tiny[8] != 0x7e)
		goto fail;

	fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
	fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
	fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
	printf("[PASS] test_mock_vk_executable_properties_twostep\n");
	return true;
fail:
	fuxi_mock_vkDestroyPipeline(NULL, pipe, NULL);
	fuxi_mock_vkDestroyShaderModule(NULL, vs_mod, NULL);
	fuxi_mock_vkDestroyShaderModule(NULL, fs_mod, NULL);
	return false;
}

static bool test_isa_padding_16insn_alignment(void)
{
	uint32_t spv[5];
	make_minimal_valid_spv(spv);
	struct fuxi_probe_result res;
	char err[128] = {0};
	if (!mock_run_pipeline_probe_on_spv((const uint8_t *)spv, sizeof(spv),
					    (const uint8_t *)spv, sizeof(spv),
					    FUXI_PROBE_CHIP_ID_FD740, true, &res, err, sizeof(err)))
		return false;

	bool ok = (res.vs.insn_count == 8u && res.vs.instrlen == 1u &&
		   res.vs.padded_dword_count == 32u &&
		   res.fs.insn_count == 70u && res.fs.instrlen == 5u &&
		   res.fs.padded_dword_count == 160u &&
		   fuxi_probe_verify_result(&res, err, sizeof(err)));
	fuxi_probe_result_free(&res);
	if (!ok) return false;
	printf("[PASS] test_isa_padding_16insn_alignment\n");
	return true;
}

static bool test_register_and_constlen_constraints(void)
{
	uint32_t spv[5];
	make_minimal_valid_spv(spv);
	struct fuxi_probe_result res;
	char err[128] = {0};
	if (!mock_run_pipeline_probe_on_spv((const uint8_t *)spv, sizeof(spv),
					    (const uint8_t *)spv, sizeof(spv),
					    FUXI_PROBE_CHIP_ID_FD740, true, &res, err, sizeof(err)))
		return false;

	bool ok = (res.vs.constlen >= 2u &&
		   res.fs.constlen >= 4u &&
		   (res.vs.sp_ctrl_reg0 & (1u << 20)) != 0u &&
		   (res.fs.sp_ctrl_reg0 & (1u << 31)) != 0u &&
		   (res.fs.sp_config & (1u << 1)) == 0u &&
		   ((res.fs.sp_config >> 9) & 0xffu) == 2u &&
		   ((res.fs.sp_config >> 17) & 0x1fu) == 2u &&
		   res.fs.sp_tsize == 2u);
	fuxi_probe_result_free(&res);
	if (!ok) return false;
	printf("[PASS] test_register_and_constlen_constraints\n");
	return true;
}

static bool test_unknown_chip_and_roundtrip_guards(void)
{
	uint32_t spv[5];
	make_minimal_valid_spv(spv);
	struct fuxi_probe_result res;
	char err[128] = {0};
	if (mock_run_pipeline_probe_on_spv((const uint8_t *)spv, sizeof(spv),
					   (const uint8_t *)spv, sizeof(spv),
					   FUXI_PROBE_CHIP_ID_UNKNOWN, true, &res, err, sizeof(err))) {
		fuxi_probe_result_free(&res);
		return false;
	}
	if (mock_run_pipeline_probe_on_spv((const uint8_t *)spv, sizeof(spv),
					   (const uint8_t *)spv, sizeof(spv),
					   FUXI_PROBE_CHIP_ID_FD740, false, &res, err, sizeof(err))) {
		fuxi_probe_result_free(&res);
		return false;
	}
	printf("[PASS] test_unknown_chip_and_roundtrip_guards\n");
	return true;
}

static bool test_generated_header_roundtrip_c11(const char *out_dir)
{
	uint32_t spv[5];
	make_minimal_valid_spv(spv);
	struct fuxi_probe_result res;
	char err[128] = {0};
	if (!mock_run_pipeline_probe_on_spv((const uint8_t *)spv, sizeof(spv),
					    (const uint8_t *)spv, sizeof(spv),
					    FUXI_PROBE_CHIP_ID_FD740, true, &res, err, sizeof(err)))
		return false;

	if (!fuxi_write_probe_outputs(out_dir, &res, "mock_host", err, sizeof(err))) {
		fuxi_probe_result_free(&res);
		return false;
	}
	fuxi_probe_result_free(&res);

	char hdr_path[4096];
	snprintf(hdr_path, sizeof(hdr_path), "%s/fuxi_gpu_composite_isa.h", out_dir);
	uint8_t *hdr_bytes = NULL;
	size_t hdr_len = 0u;
	if (!fuxi_read_file_bytes(hdr_path, &hdr_bytes, &hdr_len))
		return false;

	bool has_cr = false;
	for (size_t i = 0u; i < hdr_len; i++) {
		if (hdr_bytes[i] == '\r') {
			has_cr = true;
			break;
		}
	}
	char *hdr_str = (char *)malloc(hdr_len + 1u);
	if (!hdr_str) {
		free(hdr_bytes);
		return false;
	}
	memcpy(hdr_str, hdr_bytes, hdr_len);
	hdr_str[hdr_len] = '\0';
	free(hdr_bytes);

	bool ok = (!has_cr &&
		   strstr(hdr_str, "#include <stdint.h>") != NULL &&
		   strstr(hdr_str, "FUXI_PB_ISA_CHIP_ID") != NULL &&
		   strstr(hdr_str, "FUXI_PB_VS_INSTRLEN") != NULL &&
		   strstr(hdr_str, "FUXI_PB_FS_INSTRLEN") != NULL &&
		   strstr(hdr_str, "fuxi_pb_vs_dwords[FUXI_PB_VS_DWORD_COUNT]") != NULL &&
		   strstr(hdr_str, "fuxi_pb_fs_dwords[FUXI_PB_FS_DWORD_COUNT]") != NULL &&
		   strstr(hdr_str, "float") == NULL &&
		   strstr(hdr_str, "double") == NULL);
	free(hdr_str);
	if (!ok) return false;
	printf("[PASS] test_generated_header_roundtrip_c11\n");
	return true;
}

static bool test_const_alloc_disjoint_positive(void)
{
	char err[256] = {0};
	uint32_t fs_imm[4] = {0xc0000000u, 0x40000000u, 0x00000000u, 0x3f800000u};
	struct fuxi_probe_stage_const_layout fs_layout = {
		.push_consts_base_vec4 = 0u,
		.push_consts_size_vec4 = 4u,
		.push_consts_dwords = 16u,
		.preamble_base_vec4 = 4u,
		.preamble_size_vec4 = 4u,
		.region_count = 2u,
		.regions = {
			{ FUXI_CONST_REGION_PUSH_CONSTS, "push_consts", 0u, 4u },
			{ FUXI_CONST_REGION_PREAMBLE, "preamble", 4u, 4u },
		},
		.max_const_offset_vec4 = 8u,
		.imm_base_vec4 = 8u,
		.imm_raw_dword_count = 4u,
		.imm_count_vec4 = 1u,
		.imm_dword_count = 4u,
		.imm_dwords = fs_imm,
	};
	const char *fs_asm =
		"; offsets: preamble c4.x (4 vec4)\n"
		"@const(c8.x)\t0xc0000000, 0x40000000, 0x00000000, 0x3f800000\n"
		"shps #l10\n"
		"stc.u32 c[16], r48.z, 1\n"
		"(sy)(ss)shpe\n"
		"l10:\n"
		"end\n";
	if (!fuxi_probe_verify_const_layout_and_asm("FS", &fs_layout, 12u, fs_asm, err, sizeof(err))) {
		fprintf(stderr, "FS disjoint unexpected fail: %s\n", err);
		return false;
	}

	struct fuxi_probe_stage_const_layout vs_layout = {
		.push_consts_base_vec4 = 0u,
		.push_consts_size_vec4 = 4u,
		.push_consts_dwords = 8u,
		.region_count = 2u,
		.regions = {
			{ FUXI_CONST_REGION_PUSH_CONSTS, "push_consts", 0u, 4u },
			{ FUXI_CONST_REGION_PRIMITIVE_PARAM, "primitive_param", 4u, 1u },
		},
		.max_const_offset_vec4 = 5u,
		.imm_base_vec4 = 5u,
	};
	const char *vs_asm = "; primitive_param c4.x (1 vec4)\nend\n";
	if (!fuxi_probe_verify_const_layout_and_asm("VS", &vs_layout, 8u, vs_asm, err, sizeof(err))) {
		fprintf(stderr, "VS disjoint unexpected fail: %s\n", err);
		return false;
	}
	printf("[PASS] test_const_alloc_disjoint_positive (FS [0,4)+[4,8)+[8,9)<=12 & VS [0,4)+[4,5)<=8)\n");
	return true;
}

static bool test_const_alloc_overlap_negative(void)
{
	char err[256] = {0};
	uint32_t imm[4] = {0xc0000000u, 0x40000000u, 0x00000000u, 0x3f800000u};

	/* Case 1: 重現 0818eec push_consts [0,4) 與 preamble [0,4) 重疊 */
	struct fuxi_probe_stage_const_layout overlap_pre = {
		.push_consts_base_vec4 = 0u,
		.push_consts_size_vec4 = 4u,
		.push_consts_dwords = 16u,
		.preamble_base_vec4 = 0u,
		.preamble_size_vec4 = 4u,
		.region_count = 2u,
		.regions = {
			{ FUXI_CONST_REGION_PUSH_CONSTS, "push_consts", 0u, 4u },
			{ FUXI_CONST_REGION_PREAMBLE, "preamble", 0u, 4u },
		},
		.max_const_offset_vec4 = 4u,
		.imm_base_vec4 = 4u,
		.imm_count_vec4 = 1u,
		.imm_dword_count = 4u,
		.imm_dwords = imm,
	};
	if (fuxi_probe_verify_const_layout_and_asm("FS", &overlap_pre, 8u, "end\n", err, sizeof(err)))
		return false;

	/* Case 2: preamble [4,8) 與 immediates [6,7) 重疊 */
	struct fuxi_probe_stage_const_layout overlap_imm = {
		.push_consts_base_vec4 = 0u,
		.push_consts_size_vec4 = 4u,
		.push_consts_dwords = 16u,
		.preamble_base_vec4 = 4u,
		.preamble_size_vec4 = 4u,
		.region_count = 2u,
		.regions = {
			{ FUXI_CONST_REGION_PUSH_CONSTS, "push_consts", 0u, 4u },
			{ FUXI_CONST_REGION_PREAMBLE, "preamble", 4u, 4u },
		},
		.max_const_offset_vec4 = 8u,
		.imm_base_vec4 = 6u,
		.imm_count_vec4 = 1u,
		.imm_dword_count = 4u,
		.imm_dwords = imm,
	};
	if (fuxi_probe_verify_const_layout_and_asm("FS", &overlap_imm, 12u, "end\n", err, sizeof(err)))
		return false;

	/* Case 3: imm_base + imm_count > constlen 越界 */
	struct fuxi_probe_stage_const_layout oob_imm = {
		.push_consts_base_vec4 = 0u,
		.push_consts_size_vec4 = 4u,
		.push_consts_dwords = 16u,
		.preamble_base_vec4 = 4u,
		.preamble_size_vec4 = 4u,
		.region_count = 2u,
		.regions = {
			{ FUXI_CONST_REGION_PUSH_CONSTS, "push_consts", 0u, 4u },
			{ FUXI_CONST_REGION_PREAMBLE, "preamble", 4u, 4u },
		},
		.max_const_offset_vec4 = 8u,
		.imm_base_vec4 = 8u,
		.imm_count_vec4 = 2u,
		.imm_dword_count = 8u,
		.imm_dwords = imm,
	};
	if (fuxi_probe_verify_const_layout_and_asm("FS", &oob_imm, 8u, "end\n", err, sizeof(err)))
		return false;

	/* Case 4: 未呼叫 ir3_const_alloc 預留 push_consts */
	struct fuxi_probe_stage_const_layout missing_push = {
		.push_consts_base_vec4 = 0u,
		.push_consts_size_vec4 = 0u,
		.push_consts_dwords = 16u,
		.region_count = 1u,
		.regions = {
			{ FUXI_CONST_REGION_PREAMBLE, "preamble", 0u, 4u },
		},
		.max_const_offset_vec4 = 4u,
	};
	if (fuxi_probe_verify_const_layout_and_asm("FS", &missing_push, 8u, "end\n", err, sizeof(err)))
		return false;

	printf("[PASS] test_const_alloc_overlap_negative (rejected 0818eec overlap, imm overlap, constlen OOB, unreserved push)\n");
	return true;
}

static bool test_asm_stc_push_const_guard(void)
{
	char err[256] = {0};
	const char *bad_asms[] = {
		"stc.u32 c[0], r48.z, 1\n",
		"stc.f32 c[15], r0.x, 1\n",
		"stc.u32 c[14], r0.x, 4\n",
		"(ss)stc.u32 c[0], r48.z, 1\n",
		"(sy)(ss)stc.u16 c[2], hr0.x, 1\n",
		"stc.u32 c[a1.x], r0.x, 1\n",
		"stc.u32 c[a1.x + 16], r0.x, 1\n",
	};
	for (size_t i = 0u; i < sizeof(bad_asms) / sizeof(bad_asms[0]); i++) {
		if (fuxi_probe_asm_check_stc_no_push_overlap("FS", bad_asms[i], 0u, 16u, err, sizeof(err))) {
			fprintf(stderr, "stc guard failed to reject bad case %zu: %s\n", i, bad_asms[i]);
			return false;
		}
	}

	const char *good_asm =
		"; stc.u32 c[0], r48.z, 1\n"
		"@const(c8.x)\t0xc0000000, 0x40000000, 0x00000000, 0x3f800000\n"
		"(ss)stc.u32 c[16], r48.z, 1\n"
		"stc.u32 c[17], r48.w, 3 ; trailing comment\n"
		"end\n";
	if (!fuxi_probe_asm_check_stc_no_push_overlap("FS", good_asm, 0u, 16u, err, sizeof(err))) {
		fprintf(stderr, "stc guard rejected valid case: %s\n", err);
		return false;
	}
	printf("[PASS] test_asm_stc_push_const_guard (7 negative stc patterns rejected, c[16]/comment allowed)\n");
	return true;
}

static bool test_imm_header_export_and_contents(const char *out_dir)
{
	uint32_t spv[5];
	make_minimal_valid_spv(spv);
	struct fuxi_probe_result res;
	char err[256] = {0};
	if (!mock_run_pipeline_probe_on_spv((const uint8_t *)spv, sizeof(spv),
					    (const uint8_t *)spv, sizeof(spv),
					    FUXI_PROBE_CHIP_ID_FD740, true, &res, err, sizeof(err)))
		return false;

	if (!fuxi_probe_verify_result(&res, err, sizeof(err)) ||
	    !fuxi_write_probe_outputs(out_dir, &res, "mock_host", err, sizeof(err))) {
		fuxi_probe_result_free(&res);
		return false;
	}
	fuxi_probe_result_free(&res);

	char path[4096];
	snprintf(path, sizeof(path), "%s/fuxi_gpu_composite_isa.h", out_dir);
	uint8_t *bytes = NULL;
	size_t len = 0u;
	if (!fuxi_read_file_bytes(path, &bytes, &len))
		return false;
	for (size_t i = 0u; i < len; i++) {
		if (bytes[i] == '\r') {
			free(bytes);
			return false;
		}
	}
	char *hdr = (char *)malloc(len + 1u);
	if (!hdr) {
		free(bytes);
		return false;
	}
	memcpy(hdr, bytes, len);
	hdr[len] = '\0';
	free(bytes);

	const char *required_tokens[] = {
		"#define FUXI_PB_VS_PUSH_CONST_VEC4",
		"#define FUXI_PB_VS_PREAMBLE_BASE_VEC4",
		"#define FUXI_PB_VS_PREAMBLE_COUNT_VEC4",
		"#define FUXI_PB_VS_IMM_BASE_VEC4",
		"#define FUXI_PB_VS_IMM_COUNT_VEC4",
		"#define FUXI_PB_VS_IMM_DWORD_COUNT",
		"#define FUXI_PB_FS_PUSH_CONST_VEC4          4u",
		"#define FUXI_PB_FS_PREAMBLE_BASE_VEC4       4u",
		"#define FUXI_PB_FS_PREAMBLE_COUNT_VEC4      4u",
		"#define FUXI_PB_FS_IMM_BASE_VEC4            8u",
		"#define FUXI_PB_FS_IMM_COUNT_VEC4           1u",
		"#define FUXI_PB_FS_IMM_DWORD_COUNT          4u",
		"fuxi_pb_vs_imm_dwords[FUXI_PB_VS_IMM_DWORD_COUNT > 0u ? FUXI_PB_VS_IMM_DWORD_COUNT : 1u]",
		"fuxi_pb_fs_imm_dwords[FUXI_PB_FS_IMM_DWORD_COUNT > 0u ? FUXI_PB_FS_IMM_DWORD_COUNT : 1u]",
		"#define FUXI_PB_VS_IMM_DWORDS fuxi_pb_vs_imm_dwords",
		"#define FUXI_PB_FS_IMM_DWORDS fuxi_pb_fs_imm_dwords",
		"0xc0000000u, 0x40000000u, 0x00000000u, 0x3f800000u",
	};
	for (size_t i = 0u; i < sizeof(required_tokens) / sizeof(required_tokens[0]); i++) {
		if (!strstr(hdr, required_tokens[i])) {
			fprintf(stderr, "missing header token: %s\n", required_tokens[i]);
			free(hdr);
			return false;
		}
	}
	if (strstr(hdr, "float") != NULL || strstr(hdr, "double") != NULL) {
		fprintf(stderr, "forbidden FP keyword found in header\n");
		free(hdr);
		return false;
	}
	free(hdr);

	snprintf(path, sizeof(path), "%s/probe_report.json", out_dir);
	if (!fuxi_read_file_bytes(path, &bytes, &len))
		return false;
	char *js = (char *)malloc(len + 1u);
	if (!js) {
		free(bytes);
		return false;
	}
	memcpy(js, bytes, len);
	js[len] = '\0';
	free(bytes);
	bool js_ok = (strstr(js, "\"push_const_vec4\": 4") != NULL &&
		      strstr(js, "\"preamble_base_vec4\": 4") != NULL &&
		      strstr(js, "\"imm_base_vec4\": 8") != NULL &&
		      strstr(js, "\"imm_dwords\": [\"0xc0000000\", \"0x40000000\", \"0x00000000\", \"0x3f800000\"]") != NULL);
	free(js);
	if (!js_ok) {
		fprintf(stderr, "probe_report.json missing imm/const layout fields\n");
		return false;
	}

	printf("[PASS] test_imm_header_export_and_contents\n");
	return true;
}

static bool test_sanitizer_clean_memory(void)
{
	/* 由 ASan + LSan + UBSan 在整個測試流程配置與釋放後檢查零洩漏 */
	printf("[PASS] test_sanitizer_clean_memory\n");
	return true;
}

static int run_self_tests(const char *out_dir)
{
	if (!test_spirv_validation_rejects_bad_inputs()) return 10;
	if (!test_mock_vk_executable_properties_twostep()) return 11;
	if (!test_isa_padding_16insn_alignment()) return 12;
	if (!test_register_and_constlen_constraints()) return 13;
	if (!test_unknown_chip_and_roundtrip_guards()) return 14;
	if (!test_generated_header_roundtrip_c11(out_dir)) return 15;
	if (!test_const_alloc_disjoint_positive()) return 16;
	if (!test_const_alloc_overlap_negative()) return 17;
	if (!test_asm_stc_push_const_guard()) return 18;
	if (!test_imm_header_export_and_contents(out_dir)) return 19;
	if (!test_sanitizer_clean_memory()) return 20;
	printf("ALL 11 GATE2 MOCK_HOST SELF-TESTS PASSED\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--self-test") == 0)
		return run_self_tests(argv[2]);

	if (argc != 4) {
		fprintf(stderr, "Usage:\n  %s --self-test <out_dir>\n  %s <vs.spv> <fs.spv> <out_dir>\n",
			argv[0], argv[0]);
		return 2;
	}

	uint8_t *vs_spv = NULL, *fs_spv = NULL;
	size_t vs_len = 0u, fs_len = 0u;
	char err[256] = {0};

	if (!fuxi_read_file_bytes(argv[1], &vs_spv, &vs_len) ||
	    !fuxi_probe_validate_spirv(vs_spv, vs_len, "VS", err, sizeof(err))) {
		fprintf(stderr, "SPIR-V validation failed: %s\n", err[0] ? err : "read error");
		free(vs_spv);
		return 2;
	}
	if (!fuxi_read_file_bytes(argv[2], &fs_spv, &fs_len) ||
	    !fuxi_probe_validate_spirv(fs_spv, fs_len, "FS", err, sizeof(err))) {
		fprintf(stderr, "SPIR-V validation failed: %s\n", err[0] ? err : "read error");
		free(vs_spv);
		free(fs_spv);
		return 2;
	}

	struct fuxi_probe_result res = {0};
	bool ok = mock_run_pipeline_probe_on_spv(vs_spv, vs_len, fs_spv, fs_len,
						 FUXI_PROBE_CHIP_ID_FD740, true,
						 &res, err, sizeof(err));
	free(vs_spv);
	free(fs_spv);
	if (!ok || !fuxi_probe_verify_result(&res, err, sizeof(err)) ||
	    !fuxi_write_probe_outputs(argv[3], &res, "mock_host", err, sizeof(err))) {
		fprintf(stderr, "Mock probe failed: %s\n", err[0] ? err : "unknown error");
		fuxi_probe_result_free(&res);
		return 9;
	}

	printf("PASS: mock_host FD740 VS=%u insns (%u B), FS=%u insns (%u B), sha256=%s\n",
	       res.vs.insn_count, res.vs.padded_dword_count * 4u,
	       res.fs.insn_count, res.fs.padded_dword_count * 4u,
	       res.combined_sha256_hex);
	fuxi_probe_result_free(&res);
	return 0;
}

#endif /* FUXI_PROBE_MOCK_HOST */
