#version 450
/*
 * Phase B 2-Texture Quad Vertex Shader (Adreno A740 Sysmem DI_PT_RECTLIST)
 * 對接規格：
 * - work/localbuild/b023-gpu-scanout-phaseb-1126/PHASE_B_DESIGN_SPEC.md (§2.2, §2.5)
 * - work/localbuild/b023-gpu-scanout-phaseb-1126/src/fuxi_phaseb_3d.c:413-440, 1099-1107
 * - work/mesa-src/src/freedreno/vulkan/tu_clear_blit.cc:767-807 (r3d_ blit VS 先例)
 *
 * 搭配 GRAS_CL_CNTL = 0x381 (關閉 VP 變換與透視除法)，直接輸出螢幕半高像素座標
 * (0..1080, 0..1200) 至 gl_Position 與 varying location 0 (v_panel_xy)。
 */

layout(push_constant) uniform VsConsts {
	vec4 pos_rect; /* c0 = (px0, py0, px1, py1) */
	vec4 var_rect; /* c1 = (u0,  v0,  u1,  v1)  */
} u_vs;

layout(location = 0) out vec2 v_panel_xy;

void main()
{
	bool is_br = (gl_VertexIndex != 0);
	vec2 p = is_br ? u_vs.pos_rect.zw : u_vs.pos_rect.xy;
	vec2 v = is_br ? u_vs.var_rect.zw : u_vs.var_rect.xy;
	gl_Position = vec4(p, 0.0, 1.0);
	v_panel_xy = v;
}
