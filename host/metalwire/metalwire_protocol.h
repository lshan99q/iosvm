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
#define MW_VERSION 1u

/* the message header, on the wire as it is in memory (little-endian hosts only; the translator
 * refuses a big-endian one) */
struct mw_msg {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t length;   /* payload bytes that follow */
    uint32_t reserved; /* zero */
};

/* message types: 0x0* control, 0x1* objects, 0x2* drawing, 0x3* frames */
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

    MW_COPY_BUFFER_TO_TEXTURE = 0x0030,
    MW_FILL_BUFFER = 0x0031,

    MW_PRESENT = 0x0040,

    MW_FRAME_BEGIN = 0x0050,
    MW_FRAME_END = 0x0051,
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
    MW_FORMAT_BGRA8_UNORM = 80, /* MTLPixelFormatBGRA8Unorm */
    MW_FORMAT_RGBA8_UNORM = 70, /* MTLPixelFormatRGBA8Unorm */
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

#endif /* METALWIRE_PROTOCOL_H */
