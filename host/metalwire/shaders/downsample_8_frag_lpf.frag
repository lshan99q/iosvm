#version 450
#extension GL_EXT_samplerless_texture_functions : enable
// SPDX-License-Identifier: MIT
//
// downsample_8_frag_lpf -- hand-written Vulkan GLSL for CoreAnimation's 8x backdrop downsample
// (WP-4 item 1.2 follow-up; the T9 report, /tmp/wp4v6-evidence/T9-REPORT.md, found pipeline 1483
// `com.apple.coreanimation.draw.PBGRA_Tds8Xf_Isrc` missing: 692 skipped draws, and with the
// narrow_blur_27/19/11/7 family the backdrop stayed clear black when Control Center/Spotlight
// opened).
//
// Provenance: nothing here is Apple code, and no Apple binary was disassembled. The evidence is
// the recorded render calls and the names they carry:
//   * the function is `downsample_8_frag_lpf`; the pipeline short code is `Tds8` (texture,
//     downsample 8) and its sibling `Tds4`/`downsample_4` covers the 4x step, so the number is
//     the reduction factor, not the tap count;
//   * the same guest-log evidence as the 4x sibling: the downsample draws render into a smaller
//     target from `com.apple.coreanimation.framebuffer-surface` with
//     `setFragmentSamplerState: (nil)` -- no filtered fetch is configured, and the number of the
//     function is the source block the destination texel averages.
//
// [INFERRED] Semantics: an 8x8 box average of the premultiplied source, 64 texelFetch taps with
// weight 1/64, clamped at the source edges. A box is the only 8x reduction the names support and
// the one that does not invent a kernel; the tints the wire binds are applied as a multiply, the
// way fixed_frag_lpf_cpf applies its vertex colour.
//
// Interface: set 0 binding 1 = Metal fragment texture 3, binding 2 = sampler 0 (the recorded
// draws bind none, and this shader does not need one: all access is texel-exact), and the same
// push-constant/descriptor layout as every other metalwire fragment shader (PROTOCOL.md).
// The specialization constants below are declared with the ids the host always maps, so the same
// pipeline-specialization record works for every fragment base function.

layout(location = 0) in vec2 v_texcoord0;
layout(location = 1) in vec4 v_color;

layout(set = 0, binding = 0, std430) readonly buffer WireBuffer {
    uint wire[];
};
layout(set = 0, binding = 1) uniform texture2D u_image;

layout(location = 0) out vec4 out_color;

layout(constant_id = 0) const int HAS_TEXTURE = 1;
layout(constant_id = 1) const int FETCH_DEST = 0;
layout(constant_id = 2) const int PREMULTIPLY = 0;

void main()
{
    ivec2 size = textureSize(u_image, 0);
    // v_texcoord0 is normalised to the source region (the vertex stage applied the texture
    // matrix); the destination texel's 8x8 source block is centred on it.
    ivec2 centre = ivec2(floor(v_texcoord0 * vec2(size)));
    ivec2 last = size - ivec2(1);
    vec4 sum = vec4(0.0);

    for (int y = -4; y <= 3; ++y) {
        for (int x = -4; x <= 3; ++x) {
            ivec2 at = clamp(centre + ivec2(x, y), ivec2(0), last);
            sum += texelFetch(u_image, at, 0);
        }
    }
    out_color = sum * (1.0 / 64.0) * v_color;
}
