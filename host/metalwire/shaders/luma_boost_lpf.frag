#version 450
#extension GL_EXT_samplerless_texture_functions : enable
// SPDX-License-Identifier: MIT
//
// luma_boost_lpf -- hand-written Vulkan GLSL for CoreAnimation's luma-boost base fragment
// function (pipeline com.apple.coreanimation.draw.PBGRA_TlubXf_Isrc).
//
// The wire's third draw reads the frame that the first two draws wrote (Metal framebuffer fetch,
// [[color(0)]]) and the 128x1 R8Unorm "luma-boost ramp" CoreAnimation creates (fragment texture 3).
// This shader is that post-pass: it reads the destination, uses its luminance to index the ramp,
// scales the colour, and writes the same pixel back. A ramp of zero (the state of the wire's
// texture, because Metal's replaceRegion: is not serialized yet) makes it the identity, which is
// what the WP-2 execution is allowed to be: it executes the pass without claiming a picture.
//
// Interface: the same wire buffer, the R8 texture at fragment texture 3, sampler 0, and the
// destination snapshot as a sampled texture. The R8 texture is the first non-BGRA8 format on the
// wire; the shader reads it through a normal sampler2D and takes .r.

layout(location = 0) in vec2 v_texcoord0;
layout(location = 1) in vec4 v_color;

layout(set = 0, binding = 0, std430) readonly buffer WireBuffer {
    uint wire[];
};
layout(set = 0, binding = 1) uniform texture2D u_image;  // 128x1 R8Unorm ramp
layout(set = 0, binding = 2) uniform sampler u_sampler;
layout(set = 0, binding = 3) uniform texture2D u_fetch;  // destination snapshot

layout(push_constant) uniform WireOffsets {
    uint vertex_mvp;
    uint vertex_texmat;
    uint fragment_uniform;
    uint fragment_lod;
} offsets;

layout(location = 0) out vec4 out_color;

vec2 wire_half2(uint byte_offset)
{
    return unpackHalf2x16(wire[byte_offset >> 2]);
}

void main()
{
    vec4 dst = texelFetch(u_fetch, ivec2(gl_FragCoord.xy), 0);
    float luma = dot(dst.rgb, vec3(0.2126, 0.7152, 0.0722));
    float ramp = texture(sampler2D(u_image, u_sampler), vec2(clamp(luma, 0.0, 1.0), 0.5)).r;

    // Uniform's hdr_scale (half2 at offset 68). Zero means "no EDR scale", so the pass is the
    // identity, exactly like the record the wire carries; a non-zero scale multiplies the
    // destination by a luminance-indexed ramp factor.
    vec2 hdr = wire_half2(offsets.fragment_uniform + 68u);
    float factor = hdr.x > 0.0 ? hdr.x * (1.0 + ramp) : 1.0;

    out_color = vec4(dst.rgb * factor, dst.a);
}
