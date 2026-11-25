#version 450
#extension GL_EXT_samplerless_texture_functions : enable
// SPDX-License-Identifier: MIT
//
// narrow_blur_23_frag_lpf -- hand-written Vulkan GLSL for CoreAnimation's 23-tap narrow blur
// (WP-4 item 1.2; pipeline com.apple.coreanimation.draw.PBGRA_Tn23Xf_Isrc, 98 skipped draws in
// boot wp4-v4). Its sibling narrow_blur_15_frag_lpf is the same shader with a 15-tap kernel; the
// library family also has narrow_blur_11 (recorded in the guest log, never sent as an
// unsupported draw in wp4-v4).
//
// Provenance: nothing here is Apple code, and no Apple binary was disassembled. What the wire
// says (boot wp4-v4 guest log, mt-work/logs/wp4-v4/serial.log):
//   * the function names carry one odd number each -- 23, 15, 11 -- which is the length of a
//     one-dimensional kernel; the sibling `downsample_4`/`downsample_8` show the same convention
//     for their factor;
//   * a narrow-blur draw samples `com.apple.coreanimation.surface` at fragment texture 3 with
//     `setFragmentSamplerState: (nil)` (so the shader is written for texel-exact access), binds
//     the per-draw uniform at fragment buffer 1 and a second fragment buffer at index 6, and
//     draws 1, 2 or 10 quads into a region-sized target (100x209, 64x64, 78x173 in the log);
//   * each blur region in the recorded chains gets one narrow pass: encoder 1 downsamples into
//     180x72 and encoder 4 blurs that same region once; encoder 2 downsamples into 64x64 and
//     encoder 3 blurs it once. A separable two-pass implementation would show a second narrow
//     draw per region, which the record does not have, so this pass has to produce the full 2D
//     result.
//
// [INFERRED] Semantics: a separable Gaussian evaluated in one pass, 23 taps per axis (radius 11),
// with binomial weights -- Pascal's triangle row 22, the standard integer approximation of a
// Gaussian (the exact 1D kernel is Apple's and is not in evidence). The 2D weight of tap (i,j) is
// w(i)*w(j)/2^44, taps are texel-exact, coordinates clamp at the source edges. If a later boot
// shows the two axes drawn as two passes of this same pipeline, this file should be split into a
// one-axis kernel at that point: today the record has one pass per region.
//
// The weights are integers so the source carries them exactly: row 22 of Pascal's triangle
// (C(22,0..22) = 1, 22, 231, ... 705432, 705432, ... 231, 22, 1; sum 2^22 = 4194304). The array
// below is the symmetric half in centre-first order, C(22, 11 + |offset|), so KERNEL[abs(d)] is
// the weight of the tap d texels from the centre.
//
// Interface: set 0 binding 1 = Metal fragment texture 3, binding 2 = sampler 0 (unused here),
// bindings 0/3 and the push constants are the wire's standard layout (PROTOCOL.md). The vertex
// stage already normalised v_texcoord0 to the source region through the texture matrix.

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

const int RADIUS = 11;
// C(22, 11 + |d|), the symmetric half of the 23-tap binomial kernel, centre first.
const float KERNEL[12] =
    float[12](705432.0, 646646.0, 497420.0, 319770.0, 170544.0, 74613.0, 26334.0, 7315.0,
              1540.0, 231.0, 22.0, 1.0);
const float KERNEL_NORM = 1.0 / 4194304.0; // 2^22

void main()
{
    ivec2 size = textureSize(u_image, 0);
    ivec2 centre = ivec2(floor(v_texcoord0 * vec2(size)));
    ivec2 last = size - ivec2(1);
    vec4 sum = vec4(0.0);

    for (int y = -RADIUS; y <= RADIUS; ++y) {
        float wy = KERNEL[abs(y)] * KERNEL_NORM;
        for (int x = -RADIUS; x <= RADIUS; ++x) {
            float weight = wy * KERNEL[abs(x)] * KERNEL_NORM;
            ivec2 at = clamp(centre + ivec2(x, y), ivec2(0), last);
            sum += weight * texelFetch(u_image, at, 0);
        }
    }
    out_color = sum * v_color;
}
