#version 450
#extension GL_EXT_samplerless_texture_functions : enable
// SPDX-License-Identifier: MIT
//
// narrow_blur_15_frag_lpf -- hand-written Vulkan GLSL for CoreAnimation's 15-tap narrow blur
// (WP-4 item 1.2; pipeline com.apple.coreanimation.draw.PBGRA_Tn15Xf_Isrc, 286 skipped draws in
// boot wp4-v4). Same reconstruction and the same [INFERRED] semantics as its 23-tap sibling
// (narrow_blur_23_frag_lpf.frag); only the kernel length differs.
//
// Evidence, in one line: the function name carries the odd kernel length (15), the recorded draw
// samples `com.apple.coreanimation.surface` at fragment texture 3 with no sampler, binds the
// per-draw uniform at fragment buffer 1, and renders into a region-sized 100x209 target in a
// single draw; the family's other members are narrow_blur_11/23 and downsample_4/8.
//
// [INFERRED] A separable Gaussian evaluated in one pass, 15 taps per axis (radius 7), binomial
// weights -- Pascal's triangle row 14 (C(14,0..14) = 1, 14, 91, ... 3432, ... 14, 1; sum
// 2^14 = 16384), the standard integer approximation of a Gaussian. The 2D weight of tap (i,j) is
// w(i)*w(j)/2^28, taps are texel-exact and clamp at the source edges. Split into a one-axis
// kernel only if a later boot shows the axes as two passes of this pipeline; the recorded chains
// give one narrow pass per region.
//
// Interface: the wire's standard fragment layout (set 0 bindings 0..3, push constants); binding 1
// is Metal fragment texture 3, binding 2 is the sampler the recorded draw leaves nil and this
// shader does not need.

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

const int RADIUS = 7;
// C(14, 7 + |d|), the symmetric half of the 15-tap binomial kernel, centre first.
const float KERNEL[8] = float[8](3432.0, 3003.0, 2002.0, 1001.0, 364.0, 91.0, 14.0, 1.0);
const float KERNEL_NORM = 1.0 / 16384.0; // 2^14

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
