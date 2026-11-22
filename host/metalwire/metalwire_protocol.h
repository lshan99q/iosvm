/* SPDX-License-Identifier: MIT */
/*
 * metalwire: the wire between a Metal device inside the guest and the Vulkan translator on the
 * host. One TCP connection, little-endian messages, one acknowledgement per committed frame.
 *
 * This header is shared by the guest plug-in (guest/metal-plugin) and the host translator
 * (host/metalwire); it is the whole contract between them. The design is deliberately the same
 * shape as glwire's: the guest records what it was asked to do, the host executes it and answers
 * once per frame, and the picture lives on the host.
 *
 *   guest                                      host
 *   MW_FRAME_BEGIN(seq)  ───────────────────►  buffers the frame's commands
 *   MW_CREATE_BUFFER / MW_UPLOAD / ...  ─────►  records objects, uploads
 *   MW_COPY_BUFFER_TO_TEXTURE           ─────►  one copy
 *   MW_PRESENT                          ─────►  which texture is the frame
 *   MW_FRAME_END(seq, crc, times)  ──────────►  executes: copy on the GPU, reads back,
 *                                               publishes the picture, verifies the CRC
 *                        ◄────────────────────  MW_ACK(seq, times, status, crc result)
 *
 * The guest may pipeline frames; the acknowledgement names the frame it belongs to. A status of
 * MW_STATUS_OK with crc_ok=1 means the host read back exactly the bytes the guest says it wrote.
 */
#ifndef METALWIRE_PROTOCOL_H
#define METALWIRE_PROTOCOL_H

#include <stdint.h>

#define MW_MAGIC 0x4D575452u     /* "MWTR" */
#define MW_ACK_MAGIC 0x4D57414Bu /* "MWAK" */
/* 1: the blit/present contract (stage 29). 2: adds the render-encoder opcodes (stage C). 3: the
 * WP-3 additions: the two per-draw buffer-offset setters CoreAnimation calls between its draws,
 * texture content (replaceRegion:) and the IOSurface binding of an existing texture. 4 (WP-4):
 * dynamic-pipeline capture: a Metal library is described by its hash, size and entry-function
 * names (MW_CREATE_LIBRARY), and a render pipeline says which library each of its functions came
 * from (MW_PIPELINE_LIBRARY_REF). Both are optional messages a v4 guest may send; v2/v3 guests
 * keep their exact wire shape and the host still accepts every version. */
#define MW_VERSION 4u

/* the message header, on the wire as it is in memory (little-endian hosts only; the translator
 * refuses a big-endian one) */
struct mw_msg {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t length;   /* payload bytes that follow */
    uint32_t reserved; /* zero */
};

/* message types: 0x0* control, 0x1* objects, 0x3* drawing, 0x5* frames, 0x6* render encoders,
 * 0x7* render objects */
enum mw_type {
    MW_HELLO = 0x0001,
    MW_BYE = 0x0002,

    MW_CREATE_BUFFER = 0x0010,
    MW_UPLOAD_BUFFER = 0x0011,
    MW_DESTROY_BUFFER = 0x0012,
    /* the same upload, by reference: the bytes stay in guest RAM and the host reads the QEMU
     * RAM file at the physical addresses the guest names (the port's hakios_vtop syscall,
     * the mechanism the GL replay path already uses). One small message per frame instead of
     * 4.87 MB down the socket. */
    MW_UPLOAD_BUFFER_PHYS = 0x0014,
    MW_CREATE_TEXTURE = 0x0020,
    MW_DESTROY_TEXTURE = 0x0021,
    /* WP-3: the bytes Metal's -replaceRegion:mipmapLevel:withBytes:bytesPerRow: (and its
     * slice/bytesPerImage sibling) hands to the texture. The bytes follow the structure inside
     * the same message, like MW_UPLOAD_BUFFER. */
    MW_TEXTURE_REPLACE_REGION = 0x0022,
    /* WP-3: the physical segments behind a texture that was made from an IOSurface. The host
     * keeps them so it can put the rendered picture into the guest's real surface and read it
     * back by reference (hakios_vtop's mechanism, the same one MW_UPLOAD_BUFFER_PHYS uses). */
    MW_TEXTURE_SET_IOSURFACE = 0x0023,

    MW_COPY_BUFFER_TO_TEXTURE = 0x0030,
    MW_FILL_BUFFER = 0x0031,

    MW_PRESENT = 0x0040,

    MW_FRAME_BEGIN = 0x0050,
    MW_FRAME_END = 0x0051,

    /* A render encoder, replayed call by call (stage C). The messages arrive in the order
     * CoreAnimation recorded them; an object is created (MW_CREATE_* above / 0x0073-0x0075)
     * before the first message that names it. MW_RENDER_BEGIN starts an encoder on its render
     * target; MW_RENDER_END closes it. The frame that carries render messages uses
     * MW_FRAME_END with crc == 0 and no guest checksum: the guest did not draw the picture, so
     * it cannot hash it, and the host answers crc_ok = 1 without a read-back until the host
     * execution half of stage C exists. */
    MW_RENDER_BEGIN = 0x0060,
    MW_RENDER_SET_PIPELINE = 0x0061,
    MW_RENDER_SET_DEPTH_STENCIL = 0x0062,
    MW_RENDER_SET_VERTEX_BUFFER = 0x0063,
    MW_RENDER_SET_VERTEX_BYTES = 0x0064,
    MW_RENDER_SET_VERTEX_TEXTURE = 0x0065,
    MW_RENDER_SET_VERTEX_SAMPLER = 0x0066,
    MW_RENDER_SET_FRAGMENT_BUFFER = 0x0067,
    MW_RENDER_SET_FRAGMENT_BYTES = 0x0068,
    MW_RENDER_SET_FRAGMENT_TEXTURE = 0x0069,
    MW_RENDER_SET_FRAGMENT_SAMPLER = 0x006A,
    MW_RENDER_SET_VIEWPORT = 0x006B,
    MW_RENDER_SET_SCISSOR = 0x006C,
    MW_RENDER_SET_CULL_MODE = 0x006D,
    MW_RENDER_SET_WINDING = 0x006E,
    MW_RENDER_SET_FILL_MODE = 0x006F,
    MW_RENDER_SET_BLEND_COLOR = 0x0070,
    MW_RENDER_DRAW = 0x0071,
    MW_RENDER_END = 0x0072,

    MW_CREATE_RENDER_PIPELINE = 0x0073,
    MW_CREATE_SAMPLER = 0x0074,
    MW_CREATE_DEPTH_STENCIL = 0x0075,
    /* WP-3: -setVertexBufferOffset:atIndex: / -setFragmentBufferOffset:atIndex:. Metal replaces
     * the offset of the buffer already bound at the index; it does not bind a new buffer. */
    MW_RENDER_SET_VERTEX_BUFFER_OFFSET = 0x0076,
    MW_RENDER_SET_FRAGMENT_BUFFER_OFFSET = 0x0077,
    /* WP-4: a Metal library the guest was handed (AIR container). The message carries the
     * library's hash and entry-function names, never its bytes: the host cannot compile AIR, so
     * the hash and the names are what identify a pipeline whose base function it does not have.
     * The variable part follows as mw_library_function[function_count].
     */
    MW_CREATE_LIBRARY = 0x0078,
    /* WP-4: after MW_CREATE_RENDER_PIPELINE, which library the vertex and fragment function came
     * from. 0 means "not known". The host logs it with a skipped pipeline so the missing shader
     * has a provenance. */
    MW_PIPELINE_LIBRARY_REF = 0x0079,
};

enum mw_status {
    MW_STATUS_OK = 0,
    MW_STATUS_BAD_COMMAND = 1,
    MW_STATUS_VULKAN = 2,
    MW_STATUS_NO_MEMORY = 3,
    MW_STATUS_CRC_MISMATCH = 4,
};

/* the pixel formats metalwire knows: Metal's MTLPixelFormat numbers */
enum mw_pixel_format {
    MW_FORMAT_A8_UNORM = 1,     /* MTLPixelFormatA8Unorm: CA's alpha-only glyph/label masks */
    MW_FORMAT_BGRA8_UNORM = 80, /* MTLPixelFormatBGRA8Unorm */
    MW_FORMAT_RGBA8_UNORM = 70, /* MTLPixelFormatRGBA8Unorm */
    MW_FORMAT_R8_UNORM = 10,    /* MTLPixelFormatR8Unorm: CoreAnimation's luma-boost ramp */
    /* MTLPixelFormatRGBA16Float: four IEEE-754 binary16 values, 8 bytes a texel. CoreAnimation
     * hands the compositor its colour ramp in this format. */
    MW_FORMAT_RGBA16_FLOAT = 115,
};

/* MW_HELLO */
struct mw_hello {
    uint32_t version;
    uint32_t pid;
    char program[32];
};

/* MW_CREATE_BUFFER: LENGTH bytes of storage the guest will upload into */
struct mw_create_buffer {
    uint32_t id;
    uint32_t options; /* Metal's MTLResourceOptions, for the log */
    uint64_t length;
};

/* MW_UPLOAD_BUFFER: the bytes follow the structure */
struct mw_upload_buffer {
    uint32_t id;
    uint32_t offset;
    uint32_t length;
    uint32_t reserved;
};

/* One physically contiguous run of guest memory (the layout of the kernel's hakios_vtop
 * segments, bsd/sys/hakios.vtop.h: 16 bytes, PA then length then a reserved word). */
struct mw_segment {
    uint64_t pa;
    uint32_t len;
    uint32_t reserved;
};

/* MW_UPLOAD_BUFFER_PHYS: the segments follow this structure. Their lengths must add up to
 * `length`, and `pa` is a guest physical address (the host subtracts its RAM base). */
struct mw_upload_buffer_phys {
    uint32_t id;
    uint32_t offset;
    uint32_t length;
    uint32_t segment_count;
};

/* MW_CREATE_TEXTURE */
struct mw_create_texture {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format; /* enum mw_pixel_format */
    uint32_t usage;        /* Metal's MTLTextureUsage, for the log */
    uint32_t reserved;
};

/* MW_TEXTURE_REPLACE_REGION: the bytes follow the structure. `slice` is MTLTexture's array slice
 * (0 for CoreAnimation's 2D textures), `bytes_per_image` 0 when the caller did not pass one.
 * `width`/`height`/`depth` are the MTLRegion's extent; `bytes_per_row` is the caller's stride. */
struct mw_texture_replace_region {
    uint32_t texture_id;
    uint32_t mipmap_level;
    uint32_t slice;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t origin_z;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    uint32_t length;
};

/* MW_TEXTURE_SET_IOSURFACE: mw_segment[segment_count] follows the structure. The segments cover
 * the IOSurface's allocation, so the host can address any byte of the surface by walking them. A
 * guest with no vtop answer sends segment_count 0: the host then keeps the metadata and falls
 * back to its own read-back for the picture. */
struct mw_texture_set_iosurface {
    uint32_t texture_id;
    uint32_t bytes_per_row; /* the IOSurface's stride, which may exceed width*4 */
    uint32_t alloc_size;
    uint32_t segment_count;
};

/* MW_COPY_BUFFER_TO_TEXTURE: one row-major blit, the way a Metal blit encoder does it */
struct mw_copy_buffer_to_texture {
    uint32_t buffer_id;
    uint32_t texture_id;
    uint64_t buffer_offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
};

/* MW_PRESENT: the texture that is this frame's picture */
struct mw_present {
    uint32_t texture_id;
    uint32_t flags;
};

/* MW_FRAME_BEGIN: timestamps are mach_absolute_time() converted to nanoseconds on the guest */
struct mw_frame_begin {
    uint64_t seq;
    uint64_t guest_t_begin_ns;
};

/* MW_FRAME_END: the guest has finished recording this frame; the host executes and answers */
struct mw_frame_end {
    uint64_t seq;
    uint64_t guest_t_encoded_ns; /* begin -> the last command recorded */
    uint32_t crc;                /* CRC-32 of the picture bytes the guest believes it sent */
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
};

/* the host's answer to one MW_FRAME_END */
struct mw_ack {
    uint32_t magic;
    uint16_t version;
    uint16_t status; /* enum mw_status */
    uint64_t seq;
    uint64_t guest_t_commit_ns; /* echoed, so the guest can pair the ack with its frame */
    uint64_t host_t_done_ns;    /* CLOCK_MONOTONIC at the end of the host's work */
    uint32_t crc;               /* the CRC the host computed over its read-back */
    uint32_t crc_ok;            /* 1: crc is the guest's; 0: it differs */
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    uint32_t reserved;
};

/* ---- render encoders (stage C, protocol 2) --------------------------------------------------
 *
 * The guest records an MTLRenderCommandEncoder as the calls CoreAnimation made, in order, and
 * sends them between MW_FRAME_BEGIN and MW_FRAME_END. Object creation is hoisted: the pipeline
 * state, buffers and textures a call names have their own message first, so the host can apply
 * the stream in one pass. The host keeps per-encoder state (pipeline, bound buffers/textures,
 * viewport, ...) and executes a draw with the state that preceded it, exactly like Metal does.
 *
 * The names in these structures are UTF-8, zero-padded and not guaranteed terminated: a reader
 * uses strnlen(name, MW_NAME_MAX).
 */
#define MW_NAME_MAX 64
#define MW_MAX_RENDER_ATTRIBUTES 16
#define MW_MAX_RENDER_LAYOUTS 8

#define MW_DRAW_INDEXED 0x0001u
#define MW_DRAW_INSTANCED 0x0002u
#define MW_DRAW_BASE_VERTEX 0x0004u
#define MW_DRAW_BASE_INSTANCE 0x0008u

#define MW_STAGE_VERTEX 1u
#define MW_STAGE_FRAGMENT 2u

/* MW_RENDER_BEGIN: one render encoder, its render pass's color attachment 0, and an optional
 * depth attachment. CoreAnimation's draw pipelines use one color target, 750x1624 BGRA8, no
 * depth; the structure carries the general shape so a later client does not need a new opcode. */
struct mw_render_begin {
    uint32_t encoder_index;
    uint32_t target_texture;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t load_action;
    uint32_t store_action;
    float clear_color[4];
    uint32_t has_depth;
    uint32_t depth_texture;
    uint32_t depth_load_action;
    uint32_t depth_store_action;
    float clear_depth;
    uint32_t clear_stencil;
    uint32_t reserved;
};

struct mw_render_set_pipeline {
    uint32_t pipeline_id;
    uint32_t reserved;
};

struct mw_render_set_depth_stencil {
    uint32_t state_id;
    uint32_t reserved;
};

struct mw_render_set_buffer {
    uint32_t buffer_id;
    uint32_t index;
    uint64_t offset;
};

/* MW_RENDER_SET_VERTEX_BUFFER_OFFSET / MW_RENDER_SET_FRAGMENT_BUFFER_OFFSET:
 * -setVertexBufferOffset:atIndex: / -setFragmentBufferOffset:atIndex:. The buffer at `index` is
 * not named: Metal replaces the offset of the binding that is already there. The host applies it
 * to the encoder's current binding and a later draw sees it, exactly as Metal does. */
struct mw_render_set_buffer_offset {
    uint32_t index;
    uint32_t reserved;
    uint64_t offset;
};

/* MW_RENDER_SET_VERTEX_BYTES / MW_RENDER_SET_FRAGMENT_BYTES: the length bytes follow this
 * structure inside the same mw_msg, the same convention MW_UPLOAD_BUFFER uses. */
struct mw_render_set_bytes {
    uint32_t index;
    uint32_t length;
    uint32_t stage; /* MW_STAGE_* */
    uint32_t reserved;
};

struct mw_render_set_texture {
    uint32_t texture_id;
    uint32_t index;
};

struct mw_render_set_sampler {
    uint32_t sampler_id;
    uint32_t index;
};

struct mw_render_set_viewport {
    float x;
    float y;
    float width;
    float height;
    float znear;
    float zfar;
};

struct mw_render_set_scissor {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct mw_render_set_state {
    uint32_t value;
    uint32_t reserved;
};

struct mw_render_set_blend_color {
    float red;
    float green;
    float blue;
    float alpha;
};

/* MW_RENDER_DRAW: one draw call with the state that the encoder holds when it arrives. */
struct mw_render_draw {
    uint32_t primitive_type; /* MTLPrimitiveType */
    uint32_t flags;          /* MW_DRAW_* */
    uint32_t index_type;     /* MTLIndexType, when indexed */
    uint32_t index_buffer;   /* wire buffer id, when indexed */
    uint32_t instance_count;
    uint32_t base_instance;
    uint64_t vertex_start;
    uint64_t vertex_count;
    uint64_t index_count;
    uint64_t index_buffer_offset;
    int64_t base_vertex;
};

struct mw_render_end {
    uint32_t encoder_index;
    uint32_t draw_count;
    uint32_t set_count;
    uint32_t reserved;
};

/* MW_CREATE_RENDER_PIPELINE: the pipeline state's identity and the descriptor fields a Vulkan
 * pipeline needs. A pipeline that came from the pipeline library by name carries the name and no
 * functions (the library's function modules are stage C's WP-2); a pipeline made from a descriptor
 * carries the base function names. The variable part follows as
 * mw_vertex_attribute[vertex_attribute_count] then mw_vertex_layout[vertex_layout_count]. */
struct mw_create_render_pipeline {
    uint32_t id;
    uint32_t color_format; /* MTLPixelFormat, 0 when the descriptor did not say */
    uint32_t sample_count;
    uint32_t blend_enabled;
    uint32_t rgb_blend_operation;
    uint32_t alpha_blend_operation;
    uint32_t src_rgb_blend_factor;
    uint32_t dst_rgb_blend_factor;
    uint32_t src_alpha_blend_factor;
    uint32_t dst_alpha_blend_factor;
    uint32_t write_mask;
    uint32_t vertex_attribute_count;
    uint32_t vertex_layout_count;
    uint32_t reserved;
    char pipeline_name[MW_NAME_MAX];
    char vertex_function[MW_NAME_MAX];
    char fragment_function[MW_NAME_MAX];
};

struct mw_vertex_attribute {
    uint32_t format; /* MTLVertexFormat */
    uint32_t offset;
    uint32_t buffer_index;
    uint32_t reserved;
};

struct mw_vertex_layout {
    uint32_t stride;
    uint32_t step_function; /* MTLVertexStepFunction */
    uint32_t step_rate;
    uint32_t reserved;
};

/* MW_CREATE_SAMPLER: MTL.SamplerDescriptor's fields, as CoreAnimation's samplers carry them. */
struct mw_create_sampler {
    uint32_t id;
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t mip_filter;
    uint32_t address_s;
    uint32_t address_t;
    uint32_t address_r;
    uint32_t normalized_coords;
    uint32_t compare_function;
    float lod_min;
    float lod_max;
    uint32_t max_anisotropy;
    uint32_t reserved;
};

/* MW_CREATE_DEPTH_STENCIL: MTLDepthStencilDescriptor's depth half (the depth state CoreAnimation
 * binds; the render pass has no depth attachment in this stage's boots, but the state is recorded
 * so a later pass can use it). */
struct mw_create_depth_stencil {
    uint32_t id;
    uint32_t depth_compare;
    uint32_t depth_write;
    uint32_t reserved;
};

/* MW_CREATE_LIBRARY: where the library came from, how big it was and what it hashed to. The hash
 * is FNV-1a/64 of the bytes when the guest could read them (newLibraryWithData: always, a file or
 * URL when it is readable), 0 when it could not. byte_length is 0 then. The entry-function names
 * follow as mw_library_function[function_count]; they are the names the guest asked that library
 * for, which is what a pipeline's vertex_function/fragment_function name. */
enum mw_library_kind {
    MW_LIBRARY_UNKNOWN = 0,
    MW_LIBRARY_DATA = 1,  /* -newLibraryWithData:error: (the AIR bytes are in hand) */
    MW_LIBRARY_URL = 2,   /* -newLibraryWithURL:error: */
    MW_LIBRARY_FILE = 3,  /* -newLibraryWithFile:error: */
    MW_LIBRARY_DEFAULT = 4, /* -newDefaultLibrary */
    MW_LIBRARY_PIPELINE = 5, /* -newPipelineLibraryWithFilePath:error: */
};

#define MW_MAX_LIBRARY_FUNCTIONS 64

struct mw_create_library {
    uint32_t id;
    uint32_t kind; /* enum mw_library_kind */
    uint32_t function_count;
    uint32_t reserved;
    uint64_t hash;        /* FNV-1a/64 of the container bytes, 0 when unknown */
    uint64_t byte_length; /* container length, 0 when unknown */
};

struct mw_library_function {
    char name[MW_NAME_MAX];
    uint64_t name_hash; /* FNV-1a/64 of name, so two spellings cannot collide silently */
};

/* MW_PIPELINE_LIBRARY_REF: the library ids behind a render pipeline's two functions. Sent by a v4
 * guest right after the pipeline's MW_CREATE_RENDER_PIPELINE; the host copies them onto the
 * pipeline record and prints them when the pipeline has no shader. */
struct mw_pipeline_library_ref {
    uint32_t pipeline_id;
    uint32_t vertex_library_id;
    uint32_t fragment_library_id;
    uint32_t reserved;
};

#endif /* METALWIRE_PROTOCOL_H */
