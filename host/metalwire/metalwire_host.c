/* SPDX-License-Identifier: MIT */
/*
 * metalwire_host -- the host end of the Metal -> Vulkan translation experiment.
 *
 * A headless TCP server that speaks the metalwire protocol (metalwire_protocol.h, PROTOCOL.md):
 * a guest Metal device (guest/metal-plugin) records its buffers, textures, blits and presents,
 * sends them here, and this program executes them as Vulkan transfer commands on one chosen
 * physical device. Each MW_FRAME_END is answered with one MW_ACK after the texture has been
 * copied back, CRC-checked and published into glwire's shared-memory frame format
 * (tools/glwire/webview.py serves that file unchanged).
 *
 * It is a measurement tool, not a display server: no window, no surface, no swapchain. Nothing
 * here is Metal -- a Metal call became a wire message before this program saw it.
 *
 *   ./metalwire_host --gpu 1 --port 9111 --frames 100 --csv /tmp/mw.csv
 *   ./metalwire_gen.py 750 1624 --frames 100 --port 9111
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "metalwire_protocol.h"
#include "build/wp2_shaders.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "metalwire: the wire format and the SHM header are little-endian host order"
#endif

#define MW_DEFAULT_PORT 9111
#define MW_DEFAULT_SHM "/dev/shm/metalwire-frame"
#define MW_MAX_MESSAGE (512u * 1024u * 1024u)
#define MW_MAX_COPIES 64
#define MW_PIXEL_BYTES 4
#define MW_SHM_MAGIC 0x57464C47u /* "GLFW", glwire's magic */
#define MW_ROLLING_NS 1000000000ull

/* The wire structs must be exactly the shared header's layout; the guest uses the same file. */
_Static_assert(sizeof(struct mw_msg) == 16, "mw_msg layout");
_Static_assert(sizeof(struct mw_hello) == 40, "mw_hello layout");
_Static_assert(sizeof(struct mw_create_buffer) == 16, "mw_create_buffer layout");
_Static_assert(sizeof(struct mw_upload_buffer) == 16, "mw_upload_buffer layout");
_Static_assert(sizeof(struct mw_create_texture) == 24, "mw_create_texture layout");
_Static_assert(sizeof(struct mw_copy_buffer_to_texture) == 40, "mw_copy layout");
_Static_assert(sizeof(struct mw_present) == 8, "mw_present layout");
_Static_assert(sizeof(struct mw_frame_begin) == 16, "mw_frame_begin layout");
_Static_assert(sizeof(struct mw_frame_end) == 32, "mw_frame_end layout");
_Static_assert(sizeof(struct mw_ack) == 56, "mw_ack layout");

/* glwire's shared-memory header; kept byte-compatible with publish_picture() in
 * tools/glwire/host_glwire.c and HEADER = struct.Struct("<QQIIIIQ") in tools/glwire/webview.py. */
struct mw_shm_header {
    uint64_t magic;
    uint64_t seq;
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint32_t pad;
    uint64_t published_ns;
};

_Static_assert(sizeof(struct mw_shm_header) == 40, "glwire shm header layout");

struct mw_shm {
    char *path; /* NULL or empty: publishing disabled */
    int failed;
    int fd;
    struct mw_shm_header *header;
    size_t map_size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
};

struct mw_buffer {
    uint32_t id;
    uint32_t options;
    uint64_t length;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
};

struct mw_image {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t format; /* enum mw_pixel_format */
    uint32_t bytes;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *readback_mapped;
    int dst_this_frame;
    /* WP-3: a texture created from a real IOSurface. bytes_per_row is the surface's stride (it
     * may be larger than width*4); the segments are the guest-physical runs behind it, so the host
     * can write the rendered picture into the surface and read it back by reference. */
    int has_surface;
    uint32_t surface_bytes_per_row;
    uint32_t surface_alloc_size;
    uint32_t surface_segment_count;
    struct mw_segment *surface_segments;
};

/* One queued MW_TEXTURE_REPLACE_REGION: the guest handed over the bytes recursively; the host
 * uploads them to the image at the start of the next executed frame and frees this record. */
struct mw_pending_replace {
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
    unsigned char *bytes;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    void *staging_mapped;
};

struct mw_copy {
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

/* ---- stage C render execution -------------------------------------------------------------- */

#define MW_MAX_RENDER_BINDINGS 16
#define MW_MAX_RENDER_DRAWS 512

/* The state an MTLRenderCommandEncoder holds between calls, as the host must replay it. Offsets
 * are wire (Metal) offsets into the named buffer. */
struct mw_render_encoder_state {
    int active;
    uint32_t target_texture;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t load_action;
    uint32_t store_action;
    float clear_color[4];
    uint32_t pipeline_id;
    uint32_t vertex_buffer_id[MW_MAX_RENDER_BINDINGS];
    uint64_t vertex_buffer_offset[MW_MAX_RENDER_BINDINGS];
    uint32_t fragment_buffer_id[MW_MAX_RENDER_BINDINGS];
    uint64_t fragment_buffer_offset[MW_MAX_RENDER_BINDINGS];
    uint32_t texture_id[MW_MAX_RENDER_BINDINGS];
    uint32_t sampler_id[MW_MAX_RENDER_BINDINGS];
    float viewport[6];
    uint32_t scissor[4];
    int has_viewport;
    int has_scissor;
};

/* One draw with the encoder state that preceded it, so state changes between draws are kept. */
struct mw_render_draw_command {
    uint32_t target_texture;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t load_action;
    uint32_t store_action;
    float clear_color[4];
    uint32_t pipeline_id;
    uint32_t primitive_type;
    uint32_t flags;
    uint32_t index_type;
    uint32_t index_buffer;
    uint64_t index_count;
    uint64_t index_buffer_offset;
    uint64_t vertex_start;
    uint64_t vertex_count;
    uint32_t instance_count;
    uint32_t base_instance;
    uint64_t base_vertex;
    uint32_t vertex_buffer_id[MW_MAX_RENDER_BINDINGS];
    uint64_t vertex_buffer_offset[MW_MAX_RENDER_BINDINGS];
    uint32_t fragment_buffer_id[MW_MAX_RENDER_BINDINGS];
    uint64_t fragment_buffer_offset[MW_MAX_RENDER_BINDINGS];
    uint32_t texture_id[MW_MAX_RENDER_BINDINGS];
    uint32_t sampler_id[MW_MAX_RENDER_BINDINGS];
    float viewport[6];
    uint32_t scissor[4];
};

struct mw_sampler_state {
    uint32_t id;
    VkSampler sampler;
};

/* The Vulkan objects the execution half needs beyond the blit path: one descriptor set layout
 * for the wire buffer, the texture, the sampler and the destination snapshot; a 1x1 dummy image
 * for bindings a draw does not use; a fetch image that holds the colour attachment snapshot the
 * Metal shader reads as [[color(0)]]; and the dummy sampler. Created on first render execution. */
struct mw_render_vk {
    int ready;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    VkDescriptorPool pool;
    VkImage dummy_image;
    VkDeviceMemory dummy_memory;
    VkImageView dummy_view;
    VkSampler dummy_sampler;
    VkShaderModule modules[16];
    VkImage fetch_image;
    VkDeviceMemory fetch_memory;
    VkImageView fetch_view;
    uint32_t fetch_width;
    uint32_t fetch_height;
    uint32_t fetch_format;
};

struct mw_frame_state {
    int active;
    uint64_t seq;
    uint64_t begin_ns;
    uint64_t guest_t_begin_ns;
    uint64_t guest_t_encoded_ns;
    uint32_t guest_crc;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    int error; /* enum mw_status; the first failure in this frame */
    struct mw_copy copies[MW_MAX_COPIES];
    size_t copy_count;
    int has_present;
    uint32_t present_texture;
    /* stage C: the frame carries render encoders instead of a blit. Until the host execution half
     * exists the frame is counted and acknowledged, not executed. */
    int render_mode;
    uint32_t render_target;
    uint32_t render_pipeline;
    uint32_t render_encoders;
    uint64_t render_draw_count;
    uint64_t render_set_count;
    struct mw_render_encoder_state encoder;
    struct mw_render_draw_command draws[MW_MAX_RENDER_DRAWS];
    uint32_t draw_count;
};

/* A render pipeline state the guest created; WP-2 resolves its shaders from this record. */
struct mw_render_pipeline {
    uint32_t id;
    uint32_t color_format;
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
    struct mw_vertex_attribute attributes[MW_MAX_RENDER_ATTRIBUTES];
    struct mw_vertex_layout layouts[MW_MAX_RENDER_LAYOUTS];
    char name[MW_NAME_MAX];
    char vertex_function[MW_NAME_MAX];
    char fragment_function[MW_NAME_MAX];
    /* stage C execution: the Vulkan pipeline is created on first use (the wire's pipeline bytes
     * are enough) and cached on this record, one per Metal primitive topology. */
    VkPipeline vk_pipeline[5];
    int vk_attempted[5];
    int vk_failed;
    /* WP-4: the library ids a v4 guest reports for the two functions (0 = not known), the
     * "no shader for this base function" verdict, and how many draws it cost. A pipeline is
     * marked unsupported once, logged once, and every draw that names it is skipped instead of
     * failing the frame. */
    uint32_t vertex_library_id;
    uint32_t fragment_library_id;
    int unsupported;
    int unsupported_logged;
    uint64_t skipped_draws;
};

/* WP-4: what a v4 guest says about a Metal library: its provenance, hash and entry functions. The
 * host never sees the AIR bytes and cannot compile them; this record is what a skipped pipeline is
 * traced back to. */
struct mw_library {
    uint32_t id;
    uint32_t kind; /* enum mw_library_kind */
    uint64_t hash;
    uint64_t byte_length;
    uint32_t function_count;
    char functions[MW_MAX_LIBRARY_FUNCTIONS][MW_NAME_MAX];
    uint64_t function_hashes[MW_MAX_LIBRARY_FUNCTIONS];
};

struct mw_timing {
    uint64_t recv_ns;
    uint64_t exec_ns;
    uint64_t gpu_ns;
    uint64_t readback_ns;
    uint64_t publish_ns;
    uint64_t total_ns;
};

struct mw_frame_result {
    uint32_t crc;
    uint32_t crc_ok;
    uint64_t exec_ns;
    uint64_t gpu_ns;
    uint64_t readback_ns;
    uint64_t publish_ns;
    /* WP-4 fallback: a frame is never failed because one of its pipelines has no shader. These
     * are the counts that turn "the picture is missing an icon" into a number in the log. */
    uint64_t draws_executed;
    uint64_t draws_skipped;
};

struct mw_vulkan {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t queue_family;
    VkQueue queue;
    VkDevice device;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkFence fence;
    VkDeviceSize max_buffer_size;
    int has_dynamic_rendering;
    int has_pci_bus_info;
    uint32_t pci_domain;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
};

struct mw_server {
    struct mw_vulkan vk;
    struct mw_buffer **buffers;
    size_t buffer_count;
    size_t buffer_cap;
    struct mw_image **images;
    size_t image_count;
    size_t image_cap;
    struct mw_render_pipeline **pipelines;
    size_t pipeline_count;
    size_t pipeline_cap;
    struct mw_sampler_state **samplers;
    size_t sampler_count;
    size_t sampler_cap;
    /* WP-4: the libraries a v4 guest described (hash + entry functions). */
    struct mw_library **libraries;
    size_t library_count;
    size_t library_cap;
    /* WP-4: where --missing-report writes the pipelines without a shader, and the totals. */
    char *missing_path;
    uint64_t missing_draws_total;
    uint64_t missing_pipeline_total;
    struct mw_render_vk render;
    uint64_t render_frames_seen;
    uint64_t render_draws_logged;

    int listen_fd;
    int client_fd;
    int port;
    char bind_address[64];

    struct mw_frame_state frame;
    uint64_t frames_done;
    uint64_t frames_target; /* 0: run until stopped */
    int pending_status;

    unsigned char *payload;
    size_t payload_cap;

    /* --ram: QEMU's RAM file mapped so an upload-by-reference can be read in place (the port's
     * own trick in tools/glwire: the guest names physical memory, the host reads the same bytes).
     */
    unsigned char *ram;
    size_t ram_size;
    uint64_t ram_base;
    int ram_writable;

    /* WP-3: texture content and IOSurface binding queued by the guest, applied on the next
     * executed render frame. */
    struct mw_pending_replace *replaces;
    size_t replace_count;
    size_t replace_cap;
    unsigned char *surface_scratch;
    size_t surface_scratch_size;

    struct mw_shm shm;
    FILE *csv;
    char *out_dir;
    int png_every;
    uint32_t crc_stride;
    int byte_check;   /* compare the presented picture with the uploaded source buffer, once */
    int byte_checked; /* 1 = every row; N = every Nth row (the guest's -c N / env) */

    unsigned char *rgb_scratch;
    size_t rgb_scratch_size;
    unsigned char *bgra_scratch;
    size_t bgra_scratch_size;

    struct mw_timing *stats;
    size_t stats_count;
    size_t stats_cap;
    uint64_t *completions;
    size_t completion_count;
    size_t completion_cap;
    uint64_t first_frame_begin_ns;
    uint64_t last_completion_ns;
    uint64_t frames_ok;

    int done;
    int verbose;
    uint16_t peer_version; /* the version the guest's first message carried; acks echo it */
};

static volatile sig_atomic_t g_stop;
static int g_verbose;

static void mw_log(const char *fmt, ...)
{
    va_list ap;

    fputs("metalwire: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void mw_on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/*
 * --dump DIR: write the decoded objects out beside the log, so the execution half (and the
 * shader work) can be developed and replayed without a guest boot. Buffers are dumped whole after
 * every upload, pipeline/attachments as text. It is a debugging aid: no execution reads it.
 */
static char *g_dump_dir;

static void mw_dump_file(const char *name, const void *data, size_t len)
{
    char path[4096];
    FILE *file;

    if (g_dump_dir == NULL)
        return;
    snprintf(path, sizeof(path), "%s/%s", g_dump_dir, name);
    file = fopen(path, "wb");
    if (file == NULL) {
        mw_log("dump: cannot write %s: %s", path, strerror(errno));
        return;
    }
    if (len > 0 && fwrite(data, 1, len, file) != len)
        mw_log("dump: short write to %s: %s", path, strerror(errno));
    fclose(file);
}

static uint64_t mw_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mw_ns_to_us(uint64_t ns)
{
    return (ns + 500) / 1000;
}

/* CRC-32, the zlib polynomial (reflected 0xEDB88320, init ~0, final xor ~0). Slicing by 8 keeps
 * the 4.9 MB read-back check below the noise of a frame. */
static uint32_t mw_crc_tables[8][256];

static void mw_crc32_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;

        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        mw_crc_tables[0][n] = c;
    }
    for (uint32_t n = 0; n < 256; n++) {
        for (int k = 1; k < 8; k++) {
            uint32_t c = mw_crc_tables[k - 1][n];

            mw_crc_tables[k][n] = (c >> 8) ^ mw_crc_tables[0][c & 0xffu];
        }
    }
}

static uint32_t mw_crc32_raw(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = data;

    while (len >= 8) {
        uint32_t lo = crc ^ ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                             ((uint32_t)p[3] << 24));
        uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) |
                      ((uint32_t)p[7] << 24);

        crc = mw_crc_tables[7][lo & 0xffu] ^ mw_crc_tables[6][(lo >> 8) & 0xffu] ^
              mw_crc_tables[5][(lo >> 16) & 0xffu] ^ mw_crc_tables[4][(lo >> 24) & 0xffu] ^
              mw_crc_tables[3][hi & 0xffu] ^ mw_crc_tables[2][(hi >> 8) & 0xffu] ^
              mw_crc_tables[1][(hi >> 16) & 0xffu] ^ mw_crc_tables[0][(hi >> 24) & 0xffu];
        p += 8;
        len -= 8;
    }
    while (len-- > 0)
        crc = mw_crc_tables[0][(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    return crc;
}

/*
 * The guest's frame CRC samples every `stride`th row (metal_frame -c N, the plug-in's
 * METALWIRE_CRC_STRIDE); this end must count exactly the same rows, or every frame would compare
 * two different checksums. stride 1 is the full picture.
 */
static uint32_t mw_crc32_rows_zlib(const unsigned char *pixels, uint32_t bytes_per_row,
                                   uint32_t row_bytes, uint32_t rows, uint32_t stride)
{
    uint32_t crc = 0xFFFFFFFFu;

    if (stride == 0)
        stride = 1;
    for (uint32_t row = 0; row < rows; row += stride)
        crc = mw_crc32_raw(crc, pixels + (size_t)row * bytes_per_row, row_bytes);
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t mw_adler32(const unsigned char *data, size_t len)
{
    uint32_t a = 1;
    uint32_t b = 0;

    while (len > 0) {
        size_t n = len > 5552 ? 5552 : len;

        len -= n;
        while (n-- > 0) {
            a += *data++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

static void mw_store_be32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void mw_store_le16(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
}

/* The guest's pixel formats, converted for the two consumers that are not Vulkan: PPM/PNG files
 * (RGB) and glwire's shared file (BGRA, webview.py decodes BGRA).
 *
 * Three shapes reach these converters: four 8-bit channels (BGRA8/RGBA8), one 8-bit channel
 * (R8/A8, CoreAnimation's masks and luma ramp) and four IEEE-754 binary16 channels
 * (RGBA16Float, CA's colour ramp, 8 bytes a texel). A half float becomes unorm8 by clamping to
 * [0, 1] and rounding to nearest; NaN and negative values become 0. The diagnostic picture is
 * 8 bits a channel, so a 16-bit ramp is quantised here, never in the Vulkan read-back. */
static inline int mw_format_is_bgra(uint32_t format)
{
    return format == MW_FORMAT_BGRA8_UNORM;
}

static inline int mw_format_is_half_float(uint32_t format)
{
    return format == MW_FORMAT_RGBA16_FLOAT;
}

static inline int mw_format_is_one_byte(uint32_t format)
{
    return format == MW_FORMAT_R8_UNORM || format == MW_FORMAT_A8_UNORM;
}

/* binary16 -> binary32, bit-exact: normals, subnormals, infinities and NaN keep their value. */
static float mw_half_to_float(uint16_t half)
{
    uint32_t sign = (uint32_t)(half & 0x8000u) << 16;
    uint32_t exponent = (half >> 10) & 0x1fu;
    uint32_t mantissa = half & 0x03ffu;
    uint32_t bits;
    float value;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign; /* +/- zero */
        } else {
            exponent = 113; /* 2^-14 as a binary32 exponent: the subnormal's first normal step */
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x03ffu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13); /* infinity or NaN */
    } else {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static unsigned char mw_half_to_unorm8(uint16_t half)
{
    float value = mw_half_to_float(half);
    float scaled;

    if (!(value > 0.0f))
        return 0; /* zero, negative, NaN */
    if (value >= 1.0f)
        return 255;
    scaled = value * 255.0f + 0.5f;
    return (unsigned char)(scaled >= 255.0f ? 255.0f : scaled);
}

static uint16_t mw_load_le16(const unsigned char *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void mw_pixels_to_rgb(const unsigned char *pixels, uint32_t width, uint32_t height,
                             uint32_t format, unsigned char *rgb)
{
    size_t count = (size_t)width * height;

    if (mw_format_is_half_float(format)) {
        for (size_t i = 0; i < count; i++) {
            const unsigned char *p = pixels + i * 8;
            unsigned char *o = rgb + i * 3;

            o[0] = mw_half_to_unorm8(mw_load_le16(p + 0));
            o[1] = mw_half_to_unorm8(mw_load_le16(p + 2));
            o[2] = mw_half_to_unorm8(mw_load_le16(p + 4));
        }
        return;
    }
    if (mw_format_is_one_byte(format)) {
        /* A single channel has no colour of its own: a saved mask or ramp reads as grey. */
        for (size_t i = 0; i < count; i++)
            rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = pixels[i];
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const unsigned char *p = pixels + i * 4;
        unsigned char *o = rgb + i * 3;

        if (mw_format_is_bgra(format)) {
            o[0] = p[2];
            o[1] = p[1];
            o[2] = p[0];
        } else {
            o[0] = p[0];
            o[1] = p[1];
            o[2] = p[2];
        }
    }
}

/* `bytes` is the source size; the output is always BGRA8, so a 16-bit source yields half as many
 * bytes as it consumes. */
static void mw_pixels_to_bgra(const unsigned char *pixels, size_t bytes, uint32_t format,
                              unsigned char *bgra)
{
    if (mw_format_is_bgra(format)) {
        memcpy(bgra, pixels, bytes);
        return;
    }
    if (mw_format_is_half_float(format)) {
        for (size_t i = 0; i < bytes / 8; i++) {
            const unsigned char *p = pixels + i * 8;
            unsigned char *o = bgra + i * 4;

            o[0] = mw_half_to_unorm8(mw_load_le16(p + 4)); /* B */
            o[1] = mw_half_to_unorm8(mw_load_le16(p + 2)); /* G */
            o[2] = mw_half_to_unorm8(mw_load_le16(p + 0)); /* R */
            o[3] = mw_half_to_unorm8(mw_load_le16(p + 6)); /* A */
        }
        return;
    }
    if (mw_format_is_one_byte(format)) {
        for (size_t i = 0; i < bytes; i++) {
            bgra[i * 4 + 0] = pixels[i];
            bgra[i * 4 + 1] = pixels[i];
            bgra[i * 4 + 2] = pixels[i];
            bgra[i * 4 + 3] = 255;
        }
        return;
    }
    for (size_t i = 0; i < bytes; i += 4) {
        bgra[i + 0] = pixels[i + 2];
        bgra[i + 1] = pixels[i + 1];
        bgra[i + 2] = pixels[i + 0];
        bgra[i + 3] = pixels[i + 3];
    }
}

static unsigned char *mw_scratch_resize(unsigned char **buffer, size_t *capacity, size_t needed)
{
    unsigned char *grown;

    if (*capacity >= needed)
        return *buffer;
    grown = realloc(*buffer, needed);
    if (grown == NULL)
        return NULL;
    *buffer = grown;
    *capacity = needed;
    return grown;
}

static int mw_write_ppm(const char *path, const unsigned char *rgb, uint32_t width, uint32_t height)
{
    FILE *file = fopen(path, "wb");
    size_t bytes = (size_t)width * height * 3;

    if (file == NULL)
        return -1;
    if (fprintf(file, "P6\n%u %u\n255\n", width, height) < 0 ||
        fwrite(rgb, 1, bytes, file) != bytes) {
        fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int mw_png_chunk(FILE *file, const char type[4], const unsigned char *data, size_t len)
{
    unsigned char field[4];
    uint32_t crc = 0xFFFFFFFFu;

    mw_store_be32(field, (uint32_t)len);
    if (fwrite(field, 1, 4, file) != 4)
        return -1;
    crc = mw_crc32_raw(crc, type, 4);
    if (data != NULL && len > 0)
        crc = mw_crc32_raw(crc, data, len);
    if (fwrite(type, 1, 4, file) != 4)
        return -1;
    if (data != NULL && len > 0 && fwrite(data, 1, len, file) != len)
        return -1;
    mw_store_be32(field, crc ^ 0xFFFFFFFFu);
    if (fwrite(field, 1, 4, file) != 4)
        return -1;
    return 0;
}

/* PNG without a library: RGB, filter 0, and a legal zlib stream built from "stored" deflate
 * blocks. It is larger than a compressed PNG, which is the right trade for a diagnostic frame
 * dump -- no dependency, and the file opens everywhere. */
static int mw_write_png(const char *path, const unsigned char *rgb, uint32_t width, uint32_t height)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char ihdr[13];
    size_t stride = (size_t)width * 3 + 1;
    size_t raw_size = stride * height;
    size_t idat_cap = raw_size + (raw_size / 65535 + 2) * 5 + 8;
    unsigned char *raw = NULL;
    unsigned char *idat = NULL;
    FILE *file = NULL;
    size_t pos;
    size_t n;
    int result = -1;

    raw = malloc(raw_size);
    idat = malloc(idat_cap);
    if (raw == NULL || idat == NULL)
        goto out;
    for (uint32_t y = 0; y < height; y++) {
        unsigned char *row = raw + (size_t)y * stride;

        row[0] = 0;
        memcpy(row + 1, rgb + (size_t)y * width * 3, (size_t)width * 3);
    }

    n = 0;
    idat[n++] = 0x78; /* CMF: deflate, 32 KiB window */
    idat[n++] = 0x01; /* FLG: fastest, check bits valid */
    pos = 0;
    do {
        size_t chunk = raw_size - pos;
        uint16_t nlen;

        if (chunk > 65535)
            chunk = 65535;
        idat[n++] = (pos + chunk >= raw_size) ? 1 : 0;
        mw_store_le16(idat + n, (uint32_t)chunk);
        n += 2;
        nlen = (uint16_t)~chunk;
        mw_store_le16(idat + n, nlen);
        n += 2;
        memcpy(idat + n, raw + pos, chunk);
        n += chunk;
        pos += chunk;
    } while (pos < raw_size);
    mw_store_be32(idat + n, mw_adler32(raw, raw_size));
    n += 4;

    mw_store_be32(ihdr, width);
    mw_store_be32(ihdr + 4, height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering */
    ihdr[12] = 0; /* no interlace */

    file = fopen(path, "wb");
    if (file == NULL)
        goto out;
    if (fwrite(signature, 1, 8, file) != 8)
        goto out;
    if (mw_png_chunk(file, "IHDR", ihdr, sizeof(ihdr)) < 0)
        goto out;
    if (mw_png_chunk(file, "IDAT", idat, n) < 0)
        goto out;
    if (mw_png_chunk(file, "IEND", NULL, 0) < 0)
        goto out;
    result = 0;
out:
    if (file != NULL && fclose(file) != 0)
        result = -1;
    free(raw);
    free(idat);
    return result;
}

static void mw_shm_close(struct mw_shm *shm)
{
    if (shm->header != NULL)
        munmap(shm->header, shm->map_size);
    if (shm->fd >= 0)
        close(shm->fd);
    shm->header = NULL;
    shm->fd = -1;
    shm->map_size = 0;
    shm->width = shm->height = shm->bytes = 0;
}

static int mw_shm_prepare(struct mw_shm *shm, uint32_t width, uint32_t height)
{
    size_t size;
    int fd;
    void *map;

    if (shm->path == NULL || shm->path[0] == '\0' || shm->failed)
        return -1;
    if (shm->header != NULL && shm->width == width && shm->height == height)
        return 0;
    mw_shm_close(shm);
    size = sizeof(struct mw_shm_header) + (size_t)width * height * MW_PIXEL_BYTES;
    fd = open(shm->path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        mw_log("shm: cannot open %s: %s", shm->path, strerror(errno));
        shm->failed = 1;
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        mw_log("shm: cannot size %s to %zu bytes: %s", shm->path, size, strerror(errno));
        close(fd);
        shm->failed = 1;
        return -1;
    }
    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        mw_log("shm: cannot map %s: %s", shm->path, strerror(errno));
        close(fd);
        shm->failed = 1;
        return -1;
    }
    shm->fd = fd;
    shm->header = map;
    shm->map_size = size;
    shm->width = width;
    shm->height = height;
    shm->bytes = width * height * MW_PIXEL_BYTES;
    memset(shm->header, 0, sizeof(*shm->header));
    shm->header->magic = MW_SHM_MAGIC;
    shm->header->width = width;
    shm->header->height = height;
    shm->header->bytes = shm->bytes;
    shm->header->seq = 1;
    return 0;
}

static void mw_shm_publish(struct mw_shm *shm, const unsigned char *pixels_bgra)
{
    if (shm->header == NULL)
        return;
    memcpy((unsigned char *)shm->header + sizeof(*shm->header), pixels_bgra, shm->bytes);
    shm->header->published_ns = mw_now_ns();
    __sync_synchronize();
    shm->header->seq++;
}

/* ---- Vulkan helpers ------------------------------------------------------------------------- */

static const char *mw_device_type_name(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "CPU";
    default:
        return "other";
    }
}

static int mw_find_memory_type(const struct mw_vulkan *vk, uint32_t bits,
                               VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                               uint32_t *out_index)
{
    uint32_t fallback = UINT32_MAX;

    for (uint32_t i = 0; i < vk->memory.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = vk->memory.memoryTypes[i].propertyFlags;

        if ((bits & (1u << i)) == 0)
            continue;
        if ((flags & required) != required)
            continue;
        if ((flags & preferred) == preferred) {
            *out_index = i;
            return 0;
        }
        if (fallback == UINT32_MAX)
            fallback = i;
    }
    if (fallback != UINT32_MAX) {
        *out_index = fallback;
        return 0;
    }
    return -1;
}

static int mw_allocate_memory(struct mw_vulkan *vk, const VkMemoryRequirements *req,
                              VkMemoryPropertyFlags want, VkMemoryPropertyFlags preferred,
                              VkDeviceMemory *out)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    uint32_t index;
    VkResult result;

    if (mw_find_memory_type(vk, req->memoryTypeBits, want, preferred, &index) < 0)
        return -1;
    info.allocationSize = req->size;
    info.memoryTypeIndex = index;
    if (g_verbose)
        mw_log("vulkan: memory type %u (flags 0x%02x), %" PRIu64 " bytes", index,
               (unsigned)vk->memory.memoryTypes[index].propertyFlags, (uint64_t)req->size);
    result = vkAllocateMemory(vk->device, &info, NULL, out);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkAllocateMemory of %" PRIu64 " bytes failed: %d", (uint64_t)req->size,
               (int)result);
        return -1;
    }
    return 0;
}

static int mw_create_host_buffer(struct mw_vulkan *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkBuffer *out_buffer, VkDeviceMemory *out_memory,
                                 void **out_mapped)
{
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements req;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result;

    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(vk->device, &info, NULL, &buffer);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkCreateBuffer(%" PRIu64 ") failed: %d", (uint64_t)size, (int)result);
        return -1;
    }
    vkGetBufferMemoryRequirements(vk->device, buffer, &req);
    /* Host-cached, not device-local: a resizable-BAR/device-local host-visible type is an
     * uncached window into VRAM, and reading 4.9 MB from it costs over a second on the V100. */
    if (mw_allocate_memory(
            vk, &req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &memory) < 0) {
        vkDestroyBuffer(vk->device, buffer, NULL);
        return -1;
    }
    result = vkBindBufferMemory(vk->device, buffer, memory, 0);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkBindBufferMemory failed: %d", (int)result);
        vkDestroyBuffer(vk->device, buffer, NULL);
        vkFreeMemory(vk->device, memory, NULL);
        return -1;
    }
    if (out_mapped != NULL) {
        result = vkMapMemory(vk->device, memory, 0, VK_WHOLE_SIZE, 0, out_mapped);
        if (result != VK_SUCCESS) {
            mw_log("vulkan: vkMapMemory failed: %d", (int)result);
            vkDestroyBuffer(vk->device, buffer, NULL);
            vkFreeMemory(vk->device, memory, NULL);
            return -1;
        }
    }
    *out_buffer = buffer;
    *out_memory = memory;
    return 0;
}

static uint32_t mw_format_bytes(uint32_t format)
{
    if (mw_format_is_one_byte(format))
        return 1u;
    if (mw_format_is_half_float(format))
        return 8u;
    return MW_PIXEL_BYTES;
}

/* The formats the host can put in a Vulkan image. Anything else keeps the WP-4 fallback: the
 * texture stays absent, its draws are skipped, and the frame is still acknowledged. */
static int mw_format_is_known(uint32_t format)
{
    return format == MW_FORMAT_BGRA8_UNORM || format == MW_FORMAT_RGBA8_UNORM ||
           mw_format_is_one_byte(format) || mw_format_is_half_float(format);
}

static const char *mw_format_name(uint32_t format)
{
    switch (format) {
    case MW_FORMAT_BGRA8_UNORM:
        return "BGRA8";
    case MW_FORMAT_RGBA8_UNORM:
        return "RGBA8";
    case MW_FORMAT_R8_UNORM:
        return "R8";
    case MW_FORMAT_A8_UNORM:
        return "A8";
    case MW_FORMAT_RGBA16_FLOAT:
        return "RGBA16Float";
    default:
        return "unknown";
    }
}

static VkFormat mw_vk_format(uint32_t format)
{
    if (format == MW_FORMAT_RGBA8_UNORM)
        return VK_FORMAT_R8G8B8A8_UNORM;
    if (mw_format_is_one_byte(format))
        return VK_FORMAT_R8_UNORM;
    if (mw_format_is_half_float(format))
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    return VK_FORMAT_B8G8R8A8_UNORM;
}

static int mw_parse_pci_address(const char *text, uint32_t *domain, uint32_t *bus,
                                uint32_t *device, uint32_t *function)
{
    unsigned d = 0, b = 0, dev = 0, fn = 0;

    if (sscanf(text, "%x:%x:%x.%x", &d, &b, &dev, &fn) == 4) {
        *domain = d;
        *bus = b;
        *device = dev;
        *function = fn;
        return 0;
    }
    if (sscanf(text, "%x:%x.%x", &b, &dev, &fn) == 3) {
        *domain = 0;
        *bus = b;
        *device = dev;
        *function = fn;
        return 0;
    }
    return -1;
}

static int mw_vulkan_init(struct mw_vulkan *vk, int gpu_index, const char *pci_address,
                          int verbose)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDevice *devices = NULL;
    VkQueueFamilyProperties *families = NULL;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo buffer_info = {.sType =
                                                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceMaintenance3Properties maintenance = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkExtensionProperties *extensions = NULL;
    uint32_t count = 0;
    uint32_t family_count = 0;
    uint32_t chosen = UINT32_MAX;
    float priority = 1.0f;
    VkResult result;
    int ok = -1;

    memset(vk, 0, sizeof(*vk));
    vk->queue_family = UINT32_MAX;

    app.pApplicationName = "metalwire_host";
    app.applicationVersion = 1;
    app.pEngineName = "metalwire";
    app.engineVersion = MW_VERSION;
    app.apiVersion = VK_API_VERSION_1_3;
    instance_info.pApplicationInfo = &app;
    result = vkCreateInstance(&instance_info, NULL, &vk->instance);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkCreateInstance failed: %d", (int)result);
        return -1;
    }

    result = vkEnumeratePhysicalDevices(vk->instance, &count, NULL);
    if (result != VK_SUCCESS || count == 0) {
        mw_log("vulkan: no physical devices (result %d)", (int)result);
        goto out;
    }
    devices = calloc(count, sizeof(*devices));
    if (devices == NULL)
        goto out;
    result = vkEnumeratePhysicalDevices(vk->instance, &count, devices);
    if (result != VK_SUCCESS || count == 0) {
        mw_log("vulkan: vkEnumeratePhysicalDevices failed: %d", (int)result);
        goto out;
    }
    /* The stage-C rule: pick the card by PCI id, never by index. The other V100 can be bound to
     * another experiment's VM (vfio-pci), which renumbers every index below it. */
    if (pci_address != NULL) {
        uint32_t want_domain = 0, want_bus = 0, want_device = 0, want_function = 0;
        int found = 0;

        if (mw_parse_pci_address(pci_address, &want_domain, &want_bus, &want_device,
                                 &want_function) != 0) {
            mw_log("vulkan: --pci \"%s\" is not DOMAIN:BUS:DEVICE.FUNCTION or "
                   "BUS:DEVICE.FUNCTION",
                   pci_address);
            goto out;
        }
        for (uint32_t i = 0; i < count; i++) {
            VkPhysicalDevicePCIBusInfoPropertiesEXT info = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 properties = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

            properties.pNext = &info;
            vkGetPhysicalDeviceProperties2(devices[i], &properties);
            if (info.pciDomain == want_domain && info.pciBus == want_bus &&
                info.pciDevice == want_device && info.pciFunction == want_function) {
                gpu_index = (int)i;
                found = 1;
                break;
            }
        }
        if (!found) {
            mw_log("vulkan: no physical device at PCI %s (it may be bound to vfio-pci for another "
                   "experiment)",
                   pci_address);
            goto out;
        }
    } else if (gpu_index < 0 || (uint32_t)gpu_index >= count) {
        mw_log("vulkan: physical device %d requested, only %u present", gpu_index, count);
        goto out;
    }
    vk->physical = devices[gpu_index];
    vkGetPhysicalDeviceProperties(vk->physical, &vk->props);
    vkGetPhysicalDeviceMemoryProperties(vk->physical, &vk->memory);
    props2.pNext = &maintenance;
    vkGetPhysicalDeviceProperties2(vk->physical, &props2);
    props2.pNext = NULL;
    /* One buffer lives in one allocation, so the maintenance3 allocation limit bounds it. */
    vk->max_buffer_size = maintenance.maxMemoryAllocationSize != 0
                              ? maintenance.maxMemoryAllocationSize
                              : (VkDeviceSize)UINT64_MAX;
    mw_log("vulkan: %u physical device(s), using index %d", count, gpu_index);
    mw_log("device %d: \"%s\" (%s), API %u.%u.%u, vendor 0x%04x device 0x%04x driver 0x%08x",
           gpu_index, vk->props.deviceName, mw_device_type_name(vk->props.deviceType),
           VK_VERSION_MAJOR(vk->props.apiVersion), VK_VERSION_MINOR(vk->props.apiVersion),
           VK_VERSION_PATCH(vk->props.apiVersion), vk->props.vendorID, vk->props.deviceID,
           vk->props.driverVersion);

    result = vkEnumerateDeviceExtensionProperties(vk->physical, NULL, &count, NULL);
    if (result == VK_SUCCESS && count > 0) {
        extensions = calloc(count, sizeof(*extensions));
        if (extensions != NULL && vkEnumerateDeviceExtensionProperties(vk->physical, NULL, &count,
                                                                       extensions) == VK_SUCCESS) {
            for (uint32_t i = 0; i < count; i++) {
                if (strcmp(extensions[i].extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) == 0)
                    vk->has_pci_bus_info = 1;
            }
        }
        free(extensions);
        extensions = NULL;
    }
    if (vk->has_pci_bus_info) {
        props2.pNext = &pci;
        vkGetPhysicalDeviceProperties2(vk->physical, &props2);
        vk->pci_domain = pci.pciDomain;
        vk->pci_bus = pci.pciBus;
        vk->pci_device = pci.pciDevice;
        vk->pci_function = pci.pciFunction;
        mw_log("device %d: PCI %04x:%02x:%02x.%x (VK_EXT_pci_bus_info)", gpu_index, vk->pci_domain,
               vk->pci_bus, vk->pci_device, vk->pci_function);
    } else {
        mw_log("device %d: VK_EXT_pci_bus_info not present, no PCI address", gpu_index);
    }

    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &family_count, NULL);
    families = calloc(family_count == 0 ? 1 : family_count, sizeof(*families));
    if (families == NULL)
        goto out;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical, &family_count, families);
    for (uint32_t i = 0; i < family_count; i++) {
        VkQueueFlags flags = families[i].queueFlags;

        if ((flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT)) {
            chosen = i;
            break;
        }
    }
    if (chosen == UINT32_MAX) {
        for (uint32_t i = 0; i < family_count; i++) {
            if (families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen == UINT32_MAX) {
        mw_log("vulkan: no queue family with transfer support");
        goto out;
    }
    vk->queue_family = chosen;
    mw_log("queue family %u: flags 0x%03x, %u queue(s); compute+transfer%s", chosen,
           (unsigned)families[chosen].queueFlags, families[chosen].queueCount,
           (families[chosen].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "" : " (no compute)");
    if (verbose)
        mw_log("device limits: maxBufferSize %" PRIu64 ", maxImageDimension2D %u",
               (uint64_t)vk->max_buffer_size, vk->props.limits.maxImageDimension2D);

    {
        VkPhysicalDeviceVulkan13Features features13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features13};

        vkGetPhysicalDeviceFeatures2(vk->physical, &features2);
        vk->has_dynamic_rendering =
            features13.dynamicRendering == VK_TRUE &&
            vk->props.apiVersion >= VK_API_VERSION_1_3;
        if (verbose)
            mw_log("features: dynamicRendering %s (device API %u.%u)",
                   vk->has_dynamic_rendering ? "yes" : "no",
                   VK_VERSION_MAJOR(vk->props.apiVersion), VK_VERSION_MINOR(vk->props.apiVersion));
    }
    if (vk->has_dynamic_rendering) {
        static VkPhysicalDeviceVulkan13Features enable13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

        enable13.dynamicRendering = VK_TRUE;
        device_info.pNext = &enable13;
    }

    priority = 1.0f;
    queue_info.queueFamilyIndex = chosen;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    result = vkCreateDevice(vk->physical, &device_info, NULL, &vk->device);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkCreateDevice failed: %d", (int)result);
        goto out;
    }
    vkGetDeviceQueue(vk->device, chosen, 0, &vk->queue);

    pool_info.queueFamilyIndex = chosen;
    result = vkCreateCommandPool(vk->device, &pool_info, NULL, &vk->command_pool);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkCreateCommandPool failed: %d", (int)result);
        goto out;
    }
    buffer_info.commandPool = vk->command_pool;
    buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(vk->device, &buffer_info, &vk->command_buffer);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkAllocateCommandBuffers failed: %d", (int)result);
        goto out;
    }
    result = vkCreateFence(vk->device, &fence_info, NULL, &vk->fence);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkCreateFence failed: %d", (int)result);
        goto out;
    }
    ok = 0;
out:
    free(devices);
    free(families);
    return ok;
}

static void mw_vulkan_fini(struct mw_vulkan *vk)
{
    if (vk->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk->device);
        if (vk->fence != VK_NULL_HANDLE)
            vkDestroyFence(vk->device, vk->fence, NULL);
        if (vk->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
        vkDestroyDevice(vk->device, NULL);
    }
    if (vk->instance != VK_NULL_HANDLE)
        vkDestroyInstance(vk->instance, NULL);
    memset(vk, 0, sizeof(*vk));
}

/* ---- object tables -------------------------------------------------------------------------- */

static struct mw_buffer *mw_buffer_find(struct mw_server *s, uint32_t id)
{
    for (size_t i = 0; i < s->buffer_count; i++) {
        if (s->buffers[i]->id == id)
            return s->buffers[i];
    }
    return NULL;
}

static struct mw_image *mw_image_find(struct mw_server *s, uint32_t id)
{
    for (size_t i = 0; i < s->image_count; i++) {
        if (s->images[i]->id == id)
            return s->images[i];
    }
    return NULL;
}

static int mw_buffer_add(struct mw_server *s, struct mw_buffer *buffer)
{
    if (s->buffer_count == s->buffer_cap) {
        size_t cap = s->buffer_cap == 0 ? 8 : s->buffer_cap * 2;
        struct mw_buffer **grown = realloc(s->buffers, cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        s->buffers = grown;
        s->buffer_cap = cap;
    }
    s->buffers[s->buffer_count++] = buffer;
    return 0;
}

static int mw_image_add(struct mw_server *s, struct mw_image *image)
{
    if (s->image_count == s->image_cap) {
        size_t cap = s->image_cap == 0 ? 8 : s->image_cap * 2;
        struct mw_image **grown = realloc(s->images, cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        s->images = grown;
        s->image_cap = cap;
    }
    s->images[s->image_count++] = image;
    return 0;
}

static struct mw_sampler_state *mw_sampler_find(struct mw_server *s, uint32_t id)
{
    for (size_t i = 0; i < s->sampler_count; i++) {
        if (s->samplers[i]->id == id)
            return s->samplers[i];
    }
    return NULL;
}

static int mw_sampler_add(struct mw_server *s, struct mw_sampler_state *sampler)
{
    if (s->sampler_count == s->sampler_cap) {
        size_t cap = s->sampler_cap == 0 ? 8 : s->sampler_cap * 2;
        struct mw_sampler_state **grown = realloc(s->samplers, cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        s->samplers = grown;
        s->sampler_cap = cap;
    }
    s->samplers[s->sampler_count++] = sampler;
    return 0;
}

static void mw_sampler_state_destroy(struct mw_server *s, struct mw_sampler_state *sampler)
{
    for (size_t i = 0; i < s->sampler_count; i++) {
        if (s->samplers[i] == sampler) {
            memmove(&s->samplers[i], &s->samplers[i + 1],
                    (s->sampler_count - i - 1) * sizeof(*s->samplers));
            s->sampler_count--;
            break;
        }
    }
    if (sampler->sampler != VK_NULL_HANDLE)
        vkDestroySampler(s->vk.device, sampler->sampler, NULL);
    free(sampler);
}

static void mw_buffer_destroy(struct mw_server *s, struct mw_buffer *buffer)
{
    for (size_t i = 0; i < s->buffer_count; i++) {
        if (s->buffers[i] == buffer) {
            memmove(&s->buffers[i], &s->buffers[i + 1],
                    (s->buffer_count - i - 1) * sizeof(*s->buffers));
            s->buffer_count--;
            break;
        }
    }
    if (buffer->mapped != NULL)
        vkUnmapMemory(s->vk.device, buffer->memory);
    if (buffer->buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(s->vk.device, buffer->buffer, NULL);
    if (buffer->memory != VK_NULL_HANDLE)
        vkFreeMemory(s->vk.device, buffer->memory, NULL);
    free(buffer);
}

static void mw_image_destroy(struct mw_server *s, struct mw_image *image)
{
    for (size_t i = 0; i < s->image_count; i++) {
        if (s->images[i] == image) {
            memmove(&s->images[i], &s->images[i + 1],
                    (s->image_count - i - 1) * sizeof(*s->images));
            s->image_count--;
            break;
        }
    }
    if (image->readback_mapped != NULL)
        vkUnmapMemory(s->vk.device, image->readback_memory);
    if (image->readback != VK_NULL_HANDLE)
        vkDestroyBuffer(s->vk.device, image->readback, NULL);
    if (image->readback_memory != VK_NULL_HANDLE)
        vkFreeMemory(s->vk.device, image->readback_memory, NULL);
    if (image->view != VK_NULL_HANDLE)
        vkDestroyImageView(s->vk.device, image->view, NULL);
    if (image->image != VK_NULL_HANDLE)
        vkDestroyImage(s->vk.device, image->image, NULL);
    if (image->memory != VK_NULL_HANDLE)
        vkFreeMemory(s->vk.device, image->memory, NULL);
    free(image->surface_segments);
    free(image);
}

/* ---- protocol: frame bookkeeping ------------------------------------------------------------ */

static void mw_frame_reset(struct mw_server *s)
{
    memset(&s->frame, 0, sizeof(s->frame));
}

static void mw_frame_fail(struct mw_server *s, int status, const char *fmt, ...)
{
    char message[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    mw_log("frame error: %s (status %d)", message, status);
    if (s->frame.active) {
        if (s->frame.error == 0)
            s->frame.error = status;
    } else if (s->pending_status == 0) {
        s->pending_status = status;
    }
}

/* ---- protocol: objects ---------------------------------------------------------------------- */

static int mw_handle_create_buffer(struct mw_server *s, const struct mw_create_buffer *request)
{
    struct mw_buffer *buffer;

    if (mw_buffer_find(s, request->id) != NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "buffer id %u already exists", request->id);
        return -1;
    }
    if (request->length == 0 || request->length > s->vk.max_buffer_size) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "buffer %u length %" PRIu64 " out of range",
                      request->id, request->length);
        return -1;
    }
    buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for buffer %u", request->id);
        return -1;
    }
    buffer->id = request->id;
    buffer->options = request->options;
    buffer->length = request->length;
    if (mw_create_host_buffer(&s->vk, request->length,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              &buffer->buffer, &buffer->memory, &buffer->mapped) < 0) {
        free(buffer);
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "cannot allocate buffer %u (%" PRIu64 " bytes)",
                      request->id, request->length);
        return -1;
    }
    if (mw_buffer_add(s, buffer) < 0) {
        mw_buffer_destroy(s, buffer);
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "object table growth failed");
        return -1;
    }
    if (s->verbose)
        mw_log("buffer %u: %" PRIu64 " bytes, options 0x%x", buffer->id, buffer->length,
               buffer->options);
    return 0;
}

static int mw_handle_upload_buffer(struct mw_server *s, const struct mw_upload_buffer *request,
                                   const unsigned char *data, uint32_t len)
{
    struct mw_buffer *buffer = mw_buffer_find(s, request->id);

    if (buffer == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "upload to unknown buffer %u", request->id);
        return -1;
    }
    if (len != request->length || (uint64_t)request->offset + request->length > buffer->length) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "upload %u: offset %u length %u outside %" PRIu64 " bytes", request->id,
                      request->offset, request->length, buffer->length);
        return -1;
    }
    if (request->length > 0)
        memcpy((unsigned char *)buffer->mapped + request->offset, data, request->length);
    if (g_dump_dir != NULL) {
        char name[64];

        snprintf(name, sizeof(name), "buffer-%u.bin", buffer->id);
        mw_dump_file(name, buffer->mapped, (size_t)buffer->length);
    }
    if (s->verbose)
        mw_log("buffer %u: upload %u bytes at %u", request->id, request->length, request->offset);
    return 0;
}

/*
 * MW_UPLOAD_BUFFER_PHYS: the pixels did not come down the socket; they are in guest physical
 * memory and this process has QEMU's RAM file mapped. Every segment is checked against the file
 * before a byte is read, and the total must be exactly the upload the guest asked for.
 */
static int mw_handle_upload_buffer_phys(struct mw_server *s,
                                        const struct mw_upload_buffer_phys *request,
                                        const unsigned char *segments_data, uint32_t len)
{
    const struct mw_segment *segments = (const struct mw_segment *)segments_data;
    struct mw_buffer *buffer;
    uint64_t total = 0;
    uint32_t at = 0;

    buffer = mw_buffer_find(s, request->id);
    if (buffer == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "by-reference upload to unknown buffer %u",
                      request->id);
        return -1;
    }
    if (s->ram == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "by-reference upload of %u bytes, but --ram was not given", request->length);
        return -1;
    }
    if (request->segment_count > 8192 ||
        len != sizeof(*request) + (uint64_t)request->segment_count * sizeof(struct mw_segment)) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "by-reference upload: %u segments in %u bytes",
                      request->segment_count, len);
        return -1;
    }
    for (uint32_t i = 0; i < request->segment_count; i++) {
        if ((uint64_t)segments[i].pa < s->ram_base ||
            (uint64_t)segments[i].pa - s->ram_base + segments[i].len > s->ram_size) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                          "by-reference upload: segment %u at %#" PRIx64
                          " (%u bytes) is outside the "
                          "%zu-byte RAM file",
                          i, segments[i].pa, segments[i].len, s->ram_size);
            return -1;
        }
        total += segments[i].len;
    }
    if (total != request->length || (uint64_t)request->offset + request->length > buffer->length) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "by-reference upload %u: %" PRIu64 " bytes told, %u claimed, offset %u",
                      request->id, total, request->length, request->offset);
        return -1;
    }
    for (uint32_t i = 0; i < request->segment_count; i++) {
        memcpy((unsigned char *)buffer->mapped + request->offset + at,
               s->ram + (size_t)(segments[i].pa - s->ram_base), segments[i].len);
        at += segments[i].len;
    }
    if (g_dump_dir != NULL) {
        char name[64];

        snprintf(name, sizeof(name), "buffer-%u.bin", buffer->id);
        mw_dump_file(name, buffer->mapped, (size_t)buffer->length);
    }
    if (s->verbose)
        mw_log("buffer %u: upload %u bytes by reference (%u segment(s))", request->id,
               request->length, request->segment_count);
    return 0;
}

static int mw_handle_create_texture(struct mw_server *s, const struct mw_create_texture *request)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements req;
    VkFormatProperties format_props;
    VkFormat format;
    struct mw_image *image;
    VkResult result;

    if (mw_image_find(s, request->id) != NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "texture id %u already exists", request->id);
        return -1;
    }
    if (request->width == 0 || request->height == 0 ||
        request->width > s->vk.props.limits.maxImageDimension2D ||
        request->height > s->vk.props.limits.maxImageDimension2D) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "texture %u: %ux%u out of range", request->id,
                      request->width, request->height);
        return -1;
    }
    if (!mw_format_is_known(request->pixel_format)) {
        /* WP-4 fallback: a texture format the host does not know is not a frame error. The
         * texture stays absent; every draw that names it is skipped and the rest of the frame
         * still becomes a picture. */
        mw_log("texture %u: pixel format %u is not implemented; its draws are skipped",
               request->id, request->pixel_format);
        return 0;
    }
    format = mw_vk_format(request->pixel_format);
    vkGetPhysicalDeviceFormatProperties(s->vk.physical, format, &format_props);
    if ((format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0 ||
        (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "texture %u: device cannot transfer %s",
                      request->id, mw_format_name(request->pixel_format));
        return -1;
    }
    image = calloc(1, sizeof(*image));
    if (image == NULL) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for texture %u", request->id);
        return -1;
    }
    image->id = request->id;
    image->width = request->width;
    image->height = request->height;
    image->format = request->pixel_format;
    {
        uint64_t bytes = (uint64_t)request->width * request->height *
                         mw_format_bytes(request->pixel_format);

        if (bytes > UINT32_MAX || bytes > s->vk.max_buffer_size) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                          "texture %u: read-back of %" PRIu64 " bytes does not fit the device",
                          request->id, bytes);
            free(image);
            return -1;
        }
        image->bytes = (uint32_t)bytes;
    }
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = request->width;
    info.extent.height = request->height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    /* Metal's MTLTextureUsage bits: 1 shaderRead, 2 shaderWrite, 4 renderTarget. Stage C's render
     * target needs the color-attachment bit; a texture sampled by a later pass needs sampled. */
    if ((request->usage & 1u) != 0)
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((request->usage & 2u) != 0)
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ((request->usage & 4u) != 0)
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(s->vk.device, &info, NULL, &image->image);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkCreateImage(%ux%u) failed: %d", request->width, request->height,
               (int)result);
        free(image);
        mw_frame_fail(s, MW_STATUS_VULKAN, "vkCreateImage for texture %u failed", request->id);
        return -1;
    }
    vkGetImageMemoryRequirements(s->vk.device, image->image, &req);
    if (mw_allocate_memory(&s->vk, &req, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image->memory) <
        0) {
        vkDestroyImage(s->vk.device, image->image, NULL);
        free(image);
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "image memory for texture %u failed", request->id);
        return -1;
    }
    result = vkBindImageMemory(s->vk.device, image->image, image->memory, 0);
    if (result != VK_SUCCESS) {
        mw_log("vulkan: vkBindImageMemory failed: %d", (int)result);
        vkDestroyImage(s->vk.device, image->image, NULL);
        vkFreeMemory(s->vk.device, image->memory, NULL);
        free(image);
        mw_frame_fail(s, MW_STATUS_VULKAN, "vkBindImageMemory for texture %u failed", request->id);
        return -1;
    }
    {
        VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

        view_info.image = image->image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        /*
         * WP-4 D4: Metal's A8Unorm is CA's glyph/label mask (see MW_FORMAT_A8_UNORM). Its single
         * byte is coverage that Metal's A8 view exposes as alpha with an undefined (zero) colour;
         * the host creates the image as R8, so a plain view would hand the fragment stage
         * (coverage, 0, 0) and a label drawn as `texture * colour` came out red (boot wp4-v6:
         * every icon label's glyph pixels are (R, 0, 0)). Swizzle the view to (1, 1, 1, coverage)
         * -- one opaque white in RGB and the mask byte in alpha -- so the fixed fragment function
         * modulates the vertex colour by the coverage and composites premultiplied text the way
         * CA's mask base functions (Ired/Ialp) do. R8 textures (the luma ramp, format 10) keep a
         * plain view and still read the ramp from .r.
         */
        if (image->format == MW_FORMAT_A8_UNORM) {
            view_info.components.r = VK_COMPONENT_SWIZZLE_ONE;
            view_info.components.g = VK_COMPONENT_SWIZZLE_ONE;
            view_info.components.b = VK_COMPONENT_SWIZZLE_ONE;
            view_info.components.a = VK_COMPONENT_SWIZZLE_R;
        }
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(s->vk.device, &view_info, NULL, &image->view);
        if (result != VK_SUCCESS) {
            mw_log("vulkan: vkCreateImageView(%ux%u) failed: %d", request->width, request->height,
                   (int)result);
            vkDestroyImage(s->vk.device, image->image, NULL);
            vkFreeMemory(s->vk.device, image->memory, NULL);
            free(image);
            mw_frame_fail(s, MW_STATUS_VULKAN, "vkCreateImageView for texture %u failed",
                          request->id);
            return -1;
        }
    }
    if (mw_create_host_buffer(&s->vk, image->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              &image->readback, &image->readback_memory,
                              &image->readback_mapped) < 0) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "readback buffer for texture %u failed", request->id);
        mw_image_destroy(s, image);
        return -1;
    }
    if (mw_image_add(s, image) < 0) {
        mw_image_destroy(s, image);
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "object table growth failed");
        return -1;
    }
    if (s->verbose)
        mw_log("texture %u: %ux%u format %u, %u bytes", image->id, image->width, image->height,
               image->format, image->bytes);
    if (g_dump_dir != NULL) {
        char name[64];
        char text[512];

        snprintf(text, sizeof(text), "texture %u\nwidth %u\nheight %u\nformat %u\nusage 0x%x\n",
                 image->id, image->width, image->height, image->format, request->usage);
        snprintf(name, sizeof(name), "texture-%u.txt", image->id);
        mw_dump_file(name, text, strlen(text));
    }
    return 0;
}

/* WP-3: MW_TEXTURE_REPLACE_REGION. The bytes arrive inline; they are queued and uploaded at the
 * start of the next executed render frame. CoreAnimation uploads a layer image once and samples it
 * later in the same frame, so a queue-at-receive/apply-at-frame-start order is the recorded order
 * for every sampling draw. */
static int mw_handle_texture_replace(struct mw_server *s,
                                     const struct mw_texture_replace_region *request,
                                     const unsigned char *bytes, uint32_t body_length)
{
    struct mw_image *image;
    struct mw_pending_replace *replace;
    uint32_t bpp;
    uint32_t row_bytes;

    image = mw_image_find(s, request->texture_id);
    if (image == NULL) {
        mw_log("replaceRegion: texture %u is not on the host (unknown format?); content dropped",
               request->texture_id);
        return 0;
    }
    if (request->length != body_length) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "replaceRegion length %u, %u payload byte(s)",
                      request->length, body_length);
        return -1;
    }
    if (request->mipmap_level != 0 || request->slice != 0 || request->depth > 1) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "replaceRegion level %u slice %u depth %u",
                      request->mipmap_level, request->slice, request->depth);
        return -1;
    }
    bpp = mw_format_bytes(image->format);
    row_bytes = request->bytes_per_row;
    if (request->width == 0 || request->height == 0 || row_bytes == 0 || row_bytes % bpp != 0 ||
        row_bytes < request->width * bpp ||
        (uint64_t)request->origin_x + request->width > image->width ||
        (uint64_t)request->origin_y + request->height > image->height) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "replaceRegion texture %u: %ux%u at %u,%u row %u does not fit %ux%u",
                      request->texture_id, request->width, request->height, request->origin_x,
                      request->origin_y, row_bytes, image->width, image->height);
        return -1;
    }
    replace = realloc(s->replaces, (s->replace_count + 1) * sizeof(*replace));
    if (replace == NULL) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for a texture replacement");
        return -1;
    }
    s->replaces = replace;
    replace = &s->replaces[s->replace_count];
    memset(replace, 0, sizeof(*replace));
    replace->texture_id = request->texture_id;
    replace->mipmap_level = request->mipmap_level;
    replace->slice = request->slice;
    replace->origin_x = request->origin_x;
    replace->origin_y = request->origin_y;
    replace->origin_z = request->origin_z;
    replace->width = request->width;
    replace->height = request->height;
    replace->depth = request->depth != 0 ? request->depth : 1;
    replace->bytes_per_row = row_bytes;
    replace->bytes_per_image = request->bytes_per_image;
    replace->length = request->length;
    replace->bytes = malloc(request->length != 0 ? request->length : 1);
    if (replace->bytes == NULL) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for %u texture bytes",
                      request->length);
        return -1;
    }
    memcpy(replace->bytes, bytes, request->length);
    s->replace_count++;
    mw_log("render: texture %u: replaceRegion %ux%u at %u,%u row %u (%u byte(s)) queued",
           request->texture_id, request->width, request->height, request->origin_x,
           request->origin_y, row_bytes, request->length);
    return 0;
}

/* WP-3: MW_TEXTURE_SET_IOSURFACE. The guest hands over the physical runs behind a texture's real
 * IOSurface. Keeping them costs 16 bytes a run; the execution half uses them to put the picture
 * into the guest's surface and read it back by reference. A metadata-only message (0 segments)
 * means the guest could not resolve them, and the host keeps its own read-back path. */
static int mw_handle_texture_set_iosurface(struct mw_server *s,
                                           const struct mw_texture_set_iosurface *request,
                                           const unsigned char *segments_data, uint32_t length)
{
    struct mw_image *image;
    const struct mw_segment *segments = (const struct mw_segment *)segments_data;
    uint64_t total = 0;

    image = mw_image_find(s, request->texture_id);
    if (image == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "IOSurface binding names unknown texture %u",
                      request->texture_id);
        return -1;
    }
    if (request->segment_count > 8192 ||
        length != (uint64_t)request->segment_count * sizeof(struct mw_segment)) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "IOSurface binding: %u segment(s) in %u body byte(s)",
                      request->segment_count, length);
        return -1;
    }
    if (request->bytes_per_row < image->width * mw_format_bytes(image->format)) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "IOSurface row %u is narrower than texture %u",
                      request->bytes_per_row, request->texture_id);
        return -1;
    }
    for (uint32_t i = 0; i < request->segment_count; i++) {
        if (s->ram != NULL &&
            ((uint64_t)segments[i].pa < s->ram_base ||
             (uint64_t)segments[i].pa - s->ram_base + segments[i].len > s->ram_size)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                          "IOSurface segment %u at %#" PRIx64 " is outside the RAM file", i,
                          (uint64_t)segments[i].pa);
            return -1;
        }
        total += segments[i].len;
    }
    if (request->segment_count != 0 && total != request->alloc_size) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "IOSurface binding covers %" PRIu64 " byte(s), alloc size %u", total,
                      request->alloc_size);
        return -1;
    }
    free(image->surface_segments);
    image->surface_segments = NULL;
    image->surface_segment_count = request->segment_count;
    image->surface_bytes_per_row = request->bytes_per_row;
    image->surface_alloc_size = request->alloc_size;
    image->has_surface = request->segment_count != 0;
    if (request->segment_count != 0) {
        image->surface_segments =
            malloc((size_t)request->segment_count * sizeof(struct mw_segment));
        if (image->surface_segments == NULL) {
            image->surface_segment_count = 0;
            image->has_surface = 0;
            mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for IOSurface segments");
            return -1;
        }
        memcpy(image->surface_segments, segments,
               (size_t)request->segment_count * sizeof(struct mw_segment));
        mw_log("render: texture %u: real IOSurface, row %u, %u byte(s) in %u physical segment(s)",
               request->texture_id, request->bytes_per_row, request->alloc_size,
               request->segment_count);
    } else {
        mw_log("render: texture %u: IOSurface metadata only (row %u, %u byte(s)); the guest could "
               "not resolve the surface, the host will publish its own read-back",
               request->texture_id, request->bytes_per_row, request->alloc_size);
    }
    return 0;
}

static int mw_handle_copy(struct mw_server *s, const struct mw_copy_buffer_to_texture *request)
{
    struct mw_buffer *buffer;
    struct mw_image *image;
    struct mw_copy *copy;

    if (!s->frame.active) {
        mw_log("warning: MW_COPY_BUFFER_TO_TEXTURE outside a frame ignored");
        return 0;
    }
    buffer = mw_buffer_find(s, request->buffer_id);
    image = mw_image_find(s, request->texture_id);
    if (buffer == NULL || image == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy references unknown buffer %u / texture %u",
                      request->buffer_id, request->texture_id);
        return -1;
    }
    {
        uint32_t bpp = mw_format_bytes(image->format);

        if (request->width == 0 || request->height == 0 ||
            request->bytes_per_row % bpp != 0) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy has width %u height %u row bytes %u",
                          request->width, request->height, request->bytes_per_row);
            return -1;
        }
        if (request->bytes_per_row < request->width * bpp ||
            (uint64_t)request->origin_x + request->width > image->width ||
            (uint64_t)request->origin_y + request->height > image->height) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy rectangle out of the texture");
            return -1;
        }
        if (request->buffer_offset % bpp != 0 ||
            request->buffer_offset + (uint64_t)(request->height - 1) * request->bytes_per_row +
                    (uint64_t)request->width * bpp >
                buffer->length) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy reads outside buffer %u",
                          request->buffer_id);
            return -1;
        }
    }
    if (s->frame.copy_count >= MW_MAX_COPIES) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "more than %d copies in one frame", MW_MAX_COPIES);
        return -1;
    }
    copy = &s->frame.copies[s->frame.copy_count++];
    copy->buffer_id = request->buffer_id;
    copy->texture_id = request->texture_id;
    copy->buffer_offset = request->buffer_offset;
    copy->bytes_per_row = request->bytes_per_row;
    copy->bytes_per_image = request->bytes_per_image;
    copy->width = request->width;
    copy->height = request->height;
    copy->origin_x = request->origin_x;
    copy->origin_y = request->origin_y;
    if (s->verbose)
        mw_log("frame %" PRIu64 ": copy buffer %u -> texture %u (%ux%u at %u,%u, row %u)",
               s->frame.seq, copy->buffer_id, copy->texture_id, copy->width, copy->height,
               copy->origin_x, copy->origin_y, copy->bytes_per_row);
    return 0;
}

/* ---- protocol: render encoders (stage C) ----------------------------------------------------
 *
 * Protocol 2 carries a recorded MTLRenderCommandEncoder. The host validates the object graph
 * (every texture/buffer/pipeline a call names exists) and counts what the frame recorded. The
 * execution half -- Vulkan pipelines from the shaders of WP-2 -- is deliberately not here yet:
 * an acknowledged render frame means the commands crossed and decoded, not that pixels were
 * drawn.
 */

static struct mw_render_pipeline *mw_pipeline_find(struct mw_server *s, uint32_t id)
{
    for (size_t i = 0; i < s->pipeline_count; i++) {
        if (s->pipelines[i]->id == id)
            return s->pipelines[i];
    }
    return NULL;
}

static void mw_pipeline_destroy(struct mw_server *s, struct mw_render_pipeline *pipeline)
{
    for (size_t i = 0; i < sizeof(pipeline->vk_pipeline) / sizeof(pipeline->vk_pipeline[0]); i++) {
        if (pipeline->vk_pipeline[i] != VK_NULL_HANDLE)
            vkDestroyPipeline(s->vk.device, pipeline->vk_pipeline[i], NULL);
        pipeline->vk_pipeline[i] = VK_NULL_HANDLE;
    }
    for (size_t i = 0; i < s->pipeline_count; i++) {
        if (s->pipelines[i] == pipeline) {
            memmove(&s->pipelines[i], &s->pipelines[i + 1],
                    (s->pipeline_count - i - 1) * sizeof(s->pipelines[0]));
            s->pipeline_count--;
            break;
        }
    }
    free(pipeline);
}

static int mw_pipeline_add(struct mw_server *s, struct mw_render_pipeline *pipeline)
{
    if (s->pipeline_count == s->pipeline_cap) {
        size_t cap = s->pipeline_cap == 0 ? 16 : s->pipeline_cap * 2;
        struct mw_render_pipeline **grown =
            realloc(s->pipelines, cap * sizeof(*grown));

        if (grown == NULL)
            return -1;
        s->pipelines = grown;
        s->pipeline_cap = cap;
    }
    s->pipelines[s->pipeline_count++] = pipeline;
    return 0;
}

static void mw_pipeline_name(char *field, size_t size)
{
    field[size - 1] = '\0';
}

/* ---- WP-4: libraries and the fallback for a pipeline the host has no shader for ---- */

static struct mw_library *mw_library_find(struct mw_server *s, uint32_t id)
{
    for (size_t i = 0; i < s->library_count; i++) {
        if (s->libraries[i]->id == id)
            return s->libraries[i];
    }
    return NULL;
}

static const char *mw_library_kind_name(uint32_t kind)
{
    switch (kind) {
    case MW_LIBRARY_DATA:
        return "data";
    case MW_LIBRARY_URL:
        return "url";
    case MW_LIBRARY_FILE:
        return "file";
    case MW_LIBRARY_DEFAULT:
        return "default";
    case MW_LIBRARY_PIPELINE:
        return "pipeline-library";
    case MW_LIBRARY_UNKNOWN:
    default:
        return "unknown";
    }
}

/* The two whitelists are the whole contract between the wire's base-function names and the GLSL
 * that is embedded in this binary. A name that is not here has no implementation: the pipeline is
 * marked unsupported, its draws are skipped and it is named in the missing report. Adding a shader
 * means adding its name in exactly one of these lists and its file to the shaders Makefile. */
static const char *mw_render_vertex_shader(const struct mw_render_pipeline *pipeline)
{
    if (strcmp(pipeline->vertex_function, "fixed_vert_lpf_spc") == 0)
        return "fixed_vert_lpf_spc";
    if (strcmp(pipeline->vertex_function, "std_vert1_lpf") == 0)
        return "std_vert1_lpf";
    if (strcmp(pipeline->vertex_function, "downsample_blur_vert_lpf") == 0)
        return "downsample_blur_vert_lpf";
    return NULL;
}

static const char *mw_render_fragment_shader(const struct mw_render_pipeline *pipeline)
{
    if (strcmp(pipeline->fragment_function, "fixed_frag_lpf_cpf") == 0)
        return "fixed_frag_lpf_cpf";
    if (strcmp(pipeline->fragment_function, "luma_boost_lpf") == 0)
        return "luma_boost_lpf";
    if (strcmp(pipeline->fragment_function, "downsample_4_frag_lpf") == 0)
        return "downsample_4_frag_lpf";
    if (strcmp(pipeline->fragment_function, "downsample_8_frag_lpf") == 0)
        return "downsample_8_frag_lpf";
    if (strcmp(pipeline->fragment_function, "narrow_blur_7_frag_lpf") == 0)
        return "narrow_blur_7_frag_lpf";
    if (strcmp(pipeline->fragment_function, "narrow_blur_11_frag_lpf") == 0)
        return "narrow_blur_11_frag_lpf";
    if (strcmp(pipeline->fragment_function, "narrow_blur_15_frag_lpf") == 0)
        return "narrow_blur_15_frag_lpf";
    if (strcmp(pipeline->fragment_function, "narrow_blur_19_frag_lpf") == 0)
        return "narrow_blur_19_frag_lpf";
    if (strcmp(pipeline->fragment_function, "narrow_blur_23_frag_lpf") == 0)
        return "narrow_blur_23_frag_lpf";
    if (strcmp(pipeline->fragment_function, "narrow_blur_27_frag_lpf") == 0)
        return "narrow_blur_27_frag_lpf";
    return NULL;
}

static int mw_handle_create_library(struct mw_server *s, const unsigned char *payload, uint32_t len)
{
    struct mw_create_library request;
    struct mw_library *library;
    size_t expected;

    if (len < sizeof(request)) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_CREATE_LIBRARY (%u bytes)", len);
        return -1;
    }
    memcpy(&request, payload, sizeof(request));
    if (request.function_count > MW_MAX_LIBRARY_FUNCTIONS) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "library %u: %u entry functions over the %u limit", request.id,
                      request.function_count, MW_MAX_LIBRARY_FUNCTIONS);
        return -1;
    }
    expected = sizeof(request) + (size_t)request.function_count * sizeof(struct mw_library_function);
    if (len != expected) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "library %u: %u-byte message for %zu expected", request.id, len, expected);
        return -1;
    }
    if (mw_library_find(s, request.id) != NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "library id %u already exists", request.id);
        return -1;
    }
    library = calloc(1, sizeof(*library));
    if (library == NULL) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for library %u", request.id);
        return -1;
    }
    library->id = request.id;
    library->kind = request.kind;
    library->hash = request.hash;
    library->byte_length = request.byte_length;
    library->function_count = request.function_count;
    for (uint32_t i = 0; i < request.function_count; i++) {
        struct mw_library_function fn;

        memcpy(&fn, payload + sizeof(request) + (size_t)i * sizeof(fn), sizeof(fn));
        memcpy(library->functions[i], fn.name, MW_NAME_MAX);
        library->functions[i][MW_NAME_MAX - 1] = '\0';
        library->function_hashes[i] = fn.name_hash;
    }
    if (s->library_count == s->library_cap) {
        size_t cap = s->library_cap == 0 ? 8 : s->library_cap * 2;
        struct mw_library **grown = realloc(s->libraries, cap * sizeof(*grown));

        if (grown == NULL) {
            free(library);
            mw_frame_fail(s, MW_STATUS_NO_MEMORY, "library table growth failed");
            return -1;
        }
        s->libraries = grown;
        s->library_cap = cap;
    }
    s->libraries[s->library_count++] = library;
    mw_log("library %u: %s, %llu byte(s), hash 0x%016llx, %u entr%s", library->id,
           mw_library_kind_name(library->kind), (unsigned long long)library->byte_length,
           (unsigned long long)library->hash, library->function_count,
           library->function_count == 1 ? "y" : "ies");
    if (s->verbose) {
        for (uint32_t i = 0; i < library->function_count; i++)
            mw_log("library %u: function \"%s\"", library->id, library->functions[i]);
    }
    return 0;
}

static int mw_handle_pipeline_library_ref(struct mw_server *s, const unsigned char *payload,
                                          uint32_t len)
{
    struct mw_pipeline_library_ref request;
    struct mw_render_pipeline *pipeline;

    if (len < sizeof(request)) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_PIPELINE_LIBRARY_REF (%u bytes)", len);
        return -1;
    }
    memcpy(&request, payload, sizeof(request));
    pipeline = mw_pipeline_find(s, request.pipeline_id);
    if (pipeline == NULL) {
        /* The reference is informative; arriving after its pipeline is the contract, but a host
         * must not fail a frame over a diagnostic message. */
        mw_log("warning: MW_PIPELINE_LIBRARY_REF names unknown pipeline %u; ignored",
               request.pipeline_id);
        return 0;
    }
    pipeline->vertex_library_id = request.vertex_library_id;
    pipeline->fragment_library_id = request.fragment_library_id;
    if (s->verbose) {
        struct mw_library *vertex = mw_library_find(s, request.vertex_library_id);
        struct mw_library *fragment = mw_library_find(s, request.fragment_library_id);

        mw_log("pipeline %u: functions from library %u (%s hash 0x%016llx) and %u (%s hash "
               "0x%016llx)",
               pipeline->id, request.vertex_library_id,
               vertex != NULL ? mw_library_kind_name(vertex->kind) : "unknown",
               vertex != NULL ? (unsigned long long)vertex->hash : 0ull,
               request.fragment_library_id,
               fragment != NULL ? mw_library_kind_name(fragment->kind) : "unknown",
               fragment != NULL ? (unsigned long long)fragment->hash : 0ull);
    }
    return 0;
}

/* One line per unsupported pipeline at exit, plus the libraries it names: the list the WP-4
 * "which pipelines does the desktop need" loop reads instead of grepping a 20 MB serial log. */
static void mw_write_missing_report(struct mw_server *s)
{
    FILE *file;

    if (s->missing_path == NULL)
        return;
    file = fopen(s->missing_path, "w");
    if (file == NULL) {
        mw_log("missing report: cannot write %s: %s", s->missing_path, strerror(errno));
        return;
    }
    fprintf(file, "# metalwire missing-pipeline report\n");
    fprintf(file, "# frames %" PRIu64 ", pipelines %" PRIu64 ", skipped draws %" PRIu64 "\n",
            s->frames_done, s->missing_pipeline_total, s->missing_draws_total);
    for (size_t i = 0; i < s->pipeline_count; i++) {
        struct mw_render_pipeline *p = s->pipelines[i];

        if (!p->unsupported)
            continue;
        fprintf(file,
                "pipeline %u name \"%s\" vertex \"%s\" fragment \"%s\" "
                "vertex_library %u fragment_library %u skipped_draws %" PRIu64 "\n",
                p->id, p->name, p->vertex_function, p->fragment_function, p->vertex_library_id,
                p->fragment_library_id, p->skipped_draws);
    }
    for (size_t i = 0; i < s->library_count; i++) {
        struct mw_library *library = s->libraries[i];

        fprintf(file, "library %u kind %s bytes %llu hash 0x%016llx functions %u\n", library->id,
                mw_library_kind_name(library->kind),
                (unsigned long long)library->byte_length,
                (unsigned long long)library->hash, library->function_count);
        for (uint32_t f = 0; f < library->function_count; f++)
            fprintf(file, "  function \"%s\"\n", library->functions[f]);
    }
    fclose(file);
    mw_log("missing report: %s (%" PRIu64 " pipeline(s), %" PRIu64 " skipped draw(s))",
           s->missing_path, s->missing_pipeline_total, s->missing_draws_total);
}

/* The pipeline has no shader: mark it, say why once, and let the draw loop skip it. Never fails
 * the frame -- WP-4's contract is "a picture with a missing icon is better than no picture". */
static void mw_render_mark_unsupported(struct mw_server *s, struct mw_render_pipeline *pipeline)
{
    struct mw_library *vertex = mw_library_find(s, pipeline->vertex_library_id);
    struct mw_library *fragment = mw_library_find(s, pipeline->fragment_library_id);

    pipeline->unsupported = 1;
    s->missing_pipeline_total++;
    if (pipeline->unsupported_logged)
        return;
    pipeline->unsupported_logged = 1;
    if (pipeline->vertex_function[0] == '\0' && pipeline->fragment_function[0] == '\0') {
        mw_log("render: pipeline %u \"%s\" was made by name from the pipeline library; the wire "
               "carries no base functions for it, so its draws are skipped (the missing report "
               "names it)",
               pipeline->id, pipeline->name);
        return;
    }
    mw_log("render: pipeline %u \"%s\": no embedded Vulkan GLSL for vertex \"%s\" / fragment "
           "\"%s\"; its draws are skipped",
           pipeline->id, pipeline->name,
           pipeline->vertex_function[0] != '\0' ? pipeline->vertex_function : "(none)",
           pipeline->fragment_function[0] != '\0' ? pipeline->fragment_function : "(none)");
    if (pipeline->vertex_library_id != 0 || pipeline->fragment_library_id != 0) {
        mw_log("render: pipeline %u: functions from %s %u (hash 0x%016llx) and %s %u (hash "
               "0x%016llx)",
               pipeline->id, vertex != NULL ? mw_library_kind_name(vertex->kind) : "unknown",
               pipeline->vertex_library_id,
               vertex != NULL ? (unsigned long long)vertex->hash : 0ull,
               fragment != NULL ? mw_library_kind_name(fragment->kind) : "unknown",
               pipeline->fragment_library_id,
               fragment != NULL ? (unsigned long long)fragment->hash : 0ull);
    }
}

static int mw_handle_create_render_pipeline(struct mw_server *s, const unsigned char *payload,
                                            uint32_t len)
{
    struct mw_create_render_pipeline request;
    struct mw_render_pipeline *pipeline;
    size_t attribute_bytes;
    size_t layout_bytes;
    size_t expected;

    if (len < sizeof(request)) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_CREATE_RENDER_PIPELINE (%u bytes)", len);
        return -1;
    }
    memcpy(&request, payload, sizeof(request));
    if (request.vertex_attribute_count > MW_MAX_RENDER_ATTRIBUTES ||
        request.vertex_layout_count > MW_MAX_RENDER_LAYOUTS) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "render pipeline %u: %u attributes / %u layouts over the limit", request.id,
                      request.vertex_attribute_count, request.vertex_layout_count);
        return -1;
    }
    attribute_bytes = (size_t)request.vertex_attribute_count * sizeof(struct mw_vertex_attribute);
    layout_bytes = (size_t)request.vertex_layout_count * sizeof(struct mw_vertex_layout);
    expected = sizeof(request) + attribute_bytes + layout_bytes;
    if (len != expected) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "render pipeline %u: %u-byte message for %zu expected", request.id, len,
                      expected);
        return -1;
    }
    if (mw_pipeline_find(s, request.id) != NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "render pipeline id %u already exists",
                      request.id);
        return -1;
    }
    pipeline = calloc(1, sizeof(*pipeline));
    if (pipeline == NULL) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for render pipeline %u",
                      request.id);
        return -1;
    }
    pipeline->id = request.id;
    pipeline->color_format = request.color_format;
    pipeline->sample_count = request.sample_count;
    pipeline->blend_enabled = request.blend_enabled;
    pipeline->rgb_blend_operation = request.rgb_blend_operation;
    pipeline->alpha_blend_operation = request.alpha_blend_operation;
    pipeline->src_rgb_blend_factor = request.src_rgb_blend_factor;
    pipeline->dst_rgb_blend_factor = request.dst_rgb_blend_factor;
    pipeline->src_alpha_blend_factor = request.src_alpha_blend_factor;
    pipeline->dst_alpha_blend_factor = request.dst_alpha_blend_factor;
    pipeline->write_mask = request.write_mask;
    pipeline->vertex_attribute_count = request.vertex_attribute_count;
    pipeline->vertex_layout_count = request.vertex_layout_count;
    if (attribute_bytes > 0)
        memcpy(pipeline->attributes, payload + sizeof(request), attribute_bytes);
    if (layout_bytes > 0)
        memcpy(pipeline->layouts, payload + sizeof(request) + attribute_bytes, layout_bytes);
    memcpy(pipeline->name, request.pipeline_name, MW_NAME_MAX);
    memcpy(pipeline->vertex_function, request.vertex_function, MW_NAME_MAX);
    memcpy(pipeline->fragment_function, request.fragment_function, MW_NAME_MAX);
    mw_pipeline_name(pipeline->name, sizeof(pipeline->name));
    mw_pipeline_name(pipeline->vertex_function, sizeof(pipeline->vertex_function));
    mw_pipeline_name(pipeline->fragment_function, sizeof(pipeline->fragment_function));
    if (mw_pipeline_add(s, pipeline) < 0) {
        free(pipeline);
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "pipeline table growth failed");
        return -1;
    }
    mw_log("render pipeline %u: name \"%s\", vertex \"%s\", fragment \"%s\", format %u, "
           "blend %u, %u attributes",
           pipeline->id, pipeline->name, pipeline->vertex_function, pipeline->fragment_function,
           pipeline->color_format, pipeline->blend_enabled, pipeline->vertex_attribute_count);
    if (g_dump_dir != NULL) {
        char name[64];
        char text[8192];
        int at = 0;

        at += snprintf(text + at, sizeof(text) - (size_t)at,
                       "pipeline %u\nname %s\nvertex %s\nfragment %s\ncolor_format %u\n"
                       "sample_count %u\nblend_enabled %u\nwrite_mask 0x%x\n"
                       "blend rgb op %u src %u dst %u; alpha op %u src %u dst %u\n",
                       pipeline->id, pipeline->name, pipeline->vertex_function,
                       pipeline->fragment_function, pipeline->color_format,
                       pipeline->sample_count, pipeline->blend_enabled, pipeline->write_mask,
                       pipeline->rgb_blend_operation, pipeline->src_rgb_blend_factor,
                       pipeline->dst_rgb_blend_factor, pipeline->alpha_blend_operation,
                       pipeline->src_alpha_blend_factor, pipeline->dst_alpha_blend_factor);
        for (uint32_t i = 0; i < pipeline->vertex_attribute_count; i++)
            at += snprintf(text + at, sizeof(text) - (size_t)at,
                           "attribute %u format %u offset %u buffer %u\n", i,
                           pipeline->attributes[i].format, pipeline->attributes[i].offset,
                           pipeline->attributes[i].buffer_index);
        for (uint32_t i = 0; i < pipeline->vertex_layout_count; i++)
            at += snprintf(text + at, sizeof(text) - (size_t)at,
                           "layout %u stride %u step_function %u step_rate %u\n", i,
                           pipeline->layouts[i].stride, pipeline->layouts[i].step_function,
                           pipeline->layouts[i].step_rate);
        snprintf(name, sizeof(name), "pipeline-%u.txt", pipeline->id);
        mw_dump_file(name, text, strlen(text));
    }
    return 0;
}

static VkFilter mw_metal_filter(uint32_t filter)
{
    return filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

static VkSamplerAddressMode mw_metal_address(uint32_t mode)
{
    switch (mode) {
    case 1:
        return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 2:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 3:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 0:
    default:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static int mw_handle_create_sampler(struct mw_server *s, const struct mw_create_sampler *request)
{
    struct mw_sampler_state *state;
    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkResult result;

    if (mw_sampler_find(s, request->id) != NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "sampler id %u already exists", request->id);
        return -1;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "out of host memory for sampler %u", request->id);
        return -1;
    }
    state->id = request->id;
    info.magFilter = mw_metal_filter(request->mag_filter);
    info.minFilter = mw_metal_filter(request->min_filter);
    info.mipmapMode = request->mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                               : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = mw_metal_address(request->address_s);
    info.addressModeV = mw_metal_address(request->address_t);
    info.addressModeW = mw_metal_address(request->address_r);
    info.mipLodBias = 0.0f;
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    info.compareEnable = VK_FALSE;
    info.compareOp = VK_COMPARE_OP_NEVER;
    info.minLod = request->lod_min;
    info.maxLod = request->lod_max != 0.0f ? request->lod_max : VK_LOD_CLAMP_NONE;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.unnormalizedCoordinates = request->normalized_coords == 0 ? VK_TRUE : VK_FALSE;
    result = vkCreateSampler(s->vk.device, &info, NULL, &state->sampler);
    if (result != VK_SUCCESS) {
        free(state);
        mw_frame_fail(s, MW_STATUS_VULKAN, "vkCreateSampler for sampler %u failed", request->id);
        return -1;
    }
    if (mw_sampler_add(s, state) < 0) {
        vkDestroySampler(s->vk.device, state->sampler, NULL);
        free(state);
        mw_frame_fail(s, MW_STATUS_NO_MEMORY, "sampler table growth failed");
        return -1;
    }
    mw_log("render sampler %u: min %u mag %u mip %u address %u/%u/%u normalized %u",
           request->id, request->min_filter, request->mag_filter, request->mip_filter,
           request->address_s, request->address_t, request->address_r,
           request->normalized_coords);
    if (g_dump_dir != NULL) {
        char name[64];
        char text[1024];

        snprintf(text, sizeof(text),
                 "sampler %u\nmin %u\nmag %u\nmip %u\naddress_s %u\naddress_t %u\n"
                 "address_r %u\nnormalized %u\ncompare %u\nlod_min %g\nlod_max %g\n"
                 "max_anisotropy %u\n",
                 request->id, request->min_filter, request->mag_filter, request->mip_filter,
                 request->address_s, request->address_t, request->address_r,
                 request->normalized_coords, request->compare_function, request->lod_min,
                 request->lod_max, request->max_anisotropy);
        snprintf(name, sizeof(name), "sampler-%u.txt", request->id);
        mw_dump_file(name, text, strlen(text));
    }
    return 0;
}

static int mw_handle_create_depth_stencil(struct mw_server *s,
                                          const struct mw_create_depth_stencil *request)
{
    (void)s;
    mw_log("render depth-stencil %u: compare %u write %u", request->id, request->depth_compare,
           request->depth_write);
    return 0;
}

static int mw_handle_render_begin(struct mw_server *s, const struct mw_render_begin *request)
{
    struct mw_image *target;
    struct mw_render_encoder_state *enc = &s->frame.encoder;

    if (!s->frame.active) {
        mw_log("warning: MW_RENDER_BEGIN outside a frame ignored");
        return 0;
    }
    target = mw_image_find(s, request->target_texture);
    if (target == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "render encoder targets unknown texture %u",
                      request->target_texture);
        return -1;
    }
    memset(enc, 0, sizeof(*enc));
    enc->active = 1;
    enc->target_texture = request->target_texture;
    enc->width = request->width;
    enc->height = request->height;
    enc->pixel_format = request->pixel_format;
    enc->load_action = request->load_action;
    enc->store_action = request->store_action;
    memcpy(enc->clear_color, request->clear_color, sizeof(enc->clear_color));
    enc->viewport[0] = 0.0f;
    enc->viewport[1] = 0.0f;
    enc->viewport[2] = (float)request->width;
    enc->viewport[3] = (float)request->height;
    enc->viewport[4] = 0.0f;
    enc->viewport[5] = 1.0f;
    enc->scissor[0] = 0;
    enc->scissor[1] = 0;
    enc->scissor[2] = request->width;
    enc->scissor[3] = request->height;
    s->frame.render_mode = 1;
    s->frame.render_encoders++;
    s->frame.render_target = request->target_texture;
    s->frame.present_texture = request->target_texture;
    if (s->render_frames_seen == 0) {
        mw_log("render: encoder %u: target texture %u (%ux%u format %u), load %u store %u, "
               "clear %.3f %.3f %.3f %.3f, depth %u",
               request->encoder_index, request->target_texture, request->width, request->height,
               request->pixel_format, request->load_action, request->store_action,
               request->clear_color[0], request->clear_color[1], request->clear_color[2],
               request->clear_color[3], request->has_depth);
    }
    return 0;
}

static int mw_handle_render_set_pipeline(struct mw_server *s,
                                         const struct mw_render_set_pipeline *request)
{
    struct mw_render_pipeline *pipeline;

    if (!s->frame.render_mode) {
        mw_log("warning: MW_RENDER_SET_PIPELINE outside a render encoder ignored");
        return 0;
    }
    s->frame.render_set_count++;
    pipeline = mw_pipeline_find(s, request->pipeline_id);
    if (pipeline == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "draw pipeline %u was never created",
                      request->pipeline_id);
        return -1;
    }
    s->frame.render_pipeline = request->pipeline_id;
    s->frame.encoder.pipeline_id = request->pipeline_id;
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: pipeline %u \"%s\"", pipeline->id, pipeline->name);
    }
    return 0;
}

static int mw_handle_render_set_state(struct mw_server *s, const char *what,
                                      const struct mw_render_set_state *request)
{
    (void)request;
    if (!s->frame.render_mode) {
        mw_log("warning: %s outside a render encoder ignored", what);
        return 0;
    }
    s->frame.render_set_count++;
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: %s %u", what, request->value);
    }
    return 0;
}

static int mw_handle_render_set_buffer(struct mw_server *s, const char *what,
                                       const struct mw_render_set_buffer *request)
{
    if (!s->frame.render_mode) {
        mw_log("warning: %s outside a render encoder ignored", what);
        return 0;
    }
    if (mw_buffer_find(s, request->buffer_id) == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "%s names unknown buffer %u", what,
                      request->buffer_id);
        return -1;
    }
    s->frame.render_set_count++;
    if (request->index < MW_MAX_RENDER_BINDINGS && s->frame.encoder.active) {
        if (strncmp(what, "vertex", 6) == 0) {
            s->frame.encoder.vertex_buffer_id[request->index] = request->buffer_id;
            s->frame.encoder.vertex_buffer_offset[request->index] = request->offset;
        } else {
            s->frame.encoder.fragment_buffer_id[request->index] = request->buffer_id;
            s->frame.encoder.fragment_buffer_offset[request->index] = request->offset;
        }
    }
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: %s buffer %u offset %" PRIu64 " index %u", what, request->buffer_id,
               (uint64_t)request->offset, request->index);
    }
    return 0;
}

static int mw_handle_render_set_buffer_offset(struct mw_server *s, const char *what,
                                              const struct mw_render_set_buffer_offset *request)
{
    struct mw_render_encoder_state *enc = &s->frame.encoder;

    if (!s->frame.render_mode) {
        mw_log("warning: %s outside a render encoder ignored", what);
        return 0;
    }
    s->frame.render_set_count++;
    if (request->index < MW_MAX_RENDER_BINDINGS && enc->active) {
        int vertex = strncmp(what, "vertex", 6) == 0;

        if (vertex) {
            if (enc->vertex_buffer_id[request->index] == 0) {
                mw_log("warning: %s index %u has no buffer bound; the offset is kept for a "
                       "later draw anyway",
                       what, request->index);
            }
            enc->vertex_buffer_offset[request->index] = request->offset;
        } else {
            if (enc->fragment_buffer_id[request->index] == 0) {
                mw_log("warning: %s index %u has no buffer bound; the offset is kept for a "
                       "later draw anyway",
                       what, request->index);
            }
            enc->fragment_buffer_offset[request->index] = request->offset;
        }
    }
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: %s offset %" PRIu64 " index %u (replaces the bound buffer's offset)", what,
               (uint64_t)request->offset, request->index);
    }
    return 0;
}

static int mw_handle_render_set_texture(struct mw_server *s, const char *what,
                                        const struct mw_render_set_texture *request)
{
    if (!s->frame.render_mode) {
        mw_log("warning: %s outside a render encoder ignored", what);
        return 0;
    }
    if (mw_image_find(s, request->texture_id) == NULL) {
        mw_log("%s names texture %u which the host does not hold; the binding is ignored", what,
               request->texture_id);
        return 0;
    }
    s->frame.render_set_count++;
    if (request->index < MW_MAX_RENDER_BINDINGS && s->frame.encoder.active)
        s->frame.encoder.texture_id[request->index] = request->texture_id;
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: %s texture %u index %u", what, request->texture_id, request->index);
    }
    return 0;
}

static int mw_handle_render_set_sampler(struct mw_server *s, const char *what,
                                        const struct mw_render_set_sampler *request)
{
    (void)what;
    if (!s->frame.render_mode) {
        mw_log("warning: %s outside a render encoder ignored", what);
        return 0;
    }
    s->frame.render_set_count++;
    if (request->index < MW_MAX_RENDER_BINDINGS && s->frame.encoder.active)
        s->frame.encoder.sampler_id[request->index] = request->sampler_id;
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: %s sampler %u index %u", what, request->sampler_id, request->index);
    }
    return 0;
}

static int mw_handle_render_set_bytes(struct mw_server *s, const struct mw_render_set_bytes *request,
                                      uint32_t len)
{
    if (!s->frame.render_mode) {
        mw_log("warning: MW_RENDER_SET_*_BYTES outside a render encoder ignored");
        return 0;
    }
    if (len != sizeof(*request) + request->length) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "set-bytes message is %u bytes for %u payload",
                      len, request->length);
        return -1;
    }
    s->frame.render_set_count++;
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: stage %u bytes index %u length %u", request->stage, request->index,
               request->length);
    }
    return 0;
}

static int mw_handle_render_set_viewport(struct mw_server *s,
                                         const struct mw_render_set_viewport *request)
{
    if (!s->frame.render_mode) {
        mw_log("warning: MW_RENDER_SET_VIEWPORT outside a render encoder ignored");
        return 0;
    }
    s->frame.render_set_count++;
    if (s->frame.encoder.active) {
        s->frame.encoder.viewport[0] = request->x;
        s->frame.encoder.viewport[1] = request->y;
        s->frame.encoder.viewport[2] = request->width;
        s->frame.encoder.viewport[3] = request->height;
        s->frame.encoder.viewport[4] = request->znear;
        s->frame.encoder.viewport[5] = request->zfar;
        s->frame.encoder.has_viewport = 1;
    }
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: viewport %.1f %.1f %.1fx%.1f z %.3f..%.3f", request->x, request->y,
               request->width, request->height, request->znear, request->zfar);
    }
    return 0;
}

static int mw_handle_render_set_scissor(struct mw_server *s,
                                        const struct mw_render_set_scissor *request)
{
    if (!s->frame.render_mode) {
        mw_log("warning: MW_RENDER_SET_SCISSOR outside a render encoder ignored");
        return 0;
    }
    s->frame.render_set_count++;
    if (s->frame.encoder.active) {
        s->frame.encoder.scissor[0] = request->x;
        s->frame.encoder.scissor[1] = request->y;
        s->frame.encoder.scissor[2] = request->width;
        s->frame.encoder.scissor[3] = request->height;
        s->frame.encoder.has_scissor = 1;
    }
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: scissor %ux%u at %u,%u", request->width, request->height, request->x,
               request->y);
    }
    return 0;
}

static int mw_handle_render_set_blend_color(struct mw_server *s,
                                            const struct mw_render_set_blend_color *request)
{
    if (!s->frame.render_mode) {
        mw_log("warning: MW_RENDER_SET_BLEND_COLOR outside a render encoder ignored");
        return 0;
    }
    s->frame.render_set_count++;
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("CA set: blend color %.3f %.3f %.3f %.3f", request->red, request->green,
               request->blue, request->alpha);
    }
    return 0;
}

static int mw_handle_render_draw(struct mw_server *s, const struct mw_render_draw *request)
{
    struct mw_render_encoder_state *enc = &s->frame.encoder;
    struct mw_render_draw_command *command;

    if (!s->frame.render_mode) {
        mw_log("warning: MW_RENDER_DRAW outside a render encoder ignored");
        return 0;
    }
    if (s->frame.render_pipeline == 0) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "draw before any pipeline was bound");
        return -1;
    }
    if ((request->flags & MW_DRAW_INDEXED) != 0 && request->index_buffer != 0 &&
        mw_buffer_find(s, request->index_buffer) == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "draw names unknown index buffer %u",
                      request->index_buffer);
        return -1;
    }
    s->frame.render_draw_count++;
    if (s->render_draws_logged < 64) {
        s->render_draws_logged++;
        mw_log("CA draw: pipeline %u prim %u %s%s (indices %" PRIu64 ", vertices %" PRIu64
               ", instances %u, base vertex %" PRId64 ")",
               s->frame.render_pipeline, request->primitive_type,
               (request->flags & MW_DRAW_INDEXED) ? "indexed" : "non-indexed",
               (request->flags & MW_DRAW_INSTANCED) ? " instanced" : "",
               (uint64_t)request->index_count, (uint64_t)request->vertex_count,
               request->instance_count, (int64_t)request->base_vertex);
        {
            char geometry[512];
            size_t at = 0;

            geometry[0] = '\0';
            for (uint32_t b = 0; b < MW_MAX_RENDER_BINDINGS; b++) {
                if (enc->vertex_buffer_id[b] != 0 && at < sizeof(geometry) - 32) {
                    at += (size_t)snprintf(geometry + at, sizeof(geometry) - at,
                                           " v%u=%" PRIu64, b, enc->vertex_buffer_offset[b]);
                }
            }
            for (uint32_t b = 0; b < MW_MAX_RENDER_BINDINGS; b++) {
                if (enc->fragment_buffer_id[b] != 0 && at < sizeof(geometry) - 32) {
                    at += (size_t)snprintf(geometry + at, sizeof(geometry) - at,
                                           " f%u=%" PRIu64, b, enc->fragment_buffer_offset[b]);
                }
            }
            mw_log("CA draw: geometry: index %u at %" PRIu64 "%s", request->index_buffer,
                   (uint64_t)request->index_buffer_offset, geometry);
        }
    }
    if (s->frame.draw_count >= MW_MAX_RENDER_DRAWS) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "more than %d draws in one frame",
                      MW_MAX_RENDER_DRAWS);
        return -1;
    }
    command = &s->frame.draws[s->frame.draw_count++];
    memset(command, 0, sizeof(*command));
    command->target_texture = enc->target_texture;
    command->width = enc->width;
    command->height = enc->height;
    command->pixel_format = enc->pixel_format;
    command->load_action = enc->load_action;
    command->store_action = enc->store_action;
    memcpy(command->clear_color, enc->clear_color, sizeof(command->clear_color));
    command->pipeline_id = s->frame.render_pipeline;
    command->primitive_type = request->primitive_type;
    command->flags = request->flags;
    command->index_type = request->index_type;
    command->index_buffer = request->index_buffer;
    command->index_count = request->index_count;
    command->index_buffer_offset = request->index_buffer_offset;
    command->vertex_start = request->vertex_start;
    command->vertex_count = request->vertex_count;
    command->instance_count = request->instance_count != 0 ? request->instance_count : 1;
    command->base_instance = request->base_instance;
    command->base_vertex = (uint64_t)request->base_vertex;
    memcpy(command->vertex_buffer_id, enc->vertex_buffer_id,
           sizeof(command->vertex_buffer_id));
    memcpy(command->vertex_buffer_offset, enc->vertex_buffer_offset,
           sizeof(command->vertex_buffer_offset));
    memcpy(command->fragment_buffer_id, enc->fragment_buffer_id,
           sizeof(command->fragment_buffer_id));
    memcpy(command->fragment_buffer_offset, enc->fragment_buffer_offset,
           sizeof(command->fragment_buffer_offset));
    memcpy(command->texture_id, enc->texture_id, sizeof(command->texture_id));
    memcpy(command->sampler_id, enc->sampler_id, sizeof(command->sampler_id));
    memcpy(command->viewport, enc->viewport, sizeof(command->viewport));
    memcpy(command->scissor, enc->scissor, sizeof(command->scissor));
    return 0;
}

static int mw_handle_render_end(struct mw_server *s, const struct mw_render_end *request)
{
    if (!s->frame.render_mode) {
        mw_log("warning: MW_RENDER_END outside a render encoder ignored");
        return 0;
    }
    s->frame.encoder.active = 0;
    if (s->render_frames_seen == 0 || s->verbose) {
        mw_log("render: encoder %u ended: %u draws, %u sets", request->encoder_index,
               request->draw_count, request->set_count);
    }
    return 0;
}

/* ---- protocol: frame execution -------------------------------------------------------------- */

/* The render frame execution composites into the draws' own targets; this bounds the set the
 * per-frame "has this target been written already" bookkeeping tracks, so the first draw into a
 * target honours the wire's load action and later draws into the same target load. */
#define MW_RENDER_MAX_TARGETS 32

/*
 * MW_DUMP_DRAWS=1 (and optional MW_DUMP_DRAWS_FILE=<path>): write one machine-readable line per
 * executed render draw. The switch exists so a boot can answer per-draw questions -- the vertex
 * colour, the fragment uniform's first bytes, and the draw's target-space bounding box -- without
 * keeping a full wire dump around (WP-4's D3/D5 needed exactly those fields). It is off by
 * default: without the variable the file is never opened and the host's output is unchanged.
 */
static FILE *draw_dump;

static int mw_pipeline_fetches(const struct mw_render_pipeline *pipeline);

static void mw_draw_dump_open(void)
{
    const char *path = getenv("MW_DUMP_DRAWS_FILE");

    if (path == NULL || path[0] == '\0')
        path = "/tmp/metalwire-draws.txt";
    draw_dump = fopen(path, "w");
    if (draw_dump == NULL)
        mw_log("draw dump: cannot write %s", path);
    else
        mw_log("draw dump: %s (one line per executed draw)", path);
}

static void mw_draw_dump_close(void)
{
    if (draw_dump != NULL) {
        fclose(draw_dump);
        draw_dump = NULL;
    }
}

static int mw_draw_buf_read(const struct mw_buffer *buffer, uint64_t offset, void *out,
                            size_t bytes)
{
    if (buffer == NULL || buffer->mapped == NULL || offset > buffer->length ||
        bytes > buffer->length - offset)
        return -1;
    memcpy(out, (const unsigned char *)buffer->mapped + offset, bytes);
    return 0;
}

/* Read the draw's position attribute as float4 at vertex `index` (stream 1, the wire's layouts). */
static int mw_draw_position(const struct mw_server *s, const struct mw_render_draw_command *draw,
                            const struct mw_render_pipeline *pipeline, uint32_t index,
                            float out[4])
{
    const struct mw_vertex_attribute *attribute = &pipeline->attributes[0];
    const struct mw_buffer *buffer;
    uint32_t binding = attribute->buffer_index;
    uint64_t stride;
    uint64_t offset;

    if (attribute->format != 31 || binding >= MW_MAX_RENDER_LAYOUTS)
        return -1;
    buffer = mw_buffer_find((struct mw_server *)s, draw->vertex_buffer_id[binding]);
    if (buffer == NULL)
        return -1;
    stride = pipeline->layouts[binding].stride;
    offset = draw->vertex_buffer_offset[binding] + (uint64_t)index * stride + attribute->offset;
    return mw_draw_buf_read(buffer, offset, out, sizeof(float) * 4);
}

static void mw_dump_draw(const struct mw_server *s, uint64_t frame_seq, uint64_t frame_index,
                         const struct mw_render_draw_command *draw,
                         const struct mw_render_pipeline *pipeline,
                         const struct mw_image *draw_target)
{
    float colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    unsigned char uniform[16];
    char uniform_hex[33];
    char uniform_f32[128];
    float mvp[16];
    const struct mw_buffer *wire;
    const struct mw_buffer *index_buffer = NULL;
    uint32_t wire_id;
    uint32_t colour_binding = pipeline->attributes[3].buffer_index;
    uint32_t index_count = 0;
    uint32_t index_start = 0;
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    int have_bbox = 0;
    uint32_t i;

    if (draw_dump == NULL)
        return;

    /* The fragment uniform is the wire's fragment buffer index 1; the vertex stage reads the mvp
     * from vertex buffer index 2 (the same allocation the draw records as its wire buffer). */
    wire_id = draw->vertex_buffer_id[2] != 0
                  ? draw->vertex_buffer_id[2]
                  : (draw->fragment_buffer_id[1] != 0 ? draw->fragment_buffer_id[1]
                                                     : draw->vertex_buffer_id[1]);
    wire = mw_buffer_find((struct mw_server *)s, wire_id);
    uniform_hex[0] = '\0';
    strcpy(uniform_f32, "none");
    if (draw->fragment_buffer_id[1] != 0) {
        const struct mw_buffer *uniform_buffer =
            mw_buffer_find((struct mw_server *)s, draw->fragment_buffer_id[1]);

        if (mw_draw_buf_read(uniform_buffer, draw->fragment_buffer_offset[1], uniform,
                             sizeof(uniform)) == 0) {
            float first[4];

            for (i = 0; i < sizeof(uniform); i++)
                snprintf(uniform_hex + i * 2, 3, "%02x", uniform[i]);
            memcpy(first, uniform, sizeof(first));
            snprintf(uniform_f32, sizeof(uniform_f32), "%.4f,%.4f,%.4f,%.4f", (double)first[0],
                     (double)first[1], (double)first[2], (double)first[3]);
        } else {
            strcpy(uniform_hex, "none");
        }
    } else {
        strcpy(uniform_hex, "none");
    }

    /* The vertex colour is stream 1's attribute 3 (half4). */
    if (colour_binding < MW_MAX_RENDER_LAYOUTS) {
        const struct mw_buffer *colour_buffer =
            mw_buffer_find((struct mw_server *)s, draw->vertex_buffer_id[colour_binding]);
        uint64_t stride = pipeline->layouts[colour_binding].stride;
        uint64_t base = draw->vertex_buffer_offset[colour_binding] +
                        (uint64_t)((draw->flags & MW_DRAW_INDEXED) ? 0 : draw->vertex_start) *
                            stride +
                        pipeline->attributes[3].offset;
        unsigned short half4[4];

        if (mw_draw_buf_read(colour_buffer, base, half4, sizeof(half4)) == 0) {
            for (i = 0; i < 4; i++) {
                int sign = (half4[i] >> 15) & 1;
                int exponent = (half4[i] >> 10) & 31;
                int mantissa = half4[i] & 1023;

                if (exponent == 0)
                    colour[i] = (float)mantissa * 5.9604645e-08f;
                else if (exponent == 31)
                    colour[i] = 0.0f;
                else
                    colour[i] = (float)(1.0 + mantissa / 1024.0) * powf(2.0f,
                                                                     (float)(exponent - 15));
                if (sign)
                    colour[i] = -colour[i];
            }
        }
    }

    /* Bounding box in the target's pixel space: mvp columns times the vertex positions, NDC to
     * framebuffer (Metal's y-up NDC, rows top-down), then min/max over the transformed vertices. */
    if (wire != NULL && mw_draw_buf_read(wire, draw->vertex_buffer_offset[2], mvp, sizeof(mvp)) == 0 &&
        pipeline->attributes[0].format == 31) {
        if (draw->flags & MW_DRAW_INDEXED) {
            index_buffer = mw_buffer_find((struct mw_server *)s, draw->index_buffer);
            index_count = draw->index_count < 1024 ? draw->index_count : 1024;
            index_start = 0;
        } else {
            index_count = draw->vertex_count < 1024 ? draw->vertex_count : 1024;
            index_start = draw->vertex_start;
        }
        for (i = 0; i < index_count; i++) {
            float position[4];
            uint32_t vertex = index_start + i;

            if ((draw->flags & MW_DRAW_INDEXED) && index_buffer != NULL) {
                if (draw->index_type == 1) {
                    uint32_t index;
                    if (mw_draw_buf_read(index_buffer,
                                         draw->index_buffer_offset + (uint64_t)i * 4, &index, 4) < 0)
                        break;
                    vertex = index;
                } else {
                    unsigned short index;
                    if (mw_draw_buf_read(index_buffer,
                                         draw->index_buffer_offset + (uint64_t)i * 2, &index, 2) < 0)
                        break;
                    vertex = index;
                }
            }
            if (mw_draw_position(s, draw, pipeline, vertex, position) < 0)
                break;
            {
                float x = mvp[0] * position[0] + mvp[4] * position[1] + mvp[8] * position[2] +
                          mvp[12] * position[3];
                float y = mvp[1] * position[0] + mvp[5] * position[1] + mvp[9] * position[2] +
                          mvp[13] * position[3];
                float w = mvp[3] * position[0] + mvp[7] * position[1] + mvp[11] * position[2] +
                          mvp[15] * position[3];
                float fx, fy;

                if (w == 0.0f)
                    w = 1.0f;
                fx = (x / w * 0.5f + 0.5f) * (float)draw_target->width;
                fy = (1.0f - (y / w * 0.5f + 0.5f)) * (float)draw_target->height;
                if (!have_bbox) {
                    min_x = max_x = fx;
                    min_y = max_y = fy;
                    have_bbox = 1;
                } else {
                    if (fx < min_x) min_x = fx;
                    if (fx > max_x) max_x = fx;
                    if (fy < min_y) min_y = fy;
                    if (fy > max_y) max_y = fy;
                }
            }
        }
    }

    fprintf(draw_dump,
            "draw seq=%" PRIu64 " frame=%" PRIu64 " pipeline=\"%s\" vertex_color=%.4f,%.4f,%.4f,%.4f "
            "frag_uniform_buffer=%u frag_uniform_offset=%" PRIu64 " uniform16=%s uniform16_f32=%s "
            "bbox=%d,%d,%d,%d texture=%u fetch=%d\n",
            frame_seq, frame_index, pipeline->name, (double)colour[0], (double)colour[1],
            (double)colour[2], (double)colour[3],
            draw->fragment_buffer_id[1], draw->fragment_buffer_offset[1], uniform_hex,
            uniform_f32,
            have_bbox ? (int)floorf(min_x) : -1, have_bbox ? (int)floorf(min_y) : -1,
            have_bbox ? (int)(ceilf(max_x) - floorf(min_x)) : 0,
            have_bbox ? (int)(ceilf(max_y) - floorf(min_y)) : 0,
            draw->texture_id[3], mw_pipeline_fetches(pipeline));
}

static void mw_cmd_image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
                                 VkImageLayout new_layout, VkAccessFlags src_access,
                                 VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                                 VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};

    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void mw_vk_result_log(const char *what, VkResult result)
{
    mw_log("vulkan: %s failed: %d", what, (int)result);
}

/* Record and run one frame; the copy is a real GPU transfer: staging buffer -> optimal-tiling
 * image -> read-back buffer. Returns an enum mw_status. */
static int mw_run_frame(struct mw_server *s, struct mw_frame_state *frame,
                        struct mw_frame_result *result)
{
    struct mw_vulkan *vk = &s->vk;
    struct mw_image *present;
    VkCommandBuffer command = vk->command_buffer;
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkResult vr;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    uint64_t t4;
    int status = MW_STATUS_OK;

    memset(result, 0, sizeof(*result));
    if (frame->error != 0)
        return frame->error;
    if (s->pending_status != 0) {
        status = s->pending_status;
        s->pending_status = 0;
        return status;
    }
    if (!frame->has_present) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "frame %" PRIu64 " has no MW_PRESENT", frame->seq);
        return MW_STATUS_BAD_COMMAND;
    }
    present = mw_image_find(s, frame->present_texture);
    if (present == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "frame %" PRIu64 " presents unknown texture %u",
                      frame->seq, frame->present_texture);
        return MW_STATUS_BAD_COMMAND;
    }
    if (frame->width != present->width || frame->height != present->height) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "frame %" PRIu64 ": %ux%u does not match texture %u (%ux%u)", frame->seq,
                      frame->width, frame->height, present->id, present->width, present->height);
        return MW_STATUS_BAD_COMMAND;
    }

    for (size_t i = 0; i < s->image_count; i++)
        s->images[i]->dst_this_frame = 0;

    t0 = mw_now_ns();
    vr = vkResetCommandPool(vk->device, vk->command_pool, 0);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkResetCommandPool", vr);
        return MW_STATUS_VULKAN;
    }
    vr = vkBeginCommandBuffer(command, &begin_info);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkBeginCommandBuffer", vr);
        return MW_STATUS_VULKAN;
    }

    for (size_t i = 0; i < frame->copy_count; i++) {
        const struct mw_copy *copy = &frame->copies[i];
        struct mw_buffer *buffer = mw_buffer_find(s, copy->buffer_id);
        struct mw_image *image = mw_image_find(s, copy->texture_id);
        VkBufferImageCopy region = {0};

        if (buffer == NULL || image == NULL) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy object disappeared before execution");
            status = MW_STATUS_BAD_COMMAND;
            break;
        }
        if (!image->dst_this_frame) {
            VkImageLayout old_layout = image->layout;
            VkAccessFlags src_access = 0;
            VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

            if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
                src_access = VK_ACCESS_TRANSFER_READ_BIT;
                src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
                src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
            mw_cmd_image_barrier(
                command, image->image, old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, src_access,
                VK_ACCESS_TRANSFER_WRITE_BIT, src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
            image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            image->dst_this_frame = 1;
        }
        region.bufferOffset = copy->buffer_offset;
        region.bufferRowLength = copy->bytes_per_row / mw_format_bytes(image->format);
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.x = (int32_t)copy->origin_x;
        region.imageOffset.y = (int32_t)copy->origin_y;
        region.imageOffset.z = 0;
        region.imageExtent.width = copy->width;
        region.imageExtent.height = copy->height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command, buffer->buffer, image->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    if (status == MW_STATUS_OK) {
        VkImageLayout old_layout = present->layout;
        VkAccessFlags src_access = 0;
        VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkBufferImageCopy region = {0};

        if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            src_access = VK_ACCESS_TRANSFER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            mw_cmd_image_barrier(
                command, present->image, old_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                src_access, VK_ACCESS_TRANSFER_READ_BIT, src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
            present->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = present->width;
        region.imageExtent.height = present->height;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(command, present->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               present->readback, 1, &region);
    }

    if (status != MW_STATUS_OK) {
        vkEndCommandBuffer(command);
        for (size_t i = 0; i < s->image_count; i++)
            s->images[i]->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return status;
    }
    vr = vkEndCommandBuffer(command);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkEndCommandBuffer", vr);
        for (size_t i = 0; i < s->image_count; i++)
            s->images[i]->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return MW_STATUS_VULKAN;
    }
    t1 = mw_now_ns();
    result->exec_ns = t1 - t0;

    vkResetFences(vk->device, 1, &vk->fence);
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command;
    vr = vkQueueSubmit(vk->queue, 1, &submit_info, vk->fence);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkQueueSubmit", vr);
        for (size_t i = 0; i < s->image_count; i++)
            s->images[i]->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return MW_STATUS_VULKAN;
    }
    vr = vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX);
    t2 = mw_now_ns();
    result->gpu_ns = t2 - t1;
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkWaitForFences", vr);
        for (size_t i = 0; i < s->image_count; i++)
            s->images[i]->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return MW_STATUS_VULKAN;
    }

    {
        /* The read-back is tight width*bytes-a-texel rows, whatever format the texture has. */
        uint32_t row_bytes = present->width * mw_format_bytes(present->format);

        result->crc = mw_crc32_rows_zlib((const unsigned char *)present->readback_mapped, row_bytes,
                                         row_bytes, present->height, s->crc_stride);
    }
    result->crc_ok = result->crc == frame->guest_crc ? 1 : 0;
    t3 = mw_now_ns();
    /*
     * The CRC says the sampled rows are the client's; this says the whole thing is, once per run.
     * The source bytes are still in the staging buffer (the upload copies them there, by body or
     * by reference), so a memcmp against the read-back is a literal byte comparison.
     */
    if (s->byte_check && !s->byte_checked && frame->copy_count > 0) {
        struct mw_buffer *source = mw_buffer_find(s, frame->copies[0].buffer_id);

        if (source != NULL && source->mapped != NULL && source->length >= present->bytes) {
            int same =
                memcmp((const unsigned char *)source->mapped + frame->copies[0].buffer_offset,
                       present->readback_mapped, present->bytes) == 0;

            mw_log("picture %s: %u bytes of %ux%u, memcmp against the uploaded source buffer",
                   same ? "byte-identical" : "DIFFERS", present->bytes, present->width,
                   present->height);
            s->byte_checked = 1;
        }
    }
    result->readback_ns = t3 - t2;

    if (s->shm.path != NULL && s->shm.path[0] != '\0' && !s->shm.failed) {
        if (mw_shm_prepare(&s->shm, present->width, present->height) == 0) {
            const unsigned char *bgra = present->readback_mapped;

            if (!mw_format_is_bgra(present->format)) {
                unsigned char *scratch =
                    mw_scratch_resize(&s->bgra_scratch, &s->bgra_scratch_size, present->bytes);

                if (scratch != NULL) {
                    mw_pixels_to_bgra(present->readback_mapped, present->bytes, present->format,
                                      scratch);
                    bgra = scratch;
                }
            }
            mw_shm_publish(&s->shm, bgra);
        }
    }
    if (s->out_dir != NULL) {
        unsigned char *rgb = mw_scratch_resize(&s->rgb_scratch, &s->rgb_scratch_size,
                                               (size_t)present->width * present->height * 3);
        char path[4096];

        if (rgb != NULL) {
            mw_pixels_to_rgb(present->readback_mapped, present->width, present->height,
                             present->format, rgb);
            snprintf(path, sizeof(path), "%s/frame-%06u.ppm", s->out_dir, (unsigned)s->frames_done);
            if (mw_write_ppm(path, rgb, present->width, present->height) < 0)
                mw_log("warning: cannot write %s", path);
            if (s->png_every > 0 && s->frames_done % (uint64_t)s->png_every == 0) {
                snprintf(path, sizeof(path), "%s/frame-%06u.png", s->out_dir,
                         (unsigned)s->frames_done);
                if (mw_write_png(path, rgb, present->width, present->height) < 0)
                    mw_log("warning: cannot write %s", path);
            }
        }
    }
    t4 = mw_now_ns();
    result->publish_ns = t4 - t3;
    return MW_STATUS_OK;
}

/* ---- stage C: executing a wire render frame on Vulkan --------------------------------------
 *
 * The decode half above keeps every object the wire names; this half turns the draws the guest
 * recorded into real Vulkan work. Three decisions are worth stating:
 *
 *  - Dynamic rendering (Vulkan 1.3) instead of a VkRenderPass/Framebuffer pair: the wire carries
 *    the load/store actions and the clear value per encoder, so the attachment's loadOp is
 *    chosen when the command buffer is recorded, not when a render pass object is created.
 *  - Metal's `[[color(0)]]` framebuffer fetch is translated to a sampled snapshot: before a draw
 *    whose fragment function reads the destination, the colour image is copied to a scratch image
 *    and bound as a texture. Core Vulkan has no framebuffer fetch, and a fixed-function blend
 *    equation cannot express the 37 blend modes CA bakes into its fragment shaders.
 *  - Metal buffers share one index space per stage but have arbitrary byte offsets; Vulkan
 *    descriptor offsets are alignment-constrained, so the host binds the whole wire buffer once
 *    and passes the four byte offsets the shader needs as push constants.
 */

static VkFormat mw_vk_vertex_format(uint32_t metal_format, uint32_t *components)
{
    *components = 0;
    switch (metal_format) {
    case 28: /* MTLVertexFormatFloat */
        *components = 1;
        return VK_FORMAT_R32_SFLOAT;
    case 29: /* MTLVertexFormatFloat2 */
        *components = 2;
        return VK_FORMAT_R32G32_SFLOAT;
    case 30: /* MTLVertexFormatFloat3 */
        *components = 3;
        return VK_FORMAT_R32G32B32_SFLOAT;
    case 31: /* MTLVertexFormatFloat4 */
        *components = 4;
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 25: /* MTLVertexFormatHalf2 */
        *components = 2;
        return VK_FORMAT_R16G16_SFLOAT;
    case 26: /* MTLVertexFormatHalf3 */
        *components = 3;
        return VK_FORMAT_R16G16B16_SFLOAT;
    case 27: /* MTLVertexFormatHalf4 */
        *components = 4;
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

static VkPrimitiveTopology mw_metal_topology(uint32_t primitive)
{
    switch (primitive) {
    case 0:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 2:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 3:
    default:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static VkIndexType mw_metal_index_type(uint32_t type)
{
    return type == 1 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

static VkBlendFactor mw_metal_blend_factor(uint32_t factor)
{
    switch (factor) {
    case 0:
        return VK_BLEND_FACTOR_ZERO;
    case 2:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case 3:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6:
        return VK_BLEND_FACTOR_DST_COLOR;
    case 7:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 8:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case 9:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 10:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 11:
        return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 12:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 13:
        return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 14:
        return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 1:
    default:
        return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp mw_metal_blend_op(uint32_t op)
{
    switch (op) {
    case 1:
        return VK_BLEND_OP_SUBTRACT;
    case 2:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 3:
        return VK_BLEND_OP_MIN;
    case 4:
        return VK_BLEND_OP_MAX;
    case 0:
    default:
        return VK_BLEND_OP_ADD;
    }
}

static VkColorComponentFlags mw_metal_write_mask(uint32_t mask)
{
    VkColorComponentFlags out = 0;

    if ((mask & 0x8u) != 0)
        out |= VK_COLOR_COMPONENT_R_BIT; /* MTLColorWriteMaskRed */
    if ((mask & 0x4u) != 0)
        out |= VK_COLOR_COMPONENT_G_BIT;
    if ((mask & 0x2u) != 0)
        out |= VK_COLOR_COMPONENT_B_BIT;
    if ((mask & 0x1u) != 0)
        out |= VK_COLOR_COMPONENT_A_BIT;
    return out != 0 ? out : 0xfu;
}

static const struct mw_spirv_blob *mw_spirv_find(const char *name)
{
    for (size_t i = 0; i < sizeof(mw_spirv_blobs) / sizeof(mw_spirv_blobs[0]); i++) {
        if (strcmp(mw_spirv_blobs[i].name, name) == 0)
            return &mw_spirv_blobs[i];
    }
    return NULL;
}

static VkShaderModule mw_render_module(struct mw_server *s, const char *name)
{
    static const char *const names[] = {"fixed_vert_lpf_spc",
                                        "fixed_frag_lpf_cpf",
                                        "std_vert1_lpf",
                                        "luma_boost_lpf",
                                        "downsample_blur_vert_lpf",
                                        "downsample_4_frag_lpf",
                                        "downsample_8_frag_lpf",
                                        "narrow_blur_7_frag_lpf",
                                        "narrow_blur_11_frag_lpf",
                                        "narrow_blur_15_frag_lpf",
                                        "narrow_blur_19_frag_lpf",
                                        "narrow_blur_23_frag_lpf",
                                        "narrow_blur_27_frag_lpf"};
    struct mw_render_vk *r = &s->render;
    int slot = -1;
    const struct mw_spirv_blob *blob;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(names[i], name) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0)
        return VK_NULL_HANDLE;
    if (r->modules[slot] != VK_NULL_HANDLE)
        return r->modules[slot];
    blob = mw_spirv_find(name);
    if (blob == NULL) {
        mw_log("render: no embedded SPIR-V for %s", name);
        return VK_NULL_HANDLE;
    }
    {
        VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};

        info.codeSize = (size_t)blob->word_count * sizeof(uint32_t);
        info.pCode = blob->words;
        if (vkCreateShaderModule(s->vk.device, &info, NULL, &r->modules[slot]) != VK_SUCCESS) {
            mw_log("render: vkCreateShaderModule(%s) failed", name);
            return VK_NULL_HANDLE;
        }
    }
    return r->modules[slot];
}

static int mw_render_make_image(struct mw_server *s, uint32_t width, uint32_t height,
                                VkFormat format, VkImageUsageFlags usage, VkImage *out_image,
                                VkDeviceMemory *out_memory, VkImageView *out_view)
{
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkMemoryRequirements req;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkResult result;

    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(s->vk.device, &info, NULL, &image);
    if (result != VK_SUCCESS) {
        mw_log("render: vkCreateImage(%ux%u) failed: %d", width, height, (int)result);
        return -1;
    }
    vkGetImageMemoryRequirements(s->vk.device, image, &req);
    if (mw_allocate_memory(&s->vk, &req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &memory) < 0) {
        vkDestroyImage(s->vk.device, image, NULL);
        return -1;
    }
    if (vkBindImageMemory(s->vk.device, image, memory, 0) != VK_SUCCESS) {
        vkDestroyImage(s->vk.device, image, NULL);
        vkFreeMemory(s->vk.device, memory, NULL);
        return -1;
    }
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s->vk.device, &view_info, NULL, &view) != VK_SUCCESS) {
        vkDestroyImage(s->vk.device, image, NULL);
        vkFreeMemory(s->vk.device, memory, NULL);
        return -1;
    }
    *out_image = image;
    *out_memory = memory;
    *out_view = view;
    return 0;
}

static void mw_render_drop_image(struct mw_server *s, VkImage *image, VkDeviceMemory *memory,
                                 VkImageView *view)
{
    if (*view != VK_NULL_HANDLE)
        vkDestroyImageView(s->vk.device, *view, NULL);
    if (*image != VK_NULL_HANDLE)
        vkDestroyImage(s->vk.device, *image, NULL);
    if (*memory != VK_NULL_HANDLE)
        vkFreeMemory(s->vk.device, *memory, NULL);
    *view = VK_NULL_HANDLE;
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
}

/* One-shot layout transition for objects created outside a frame (the dummy image). */
static int mw_render_transition_now(struct mw_server *s, VkImage image, VkImageLayout new_layout)
{
    VkCommandBuffer command = s->vk.command_buffer;
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkResult result;

    result = vkResetCommandPool(s->vk.device, s->vk.command_pool, 0);
    if (result != VK_SUCCESS)
        return -1;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS)
        return -1;
    mw_cmd_image_barrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, new_layout, 0,
                         VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (vkEndCommandBuffer(command) != VK_SUCCESS)
        return -1;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command;
    vkResetFences(s->vk.device, 1, &s->vk.fence);
    if (vkQueueSubmit(s->vk.queue, 1, &submit_info, s->vk.fence) != VK_SUCCESS)
        return -1;
    return vkWaitForFences(s->vk.device, 1, &s->vk.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS ? 0
                                                                                           : -1;
}

static int mw_render_vk_init(struct mw_server *s)
{
    struct mw_render_vk *r = &s->render;
    VkDescriptorSetLayoutBinding bindings[4];
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                             VK_SHADER_STAGE_FRAGMENT_BIT,
                                .offset = 0,
                                .size = 4 * sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize sizes[3];
    VkDescriptorPoolCreateInfo pool_info = {.sType =
                                                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkResult result;

    if (r->ready)
        return 0;
    if (!s->vk.has_dynamic_rendering) {
        mw_log("render: the device has no Vulkan 1.3 dynamic rendering; render frames stay "
               "decode-only");
        return -1;
    }
    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_info.bindingCount = 4;
    layout_info.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(s->vk.device, &layout_info, NULL, &r->set_layout);
    if (result != VK_SUCCESS) {
        mw_log("render: vkCreateDescriptorSetLayout failed: %d", (int)result);
        return -1;
    }
    pipeline_info.setLayoutCount = 1;
    pipeline_info.pSetLayouts = &r->set_layout;
    pipeline_info.pushConstantRangeCount = 1;
    pipeline_info.pPushConstantRanges = &push;
    result = vkCreatePipelineLayout(s->vk.device, &pipeline_info, NULL, &r->pipeline_layout);
    if (result != VK_SUCCESS) {
        mw_log("render: vkCreatePipelineLayout failed: %d", (int)result);
        return -1;
    }
    memset(sizes, 0, sizeof(sizes));
    sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[0].descriptorCount = 256;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[1].descriptorCount = 768;
    sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[2].descriptorCount = 256;
    pool_info.maxSets = 256;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = sizes;
    result = vkCreateDescriptorPool(s->vk.device, &pool_info, NULL, &r->pool);
    if (result != VK_SUCCESS) {
        mw_log("render: vkCreateDescriptorPool failed: %d", (int)result);
        return -1;
    }
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(s->vk.device, &sampler_info, NULL, &r->dummy_sampler) != VK_SUCCESS) {
        mw_log("render: vkCreateSampler failed");
        return -1;
    }
    if (mw_render_make_image(s, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             &r->dummy_image, &r->dummy_memory, &r->dummy_view) < 0) {
        mw_log("render: cannot create the 1x1 dummy image");
        return -1;
    }
    if (mw_render_transition_now(s, r->dummy_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) < 0) {
        mw_log("render: cannot transition the dummy image");
        return -1;
    }
    r->ready = 1;
    mw_log("render: execution ready (dynamic rendering, embedded shaders, negative viewport "
           "height for Metal's y-up NDC)");
    return 0;
}

static int mw_render_ensure_fetch(struct mw_server *s, uint32_t width, uint32_t height,
                                  uint32_t format)
{
    struct mw_render_vk *r = &s->render;

    if (r->fetch_image != VK_NULL_HANDLE && r->fetch_width == width &&
        r->fetch_height == height && r->fetch_format == format)
        return 0;
    mw_render_drop_image(s, &r->fetch_image, &r->fetch_memory, &r->fetch_view);
    if (mw_render_make_image(s, width, height, mw_vk_format(format),
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             &r->fetch_image, &r->fetch_memory, &r->fetch_view) < 0) {
        mw_log("render: cannot create the %ux%u framebuffer-fetch snapshot", width, height);
        return -1;
    }
    r->fetch_width = width;
    r->fetch_height = height;
    r->fetch_format = format;
    return 0;
}

static void mw_render_vk_fini(struct mw_server *s)
{
    struct mw_render_vk *r = &s->render;

    for (size_t i = 0; i < sizeof(r->modules) / sizeof(r->modules[0]); i++) {
        if (r->modules[i] != VK_NULL_HANDLE)
            vkDestroyShaderModule(s->vk.device, r->modules[i], NULL);
    }
    mw_render_drop_image(s, &r->fetch_image, &r->fetch_memory, &r->fetch_view);
    mw_render_drop_image(s, &r->dummy_image, &r->dummy_memory, &r->dummy_view);
    if (r->dummy_sampler != VK_NULL_HANDLE)
        vkDestroySampler(s->vk.device, r->dummy_sampler, NULL);
    if (r->pool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(s->vk.device, r->pool, NULL);
    if (r->pipeline_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(s->vk.device, r->pipeline_layout, NULL);
    if (r->set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(s->vk.device, r->set_layout, NULL);
    memset(r, 0, sizeof(*r));
}

static int mw_pipeline_fetches(const struct mw_render_pipeline *pipeline)
{
    return strstr(pipeline->name, "Bsov") != NULL || strstr(pipeline->name, "Tlub") != NULL;
}

/* Which wire pipelines sample Metal fragment texture 3, as their name code says: Timg (the layer
 * image), Tlub (the luma ramp), Tds4/Tds8 (downsample 4x/8x) and Tn11/Tn15/Tn23 (narrow blur with
 * an 11/15/23-tap kernel). The blur family was added in WP-4 item 1.2; before it, only Timg/Tlub
 * were known, and a `Tds*`/`Tn*` pipeline would have been created without a sampler binding. */
static int mw_pipeline_has_texture(const struct mw_render_pipeline *pipeline)
{
    /*
     * WP-4 D3/D5: `Tmua` is CA's "multiply alpha" material base function -- the Dock/Search
     * material binds a support texture (fragment texture 3) that modulates the layer colour. It
     * was missing from this list, so the fragment shader specialised HAS_TEXTURE=0 and painted the
     * wire's white vertex colour opaque over the fetched destination: the flat white Dock/Search
     * band (boot wp4-v12 dump lines 100/103/310/313, host log "texture 0, fetch 1"). Treating it
     * as textured hands the draw to the existing texture * colour path; a Tmua draw without a
     * bound texture keeps the dummy view and the previous white behaviour.
     */
    return strstr(pipeline->name, "Timg") != NULL || strstr(pipeline->name, "Tlub") != NULL ||
           strstr(pipeline->name, "Tmua") != NULL ||
           strstr(pipeline->name, "Tds4") != NULL || strstr(pipeline->name, "Tds8") != NULL ||
           strstr(pipeline->name, "Tnb7") != NULL || strstr(pipeline->name, "Tn11") != NULL ||
           strstr(pipeline->name, "Tn15") != NULL || strstr(pipeline->name, "Tn19") != NULL ||
           strstr(pipeline->name, "Tn23") != NULL || strstr(pipeline->name, "Tn27") != NULL;
}

static int mw_render_ensure_pipeline(struct mw_server *s, struct mw_render_pipeline *pipeline,
                                     uint32_t primitive)
{
    int index = primitive <= 4 ? (int)primitive : 3;
    int has_texture;
    int uses_fetch;
    const char *vertex_name;
    const char *fragment_name;
    VkShaderModule vertex;
    VkShaderModule fragment;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription bindings[MW_MAX_RENDER_LAYOUTS];
    VkVertexInputAttributeDescription attributes[MW_MAX_RENDER_ATTRIBUTES];
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend = {0};
    VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkFormat color_format;
    uint32_t binding_count = 0;
    uint32_t attribute_count = 0;
    uint32_t vconst = 0;
    uint32_t fconst[3] = {0, 0, 0};
    VkSpecializationMapEntry ventry = {0, 0, sizeof(uint32_t)};
    VkSpecializationMapEntry fentries[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo vspec = {.mapEntryCount = 1, .pMapEntries = &ventry,
                                  .dataSize = sizeof(uint32_t), .pData = &vconst};
    VkSpecializationInfo fspec = {.mapEntryCount = 3, .pMapEntries = fentries,
                                  .dataSize = sizeof(fconst), .pData = fconst};
    VkResult result;

    if (pipeline->unsupported)
        return -2;
    /* Strict whitelist: an unknown base function is not silently replaced by a shader that
     * happens to exist. WP-4's fallback is to skip the draw and say so. */
    if (mw_render_vertex_shader(pipeline) == NULL || mw_render_fragment_shader(pipeline) == NULL) {
        mw_render_mark_unsupported(s, pipeline);
        return -2;
    }
    if (pipeline->vk_attempted[index])
        return pipeline->vk_failed ? -1 : 0;
    pipeline->vk_attempted[index] = 1;
    if (mw_render_vk_init(s) < 0) {
        pipeline->vk_failed = 1;
        return -1;
    }
    has_texture = mw_pipeline_has_texture(pipeline);
    uses_fetch = mw_pipeline_fetches(pipeline);
    vertex_name = mw_render_vertex_shader(pipeline);
    fragment_name = mw_render_fragment_shader(pipeline);
    vertex = mw_render_module(s, vertex_name);
    fragment = mw_render_module(s, fragment_name);
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        pipeline->vk_failed = 1;
        return -1;
    }
    vconst = has_texture ? 1u : 0u;
    fconst[0] = has_texture ? 1u : 0u;
    fconst[1] = uses_fetch ? 1u : 0u;
    fconst[2] = 0u;

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].pName = "main";
    stages[0].module = vertex;
    stages[0].pSpecializationInfo = &vspec;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].pName = "main";
    stages[1].module = fragment;
    stages[1].pSpecializationInfo = strcmp(fragment_name, "luma_boost_lpf") == 0 ? NULL : &fspec;

    for (uint32_t i = 0; i < pipeline->vertex_layout_count && binding_count < MW_MAX_RENDER_LAYOUTS;
         i++) {
        if (pipeline->layouts[i].stride == 0)
            continue;
        bindings[binding_count].binding = i;
        bindings[binding_count].stride = pipeline->layouts[i].stride;
        bindings[binding_count].inputRate = pipeline->layouts[i].step_function == 2
                                                ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                : VK_VERTEX_INPUT_RATE_VERTEX;
        binding_count++;
    }
    for (uint32_t i = 0; i < pipeline->vertex_attribute_count &&
                         attribute_count < MW_MAX_RENDER_ATTRIBUTES;
         i++) {
        uint32_t components = 0;
        VkFormat format = mw_vk_vertex_format(pipeline->attributes[i].format, &components);

        if (format == VK_FORMAT_UNDEFINED || pipeline->attributes[i].buffer_index >=
                                                 MW_MAX_RENDER_LAYOUTS)
            continue;
        attributes[attribute_count].location = i;
        attributes[attribute_count].binding = pipeline->attributes[i].buffer_index;
        attributes[attribute_count].format = format;
        attributes[attribute_count].offset = pipeline->attributes[i].offset;
        attribute_count++;
    }
    vertex_input.vertexBindingDescriptionCount = binding_count;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = attribute_count;
    vertex_input.pVertexAttributeDescriptions = attributes;

    assembly.topology = mw_metal_topology(primitive);
    assembly.primitiveRestartEnable = VK_FALSE;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend.colorWriteMask = mw_metal_write_mask(pipeline->write_mask);
    if (pipeline->blend_enabled) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = mw_metal_blend_factor(pipeline->src_rgb_blend_factor);
        blend.dstColorBlendFactor = mw_metal_blend_factor(pipeline->dst_rgb_blend_factor);
        blend.colorBlendOp = mw_metal_blend_op(pipeline->rgb_blend_operation);
        blend.srcAlphaBlendFactor = mw_metal_blend_factor(pipeline->src_alpha_blend_factor);
        blend.dstAlphaBlendFactor = mw_metal_blend_factor(pipeline->dst_alpha_blend_factor);
        blend.alphaBlendOp = mw_metal_blend_op(pipeline->alpha_blend_operation);
    }
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    color_format = mw_vk_format(pipeline->color_format);
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = s->render.pipeline_layout;
    result = vkCreateGraphicsPipelines(s->vk.device, VK_NULL_HANDLE, 1, &info, NULL,
                                       &pipeline->vk_pipeline[index]);
    if (result != VK_SUCCESS) {
        mw_log("render: vkCreateGraphicsPipelines(%s, topology %u) failed: %d", pipeline->name,
               assembly.topology, (int)result);
        pipeline->vk_failed = 1;
        return -1;
    }
    mw_log("render: pipeline %u \"%s\" -> Vulkan pipeline (topology %u, texture %d, fetch %d)",
           pipeline->id, pipeline->name, assembly.topology, has_texture, uses_fetch);
    return 0;
}

static void mw_render_push_offsets(struct mw_server *s, VkCommandBuffer command,
                                   const struct mw_render_draw_command *draw)
{
    uint32_t offsets[4];

    offsets[0] = (uint32_t)draw->vertex_buffer_offset[2];
    offsets[1] = (uint32_t)draw->vertex_buffer_offset[3];
    offsets[2] = (uint32_t)draw->fragment_buffer_offset[1];
    offsets[3] = (uint32_t)draw->fragment_buffer_offset[2];
    vkCmdPushConstants(command, s->render.pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(offsets), offsets);
}

/* WP-3: apply the guest's queued texture content to the Vulkan images. The guest names a region
 * and a stride; Vulkan's bufferRowLength is in texels, so the same stride is expressed that way.
 * Nothing here knows about frames or draws: the content is uploaded before the first draw of the
 * frame that asked for it, which is the order CoreAnimation's upload/sample sequence needs. */
static int mw_apply_pending_replaces(struct mw_server *s, VkCommandBuffer command)
{
    for (size_t i = 0; i < s->replace_count; i++) {
        struct mw_pending_replace *replace = &s->replaces[i];
        struct mw_image *image = mw_image_find(s, replace->texture_id);
        VkBufferImageCopy region = {0};
        uint32_t bpp;
        VkImageLayout old_layout;
        VkAccessFlags src_access = 0;
        VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        if (image == NULL || replace->length == 0)
            continue;
        bpp = mw_format_bytes(image->format);
        if (replace->bytes_per_row % bpp != 0)
            return -1;
        if (mw_create_host_buffer(&s->vk, replace->length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  &replace->staging, &replace->staging_memory,
                                  &replace->staging_mapped) < 0) {
            mw_log("render: texture %u: staging buffer for %u replaceRegion byte(s) failed",
                   replace->texture_id, replace->length);
            return -1;
        }
        memcpy(replace->staging_mapped, replace->bytes, replace->length);
        old_layout = image->layout;
        if (old_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            src_access = VK_ACCESS_SHADER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            src_access = VK_ACCESS_TRANSFER_READ_BIT;
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        mw_cmd_image_barrier(command, image->image, old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             src_access, VK_ACCESS_TRANSFER_WRITE_BIT, src_stage,
                             VK_PIPELINE_STAGE_TRANSFER_BIT);
        image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        region.bufferOffset = 0;
        region.bufferRowLength = replace->bytes_per_row / bpp;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = replace->mipmap_level;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.x = (int32_t)replace->origin_x;
        region.imageOffset.y = (int32_t)replace->origin_y;
        region.imageOffset.z = (int32_t)replace->origin_z;
        region.imageExtent.width = replace->width;
        region.imageExtent.height = replace->height;
        region.imageExtent.depth = replace->depth;
        vkCmdCopyBufferToImage(command, replace->staging, image->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    return 0;
}

static void mw_drop_pending_replaces(struct mw_server *s)
{
    for (size_t i = 0; i < s->replace_count; i++) {
        struct mw_pending_replace *replace = &s->replaces[i];

        if (replace->staging_mapped != NULL)
            vkUnmapMemory(s->vk.device, replace->staging_memory);
        if (replace->staging != VK_NULL_HANDLE)
            vkDestroyBuffer(s->vk.device, replace->staging, NULL);
        if (replace->staging_memory != VK_NULL_HANDLE)
            vkFreeMemory(s->vk.device, replace->staging_memory, NULL);
        free(replace->bytes);
    }
    s->replace_count = 0;
}

/* WP-3: the real-IOSurface half. Every byte of the surface is reachable through the guest's
 * physical segments; the host writes the rendered picture into those bytes (the guest's real
 * surface then holds the picture) and reads them back for the browser. This is the same
 * by-reference idea MW_UPLOAD_BUFFER_PHYS uses in the other direction. */
static int mw_surface_ram_write(struct mw_server *s, const struct mw_image *image,
                                uint64_t offset, const unsigned char *bytes, uint32_t length)
{
    uint32_t i = 0;
    uint64_t base = 0;

    if (!image->has_surface || !s->ram_writable || s->ram == NULL)
        return -1;
    if (offset + length > image->surface_alloc_size)
        return -1;
    while (i < image->surface_segment_count && offset >= base + image->surface_segments[i].len) {
        base += image->surface_segments[i].len;
        i++;
    }
    while (length > 0 && i < image->surface_segment_count) {
        uint64_t in_segment = offset - base;
        uint64_t room = image->surface_segments[i].len - in_segment;
        uint32_t take = room < length ? (uint32_t)room : length;

        memcpy(s->ram + (size_t)(image->surface_segments[i].pa - s->ram_base) + in_segment, bytes,
               take);
        bytes += take;
        offset += take;
        length -= take;
        base += image->surface_segments[i].len;
        i++;
    }
    return length == 0 ? 0 : -1;
}

static int mw_surface_ram_read(struct mw_server *s, const struct mw_image *image,
                               uint64_t offset, unsigned char *bytes, uint32_t length)
{
    uint32_t i = 0;
    uint64_t base = 0;

    if (!image->has_surface || s->ram == NULL)
        return -1;
    if (offset + length > image->surface_alloc_size)
        return -1;
    while (i < image->surface_segment_count && offset >= base + image->surface_segments[i].len) {
        base += image->surface_segments[i].len;
        i++;
    }
    while (length > 0 && i < image->surface_segment_count) {
        uint64_t in_segment = offset - base;
        uint64_t room = image->surface_segments[i].len - in_segment;
        uint32_t take = room < length ? (uint32_t)room : length;

        memcpy(bytes, s->ram + (size_t)(image->surface_segments[i].pa - s->ram_base) + in_segment,
               take);
        bytes += take;
        offset += take;
        length -= take;
        base += image->surface_segments[i].len;
        i++;
    }
    return length == 0 ? 0 : -1;
}

/* Put the tight width*bytes-a-texel read-back into the surface's row-padded layout and read it
 * back. Returns a host-visible copy of what the guest's surface now contains, or NULL when the
 * surface or the writable RAM file is not there (the caller then publishes its own read-back). */
static unsigned char *mw_surface_roundtrip(struct mw_server *s, struct mw_image *image)
{
    uint32_t row_bytes;
    unsigned char *copy;

    if (!image->has_surface || !s->ram_writable || image->readback_mapped == NULL)
        return NULL;
    row_bytes = image->width * mw_format_bytes(image->format);
    if (image->surface_bytes_per_row < row_bytes ||
        image->surface_alloc_size < (uint64_t)image->surface_bytes_per_row * image->height)
        return NULL;
    for (uint32_t y = 0; y < image->height; y++) {
        const unsigned char *row = (const unsigned char *)image->readback_mapped +
                                   (uint64_t)y * row_bytes;

        if (mw_surface_ram_write(s, image, (uint64_t)y * image->surface_bytes_per_row, row,
                                 row_bytes) != 0)
            return NULL;
    }
    copy = mw_scratch_resize(&s->surface_scratch, &s->surface_scratch_size, image->bytes);
    if (copy == NULL)
        return NULL;
    for (uint32_t y = 0; y < image->height; y++) {
        if (mw_surface_ram_read(s, image, (uint64_t)y * image->surface_bytes_per_row,
                                copy + (uint64_t)y * row_bytes, row_bytes) != 0)
            return NULL;
    }
    if (memcmp(copy, image->readback_mapped, image->bytes) != 0) {
        size_t differences = 0;

        for (size_t i = 0; i < image->bytes; i++) {
            if (copy[i] != ((const unsigned char *)image->readback_mapped)[i])
                differences++;
        }
        mw_log("render: IOSurface round trip differs in %zu byte(s) of %u", differences,
               image->bytes);
    } else {
        mw_log("render: picture written into the guest's real IOSurface and read back by "
               "reference (%u byte(s), %u segment(s), row %u)",
               image->bytes, image->surface_segment_count, image->surface_bytes_per_row);
    }
    return copy;
}

static int mw_run_render_frame(struct mw_server *s, struct mw_frame_state *frame,
                               struct mw_frame_result *result)
{
    struct mw_vulkan *vk = &s->vk;
    struct mw_image *target;
    VkCommandBuffer command = vk->command_buffer;
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkImageLayout fetch_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    int status = MW_STATUS_OK;
    VkResult vr;

    memset(result, 0, sizeof(*result));
    if (frame->error != 0)
        return frame->error;
    target = mw_image_find(s, frame->render_target);
    if (target == NULL) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "render frame names unknown target %u",
                      frame->render_target);
        return MW_STATUS_BAD_COMMAND;
    }
    if (frame->draw_count == 0) {
        mw_log("render frame %" PRIu64 ": no draws; acknowledged", frame->seq);
        return MW_STATUS_OK;
    }
    if (mw_render_vk_init(s) < 0)
        return MW_STATUS_VULKAN;
    if (mw_render_ensure_fetch(s, target->width, target->height, target->format) < 0)
        return MW_STATUS_NO_MEMORY;

    {
        /* Debug aid for the WP-2 shader bring-up: execute only the first N draws of the frame. */
        const char *limit = getenv("MW_DEBUG_DRAWS");

        if (limit != NULL) {
            uint32_t debug_limit = (uint32_t)strtoul(limit, NULL, 10);

            if (debug_limit > 0 && debug_limit < frame->draw_count)
                frame->draw_count = debug_limit;
        }
    }
    t0 = mw_now_ns();
    vr = vkResetCommandPool(vk->device, vk->command_pool, 0);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkResetCommandPool", vr);
        return MW_STATUS_VULKAN;
    }
    vr = vkResetDescriptorPool(vk->device, s->render.pool, 0);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkResetDescriptorPool", vr);
        return MW_STATUS_VULKAN;
    }
    vr = vkBeginCommandBuffer(command, &begin_info);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkBeginCommandBuffer", vr);
        return MW_STATUS_VULKAN;
    }
    if (mw_apply_pending_replaces(s, command) < 0) {
        mw_frame_fail(s, MW_STATUS_VULKAN, "cannot upload the queued texture content");
        status = MW_STATUS_VULKAN;
    }

    {
        uint32_t written[MW_RENDER_MAX_TARGETS];
        uint32_t written_targets = 0;
        int first_draw_to_target;

    for (uint32_t d = 0; d < frame->draw_count && status == MW_STATUS_OK; d++) {
        const struct mw_render_draw_command *draw = &frame->draws[d];
        struct mw_render_pipeline *pipeline = mw_pipeline_find(s, draw->pipeline_id);
        struct mw_buffer *wire_buffer = NULL;
        struct mw_buffer *index_buffer = NULL;
        struct mw_image *texture_image = NULL;
        struct mw_sampler_state *sampler_state = NULL;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        VkWriteDescriptorSet writes[4];
        VkDescriptorBufferInfo buffer_info;
        VkDescriptorImageInfo image_info;
        VkDescriptorImageInfo sampler_info;
        VkDescriptorImageInfo fetch_info;
        VkRenderingAttachmentInfo color_attachment = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO};
        VkViewport vp;
        VkRect2D scissor;
        int uses_fetch;
        uint32_t wire_id;

        if (pipeline == NULL) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "draw names unknown pipeline %u",
                          draw->pipeline_id);
            status = MW_STATUS_BAD_COMMAND;
            break;
        }
        {
            int pipeline_status = mw_render_ensure_pipeline(s, pipeline, draw->primitive_type);

            if (pipeline_status != 0) {
                /* WP-4 fallback: -2 is "no shader for this pipeline", -1 is a Vulkan pipeline
                 * that did not build. Either way: skip this draw, keep the frame and the picture
                 * it can still produce. */
                if (pipeline_status != -2)
                    mw_log("render: pipeline %u \"%s\": the Vulkan pipeline did not build; draw "
                           "skipped",
                           pipeline->id, pipeline->name);
                pipeline->skipped_draws++;
                result->draws_skipped++;
                s->missing_draws_total++;
                continue;
            }
        }
        result->draws_executed++;
        /*
         * Per-draw target (WP-4 D3/D5). A wire frame carries one render encoder per CA render
         * pass, and those passes composite into different textures: CA's backdrop/blur chain
         * renders into intermediate surfaces and the last pass draws the display target. The
         * execution used to feed every draw to the frame's last target, so an intermediate pass
         * painted the wrong surface and the material that later samples it saw whatever was left
         * there (a white Dock/Search band). Resolve the draw's own target here; the frame's
         * target stays the read-back/published picture.
         */
        struct mw_image *draw_target = target;

        if (draw->target_texture != 0 && draw->target_texture != draw_target->id) {
            draw_target = mw_image_find(s, draw->target_texture);
            if (draw_target == NULL) {
                mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                              "draw targets unknown texture %u", draw->target_texture);
                status = MW_STATUS_BAD_COMMAND;
                break;
            }
        }
        {
            int target_written = 0;
            uint32_t w;

            for (w = 0; w < written_targets; w++) {
                if (written[w] == draw_target->id) {
                    target_written = 1;
                    break;
                }
            }
            if (!target_written && written_targets < MW_RENDER_MAX_TARGETS) {
                written[written_targets++] = draw_target->id;
            }
            first_draw_to_target = !target_written;
        }
        mw_dump_draw(s, frame->seq, s->render_frames_seen, draw, pipeline, draw_target);
        /* The shader's Metal buffer arguments live in the wire buffer; the one the vertex stage
         * reads at index 2 (mvp_matrix) is the authoritative allocation for this draw. */
        wire_id = draw->vertex_buffer_id[2] != 0
                      ? draw->vertex_buffer_id[2]
                      : (draw->fragment_buffer_id[1] != 0 ? draw->fragment_buffer_id[1]
                                                         : draw->vertex_buffer_id[1]);
        wire_buffer = mw_buffer_find(s, wire_id);
        if (wire_buffer == NULL) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "draw has no wire buffer");
            status = MW_STATUS_BAD_COMMAND;
            break;
        }
        if (draw->fragment_buffer_id[1] != 0 && draw->fragment_buffer_id[1] != wire_id)
            mw_log("warning: frame buffer 1 is buffer %u, not the vertex stage's buffer %u; "
                   "the push-constant offsets assume one allocation",
                   draw->fragment_buffer_id[1], wire_id);
        if ((draw->flags & MW_DRAW_INDEXED) != 0) {
            index_buffer = mw_buffer_find(s, draw->index_buffer);
            if (index_buffer == NULL) {
                mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "draw names unknown index buffer %u",
                              draw->index_buffer);
                status = MW_STATUS_BAD_COMMAND;
                break;
            }
        }
        if (draw->texture_id[3] != 0) {
            texture_image = mw_image_find(s, draw->texture_id[3]);
            if (texture_image == NULL) {
                mw_log("draw names texture %u which the host does not hold; draw skipped",
                       draw->texture_id[3]);
                result->draws_skipped++;
                s->missing_draws_total++;
                continue;
            }
            if (texture_image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                VkAccessFlags src_access = 0;
                VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

                if (texture_image->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                    src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
                    src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                } else if (texture_image->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                    /* A draw in this frame rendered into it (per-draw targets: the backdrop/blur
                     * chains hand a pass's own attachment to the next pass as a texture). The
                     * transition must wait for the attachment writes. */
                    src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                }
                mw_cmd_image_barrier(
                    command, texture_image->image, texture_image->layout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, src_access,
                    VK_ACCESS_SHADER_READ_BIT, src_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                texture_image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        if (draw->sampler_id[0] != 0)
            sampler_state = mw_sampler_find(s, draw->sampler_id[0]);
        uses_fetch = mw_pipeline_fetches(pipeline);

        if (uses_fetch && result->draws_executed > 0 &&
            (draw_target->width != s->render.fetch_width ||
             draw_target->height != s->render.fetch_height)) {
            static int warned_fetch_size;

            if (!warned_fetch_size) {
                warned_fetch_size = 1;
                mw_log("render: a draw fetches a %ux%u target while the frame's fetch image is "
                       "%ux%u; the fetch is skipped for this draw",
                       draw_target->width, draw_target->height, s->render.fetch_width,
                       s->render.fetch_height);
            }
        } else if (uses_fetch && result->draws_executed > 0) {
            VkImageCopy copy = {0};

            mw_cmd_image_barrier(command, draw_target->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT);
            mw_cmd_image_barrier(
                command, s->render.fetch_image, fetch_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                fetch_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                fetch_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                          : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
            copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.srcSubresource.layerCount = 1;
            copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.dstSubresource.layerCount = 1;
            copy.extent.width = draw_target->width;
            copy.extent.height = draw_target->height;
            copy.extent.depth = 1;
            vkCmdCopyImage(command, draw_target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s->render.fetch_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            mw_cmd_image_barrier(command, s->render.fetch_image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            fetch_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mw_cmd_image_barrier(command, draw_target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_ACCESS_TRANSFER_READ_BIT,
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            draw_target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else if (first_draw_to_target &&
                   draw_target->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            VkAccessFlags src_access = 0;
            VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

            if (draw_target->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
                src_access = VK_ACCESS_TRANSFER_READ_BIT;
                src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            } else if (draw_target->layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
                src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }
            mw_cmd_image_barrier(command, draw_target->image, draw_target->layout,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, src_access,
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 src_stage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            draw_target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }

        allocate.descriptorPool = s->render.pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &s->render.set_layout;
        vr = vkAllocateDescriptorSets(vk->device, &allocate, &descriptor);
        if (vr != VK_SUCCESS) {
            mw_vk_result_log("vkAllocateDescriptorSets", vr);
            status = MW_STATUS_VULKAN;
            break;
        }
        buffer_info.buffer = wire_buffer->buffer;
        buffer_info.offset = 0;
        buffer_info.range = VK_WHOLE_SIZE;
        image_info.sampler = VK_NULL_HANDLE;
        image_info.imageView =
            texture_image != NULL ? texture_image->view : s->render.dummy_view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sampler_info.sampler =
            sampler_state != NULL ? sampler_state->sampler : s->render.dummy_sampler;
        sampler_info.imageView = VK_NULL_HANDLE;
        sampler_info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        fetch_info.sampler = VK_NULL_HANDLE;
        fetch_info.imageView = uses_fetch ? s->render.fetch_view : s->render.dummy_view;
        fetch_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(writes, 0, sizeof(writes));
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &buffer_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptor;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[1].pImageInfo = &image_info;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptor;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[2].pImageInfo = &sampler_info;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = descriptor;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[3].pImageInfo = &fetch_info;
        vkUpdateDescriptorSets(vk->device, 4, writes, 0, NULL);

        color_attachment.imageView = draw_target->view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp =
            !first_draw_to_target ? VK_ATTACHMENT_LOAD_OP_LOAD
                  : (draw->load_action == 2
                         ? VK_ATTACHMENT_LOAD_OP_CLEAR
                         : (draw->load_action == 1 ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                   : VK_ATTACHMENT_LOAD_OP_DONT_CARE));
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color.float32[0] = draw->clear_color[0];
        color_attachment.clearValue.color.float32[1] = draw->clear_color[1];
        color_attachment.clearValue.color.float32[2] = draw->clear_color[2];
        color_attachment.clearValue.color.float32[3] = draw->clear_color[3];
        rendering.renderArea.offset.x = 0;
        rendering.renderArea.offset.y = 0;
        rendering.renderArea.extent.width = draw_target->width;
        rendering.renderArea.extent.height = draw_target->height;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color_attachment;
        vkCmdBeginRendering(command, &rendering);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline->vk_pipeline[draw->primitive_type <= 4
                                                    ? draw->primitive_type
                                                    : 3]);
        /*
         * Metal's NDC has y up, Vulkan's has y down, and CA's recorded viewport is in Metal's
         * top-left window space. Passing the height through positive therefore lands every CA
         * draw vertically mirrored in the framebuffer -- that was WP-4's desktop symptom (status
         * bar at the bottom, glyphs inverted) while the BootUI "looked right" only because the
         * Apple logo is nearly symmetric.
         *
         * The compensation is one negative viewport height: the viewport still covers
         * [x, x+width) x [y, y+height) of the framebuffer, but NDC y=+1 now maps to the top edge.
         * Negative height is core Vulkan 1.1 (promoted from VK_KHR_maintenance1), and this render
         * path already requires a Vulkan 1.3 device with dynamic rendering, so every device that
         * reaches here supports it (lavapipe reports 1.4; the V100 driver 1.3+). Doing it at this
         * single point keeps fixed/std/downsample/narrow-blur on one convention -- the vertex
         * shaders stay a faithful transcription of CA's mvp -- while the colour-attachment
         * read-back, the PPM/PNG files, the SHM picture and the IOSurface by-reference write-back
         * all stay in the framebuffer order they already had.
         */
        vp.x = draw->viewport[0];
        if (draw->viewport[3] > 0.0f) {
            vp.y = draw->viewport[1] + draw->viewport[3];
            vp.height = -draw->viewport[3];
        } else {
            vp.y = draw->viewport[1];
            vp.height = draw->viewport[3];
        }
        vp.width = draw->viewport[2];
        vp.minDepth = draw->viewport[4];
        vp.maxDepth = draw->viewport[5];
        vkCmdSetViewport(command, 0, 1, &vp);
        scissor.offset.x = (int32_t)draw->scissor[0];
        scissor.offset.y = (int32_t)draw->scissor[1];
        scissor.extent.width = draw->scissor[2];
        scissor.extent.height = draw->scissor[3];
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                s->render.pipeline_layout, 0, 1, &descriptor, 0, NULL);
        for (uint32_t b = 0; b < MW_MAX_RENDER_LAYOUTS; b++) {
            struct mw_buffer *vertex_buffer;

            if (pipeline->layouts[b].stride == 0 || draw->vertex_buffer_id[b] == 0)
                continue;
            vertex_buffer = mw_buffer_find(s, draw->vertex_buffer_id[b]);
            if (vertex_buffer == NULL) {
                mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "vertex buffer %u is missing",
                              draw->vertex_buffer_id[b]);
                status = MW_STATUS_BAD_COMMAND;
                break;
            }
            vkCmdBindVertexBuffers(command, b, 1, &vertex_buffer->buffer,
                                   &draw->vertex_buffer_offset[b]);
        }
        if (status != MW_STATUS_OK) {
            vkCmdEndRendering(command);
            break;
        }
        if ((draw->flags & MW_DRAW_INDEXED) != 0)
            vkCmdBindIndexBuffer(command, index_buffer->buffer, draw->index_buffer_offset,
                                 mw_metal_index_type(draw->index_type));
        mw_render_push_offsets(s, command, draw);
        if ((draw->flags & MW_DRAW_INDEXED) != 0) {
            vkCmdDrawIndexed(command, (uint32_t)draw->index_count, draw->instance_count, 0,
                             (int32_t)draw->base_vertex, draw->base_instance);
        } else {
            vkCmdDraw(command, (uint32_t)draw->vertex_count, draw->instance_count,
                      (uint32_t)draw->vertex_start, draw->base_instance);
        }
        vkCmdEndRendering(command);
    }
    if (draw_dump != NULL)
        fflush(draw_dump);
    }

    if (status == MW_STATUS_OK) {
        VkImageLayout layout = target->layout;
        VkAccessFlags src_access = layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                        ? VK_ACCESS_TRANSFER_READ_BIT
                                        : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkPipelineStageFlags src_stage = layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                             ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                             : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkBufferImageCopy copy = {0};

        if (layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            mw_cmd_image_barrier(command, target->image, layout,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_access,
                                 VK_ACCESS_TRANSFER_READ_BIT, src_stage,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT);
            target->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = target->width;
        copy.imageExtent.height = target->height;
        copy.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(command, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               target->readback, 1, &copy);
    }

    if (status != MW_STATUS_OK) {
        vkEndCommandBuffer(command);
        for (size_t i = 0; i < s->image_count; i++)
            s->images[i]->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return status;
    }
    vr = vkEndCommandBuffer(command);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkEndCommandBuffer", vr);
        return MW_STATUS_VULKAN;
    }
    t1 = mw_now_ns();
    result->exec_ns = t1 - t0;
    vkResetFences(vk->device, 1, &vk->fence);
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command;
    vr = vkQueueSubmit(vk->queue, 1, &submit_info, vk->fence);
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkQueueSubmit", vr);
        return MW_STATUS_VULKAN;
    }
    vr = vkWaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX);
    t2 = mw_now_ns();
    result->gpu_ns = t2 - t1;
    if (vr != VK_SUCCESS) {
        mw_vk_result_log("vkWaitForFences", vr);
        return MW_STATUS_VULKAN;
    }
    result->crc = mw_crc32_rows_zlib((const unsigned char *)target->readback_mapped,
                                     target->width * mw_format_bytes(target->format),
                                     target->width * mw_format_bytes(target->format),
                                     target->height, s->crc_stride);
    result->crc_ok = 1;
    t3 = mw_now_ns();
    result->readback_ns = t3 - t2;
    {
        const unsigned char *bytes = target->readback_mapped;
        size_t count = (size_t)target->bytes;
        size_t nonzero = 0;

        for (size_t i = 0; i < count; i++) {
            if (bytes[i] != 0) {
                nonzero++;
            }
        }
        mw_log("render frame %" PRIu64 ": executed %" PRIu64 " of %u draw(s) on texture %u "
               "(%ux%u), crc 0x%08x, %zu/%zu non-zero byte(s)",
               frame->seq, result->draws_executed, frame->draw_count, target->id, target->width,
               target->height, result->crc, nonzero, count);
    }
    {
        /* WP-3: when the target is a real IOSurface, the picture goes into the guest's surface
         * first and the browser copy is read out of that memory by reference. Otherwise the
         * Vulkan read-back is the picture, exactly as WP-2 left it. */
        unsigned char *surface_copy = mw_surface_roundtrip(s, target);
        const unsigned char *bgra = surface_copy != NULL ? surface_copy : target->readback_mapped;
        /* Once converted, the bytes are BGRA8 whatever the texture's format was; the PPM step
         * must read them in that order, not convert the 16-bit ramp twice. */
        uint32_t bgra_format = target->format;

        if (!mw_format_is_bgra(target->format)) {
            unsigned char *scratch = mw_scratch_resize(&s->bgra_scratch, &s->bgra_scratch_size,
                                                       target->bytes);

            if (scratch != NULL) {
                mw_pixels_to_bgra(bgra, target->bytes, target->format, scratch);
                bgra = scratch;
                bgra_format = MW_FORMAT_BGRA8_UNORM;
            }
        }
        if (s->shm.path != NULL && s->shm.path[0] != '\0' && !s->shm.failed) {
            if (mw_shm_prepare(&s->shm, target->width, target->height) == 0)
                mw_shm_publish(&s->shm, bgra);
        }
        if (s->out_dir != NULL) {
            unsigned char *rgb = mw_scratch_resize(&s->rgb_scratch, &s->rgb_scratch_size,
                                                   (size_t)target->width * target->height * 3);
            char path[4096];

            if (rgb != NULL) {
                mw_pixels_to_rgb(bgra, target->width, target->height, bgra_format, rgb);
                snprintf(path, sizeof(path), "%s/frame-%06u.ppm", s->out_dir,
                         (unsigned)s->frames_done);
                if (mw_write_ppm(path, rgb, target->width, target->height) < 0)
                    mw_log("warning: cannot write %s", path);
                if (s->png_every > 0 && s->frames_done % (uint64_t)s->png_every == 0) {
                    snprintf(path, sizeof(path), "%s/frame-%06u.png", s->out_dir,
                             (unsigned)s->frames_done);
                    if (mw_write_png(path, rgb, target->width, target->height) < 0)
                        mw_log("warning: cannot write %s", path);
                }
            }
        }
    }
    return MW_STATUS_OK;
}

static void mw_stats_grow(struct mw_server *s, size_t needed)
{
    if (needed <= s->stats_cap)
        return;
    size_t cap = s->stats_cap == 0 ? 64 : s->stats_cap;

    while (cap < needed)
        cap *= 2;
    struct mw_timing *grown = realloc(s->stats, cap * sizeof(*grown));

    if (grown == NULL)
        return;
    s->stats = grown;
    s->stats_cap = cap;
}

static void mw_completions_grow(struct mw_server *s, size_t needed)
{
    if (needed <= s->completion_cap)
        return;
    size_t cap = s->completion_cap == 0 ? 64 : s->completion_cap;

    while (cap < needed)
        cap *= 2;
    uint64_t *grown = realloc(s->completions, cap * sizeof(*grown));

    if (grown == NULL)
        return;
    s->completions = grown;
    s->completion_cap = cap;
}

static double mw_rolling_fps(struct mw_server *s, uint64_t now)
{
    uint64_t start = now > MW_ROLLING_NS ? now - MW_ROLLING_NS : 0;
    size_t count = 0;

    if (start < s->first_frame_begin_ns)
        start = s->first_frame_begin_ns;
    for (size_t i = s->completion_count; i > 0; i--) {
        if (s->completions[i - 1] <= start)
            break;
        count++;
    }
    if (count == 0)
        return 0.0;
    if (now <= start)
        return 0.0;
    return (double)count * 1e9 / (double)(now - start);
}

static void mw_csv_row(struct mw_server *s, uint64_t seq, uint64_t bytes,
                       const struct mw_timing *timing, int crc_ok, double fps)
{
    if (s->csv == NULL)
        return;
    fprintf(s->csv,
            "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%d,%.2f\n",
            seq, bytes, mw_ns_to_us(timing->recv_ns), mw_ns_to_us(timing->exec_ns),
            mw_ns_to_us(timing->gpu_ns), mw_ns_to_us(timing->readback_ns),
            mw_ns_to_us(timing->publish_ns), mw_ns_to_us(timing->total_ns), crc_ok, fps);
    fflush(s->csv);
}

static int mw_send_all(int fd, const void *data, size_t bytes)
{
    const unsigned char *p = data;

    while (bytes > 0) {
        ssize_t sent = send(fd, p, bytes, MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (sent == 0)
            return -1;
        p += sent;
        bytes -= (size_t)sent;
    }
    return 0;
}

/* Answer one FRAME_END, timing the host's whole contribution to it. */
static int mw_handle_frame_end(struct mw_server *s, const struct mw_frame_end *request)
{
    struct mw_frame_state *frame = &s->frame;
    struct mw_frame_result result;
    struct mw_ack ack;
    struct mw_timing timing;
    uint64_t t_end = mw_now_ns();
    uint64_t bytes;
    double fps;
    int status;

    if (!frame->active) {
        mw_log("warning: MW_FRAME_END(%" PRIu64 ") without MW_FRAME_BEGIN", request->seq);
        memset(&ack, 0, sizeof(ack));
        ack.magic = MW_ACK_MAGIC;
        ack.version = s->peer_version != 0 ? s->peer_version : MW_VERSION;
        ack.status = MW_STATUS_BAD_COMMAND;
        ack.seq = request->seq;
        ack.guest_t_commit_ns = request->guest_t_encoded_ns;
        ack.host_t_done_ns = mw_now_ns();
        ack.width = request->width;
        ack.height = request->height;
        ack.bytes_per_row = request->bytes_per_row;
        s->frames_done++;
        if (mw_send_all(s->client_fd, &ack, sizeof(ack)) < 0)
            return -1;
        if (s->frames_target != 0 && s->frames_done >= s->frames_target)
            s->done = 1;
        return 0;
    }

    frame->guest_t_encoded_ns = request->guest_t_encoded_ns;
    frame->guest_crc = request->crc;
    frame->width = request->width;
    frame->height = request->height;
    frame->bytes_per_row = request->bytes_per_row;
    frame->seq = request->seq;

    /* Stage C: a frame the guest recorded from CoreAnimation's render encoders. The commands are
     * decoded and validated above; execution waits for WP-2's shaders. The guest cannot checksum a
     * picture it did not draw, so the ack reports crc_ok without a read-back (crc 0). */
    if (frame->render_mode) {
        struct mw_ack ack;
        struct mw_frame_result result;

        memset(&result, 0, sizeof(result));
        status = mw_run_render_frame(s, frame, &result);
        mw_drop_pending_replaces(s);
        memset(&ack, 0, sizeof(ack));
        ack.magic = MW_ACK_MAGIC;
        ack.version = s->peer_version != 0 ? s->peer_version : MW_VERSION;
        ack.status = (uint16_t)status;
        ack.seq = frame->seq;
        ack.guest_t_commit_ns = frame->guest_t_encoded_ns;
        ack.host_t_done_ns = mw_now_ns();
        ack.crc = result.crc;
        ack.crc_ok = 1; /* the guest did not hash a picture it did not draw */
        ack.width = frame->width;
        ack.height = frame->height;
        ack.bytes_per_row = frame->bytes_per_row;
        mw_log("render frame %" PRIu64 ": %u encoder(s), %" PRIu64 " draws, %" PRIu64
               " sets, target texture %u %ux%u, status %d (%s)",
               frame->seq, frame->render_encoders, frame->render_draw_count,
               frame->render_set_count, frame->render_target, frame->width, frame->height,
               status,
               status == MW_STATUS_OK
                   ? "executed on Vulkan"
                   : (status == MW_STATUS_VULKAN ? "Vulkan error" : "wire error"));
        if (frame->render_mode && (result.draws_skipped != 0 || result.draws_executed == 0)) {
            mw_log("render frame %" PRIu64 ": executed %" PRIu64 " draw(s), skipped %" PRIu64
                   " (pipelines without an embedded shader are listed once, above)",
                   frame->seq, result.draws_executed, result.draws_skipped);
        }
        s->render_frames_seen++;
        if (mw_send_all(s->client_fd, &ack, sizeof(ack)) < 0) {
            mw_frame_reset(s);
            return -1;
        }
        s->frames_done++;
        mw_frame_reset(s);
        if (s->frames_target != 0 && s->frames_done >= s->frames_target)
            s->done = 1;
        return 0;
    }

    memset(&timing, 0, sizeof(timing));
    timing.recv_ns = t_end > frame->begin_ns ? t_end - frame->begin_ns : 0;
    status = mw_run_frame(s, frame, &result);
    mw_drop_pending_replaces(s);
    if (status == MW_STATUS_OK && !result.crc_ok)
        status = MW_STATUS_CRC_MISMATCH;

    bytes = (uint64_t)frame->width * frame->height * MW_PIXEL_BYTES;
    if (status == MW_STATUS_BAD_COMMAND)
        bytes = 0;

    timing.exec_ns = result.exec_ns;
    timing.gpu_ns = result.gpu_ns;
    timing.readback_ns = result.readback_ns;
    timing.publish_ns = result.publish_ns;

    memset(&ack, 0, sizeof(ack));
    ack.magic = MW_ACK_MAGIC;
    ack.version = s->peer_version != 0 ? s->peer_version : MW_VERSION;
    ack.status = (uint16_t)status;
    ack.seq = frame->seq;
    ack.guest_t_commit_ns = frame->guest_t_encoded_ns;
    ack.host_t_done_ns = mw_now_ns();
    ack.crc = result.crc;
    ack.crc_ok = result.crc_ok;
    ack.width = frame->width;
    ack.height = frame->height;
    ack.bytes_per_row = frame->bytes_per_row;
    if (mw_send_all(s->client_fd, &ack, sizeof(ack)) < 0) {
        mw_frame_reset(s);
        return -1;
    }
    timing.total_ns = mw_now_ns() - t_end;

    if (status == MW_STATUS_OK || status == MW_STATUS_CRC_MISMATCH) {
        mw_completions_grow(s, s->completion_count + 1);
        if (s->completion_count < s->completion_cap) {
            s->completions[s->completion_count++] = mw_now_ns();
            s->last_completion_ns = s->completions[s->completion_count - 1];
        }
        fps = mw_rolling_fps(s, mw_now_ns());
        mw_stats_grow(s, s->stats_count + 1);
        if (s->stats_count < s->stats_cap) {
            s->stats[s->stats_count].recv_ns = timing.recv_ns;
            s->stats[s->stats_count].exec_ns = timing.exec_ns;
            s->stats[s->stats_count].gpu_ns = timing.gpu_ns;
            s->stats[s->stats_count].readback_ns = timing.readback_ns;
            s->stats[s->stats_count].publish_ns = timing.publish_ns;
            s->stats[s->stats_count].total_ns = timing.total_ns;
            s->stats_count++;
        }
        if (status == MW_STATUS_OK)
            s->frames_ok++;
        mw_csv_row(s, frame->seq, bytes, &timing, result.crc_ok, fps);
        mw_log("frame %" PRIu64 ": %" PRIu64
               " bytes, recv %.3f ms, exec %.3f ms, gpu %.3f ms, readback %.3f ms, publish %.3f "
               "ms, total %.3f ms, crc %s (0x%08x), %.1f fps",
               frame->seq, bytes, (double)timing.recv_ns / 1e6, (double)timing.exec_ns / 1e6,
               (double)timing.gpu_ns / 1e6, (double)timing.readback_ns / 1e6,
               (double)timing.publish_ns / 1e6, (double)timing.total_ns / 1e6,
               result.crc_ok ? "ok" : "MISMATCH", result.crc, fps);
    } else {
        mw_csv_row(s, frame->seq, 0, &timing, 0, 0.0);
        mw_log("frame %" PRIu64 ": failed with status %d%s", frame->seq, status,
               status == MW_STATUS_BAD_COMMAND ? " (bad command)" : "");
    }

    s->frames_done++;
    mw_frame_reset(s);
    if (s->frames_target != 0 && s->frames_done >= s->frames_target)
        s->done = 1;
    return 0;
}

/* ---- protocol: connection ------------------------------------------------------------------- */

static int mw_recv_full(int fd, void *data, size_t bytes)
{
    unsigned char *p = data;
    size_t got = 0;

    while (got < bytes) {
        struct pollfd poll_fd = {fd, POLLIN, 0};
        int ready;

        if (g_stop)
            return 0;
        ready = poll(&poll_fd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ready == 0)
            continue;
        ssize_t n = recv(fd, p + got, bytes - got, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return 0;
        got += (size_t)n;
    }
    return 1;
}

static const char *mw_type_name(uint16_t type)
{
    switch (type) {
    case MW_HELLO:
        return "MW_HELLO";
    case MW_BYE:
        return "MW_BYE";
    case MW_CREATE_BUFFER:
        return "MW_CREATE_BUFFER";
    case MW_UPLOAD_BUFFER:
        return "MW_UPLOAD_BUFFER";
    case MW_UPLOAD_BUFFER_PHYS:
        return "MW_UPLOAD_BUFFER_PHYS";
    case MW_DESTROY_BUFFER:
        return "MW_DESTROY_BUFFER";
    case MW_CREATE_TEXTURE:
        return "MW_CREATE_TEXTURE";
    case MW_DESTROY_TEXTURE:
        return "MW_DESTROY_TEXTURE";
    case MW_TEXTURE_REPLACE_REGION:
        return "MW_TEXTURE_REPLACE_REGION";
    case MW_TEXTURE_SET_IOSURFACE:
        return "MW_TEXTURE_SET_IOSURFACE";
    case MW_COPY_BUFFER_TO_TEXTURE:
        return "MW_COPY_BUFFER_TO_TEXTURE";
    case MW_FILL_BUFFER:
        return "MW_FILL_BUFFER";
    case MW_PRESENT:
        return "MW_PRESENT";
    case MW_FRAME_BEGIN:
        return "MW_FRAME_BEGIN";
    case MW_FRAME_END:
        return "MW_FRAME_END";
    case MW_RENDER_BEGIN:
        return "MW_RENDER_BEGIN";
    case MW_RENDER_SET_PIPELINE:
        return "MW_RENDER_SET_PIPELINE";
    case MW_RENDER_SET_DEPTH_STENCIL:
        return "MW_RENDER_SET_DEPTH_STENCIL";
    case MW_RENDER_SET_VERTEX_BUFFER:
    case MW_RENDER_SET_FRAGMENT_BUFFER:
        return type == MW_RENDER_SET_VERTEX_BUFFER ? "MW_RENDER_SET_VERTEX_BUFFER"
                                                   : "MW_RENDER_SET_FRAGMENT_BUFFER";
    case MW_RENDER_SET_VERTEX_BUFFER_OFFSET:
    case MW_RENDER_SET_FRAGMENT_BUFFER_OFFSET:
        return type == MW_RENDER_SET_VERTEX_BUFFER_OFFSET
                   ? "MW_RENDER_SET_VERTEX_BUFFER_OFFSET"
                   : "MW_RENDER_SET_FRAGMENT_BUFFER_OFFSET";
    case MW_RENDER_SET_VERTEX_BYTES:
        return "MW_RENDER_SET_VERTEX_BYTES";
    case MW_RENDER_SET_VERTEX_TEXTURE:
        return "MW_RENDER_SET_VERTEX_TEXTURE";
    case MW_RENDER_SET_VERTEX_SAMPLER:
        return "MW_RENDER_SET_VERTEX_SAMPLER";
    case MW_RENDER_SET_FRAGMENT_BYTES:
        return "MW_RENDER_SET_FRAGMENT_BYTES";
    case MW_RENDER_SET_FRAGMENT_TEXTURE:
        return "MW_RENDER_SET_FRAGMENT_TEXTURE";
    case MW_RENDER_SET_FRAGMENT_SAMPLER:
        return "MW_RENDER_SET_FRAGMENT_SAMPLER";
    case MW_RENDER_SET_VIEWPORT:
        return "MW_RENDER_SET_VIEWPORT";
    case MW_RENDER_SET_SCISSOR:
        return "MW_RENDER_SET_SCISSOR";
    case MW_RENDER_SET_CULL_MODE:
        return "MW_RENDER_SET_CULL_MODE";
    case MW_RENDER_SET_WINDING:
        return "MW_RENDER_SET_WINDING";
    case MW_RENDER_SET_FILL_MODE:
        return "MW_RENDER_SET_FILL_MODE";
    case MW_RENDER_SET_BLEND_COLOR:
        return "MW_RENDER_SET_BLEND_COLOR";
    case MW_RENDER_DRAW:
        return "MW_RENDER_DRAW";
    case MW_RENDER_END:
        return "MW_RENDER_END";
    case MW_CREATE_RENDER_PIPELINE:
        return "MW_CREATE_RENDER_PIPELINE";
    case MW_CREATE_SAMPLER:
        return "MW_CREATE_SAMPLER";
    case MW_CREATE_DEPTH_STENCIL:
        return "MW_CREATE_DEPTH_STENCIL";
    default:
        return "unknown";
    }
}

/* One message. Returns 0 to keep the connection, -1 to close it. */
static int mw_handle_message(struct mw_server *s, uint16_t type, const unsigned char *payload,
                             uint32_t len)
{
    if (s->verbose)
        mw_log("<- %s (0x%04x), %u byte(s)", mw_type_name(type), type, len);

    switch (type) {
    case MW_HELLO: {
        struct mw_hello hello;

        if (len < sizeof(hello)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_HELLO (%u bytes)", len);
            break;
        }
        memcpy(&hello, payload, sizeof(hello));
        hello.program[sizeof(hello.program) - 1] = '\0';
        mw_log("hello: version %u pid %u program \"%s\"", hello.version, hello.pid, hello.program);
        break;
    }
    case MW_BYE:
        mw_log("bye");
        return -1;
    case MW_CREATE_BUFFER: {
        struct mw_create_buffer request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_CREATE_BUFFER (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_create_buffer(s, &request);
        break;
    }
    case MW_UPLOAD_BUFFER: {
        struct mw_upload_buffer request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_UPLOAD_BUFFER (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_upload_buffer(s, &request, payload + sizeof(request), len - sizeof(request));
        break;
    }
    case MW_UPLOAD_BUFFER_PHYS: {
        struct mw_upload_buffer_phys request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_UPLOAD_BUFFER_PHYS (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_upload_buffer_phys(s, &request, payload + sizeof(request), len);
        break;
    }
    case MW_DESTROY_BUFFER: {
        struct mw_create_buffer request;
        struct mw_buffer *buffer;

        if (len < 4) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_DESTROY_BUFFER (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request.id));
        buffer = mw_buffer_find(s, request.id);
        if (buffer == NULL) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "destroy of unknown buffer %u", request.id);
            break;
        }
        mw_buffer_destroy(s, buffer);
        break;
    }
    case MW_CREATE_TEXTURE: {
        struct mw_create_texture request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_CREATE_TEXTURE (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_create_texture(s, &request);
        break;
    }
    case MW_TEXTURE_REPLACE_REGION: {
        struct mw_texture_replace_region request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_TEXTURE_REPLACE_REGION (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_texture_replace(s, &request, payload + sizeof(request), len - sizeof(request));
        break;
    }
    case MW_TEXTURE_SET_IOSURFACE: {
        struct mw_texture_set_iosurface request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_TEXTURE_SET_IOSURFACE (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_texture_set_iosurface(s, &request, payload + sizeof(request),
                                        len - sizeof(request));
        break;
    }
    case MW_DESTROY_TEXTURE: {
        uint32_t id;
        struct mw_image *image;

        if (len < sizeof(id)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_DESTROY_TEXTURE (%u bytes)", len);
            break;
        }
        memcpy(&id, payload, sizeof(id));
        image = mw_image_find(s, id);
        if (image == NULL) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "destroy of unknown texture %u", id);
            break;
        }
        mw_image_destroy(s, image);
        break;
    }
    case MW_COPY_BUFFER_TO_TEXTURE: {
        struct mw_copy_buffer_to_texture request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_COPY_BUFFER_TO_TEXTURE (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_copy(s, &request);
        break;
    }
    case MW_FILL_BUFFER:
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                      "MW_FILL_BUFFER has no wire structure in protocol v1; the host does not "
                      "implement it");
        break;
    case MW_PRESENT: {
        struct mw_present request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_PRESENT (%u bytes)", len);
            break;
        }
        if (!s->frame.active) {
            mw_log("warning: MW_PRESENT outside a frame ignored");
            break;
        }
        memcpy(&request, payload, sizeof(request));
        s->frame.has_present = 1;
        s->frame.present_texture = request.texture_id;
        if (s->verbose)
            mw_log("frame %" PRIu64 ": present texture %u", s->frame.seq, request.texture_id);
        break;
    }
    case MW_FRAME_BEGIN: {
        struct mw_frame_begin request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_FRAME_BEGIN (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        if (s->frame.active)
            mw_log("warning: frame %" PRIu64 " began while frame %" PRIu64
                   " was open; dropping the "
                   "older one",
                   request.seq, s->frame.seq);
        mw_frame_reset(s);
        s->frame.active = 1;
        s->frame.seq = request.seq;
        s->frame.guest_t_begin_ns = request.guest_t_begin_ns;
        s->frame.begin_ns = mw_now_ns();
        if (s->first_frame_begin_ns == 0)
            s->first_frame_begin_ns = s->frame.begin_ns;
        break;
    }
    case MW_FRAME_END: {
        struct mw_frame_end request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_FRAME_END (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        return mw_handle_frame_end(s, &request);
    }
    /* ---- stage C: render encoders and their objects ---- */
    case MW_CREATE_RENDER_PIPELINE:
        mw_handle_create_render_pipeline(s, payload, len);
        break;
    /* ---- WP-4: dynamic pipelines and their libraries ---- */
    case MW_CREATE_LIBRARY:
        mw_handle_create_library(s, payload, len);
        break;
    case MW_PIPELINE_LIBRARY_REF:
        mw_handle_pipeline_library_ref(s, payload, len);
        break;
    case MW_CREATE_SAMPLER: {
        struct mw_create_sampler request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_CREATE_SAMPLER (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_create_sampler(s, &request);
        break;
    }
    case MW_CREATE_DEPTH_STENCIL: {
        struct mw_create_depth_stencil request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_CREATE_DEPTH_STENCIL (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_create_depth_stencil(s, &request);
        break;
    }
    case MW_RENDER_BEGIN: {
        struct mw_render_begin request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_BEGIN (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_begin(s, &request);
        break;
    }
    case MW_RENDER_SET_PIPELINE: {
        struct mw_render_set_pipeline request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_PIPELINE (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_pipeline(s, &request);
        break;
    }
    case MW_RENDER_SET_DEPTH_STENCIL: {
        struct mw_render_set_depth_stencil request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_DEPTH_STENCIL (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_state(s, "depth-stencil",
                                   (const struct mw_render_set_state *)&request);
        break;
    }
    case MW_RENDER_SET_VERTEX_BUFFER:
    case MW_RENDER_SET_FRAGMENT_BUFFER: {
        struct mw_render_set_buffer request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_*_BUFFER (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_buffer(s,
                                    type == MW_RENDER_SET_VERTEX_BUFFER ? "vertex buffer"
                                                                        : "fragment buffer",
                                    &request);
        break;
    }
    case MW_RENDER_SET_VERTEX_BUFFER_OFFSET:
    case MW_RENDER_SET_FRAGMENT_BUFFER_OFFSET: {
        struct mw_render_set_buffer_offset request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND,
                          "short MW_RENDER_SET_*_BUFFER_OFFSET (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_buffer_offset(
            s, type == MW_RENDER_SET_VERTEX_BUFFER_OFFSET ? "vertex buffer offset"
                                                          : "fragment buffer offset",
            &request);
        break;
    }
    case MW_RENDER_SET_VERTEX_BYTES:
    case MW_RENDER_SET_FRAGMENT_BYTES: {
        struct mw_render_set_bytes request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_*_BYTES (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_bytes(s, &request, len);
        break;
    }
    case MW_RENDER_SET_VERTEX_TEXTURE:
    case MW_RENDER_SET_FRAGMENT_TEXTURE: {
        struct mw_render_set_texture request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_*_TEXTURE (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_texture(s,
                                     type == MW_RENDER_SET_VERTEX_TEXTURE ? "vertex texture"
                                                                         : "fragment texture",
                                     &request);
        break;
    }
    case MW_RENDER_SET_VERTEX_SAMPLER:
    case MW_RENDER_SET_FRAGMENT_SAMPLER: {
        struct mw_render_set_sampler request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_*_SAMPLER (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_sampler(s,
                                     type == MW_RENDER_SET_VERTEX_SAMPLER ? "vertex sampler"
                                                                         : "fragment sampler",
                                     &request);
        break;
    }
    case MW_RENDER_SET_VIEWPORT: {
        struct mw_render_set_viewport request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_VIEWPORT (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_viewport(s, &request);
        break;
    }
    case MW_RENDER_SET_SCISSOR: {
        struct mw_render_set_scissor request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_SCISSOR (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_scissor(s, &request);
        break;
    }
    case MW_RENDER_SET_CULL_MODE:
    case MW_RENDER_SET_WINDING:
    case MW_RENDER_SET_FILL_MODE: {
        struct mw_render_set_state request;
        const char *what = type == MW_RENDER_SET_CULL_MODE   ? "cull mode"
                           : type == MW_RENDER_SET_WINDING ? "winding"
                                                           : "fill mode";

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_STATE (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_state(s, what, &request);
        break;
    }
    case MW_RENDER_SET_BLEND_COLOR: {
        struct mw_render_set_blend_color request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_SET_BLEND_COLOR (%u bytes)",
                          len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_set_blend_color(s, &request);
        break;
    }
    case MW_RENDER_DRAW: {
        struct mw_render_draw request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_DRAW (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_draw(s, &request);
        break;
    }
    case MW_RENDER_END: {
        struct mw_render_end request;

        if (len < sizeof(request)) {
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "short MW_RENDER_END (%u bytes)", len);
            break;
        }
        memcpy(&request, payload, sizeof(request));
        mw_handle_render_end(s, &request);
        break;
    }
    default:
        if (s->frame.active)
            mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "unknown message type 0x%04x", type);
        else
            mw_log("warning: unknown message type 0x%04x outside a frame ignored", type);
        break;
    }
    return 0;
}

static void mw_serve_client(struct mw_server *s)
{
    while (!s->done && !g_stop) {
        struct mw_msg header;
        int status;
        int received;

        received = mw_recv_full(s->client_fd, &header, sizeof(header));
        if (received <= 0)
            break;
        if (header.magic != MW_MAGIC || header.version < 2 || header.version > MW_VERSION) {
            mw_log("protocol: bad header (magic 0x%08x version %u); closing the connection",
                   header.magic, header.version);
            break;
        }
        if (s->peer_version == 0 || s->peer_version != header.version) {
            if (s->peer_version != 0 && s->peer_version != header.version)
                mw_log("protocol: peer switched from version %u to %u; closing the connection",
                       s->peer_version, header.version);
            s->peer_version = header.version;
        }
        if (header.length > MW_MAX_MESSAGE) {
            mw_log("protocol: message length %u over the %u-byte limit; closing the connection",
                   header.length, MW_MAX_MESSAGE);
            break;
        }
        if (header.length > s->payload_cap) {
            unsigned char *grown = realloc(s->payload, header.length);

            if (grown == NULL) {
                mw_log("out of host memory for a %u-byte message", header.length);
                break;
            }
            s->payload = grown;
            s->payload_cap = header.length;
        }
        if (header.length > 0) {
            received = mw_recv_full(s->client_fd, s->payload, header.length);
            if (received <= 0)
                break;
        }
        status = mw_handle_message(s, header.type, s->payload, header.length);
        if (status < 0)
            break;
    }
    if (s->client_fd >= 0) {
        close(s->client_fd);
        s->client_fd = -1;
    }
    mw_frame_reset(s);
    mw_log("client disconnected after %" PRIu64 " frame(s)", s->frames_done);
}

static int mw_wait_for_client(struct mw_server *s)
{
    while (!g_stop && !s->done) {
        struct pollfd poll_fd = {s->listen_fd, POLLIN, 0};
        int ready = poll(&poll_fd, 1, 200);
        int fd;

        if (ready < 0) {
            if (errno == EINTR)
                continue;
            mw_log("poll: %s", strerror(errno));
            return -1;
        }
        if (ready == 0)
            continue;
        fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            mw_log("accept: %s", strerror(errno));
            return -1;
        }
        {
            int one = 1;

            /* The guest is synchronous: without TCP_NODELAY its small command packets wait on
             * delayed ACKs and each frame gains a spurious ~40 ms. */
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
        s->client_fd = fd;
        s->pending_status = 0;
        mw_frame_reset(s);
        mw_log("client connected");
        return 1;
    }
    return 0;
}

/* ---- summary -------------------------------------------------------------------------------- */

static int mw_cmp_u64(const void *a, const void *b)
{
    uint64_t left = *(const uint64_t *)a;
    uint64_t right = *(const uint64_t *)b;

    return left < right ? -1 : (left > right ? 1 : 0);
}

static void mw_print_metric(const char *name, const struct mw_timing *stats, size_t count,
                            uint64_t (*get)(const struct mw_timing *))
{
    uint64_t *values = malloc(count * sizeof(*values));
    uint64_t sum = 0;

    if (values == NULL)
        return;
    for (size_t i = 0; i < count; i++) {
        values[i] = get(&stats[i]);
        sum += values[i];
    }
    qsort(values, count, sizeof(*values), mw_cmp_u64);
    {
        double median = count % 2 == 1
                            ? (double)values[count / 2]
                            : ((double)values[count / 2 - 1] + (double)values[count / 2]) / 2.0;

        mw_log("  %-8s mean %8.3f ms  median %8.3f ms  min %8.3f ms  max %8.3f ms", name,
               (double)sum / (double)count / 1e6, median / 1e6, (double)values[0] / 1e6,
               (double)values[count - 1] / 1e6);
    }
    free(values);
}

static uint64_t mw_get_recv(const struct mw_timing *t)
{
    return t->recv_ns;
}
static uint64_t mw_get_exec(const struct mw_timing *t)
{
    return t->exec_ns;
}
static uint64_t mw_get_gpu(const struct mw_timing *t)
{
    return t->gpu_ns;
}
static uint64_t mw_get_readback(const struct mw_timing *t)
{
    return t->readback_ns;
}
static uint64_t mw_get_publish(const struct mw_timing *t)
{
    return t->publish_ns;
}
static uint64_t mw_get_total(const struct mw_timing *t)
{
    return t->total_ns;
}

static void mw_print_summary(struct mw_server *s)
{
    if (s->stats_count == 0) {
        mw_log("summary: no frames were executed");
        return;
    }
    double span = 0.0;

    if (s->last_completion_ns > s->first_frame_begin_ns)
        span = (double)(s->last_completion_ns - s->first_frame_begin_ns) / 1e9;
    mw_log("summary: %" PRIu64 " frame(s) acknowledged, %" PRIu64
           " executed with crc_ok, %.3f s, %.1f fps",
           s->frames_done, s->frames_ok, span, span > 0.0 ? (double)s->stats_count / span : 0.0);
    mw_print_metric("recv", s->stats, s->stats_count, mw_get_recv);
    mw_print_metric("exec", s->stats, s->stats_count, mw_get_exec);
    mw_print_metric("gpu", s->stats, s->stats_count, mw_get_gpu);
    mw_print_metric("readback", s->stats, s->stats_count, mw_get_readback);
    mw_print_metric("publish", s->stats, s->stats_count, mw_get_publish);
    mw_print_metric("total", s->stats, s->stats_count, mw_get_total);
}

/* ---- setup / teardown ----------------------------------------------------------------------- */

static int mw_open_listener(struct mw_server *s)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        mw_log("socket: %s", strerror(errno));
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)s->port);
    if (inet_pton(AF_INET, s->bind_address, &address.sin_addr) != 1) {
        mw_log("invalid bind address \"%s\"", s->bind_address);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        mw_log("bind %s:%d: %s", s->bind_address, s->port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        mw_log("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&address, &length) == 0)
        s->port = ntohs(address.sin_port);
    s->listen_fd = fd;
    mw_log("listening on %s:%d", s->bind_address, s->port);
    return 0;
}

static int mw_open_csv(struct mw_server *s, const char *path)
{
    FILE *csv = fopen(path, "w");

    if (csv == NULL) {
        mw_log("csv: cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fprintf(csv, "seq,bytes,recv_us,exec_us,gpu_us,readback_us,publish_us,total_us,crc_ok,"
                     "fps\n") < 0) {
        fclose(csv);
        return -1;
    }
    fflush(csv);
    s->csv = csv;
    return 0;
}

static int mw_prepare_out_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            mw_log("--out %s exists and is not a directory", path);
            return -1;
        }
        return 0;
    }
    if (mkdir(path, 0755) < 0) {
        mw_log("--out: cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void mw_server_cleanup(struct mw_server *s)
{
    if (s->client_fd >= 0)
        close(s->client_fd);
    if (s->listen_fd >= 0)
        close(s->listen_fd);
    if (s->csv != NULL)
        fclose(s->csv);
    mw_shm_close(&s->shm);
    while (s->image_count > 0)
        mw_image_destroy(s, s->images[0]);
    while (s->buffer_count > 0)
        mw_buffer_destroy(s, s->buffers[0]);
    while (s->pipeline_count > 0)
        mw_pipeline_destroy(s, s->pipelines[0]);
    while (s->sampler_count > 0)
        mw_sampler_state_destroy(s, s->samplers[0]);
    mw_drop_pending_replaces(s);
    mw_render_vk_fini(s);
    free(s->replaces);
    free(s->surface_scratch);
    free(s->images);
    free(s->buffers);
    free(s->pipelines);
    free(s->samplers);
    free(s->payload);
    free(s->rgb_scratch);
    free(s->bgra_scratch);
    free(s->stats);
    free(s->completions);
    free(s->out_dir);
    if (s->ram != NULL)
        munmap(s->ram, s->ram_size);
    mw_vulkan_fini(&s->vk);
}

static void mw_usage(FILE *out, const char *program)
{
    fprintf(
        out,
        "usage: %s [options]\n"
        "\n"
        "The Vulkan end of the metalwire experiment: listens for a guest Metal device's frames\n"
        "and executes them as Vulkan copies on one physical device. Headless.\n"
        "\n"
        "  --port PORT       TCP port (default %d; 0 picks a free one and prints it)\n"
        "  --bind ADDRESS    IPv4 address to bind (default 127.0.0.1)\n"
        "  --gpu INDEX       Vulkan physical-device index (default: $METALWIRE_GPU else 1)\n"
        "  --pci ADDR        choose the device by PCI id instead of index, e.g. 0000:05:00.0\n"
        "                    (stage C; a vfio-bound card renumbers the indices)\n"
        "  --shm PATH        publish frames in glwire's shared-memory format\n"
        "                    (default: $METALWIRE_SHM else %s; \"\" disables)\n"
        "  --frames N        exit after N acknowledged frames (0 = until stopped)\n"
        "  --once            shorthand for --frames 1\n"
        "  --out DIR         save every frame as DIR/frame-%%06u.ppm (P6)\n"
        "  --png-every N     also save every Nth frame as PNG (default 0 = never)\n"
        "  --csv FILE        write per-frame timings as CSV\n"
        "  --crc-stride N    checksum every Nth row (default 1; must match the guest's -c N)\n"
        "  --byte-check      compare the first presented picture with its source bytes, byte for "
        "byte\n"
        "  --ram PATH        QEMU's RAM file, so an upload-by-reference can be read in place\n"
        "  --ram-base HEX    guest physical address of the RAM file's first byte (default "
        "0x40000000)\n"
        "  --dump DIR        write decoded buffers and object descriptors to DIR (debug aid)\n"
        "  --missing-report FILE  write the pipelines that had no embedded shader to FILE\n"
        "                    (one line each, with its libraries and skipped draw count)\n"
        "  --verbose         log every wire message\n"
        "  --help            this text\n",
        program, MW_DEFAULT_PORT, MW_DEFAULT_SHM);
}

static long mw_parse_long(const char *what, const char *text, long minimum, long maximum)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < minimum || value > maximum) {
        mw_log("invalid %s \"%s\"", what, text);
        exit(2);
    }
    return value;
}

int main(int argc, char **argv)
{
    struct mw_server server;
    static const struct option options[] = {
        {"port", required_argument, NULL, 'p'},     {"bind", required_argument, NULL, 'b'},
        {"gpu", required_argument, NULL, 'g'},      {"shm", required_argument, NULL, 's'},
        {"frames", required_argument, NULL, 'f'},   {"once", no_argument, NULL, '1'},
        {"out", required_argument, NULL, 'o'},      {"png-every", required_argument, NULL, 'P'},
        {"csv", required_argument, NULL, 'c'},      {"ram", required_argument, NULL, 'r'},
        {"ram-base", required_argument, NULL, 'R'}, {"crc-stride", required_argument, NULL, 1000},
        {"pci", required_argument, NULL, 1002},
        {"byte-check", no_argument, NULL, 1001},    {"verbose", no_argument, NULL, 'v'},
        {"dump", required_argument, NULL, 1003},
        {"missing-report", required_argument, NULL, 1004},
        {"help", no_argument, NULL, 'h'},           {NULL, 0, NULL, 0},
    };
    const char *env_gpu = getenv("METALWIRE_GPU");
    const char *pci_address = getenv("METALWIRE_PCI");
    const char *env_shm = getenv("METALWIRE_SHM");
    const char *csv_path = NULL;
    const char *out_dir = NULL;
    const char *ram_path = getenv("METALWIRE_RAM");
    const char *env_crc_stride = getenv("METALWIRE_CRC_STRIDE");
    const char *env_draw_dump = getenv("MW_DUMP_DRAWS");
    uint64_t ram_base = 0x40000000ULL;
    long crc_stride = env_crc_stride != NULL && env_crc_stride[0] != '\0'
                          ? mw_parse_long("crc-stride", env_crc_stride, 1, 4096)
                          : 1;
    const char *shm_path = (env_shm != NULL && env_shm[0] != '\0') ? env_shm : MW_DEFAULT_SHM;
    long gpu_index =
        env_gpu != NULL && env_gpu[0] != '\0' ? mw_parse_long("gpu", env_gpu, 0, 1024) : 1;
    long png_every = 0;
    int once = 0;
    int option;

    if (env_draw_dump != NULL && env_draw_dump[0] != '\0' && atoi(env_draw_dump) != 0) {
        mw_draw_dump_open();
        atexit(mw_draw_dump_close);
    }
    memset(&server, 0, sizeof(server));
    server.listen_fd = -1;
    server.client_fd = -1;
    server.port = MW_DEFAULT_PORT;
    snprintf(server.bind_address, sizeof(server.bind_address), "%s", "127.0.0.1");

    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 'p':
            server.port = (int)mw_parse_long("port", optarg, 0, 65535);
            break;
        case 'b':
            snprintf(server.bind_address, sizeof(server.bind_address), "%s", optarg);
            break;
        case 'g':
            gpu_index = mw_parse_long("gpu", optarg, 0, 1024);
            break;
        case 's':
            shm_path = optarg;
            break;
        case 'f':
            server.frames_target = (uint64_t)mw_parse_long("frames", optarg, 0, 1000000000);
            break;
        case '1':
            once = 1;
            break;
        case 'o':
            out_dir = optarg;
            break;
        case 'P':
            png_every = mw_parse_long("png-every", optarg, 0, 1000000);
            break;
        case 'c':
            csv_path = optarg;
            break;
        case 'r':
            ram_path = optarg;
            break;
        case 'R':
            ram_base = strtoull(optarg, NULL, 16);
            break;
        case 1000:
            crc_stride = mw_parse_long("crc-stride", optarg, 1, 4096);
            break;
        case 1001:
            server.byte_check = 1;
            break;
        case 1002:
            pci_address = optarg;
            break;
        case 1003:
            g_dump_dir = optarg;
            break;
        case 1004:
            server.missing_path = optarg;
            break;
        case 'v':
            server.verbose = 1;
            break;
        case 'h':
            mw_usage(stdout, argv[0]);
            return 0;
        default:
            mw_usage(stderr, argv[0]);
            return 2;
        }
    }
    if (optind < argc) {
        mw_log("unexpected argument \"%s\"", argv[optind]);
        mw_usage(stderr, argv[0]);
        return 2;
    }
    if (once && server.frames_target == 0)
        server.frames_target = 1;
    server.shm.path = (char *)shm_path;
    server.shm.fd = -1;
    server.png_every = (int)png_every;
    if (png_every > 0 && out_dir == NULL) {
        mw_log("--png-every needs --out");
        return 2;
    }
    if (out_dir != NULL) {
        if (mw_prepare_out_dir(out_dir) < 0)
            return 1;
        server.out_dir = strdup(out_dir);
        if (server.out_dir == NULL)
            return 1;
    }
    if (g_dump_dir != NULL) {
        if (mkdir(g_dump_dir, 0777) != 0 && errno != EEXIST) {
            mw_log("--dump %s: cannot create the directory: %s", g_dump_dir, strerror(errno));
            return 1;
        }
        mw_log("dump: writing decoded objects to %s", g_dump_dir);
    }

    g_verbose = server.verbose;
    server.ram_base = ram_base;
    server.crc_stride = (uint32_t)crc_stride;
    if (ram_path != NULL && ram_path[0] != '\0') {
        struct stat st;
        int fd = open(ram_path, O_RDWR);
        int prot = PROT_READ | PROT_WRITE;

        if (fd < 0) {
            fd = open(ram_path, O_RDONLY);
            prot = PROT_READ;
        }
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
            mw_log("--ram %s: cannot open or size the RAM file: %s", ram_path, strerror(errno));
            if (fd >= 0)
                close(fd);
            return 1;
        }
        server.ram_size = (size_t)st.st_size;
        server.ram = mmap(NULL, server.ram_size, prot, MAP_SHARED, fd, 0);
        close(fd);
        if (server.ram == MAP_FAILED) {
            mw_log("--ram %s: mmap failed: %s", ram_path, strerror(errno));
            server.ram = NULL;
            return 1;
        }
        server.ram_writable = (prot & PROT_WRITE) != 0;
        mw_log("ram: %s mapped, %zu bytes, guest physical base %#" PRIx64 " (%s)", ram_path,
               server.ram_size, server.ram_base,
               server.ram_writable ? "read/write: IOSurface write-back is available"
                                   : "read-only: the picture falls back to the host read-back");
    }
    mw_crc32_init();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, mw_on_signal);
    signal(SIGTERM, mw_on_signal);

    if (mw_vulkan_init(&server.vk, (int)gpu_index, pci_address, server.verbose) < 0) {
        mw_server_cleanup(&server);
        return 1;
    }
    if (mw_open_listener(&server) < 0) {
        mw_server_cleanup(&server);
        return 1;
    }
    if (csv_path != NULL && mw_open_csv(&server, csv_path) < 0) {
        mw_server_cleanup(&server);
        return 1;
    }
    mw_log("limits: frames %s, shm %s, csv %s, crc every %u row(s)",
           server.frames_target == 0 ? "unbounded" : "capped", shm_path[0] ? shm_path : "(off)",
           csv_path != NULL ? csv_path : "(off)", server.crc_stride);

    while (!g_stop && !server.done) {
        if (server.client_fd < 0) {
            int waited = mw_wait_for_client(&server);

            if (waited <= 0)
                break;
        }
        mw_serve_client(&server);
    }
    mw_print_summary(&server);
    mw_write_missing_report(&server);
    mw_server_cleanup(&server);
    return 0;
}
