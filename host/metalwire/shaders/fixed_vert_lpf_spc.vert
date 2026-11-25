#version 450
// SPDX-License-Identifier: MIT
//
// fixed_vert_lpf_spc -- hand-written Vulkan GLSL equivalent of CoreAnimation's Metal base vertex
// function of the same name (the `lpf`/BGRA8 runtime-specialisation family; WP-2 of stage C).
//
// It is one of the four base functions the wire names. CoreAnimation's own source is not used:
// the interface below was reconstructed from the recorded render encoder (the 16-attribute vertex
// descriptor, the wire's buffer/texture/sampler bindings) and the on-disk Metal pipeline library's
// sibling `lph` modules, which carry the same CA::OGL::Metal argument layout. See
// host/metalwire/shaders/README.md and docs/spike-wp2-vulkan-glsl.md.
//
// Interface (the translator's mapping; see PROTOCOL.md and the WP-2 note):
//   * four vertex attributes of the pipeline's vertex descriptor, stream 1, stride 48:
//       location 0  float4  position       (MTLVertexFormatFloat4,  offset 0)
//       location 1  float2  texcoord0      (MTLVertexFormatFloat2,  offset 16)
//       location 2  float2  texcoord1      (MTLVertexFormatFloat2,  offset 24)
//       location 3  float4  color          (MTLVertexFormatHalf4,   offset 32, widened to float)
//   * the small buffers the Metal function takes as arguments (mvp_matrix at vertex buffer 2,
//     texture_matrix at vertex buffer 3) are word-addressed inside the single wire buffer; the
//     byte offsets travel in push constants, because Metal buffer offsets are not Vulkan
//     descriptor alignment-safe for a host that must keep the wire's layout.
//
// Function constants of the Metal base function become specialization constants: the `spc` family
// is the same code with a different set of bound attributes/buffers, so USE_TEXTURE_MATRIX selects
// the `Timg`/`Tlub` variants that consume the texture matrix.

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec2 in_texcoord0;
layout(location = 2) in vec2 in_texcoord1;
layout(location = 3) in vec4 in_color;

layout(set = 0, binding = 0, std430) readonly buffer WireBuffer {
    uint wire[];
};

layout(push_constant) uniform WireOffsets {
    uint vertex_mvp;       // byte offset of the 4x4 float mvp matrix
    uint vertex_texmat;    // byte offset of the float4 texture matrix
    uint fragment_uniform; // byte offset of CA::OGL::Metal::Uniform (fragment stage)
    uint fragment_lod;     // byte offset of the float lod bias (fragment stage)
} offsets;

layout(location = 0) out vec2 v_texcoord0;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_texcoord1;

layout(constant_id = 0) const int USE_TEXTURE_MATRIX = 0;

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

    // CA::OGL::Metal's vertex shader multiplies the four matrix rows by the position components:
    // out = m[0]*p.x + m[1]*p.y + m[2]*p.z + m[3]*p.w.
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
