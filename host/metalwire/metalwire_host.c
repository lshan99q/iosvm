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
    VkImageLayout layout;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *readback_mapped;
    int dst_this_frame;
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
 * (RGB) and glwire's shared file (BGRA, webview.py decodes BGRA). */
static inline int mw_format_is_bgra(uint32_t format)
{
    return format == MW_FORMAT_BGRA8_UNORM;
}

static void mw_pixels_to_rgb(const unsigned char *pixels, uint32_t width, uint32_t height,
                             uint32_t format, unsigned char *rgb)
{
    size_t count = (size_t)width * height;

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

static void mw_pixels_to_bgra(const unsigned char *pixels, size_t bytes, uint32_t format,
                              unsigned char *bgra)
{
    if (mw_format_is_bgra(format)) {
        memcpy(bgra, pixels, bytes);
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

static VkFormat mw_vk_format(uint32_t format)
{
    return format == MW_FORMAT_RGBA8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
}

static int mw_vulkan_init(struct mw_vulkan *vk, int gpu_index, int verbose)
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
    app.apiVersion = VK_API_VERSION_1_1;
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
    if (gpu_index < 0 || (uint32_t)gpu_index >= count) {
        mw_log("vulkan: physical device %d requested, only %u present", gpu_index, count);
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
    if (image->image != VK_NULL_HANDLE)
        vkDestroyImage(s->vk.device, image->image, NULL);
    if (image->memory != VK_NULL_HANDLE)
        vkFreeMemory(s->vk.device, image->memory, NULL);
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
    if (mw_create_host_buffer(&s->vk, request->length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
    if (request->pixel_format != MW_FORMAT_BGRA8_UNORM &&
        request->pixel_format != MW_FORMAT_RGBA8_UNORM) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "texture %u: unknown pixel format %u", request->id,
                      request->pixel_format);
        return -1;
    }
    format = mw_vk_format(request->pixel_format);
    vkGetPhysicalDeviceFormatProperties(s->vk.physical, format, &format_props);
    if ((format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0 ||
        (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "texture %u: device cannot transfer %s",
                      request->id,
                      request->pixel_format == MW_FORMAT_BGRA8_UNORM ? "BGRA8" : "RGBA8");
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
        uint64_t bytes = (uint64_t)request->width * request->height * MW_PIXEL_BYTES;

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
    if (request->width == 0 || request->height == 0 ||
        request->bytes_per_row % MW_PIXEL_BYTES != 0) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy has width %u height %u row bytes %u",
                      request->width, request->height, request->bytes_per_row);
        return -1;
    }
    if (request->bytes_per_row < request->width * MW_PIXEL_BYTES ||
        (uint64_t)request->origin_x + request->width > image->width ||
        (uint64_t)request->origin_y + request->height > image->height) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy rectangle out of the texture");
        return -1;
    }
    if (request->buffer_offset % MW_PIXEL_BYTES != 0 ||
        request->buffer_offset + (uint64_t)(request->height - 1) * request->bytes_per_row +
                (uint64_t)request->width * MW_PIXEL_BYTES >
            buffer->length) {
        mw_frame_fail(s, MW_STATUS_BAD_COMMAND, "copy reads outside buffer %u", request->buffer_id);
        return -1;
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

/* ---- protocol: frame execution -------------------------------------------------------------- */

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
        region.bufferRowLength = copy->bytes_per_row / MW_PIXEL_BYTES;
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

    result->crc =
        mw_crc32_rows_zlib((const unsigned char *)present->readback_mapped, present->width * 4,
                           present->width * 4, present->height, s->crc_stride);
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
        ack.version = MW_VERSION;
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

    memset(&timing, 0, sizeof(timing));
    timing.recv_ns = t_end > frame->begin_ns ? t_end - frame->begin_ns : 0;
    status = mw_run_frame(s, frame, &result);
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
    ack.version = MW_VERSION;
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
        if (header.magic != MW_MAGIC || header.version != MW_VERSION) {
            mw_log("protocol: bad header (magic 0x%08x version %u); closing the connection",
                   header.magic, header.version);
            break;
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
    free(s->images);
    free(s->buffers);
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
        {"byte-check", no_argument, NULL, 1001},    {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},           {NULL, 0, NULL, 0},
    };
    const char *env_gpu = getenv("METALWIRE_GPU");
    const char *env_shm = getenv("METALWIRE_SHM");
    const char *csv_path = NULL;
    const char *out_dir = NULL;
    const char *ram_path = getenv("METALWIRE_RAM");
    const char *env_crc_stride = getenv("METALWIRE_CRC_STRIDE");
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

    g_verbose = server.verbose;
    server.ram_base = ram_base;
    server.crc_stride = (uint32_t)crc_stride;
    if (ram_path != NULL && ram_path[0] != '\0') {
        struct stat st;
        int fd = open(ram_path, O_RDONLY);

        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
            mw_log("--ram %s: cannot open or size the RAM file: %s", ram_path, strerror(errno));
            if (fd >= 0)
                close(fd);
            return 1;
        }
        server.ram_size = (size_t)st.st_size;
        server.ram = mmap(NULL, server.ram_size, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (server.ram == MAP_FAILED) {
            mw_log("--ram %s: mmap failed: %s", ram_path, strerror(errno));
            server.ram = NULL;
            return 1;
        }
        mw_log("ram: %s mapped, %zu bytes, guest physical base %#" PRIx64, ram_path,
               server.ram_size, server.ram_base);
    }
    mw_crc32_init();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, mw_on_signal);
    signal(SIGTERM, mw_on_signal);

    if (mw_vulkan_init(&server.vk, (int)gpu_index, server.verbose) < 0) {
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
    mw_server_cleanup(&server);
    return 0;
}
