#version 450
// SPDX-License-Identifier: MIT
//
// std_vert1_lpf -- hand-written Vulkan GLSL for the second vertex base function the wire names.
// Unlike fixed_vert_lpf_spc/fixed_frag_lpf_cpf/luma_boost_lpf, its Metal source is not among the
// on-disk pipeline library's sibling modules, so the interface is reconstructed from the draw the
// wire recorded: the same 16-attribute descriptor (stream 1, stride 48), the mvp matrix at vertex
// buffer 2, the texture matrix at vertex buffer 3, and the full-screen quad used by the luma-boost
// pass. It is the "standard" variant of the fixed transform: position through the matrix, texture
// coordinates and colour passed through.
//
// The interface mirrors fixed_vert_lpf_spc (same attributes and buffer offsets); keeping the two
// shaders separate keeps the base-function names the wire carries honest.

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec2 in_texcoord0;
layout(location = 2) in vec2 in_texcoord1;
layout(location = 3) in vec4 in_color;

layout(set = 0, binding = 0, std430) readonly buffer WireBuffer {
    uint wire[];
};

layout(push_constant) uniform WireOffsets {
    uint vertex_mvp;
    uint vertex_texmat;
    uint fragment_uniform;
    uint fragment_lod;
} offsets;

layout(location = 0) out vec2 v_texcoord0;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_texcoord1;

layout(constant_id = 0) const int USE_TEXTURE_MATRIX = 1;

float wire_f32(uint byte_offset)
{
    return uintBitsToFloat(wire[byte_offset >> 2]);
}

vec4 wire_f32x4(uint byte_offset)
{
    return vec4(wire_f32(byte_offset), wire_f32(byte_offset + 4), wire_f32(byte_offset + 8),
                wire_f32(byte_offset + 12));
}

void main()
{
    vec4 p = in_position;
    vec4 m0 = wire_f32x4(offsets.vertex_mvp + 0);
    vec4 m1 = wire_f32x4(offsets.vertex_mvp + 16);
    vec4 m2 = wire_f32x4(offsets.vertex_mvp + 32);
    vec4 m3 = wire_f32x4(offsets.vertex_mvp + 48);
    // CA stores the matrix as four float4 columns and multiplies them by the position's
    // components: out = col0*x + col1*y + col2*z + col3*w (the AIR IR's m0*p.x + ... + m3*p.w).
    gl_Position = m0 * p.x + m1 * p.y + m2 * p.z + m3 * p.w;

    vec2 tc0 = in_texcoord0;
    if (USE_TEXTURE_MATRIX != 0) {
        vec4 texture_matrix = wire_f32x4(offsets.vertex_texmat);
        tc0 = tc0 * texture_matrix.xy + texture_matrix.zw;
    }

    v_texcoord0 = tc0;
    v_texcoord1 = in_texcoord1;
    v_color = in_color;
}
