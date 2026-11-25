/* SPDX-License-Identifier: MIT */
/*
 * mw_wire: the guest end of the metalwire socket (see mw_wire.h and the shared contract in
 * host/metalwire/metalwire_protocol.h).
 *
 * The socket is made only when a frame needs it, so a device Metal instantiates costs nothing and a
 * missing host cannot delay a boot: a connect attempt is bounded by METALWIRE_CONNECT_MS (2 s by
 * default) and is repeated at most every 5 s. Every operation here logs one line through the
 * "metalwire:" prefix the serial console is read for.
 */
#include "mw_wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---- log ---- */

static void mw_wire_log(const char *fmt, ...)
{
    va_list ap;
    char text[512];

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    fprintf(stderr, "metalwire: %s\n", text);
    fflush(stderr);
}

/* ---- time ---- */

static mach_timebase_info_data_t g_timebase;
static pthread_once_t g_timebase_once = PTHREAD_ONCE_INIT;

static void mw_timebase_init(void)
{
    if (mach_timebase_info(&g_timebase) != 0 || g_timebase.denom == 0) {
        g_timebase.numer = 1;
        g_timebase.denom = 1;
    }
}

uint64_t mw_now_ns(void)
{
    pthread_once(&g_timebase_once, mw_timebase_init);
    return (uint64_t)((__uint128_t)mach_absolute_time() * g_timebase.numer / g_timebase.denom);
}

/* ---- CRC-32 (zlib) ---- */

static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;

/*
 * CRC-32 (the zlib polynomial), slice by 8.
 *
 * The first working frame path computed this a byte at a time: 4.87 MB a frame is 4.87 million
 * table lookups, and under TCG that measured 48 ms a frame -- more than the rest of the translation
 * put together (stage 29 T3). Eight bytes at a time keeps the same CRC (it is the same standard
 * CRC-32, so the host's own implementation still agrees) with an eighth of the loop.
 */
static uint32_t g_crc_table8[8][256];

static void crc_table_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;

        for (int k = 0; k < 8; k++) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        g_crc_table8[0][n] = c;
    }
    for (uint32_t n = 0; n < 256; n++) {
        for (int slice = 1; slice < 8; slice++) {
            uint32_t previous = g_crc_table8[slice - 1][n];

            g_crc_table8[slice][n] = g_crc_table8[0][previous & 0xffu] ^ (previous >> 8);
        }
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length >= 8) {
        uint32_t word;

        memcpy(&word, data, sizeof(word)); /* arm64 little-endian; the wire's CRC-32 too */
        crc ^= word;
        crc = g_crc_table8[7][crc & 0xffu] ^ g_crc_table8[6][(crc >> 8) & 0xffu] ^
              g_crc_table8[5][(crc >> 16) & 0xffu] ^ g_crc_table8[4][(crc >> 24) & 0xffu] ^
              g_crc_table8[3][data[4]] ^ g_crc_table8[2][data[5]] ^ g_crc_table8[1][data[6]] ^
              g_crc_table8[0][data[7]];
        data += 8;
        length -= 8;
    }
    for (size_t i = 0; i < length; i++) {
        crc = g_crc_table8[0][(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t mw_crc32(const void *data, size_t length)
{
    pthread_once(&g_crc_once, crc_table_init);
    return crc_update(0xFFFFFFFFu, (const uint8_t *)data, length) ^ 0xFFFFFFFFu;
}

uint32_t mw_crc32_rows_stride(const void *base, uint64_t length, uint32_t bytes_per_row,
                              uint32_t row_bytes, uint32_t rows, uint32_t stride)
{
    const uint8_t *bytes = (const uint8_t *)base;
    uint32_t crc = 0xFFFFFFFFu;

    pthread_once(&g_crc_once, crc_table_init);
    if (bytes == NULL || bytes_per_row == 0) {
        return 0;
    }
    if (stride == 0) {
        stride = 1;
    }
    for (uint32_t row = 0; row < rows; row += stride) {
        uint64_t at = (uint64_t)row * bytes_per_row;

        if (at + row_bytes > length) {
            mw_wire_log("crc: row %u of %u runs past the buffer (%llu of %llu bytes); stopped", row,
                        rows, (unsigned long long)(at + row_bytes), (unsigned long long)length);
            break;
        }
        crc = crc_update(crc, bytes + at, row_bytes);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* The whole picture: every row (the default). */
uint32_t mw_crc32_rows(const void *base, uint64_t length, uint32_t bytes_per_row,
                       uint32_t row_bytes, uint32_t rows)
{
    return mw_crc32_rows_stride(base, length, bytes_per_row, row_bytes, rows, 1);
}

/* ---- the socket ---- */

static int mw_send_raw(struct mw_conn *conn, const void *data, size_t length)
{
    const uint8_t *at = (const uint8_t *)data;

    while (length > 0) {
        ssize_t sent = send(conn->fd, at, length, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(conn->last_error, sizeof(conn->last_error), "send: %s", strerror(errno));
            return -1;
        }
        if (sent == 0) {
            snprintf(conn->last_error, sizeof(conn->last_error),
                     "send: the host closed the connection");
            return -1;
        }
        at += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int mw_read_exact(struct mw_conn *conn, void *buffer, size_t length)
{
    uint8_t *at = (uint8_t *)buffer;

    while (length > 0) {
        struct pollfd pfd = {.fd = conn->fd, .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, conn->ack_ms);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(conn->last_error, sizeof(conn->last_error), "poll: %s", strerror(errno));
            return -1;
        }
        if (ready == 0) {
            snprintf(conn->last_error, sizeof(conn->last_error), "no acknowledgement in %d ms",
                     conn->ack_ms);
            return -1;
        }
        ssize_t got = recv(conn->fd, at, length, 0);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(conn->last_error, sizeof(conn->last_error), "recv: %s", strerror(errno));
            return -1;
        }
        if (got == 0) {
            snprintf(conn->last_error, sizeof(conn->last_error), "the host closed the connection");
            return -1;
        }
        at += got;
        length -= (size_t)got;
    }
    return 0;
}

static int mw_tcp_connect(struct mw_conn *conn)
{
    struct sockaddr_in address;
    struct pollfd pfd;
    socklen_t error_length = sizeof(int);
    struct timeval timeout;
    int one = 1;
    int fd;
    int flags;
    int error = 0;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)conn->port);
    if (inet_pton(AF_INET, conn->host, &address.sin_addr) != 1) {
        snprintf(conn->last_error, sizeof(conn->last_error), "%s is not an IPv4 address",
                 conn->host);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(conn->last_error, sizeof(conn->last_error), "socket: %s", strerror(errno));
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        snprintf(conn->last_error, sizeof(conn->last_error), "fcntl: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS) {
            snprintf(conn->last_error, sizeof(conn->last_error), "connect: %s", strerror(errno));
            close(fd);
            return -1;
        }
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int ready = poll(&pfd, 1, conn->connect_ms);

        if (ready < 0) {
            snprintf(conn->last_error, sizeof(conn->last_error), "connect poll: %s",
                     strerror(errno));
            close(fd);
            return -1;
        }
        if (ready == 0) {
            snprintf(conn->last_error, sizeof(conn->last_error), "connect timed out after %d ms",
                     conn->connect_ms);
            close(fd);
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 || error != 0) {
            snprintf(conn->last_error, sizeof(conn->last_error), "connect: %s",
                     strerror(error != 0 ? error : errno));
            close(fd);
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, flags) != 0) {
        snprintf(conn->last_error, sizeof(conn->last_error), "fcntl: %s", strerror(errno));
        close(fd);
        return -1;
    }
    /* a stalled host must not hang the guest forever either */
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

void mw_conn_init(struct mw_conn *conn)
{
    const char *host = getenv("METALWIRE_HOST");
    const char *port = getenv("METALWIRE_PORT");
    const char *off = getenv("METALWIRE_OFF");
    const char *connect_ms = getenv("METALWIRE_CONNECT_MS");
    const char *ack_ms = getenv("METALWIRE_ACK_MS");

    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->disabled = off != NULL && off[0] != '\0' && strcmp(off, "0") != 0;
    snprintf(conn->host, sizeof(conn->host), "%s",
             host != NULL && host[0] != '\0' ? host : MW_DEFAULT_HOST);
    conn->port = port != NULL ? atoi(port) : MW_DEFAULT_PORT;
    if (conn->port <= 0 || conn->port > 65535) {
        conn->port = MW_DEFAULT_PORT;
    }
    conn->connect_ms = connect_ms != NULL ? atoi(connect_ms) : MW_DEFAULT_CONNECT_MS;
    if (conn->connect_ms < 100) {
        conn->connect_ms = 100;
    }
    if (conn->connect_ms > 10000) {
        conn->connect_ms = 10000;
    }
    conn->ack_ms = ack_ms != NULL ? atoi(ack_ms) : MW_DEFAULT_ACK_MS;
    if (conn->ack_ms < 100) {
        conn->ack_ms = 100;
    }
    if (conn->ack_ms > 120000) {
        conn->ack_ms = 120000;
    }
    snprintf(conn->target, sizeof(conn->target), "%s:%d", conn->host, conn->port);
    mw_wire_log("wire: %s (connect timeout %d ms, ack timeout %d ms)%s", conn->target,
                conn->connect_ms, conn->ack_ms,
                conn->disabled ? "; METALWIRE_OFF disables the wire, every frame is a no-op" : "");
}

int mw_conn_ensure(struct mw_conn *conn, uint64_t now_ns)
{
    int fd;

    if (conn->disabled) {
        return -1;
    }
    if (conn->fd >= 0) {
        return conn->fd;
    }
    if (now_ns < conn->next_retry_ns) {
        return -1;
    }
    conn->next_retry_ns = now_ns + MW_RETRY_INTERVAL_NS;
    fd = mw_tcp_connect(conn);
    if (fd < 0) {
        conn->attempts++;
        if (conn->attempts <= 2 || conn->attempts % 10 == 0) {
            mw_wire_log("%s is not reachable (%s); frames are no-ops, retrying every 5 s",
                        conn->target, conn->last_error);
        }
        return -1;
    }
    conn->fd = fd;
    conn->attempts = 0;
    conn->last_error[0] = '\0';
    mw_wire_log("connected to %s", conn->target);
    return fd;
}

void mw_conn_close(struct mw_conn *conn, const char *why)
{
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
        mw_wire_log("connection to %s closed (%s)", conn->target, why != NULL ? why : "no reason");
    }
}

/* ---- messages ---- */

int mw_send(struct mw_conn *conn, uint16_t type, const void *payload, uint32_t length)
{
    struct mw_msg header;

    if (conn->fd < 0) {
        return -1;
    }
    header.magic = MW_MAGIC;
    header.version = MW_VERSION;
    header.type = type;
    header.length = length;
    header.reserved = 0;
    if (mw_send_raw(conn, &header, sizeof(header)) != 0 ||
        (length != 0 && payload != NULL && mw_send_raw(conn, payload, length) != 0)) {
        mw_conn_close(conn, conn->last_error);
        return -1;
    }
    return 0;
}

int mw_send_with_body(struct mw_conn *conn, uint16_t type, const void *payload, uint32_t length,
                      const void *body, uint64_t body_length)
{
    struct mw_msg header;

    if (conn->fd < 0) {
        return -1;
    }
    /*
     * One message, one length: the header's length counts the structure AND the body, because the
     * host reads exactly that many bytes before it looks at the next header (a body sent after a
     * short header desynchronises the stream -- that was the first boot's bug).
     */
    if (body_length > UINT32_MAX - length) {
        snprintf(conn->last_error, sizeof(conn->last_error), "body too long");
        return -1;
    }
    header.magic = MW_MAGIC;
    header.version = MW_VERSION;
    header.type = type;
    header.length = length + (uint32_t)body_length;
    header.reserved = 0;
    if (mw_send_raw(conn, &header, sizeof(header)) != 0 ||
        (length != 0 && payload != NULL && mw_send_raw(conn, payload, length) != 0) ||
        (body_length != 0 && body != NULL && mw_send_raw(conn, body, (size_t)body_length) != 0)) {
        mw_conn_close(conn, conn->last_error);
        return -1;
    }
    return 0;
}

int mw_send_hello(struct mw_conn *conn, uint32_t pid, const char *program)
{
    struct mw_hello hello;

    memset(&hello, 0, sizeof(hello));
    hello.version = MW_VERSION;
    hello.pid = pid;
    if (program != NULL) {
        strncpy(hello.program, program, sizeof(hello.program) - 1);
    }
    return mw_send(conn, MW_HELLO, &hello, sizeof(hello));
}

int mw_recv_ack(struct mw_conn *conn, struct mw_ack *ack)
{
    union {
        struct mw_msg msg;
        uint32_t magic;
        uint8_t bytes[sizeof(struct mw_msg)];
    } head;
    struct mw_msg frame;
    uint32_t magic;

    if (conn->fd < 0) {
        return -1;
    }
    if (mw_read_exact(conn, &head, 4) != 0) {
        goto fail;
    }
    memcpy(&magic, &head, sizeof(magic));
    if (magic == MW_ACK_MAGIC) {
        /* a raw struct mw_ack, which is what the ack's own magic and the protocol note suggest */
        memcpy(ack, &head, sizeof(magic));
        if (mw_read_exact(conn, (uint8_t *)ack + sizeof(magic), sizeof(*ack) - sizeof(magic)) !=
            0) {
            goto fail;
        }
    } else if (magic == MW_MAGIC) {
        /* a mw_msg frame carrying the ack; accepted so either end's reading of the protocol works
         */
        if (mw_read_exact(conn, head.bytes + 4, sizeof(struct mw_msg) - 4) != 0) {
            goto fail;
        }
        frame = head.msg;
        if (frame.version != MW_VERSION) {
            snprintf(conn->last_error, sizeof(conn->last_error),
                     "ack frame version %u (expected %u)", frame.version, MW_VERSION);
            goto fail;
        }
        if (frame.length < sizeof(*ack)) {
            uint8_t drain[64];

            for (uint32_t left = frame.length; left > 0;) {
                uint32_t chunk = left > sizeof(drain) ? sizeof(drain) : left;

                if (mw_read_exact(conn, drain, chunk) != 0) {
                    goto fail;
                }
                left -= chunk;
            }
            snprintf(conn->last_error, sizeof(conn->last_error),
                     "ack frame of %u bytes (expected %zu)", frame.length, sizeof(*ack));
            goto fail;
        }
        if (mw_read_exact(conn, ack, sizeof(*ack)) != 0) {
            goto fail;
        }
        for (uint32_t left = frame.length - (uint32_t)sizeof(*ack); left > 0;) {
            uint8_t drain[64];
            uint32_t chunk = left > sizeof(drain) ? sizeof(drain) : left;

            if (mw_read_exact(conn, drain, chunk) != 0) {
                goto fail;
            }
            left -= chunk;
        }
    } else {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "the host sent 0x%08x where an ack (0x%08x) or a frame (0x%08x) belongs", magic,
                 MW_ACK_MAGIC, MW_MAGIC);
        goto fail;
    }
    if (ack->magic != MW_ACK_MAGIC) {
        snprintf(conn->last_error, sizeof(conn->last_error), "ack magic 0x%08x (expected 0x%08x)",
                 ack->magic, MW_ACK_MAGIC);
        goto fail;
    }
    if (ack->version != MW_VERSION) {
        snprintf(conn->last_error, sizeof(conn->last_error), "ack version %u (expected %u)",
                 ack->version, MW_VERSION);
        goto fail;
    }
    return 0;

fail:
    mw_conn_close(conn, conn->last_error);
    return -1;
}
