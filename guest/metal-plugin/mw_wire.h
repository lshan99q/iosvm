/* SPDX-License-Identifier: MIT */
/*
 * mw_wire: the guest end of the metalwire socket, used by the Metal device bundle only.
 *
 * The wire contract is host/metalwire/metalwire_protocol.h — this header does not restate the
 * structs, it includes them (build.sh passes -I host/metalwire). One TCP connection, guest is the
 * client, little-endian messages framed by struct mw_msg, one struct mw_ack per MW_FRAME_END.
 *
 * Everything here is synchronous and single-connection: the plug-in serialises a whole frame (its
 * sends and the wait for that frame's acknowledgement) under one lock, so v1 never pipelines.
 *
 * The acknowledgement is read tolerantly: the protocol note says every message is a mw_msg frame,
 * while struct mw_ack carries its own magic. The host may send either a raw struct mw_ack or a
 * mw_msg frame whose payload is one; mw_recv_ack accepts both (see the comment there). That is a
 * deliberate hedge: the wire header was written before either end existed.
 */
#ifndef METALWIRE_GUEST_WIRE_H
#define METALWIRE_GUEST_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "metalwire_protocol.h"

/* Where the host translator listens by default: QEMU's user-mode network maps 10.0.2.2 to the
 * host's loopback. The environment overrides are for runbook variations only. */
#define MW_DEFAULT_HOST "10.0.2.2"
#define MW_DEFAULT_PORT 9111
#define MW_DEFAULT_CONNECT_MS 2000
#define MW_DEFAULT_ACK_MS 15000
#define MW_RETRY_INTERVAL_NS (5ull * 1000 * 1000 * 1000)

struct mw_conn {
    int fd;       /* -1 until connected */
    int disabled; /* METALWIRE_OFF=1: no socket is ever made and every frame is a no-op */
    uint64_t
        next_retry_ns; /* do not try again before this, so a missing host costs no frame time */
    char host[64];
    int port;
    int connect_ms;
    int ack_ms;
    char target[96];      /* "host:port", for the log */
    char last_error[128]; /* why the last connect attempt/read failed */
    unsigned attempts;    /* how many attempts failed since the last success */
};

/* Reads the environment and fills mw_conn. Does not touch the network. Call once per process. */
void mw_conn_init(struct mw_conn *conn);

/* Connects if there is no connection and the retry interval has passed. Returns the socket, or -1
 * when the host is unavailable (the caller turns that into a no-op frame). Never blocks longer than
 * connect_ms. */
int mw_conn_ensure(struct mw_conn *conn, uint64_t now_ns);

/* Closes the socket, if any, and says why in the log. Idempotent. */
void mw_conn_close(struct mw_conn *conn, const char *why);

/* Sends one mw_msg-framed message; returns 0 or -1 (on failure the socket is closed). */
int mw_send(struct mw_conn *conn, uint16_t type, const void *payload, uint32_t length);

/* A message whose length counts the structure AND the body that follows it (MW_UPLOAD_BUFFER's
 * bytes). The host reads one length before it looks for the next header, so the body lives inside
 * this message; a body sent after a short header desynchronises the stream (the first guest boot's
 * trans1 run found exactly that, with the host closing the connection on a short upload). */
int mw_send_with_body(struct mw_conn *conn, uint16_t type, const void *payload, uint32_t length,
                      const void *body, uint64_t body_length);

/* Sends MW_HELLO. The program name is cut to the 32 bytes the struct has room for. */
int mw_send_hello(struct mw_conn *conn, uint32_t pid, const char *program);

/* Waits up to conn->ack_ms for one acknowledgement. Returns 0 and fills ack, or -1 (socket
 * closed); the caller reports the frame as failed. */
int mw_recv_ack(struct mw_conn *conn, struct mw_ack *ack);

/* CLOCK_MONOTONIC-equivalent nanoseconds (mach_absolute_time converted). */
uint64_t mw_now_ns(void);

/* CRC-32, zlib polynomial, init 0xFFFFFFFF, final XOR 0xFFFFFFFF — the protocol's CRC. */
uint32_t mw_crc32(const void *data, size_t length);

/* The same over a row-major rectangle inside a buffer: rows consecutive only in meaning, each
 * bytes_per_row apart, row_bytes copied per row. This is the bytes a Metal blit of that rectangle
 * reads, and the bytes the host's read-back is compared against. Rows that would run past length
 * are left out (and that is logged as a short read). */
uint32_t mw_crc32_rows(const void *base, uint64_t length, uint32_t bytes_per_row,
                       uint32_t row_bytes, uint32_t rows);
uint32_t mw_crc32_rows_stride(const void *base, uint64_t length, uint32_t bytes_per_row,
                              uint32_t row_bytes, uint32_t rows, uint32_t stride);

#endif /* METALWIRE_GUEST_WIRE_H */
