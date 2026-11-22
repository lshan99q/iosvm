#version 450
// SPDX-License-Identifier: MIT
//
// downsample_blur_vert_lpf -- hand-written Vulkan GLSL for CoreAnimation's shared vertex base
// function of the background/Dock blur passes (WP-4 item 1.2).
//
// Provenance: nothing here is Apple code. The interface is the wire's, the semantics are
// reconstructed from the recorded render calls; both are cited below. See shaders/README.md.
//
// The wire evidence (guest log of boot wp4-v4, mt-work/logs/wp4-v4/serial.log):
//   * the same base name is on five pipeline creations -- downsample_4, downsample_8,
//     narrow_blur_11/15/23: one vertex function for the whole blur family;
//   * the recorded blur draws bind Metal vertex buffers 1, 2 and 3 exactly like the layer
//     pipelines (`setVertexBuffer ... atIndex:1/2/3`), i.e. the attribute stream, the mvp
//     matrix and the texture matrix; their fragment draws bind the per-draw uniform at index 1
//     (and a second fragment buffer at index 6 -- see the fragment shaders);
//   * the pipelines carry the standard 16-attribute vertex descriptor ("16 attributes" in the
//     host's pipeline log for every CA pipeline), and the draws are indexed quads (6, 12, 48,
//     60, 150 indices = 1, 2, 8, 10, 25 quads), so the vertex stage is the ordinary transform of
//     a quad list, not a per-vertex blur.
//
// [INFERRED] Therefore this is the same transform as fixed_vert_lpf_spc: position through the
// four column vectors of the mvp matrix, texcoords through the texture matrix when the pipeline
// samples a texture, colour passed through. The blur itself is in the fragment stage.
//
// Interface (the translator's mapping; see PROTOCOL.md):
//   * location 0  float4 position   (MTLVertexFormatFloat4, offset 0)
//   * location 1  float2 texcoord0  (MTLVertexFormatFloat2, offset 16)
//   * location 2  float2 texcoord1  (MTLVertexFormatFloat2, offset 24)
//   * location 3  float4 colour     (MTLVertexFormatHalf4,   offset 32, widened to float)
//   * vertex buffer 2 = mvp matrix, vertex buffer 3 = texture matrix, word-addressed in the one
//     wire buffer; their byte offsets arrive in push constants.
//
// Function constants become specialization constants: the blur pipelines are all `T...` (they
// sample Metal fragment texture 3), so USE_TEXTURE_MATRIX is 1 for them. Keeping this file
// separate from fixed_vert_lpf_spc keeps the wire's base-function names 1:1 with the files, so a
// later correction of the blur passes cannot silently change the layer passes.

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

    // Same four-column multiply the fixed vertex function uses: out = col0*x + col1*y + col2*z +
    // col3*w.
    vec4 m0 = wire_f32x4(offsets.vertex_mvp + 0);
    vec4 m1 = wire_f32x4(offsets.vertex_mvp + 16);
    vec4 m2 = wire_f32x4(offsets.vertex_mvp + 32);
    vec4 m3 = wire_f32x4(offsets.vertex_mvp + 48);
    gl_Position = m0 * p.x + m1 * p.y + m2 * p.z + m3 * p.w;

    // The recorded blur draws bind the texture matrix (vertex buffer 3); the blur fragment
    // shaders expect v_texcoord0 normalised to the source region, so applying it here is what
    // turns the wire's pixel-space texcoords into sampling coordinates.
    vec2 tc0 = in_texcoord0;
    if (USE_TEXTURE_MATRIX != 0) {
        vec4 texture_matrix = wire_f32x4(offsets.vertex_texmat);
        tc0 = tc0 * texture_matrix.xy + texture_matrix.zw;
    }

    v_texcoord0 = tc0;
    v_texcoord1 = in_texcoord1;
    v_color = in_color;
}
