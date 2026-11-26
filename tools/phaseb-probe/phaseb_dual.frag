#version 450
/*
 * Phase B 2-Texture Dual-Layer Composite Fragment Shader (Adreno A740 Sysmem)
 * 對接規格：
 * - work/localbuild/b023-gpu-scanout-phaseb-1126/PHASE_B_DESIGN_SPEC.md (§2.2, §2.5, §3.1-3.3)
 * - work/localbuild/b023-gpu-scanout-phaseb-1126/src/fuxi_phaseb_3d.c:442-545, 1109-1140
 *
 * 綁定與常數契約：
 * - Bindful sampler2D: u_tex0 (t#0/s#0 = Layer 0), u_tex1 (t#1/s#1 = Layer 1)
 * - TPL1_TEX_SAMP.UNNORM_COORDS = 1 (像素空間座標直接定址 +0.5 texel 中心對齊)
 * - c0 = (pass_y, y_base, y_end, alpha_flags: +1=l0_has_alpha, +2=l1_has_alpha)
 * - c1 = (l0_px0, l0_py0, l0_px1, l0_py1)
 * - c2 = (l0_sx_scale, l0_sy_scale, l1_sx_scale, l1_sy_scale)
 * - c3 = (l1_px0, l1_py0, l1_px1, l1_py1)
 */

layout(set = 0, binding = 0) uniform sampler2D u_tex0;
layout(set = 0, binding = 1) uniform sampler2D u_tex1;

layout(push_constant) uniform FsConsts {
	vec4 c0;
	vec4 c1;
	vec4 c2;
	vec4 c3;
} u_fs;

layout(location = 0) in vec2 v_panel_xy;
layout(location = 0) out vec4 o_color;

void main()
{
	float px = floor(v_panel_xy.x);
	float py = floor(v_panel_xy.y) * 2.0 + u_fs.c0.x;
	float py_c = py + 0.5;

	/* 來源 texel 中心對齊 (搭配 TPL1_TEX_SAMP UNNORM_COORDS = 1) */
	vec2 uv0 = floor(vec2(v_panel_xy.x - u_fs.c1.x, py_c - u_fs.c1.y) * u_fs.c2.xy) + 0.5;
	vec2 uv1 = floor(vec2(v_panel_xy.x - u_fs.c3.x, py_c - u_fs.c3.y) * u_fs.c2.zw) + 0.5;

	vec4 s0 = texture(u_tex0, uv0);
	vec4 s1 = texture(u_tex1, uv1);

	bool l1_has_alpha = (u_fs.c0.w >= 2.0);
	float rem_flags = l1_has_alpha ? (u_fs.c0.w - 2.0) : u_fs.c0.w;
	bool l0_has_alpha = (rem_flags >= 1.0);

	/* Layer 0: 視口裁剪 + sa==0 置黑清除殘留 dirty RGB + 強制不透明 */
	bool in_l0 = (px >= u_fs.c1.x && px < u_fs.c1.z && py >= u_fs.c1.y && py < u_fs.c1.w);
	bool l0_vis = in_l0 && (!l0_has_alpha || s0.a != 0.0);
	vec3 c0_rgb = l0_vis ? s0.rgb : vec3(0.0);

	/* Layer 1: 視口裁剪 + Premultiplied SRC_OVER */
	bool in_l1 = (px >= u_fs.c3.x && px < u_fs.c3.z && py >= u_fs.c3.y && py < u_fs.c3.w);
	float eff_a = l1_has_alpha ? s1.a : 1.0;
	vec3 blend_rgb = c0_rgb * (1.0 - eff_a) + s1.rgb;
	vec3 out_rgb = in_l1 ? blend_rgb : c0_rgb;

	/* fit==1 [y_base, y_end) 上下黑邊裁剪，強制輸出不透明 RGBA */
	bool in_fit = (py >= u_fs.c0.y && py < u_fs.c0.z);
	o_color = vec4(in_fit ? out_rgb : vec3(0.0), 1.0);
}
