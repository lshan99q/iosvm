#version 450
#extension GL_EXT_samplerless_texture_functions : enable
// SPDX-License-Identifier: MIT
//
// downsample_4_frag_lpf -- hand-written Vulkan GLSL for CoreAnimation's 4x backdrop downsample
// (WP-4 item 1.2; pipeline com.apple.coreanimation.draw.PBGRA_Tds4Xf_Isrc, 290 skipped draws in
// boot wp4-v4).
//
// Provenance: nothing here is Apple code, and no Apple binary was disassembled. The evidence is
// the recorded render calls and the names they carry:
//   * the function is `downsample_4_frag_lpf`; the pipeline short code is `Tds4` (texture,
//     downsample 4) and the same library family has `Tds8`/`downsample_8` for a coarser step, so
//     the number is the reduction factor, not the tap count;
//   * guest log boot wp4-v4 (serial.log, line ~12978): the downsample draw renders into a
//     180x72 texture from `com.apple.coreanimation.framebuffer-surface` with
//     `setFragmentSamplerState: (nil)` -- i.e. a filtered fetch is not configured, and a 180x72
//     target covers a 4x-larger 720x288 source region (the Dock/wallpaper band).
//
// [INFERRED] Semantics: a 4x4 box average of the premultiplied source, 16 texelFetch taps with
// weight 1/16, clamped at the source edges. A box is the only 4x reduction the names support and
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
    // matrix); the destination texel's 4x4 source block is centred on it.
    ivec2 centre = ivec2(floor(v_texcoord0 * vec2(size)));
    ivec2 last = size - ivec2(1);
    vec4 sum = vec4(0.0);

    for (int y = -2; y <= 1; ++y) {
        for (int x = -2; x <= 1; ++x) {
            ivec2 at = clamp(centre + ivec2(x, y), ivec2(0), last);
            sum += texelFetch(u_image, at, 0);
        }
    }
    out_color = sum * (1.0 / 16.0) * v_color;
}
