#version 450
#extension GL_EXT_samplerless_texture_functions : enable
// SPDX-License-Identifier: MIT
//
// narrow_blur_19_frag_lpf -- hand-written Vulkan GLSL for CoreAnimation's 19-tap narrow blur (WP-4 item
// 1.2 follow-up; the T9 report, /tmp/wp4v6-evidence/T9-REPORT.md, found pipeline pipe 1298
// `PBGRA_Tn19Xf_Isrc` missing: 15 skipped draws, backdrop left as clear black when Control
// Center/Spotlight opened). Same reconstruction as the 23/15-tap siblings
// (narrow_blur_23_frag_lpf.frag); only the kernel length differs.
//
// Evidence: the function name carries the odd kernel length (19); the boot's missing report
// pairs it with the shared `downsample_blur_vert_lpf` vertex function and the `PBGRA_Tn19Xf_Isrc` pipeline
// code, and the family's other members are narrow_blur_7/11/15/19/23/27 and downsample_4/8.
//
// [INFERRED] A separable Gaussian evaluated in one pass, 19 taps per axis (radius 9),
// binomial weights -- Pascal's triangle row 18, normalised (2^18 = 262144), the standard
// integer approximation of a Gaussian. The 2D weight of tap (i,j) is w(i)*w(j)/2^36, taps are
// texel-exact and clamp at the source edges. Split into a one-axis kernel only if a later boot
// shows the axes as two passes of this pipeline; the recorded chains give one narrow pass per
// region.
//
// The weights are integers so the source carries them exactly: C(18, 18/2 + |d|), the
// symmetric half in centre-first order, sum 2^18.
//
// Interface: the wire's standard fragment layout (set 0 bindings 0..3, push constants); binding 1
// is Metal fragment texture 3, binding 2 is the sampler the recorded draws leave nil and this
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

const int RADIUS = 9;
// C(18, 9 + |d|), the symmetric half of the 19-tap binomial kernel, centre
// first.
const float KERNEL[10] =
    float[10](48620.0, 43758.0, 31824.0, 18564.0, 8568.0,
              3060.0, 816.0, 153.0, 18.0, 1.0);
const float KERNEL_NORM = 1.0 / 262144.0; // 2^18

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
