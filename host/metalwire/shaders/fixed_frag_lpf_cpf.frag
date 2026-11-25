#version 450
#extension GL_EXT_samplerless_texture_functions : enable
// SPDX-License-Identifier: MIT
//
// fixed_frag_lpf_cpf -- hand-written Vulkan GLSL equivalent of CoreAnimation's Metal base fragment
// function of the same name (the `lpf`/BGRA8 runtime-specialisation family; WP-2 of stage C).
//
// The base function is the layer-pass fragment shader of CA's Metal renderer. The wire shows the
// same base name on two pipelines that are specialised differently:
//   * com.apple.coreanimation.draw.PBGRA_Xfc                 -- solid fill, no texture, no dest read
//   * com.apple.coreanimation.draw.PBGRABsov_TimgXfc_Isrc    -- textured, reads the destination
//                                                                and does source-over in-shader
// The Metal function constants that select those paths (and the sibling AIR modules' baked
// branches) become Vulkan specialization constants here.
//
// Metal's framebuffer fetch input ([[color(0)]]) is not available in core Vulkan, so the translator
// binds a snapshot of the colour attachment as a sampled 2D texture (`u_fetch`) and reads it with
// texelFetch. That is an explicit, portable spelling of the same operation; see the WP-2 note.
//
// Interface:
//   set 0, binding 0       the wire buffer (all Metal buffer bindings, word-addressed)
//   set 0, binding 1       Metal fragment texture 3    (BGRA8 layer image, or R8 ramp for the
//                                                      luma pass which uses its own shader)
//   set 0, binding 2       Metal fragment sampler 0
//   set 0, binding 3       the destination colour snapshot (Metal's implicit color(0) input)
//   push constants         byte offsets of the fragment Uniform and the lod bias
//
// CA::OGL::Metal::Uniform layout (176 bytes) as the on-disk lph modules carry it:
//   0    float4 image_arg[3]       48
//   48   half4  blend_colors[2]    64
//   64   half   edr_factors[2]     68
//   68   half   hdr_scale[2]       72
//   72   uchar  premultiply        73
//   73   uchar  linear_edr         76
//   76   half2  clamp_range        80
//   80   float4 filter_arg[2]     112
//   112  float4 coord_arg         128
//   128  union  function_arg      168..176

layout(location = 0) in vec2 v_texcoord0;
layout(location = 1) in vec4 v_color;

layout(set = 0, binding = 0, std430) readonly buffer WireBuffer {
    uint wire[];
};
layout(set = 0, binding = 1) uniform texture2D u_image;
layout(set = 0, binding = 2) uniform sampler u_sampler;
layout(set = 0, binding = 3) uniform texture2D u_fetch;

layout(push_constant) uniform WireOffsets {
    uint vertex_mvp;
    uint vertex_texmat;
    uint fragment_uniform;
    uint fragment_lod;
} offsets;

layout(location = 0) out vec4 out_color;

layout(constant_id = 0) const int HAS_TEXTURE = 0;
layout(constant_id = 1) const int FETCH_DEST = 0;
layout(constant_id = 2) const int PREMULTIPLY = 0;

uint wire_u8(uint byte_offset)
{
    return (wire[byte_offset >> 2] >> ((byte_offset & 3u) * 8u)) & 0xffu;
}

vec4 wire_half4(uint byte_offset)
{
    return vec4(unpackHalf2x16(wire[byte_offset >> 2]),
                unpackHalf2x16(wire[(byte_offset + 4) >> 2]));
}

vec2 wire_half2(uint byte_offset)
{
    return unpackHalf2x16(wire[byte_offset >> 2]);
}

void main()
{
    vec4 src = v_color;

    if (HAS_TEXTURE != 0) {
        // texture 3 / sampler 0 as the wire binds them; the layer image is premultiplied BGRA.
        src = texture(sampler2D(u_image, u_sampler), v_texcoord0) * v_color;
    }

    // The uniform's premultiply flag and the "P" in the pipeline name both say the source is
    // premultiplied; the specialization constant covers a pipeline whose flag was baked.
    bool premultiplied =
        PREMULTIPLY != 0 || wire_u8(offsets.fragment_uniform + 72u) != 0u;
    if (premultiplied)
        src.rgb *= src.a;

    vec4 dst = vec4(0.0);
    if (FETCH_DEST != 0)
        dst = texelFetch(u_fetch, ivec2(gl_FragCoord.xy), 0);

    // Source-over, using the framebuffer fetch the Metal function uses in place of fixed-function
    // blending. This is the whole reason the pipeline descriptor can say blendingEnabled = NO.
    vec3 rgb = src.rgb + dst.rgb * (1.0 - src.a);
    float alpha = src.a + dst.a * (1.0 - src.a);
    out_color = vec4(rgb, alpha);
}
