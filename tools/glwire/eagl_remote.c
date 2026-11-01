/* eagl_remote: carry QuartzCore's GL calls to the host's GPU.
 *
 * The interface is drawn by AArch64 instructions under TCG and costs about 700 ms a frame; the same
 * frame costs the host's GPU 6-14 ms (tools/host-glquad). This module is the guest half: it records
 * the calls QuartzCore makes into a buffer, hands the buffer to the host once a frame, and takes the
 * picture back into the memory the display reads.
 *
 * Nothing here decides *when* a frame ends -- eagl.c knows that (frame_end()), and calls
 * eagl_remote_flush() from there.
 *
 * The wire format and the recorders are generated from tools/glwire/spec.def, so the two halves
 * cannot drift apart.
 */
#define GLWIRE_GUEST
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "wire.h"

#define REMOTE_MAGIC_RECORDS 0x52454d4f54453130ULL   /* "REMOTE10" */
#define REMOTE_MAGIC_FRAME   0x52454d4f54453230ULL
/* A frame's texture upload is the size of the panel (10.96 MB), and a page can upload more than one
 * layer; 4 MiB silently dropped the rest of the frame, so the host was sent a truncated record. */
#define REMOTE_ARENA_BYTES   (32u << 20)

static unsigned char remote_arena[REMOTE_ARENA_BYTES] __attribute__((aligned(4096)));
static wire_buffer w;  /* the generated recorders append here */
static unsigned char *remote_frame;             /* where the host writes the picture */
static size_t remote_frame_bytes;
static int remote_fd = -1;
static int remote_enabled;
static int remote_frames;
static long remote_total_us;
/*
 * Every process that loads this library has its own arena and its own picture, and the host finds
 * them by these words in the RAM file. One fixed pair would make every process's buffer look alike,
 * and the host would always read the first one it found; the process id makes each pair unique.
 */
static uint64_t remote_records_magic = REMOTE_MAGIC_RECORDS;
static uint64_t remote_frame_magic = REMOTE_MAGIC_FRAME;

/* The vertex arrays of the attributes that are on, as the guest last gave them to glVertexAttribPointer. */
static struct {
	const void *pointer;
	GLint size;
	GLenum type;
	GLsizei stride;
	GLboolean normalized;
} remote_attribs[16];
static int remote_attrib_enabled[16];


int remote_host_port = 9110;
char remote_host_address[64] = "10.0.2.2";

static double
remote_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* The picture the host draws lands directly after the frame's magic word, in guest memory the host
 * has mapped as the RAM file. The guest only has to read it. */
/* Where the host is, and whether to use it at all: the file exists only when the boot was set up
 * for it, so a machine without it behaves exactly as it did before. */
void
eagl_remote_begin(void)
{
	FILE *file;
	char host[64] = "10.0.2.2";
	int port = 9110;

	if (remote_enabled) {
		return;
	}
	/* the switch is a file's existence: a boot that ships /redwood/bin/glremote_on wants the host */
	if (access("/redwood/bin/glremote_on", F_OK) != 0) {
		return;
	}
	{
		uint32_t tag = (uint32_t)getpid() & 0xffffffu;

		remote_records_magic = 0x52454d4f00000000ULL | tag;
		remote_frame_magic = 0x52454d4f01000000ULL | tag;
	}
	file = fopen("/redwood/etc/glremote", "r");
	if (file != NULL) {
		if (fscanf(file, "%63s %d", host, &port) != 2) {
			fclose(file);
			return;
		}
		fclose(file);
		strncpy(remote_host_address, host, sizeof(remote_host_address) - 1);
		remote_host_port = port;
	}
	remote_enabled = 1;
	wire_reset(&w, remote_arena + 8, REMOTE_ARENA_BYTES - 8);
	memcpy(remote_arena, &remote_records_magic, 8);
}

void
eagl_remote_resize(size_t bytes)
{
	if (remote_frame_bytes >= bytes) {
		return;
	}
	free(remote_frame);
	remote_frame = malloc(bytes + 8);
	if (remote_frame == NULL) {
		remote_frame_bytes = 0;
		return;
	}
	/* A new allocation gets a new word: the host may still have the old buffer's address cached,
	 * and its magic may still be in the freed pages. */
	remote_frame_magic += 1ULL << 24;
	memcpy(remote_frame, &remote_frame_magic, 8);
	remote_frame_bytes = bytes;
}

int
eagl_remote_is_on(void)
{
	return remote_enabled;
}

size_t
eagl_remote_pending(void)
{
	return w.at;
}

/* Hand the frame to the host and wait for it to say the picture is there. The host replays the
 * records on its own GL context and writes the pixels into our frame buffer, which it finds by the
 * magic in front of it -- so no address travels over the wire. */
int
eagl_remote_flush(int width, int height)
{
	struct sockaddr_in address;
	char request[96], reply[64];
	double started;
	int at = 0;

	if (!remote_enabled || w.at == 0) {
		return 0;
	}
	eagl_remote_resize((size_t)width * height * 4);
	if (remote_frame == NULL) {
		return 0;
	}
	if (remote_fd < 0) {
		remote_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (remote_fd < 0) {
			return 0;
		}
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons((unsigned short)remote_host_port);
		inet_pton(AF_INET, remote_host_address, &address.sin_addr);
		if (connect(remote_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
			close(remote_fd);
			remote_fd = -1;
			return 0;
		}
		setsockopt(remote_fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
	}
	started = remote_now_ms();
	snprintf(request, sizeof(request), "R %llu %llu %d %d %zu %zu\n",
	    (unsigned long long)remote_records_magic, (unsigned long long)remote_frame_magic,
	    width, height, w.at, (size_t)width * height * 4);
	if (send(remote_fd, request, strlen(request), 0) != (ssize_t)strlen(request) ||
	    send(remote_fd, w.base, w.at, 0) != (ssize_t)w.at) {
		close(remote_fd);
		remote_fd = -1;
		return 0;
	}
	while (at < (int)sizeof(reply) - 2 && recv(remote_fd, &reply[at], 1, 0) == 1 && reply[at] != '\n') {
		at++;
	}
	reply[at] = '\0';
	remote_total_us += (long)((remote_now_ms() - started) * 1000.0);
	remote_frames++;
	wire_reset(&w, remote_arena + 8, REMOTE_ARENA_BYTES - 8);
	return reply[0] == 'R' ? 1 : 0;
}

/* The picture the host drew, for whoever presents it. */
const void *
eagl_remote_frame(void)
{
	return remote_frame != NULL ? remote_frame + 8 : NULL;
}

int
eagl_remote_stats(long *frames, long *average_us)
{
	*frames = remote_frames;
	*average_us = remote_frames > 0 ? remote_total_us / remote_frames : 0;
	return remote_enabled;
}

/*
 * Vertex arrays. The GL signature of glVertexAttribPointer has no size, and the array is only read
 * when a draw is submitted -- so the pointer alone travels with that call, and the bytes travel with
 * the draw, when the count (or the largest index) says how many of them there are.
 */
static size_t
remote_max_index(const void *indices, GLsizei count, GLenum type)
{
	size_t max = 0;

	if (indices == NULL || count <= 0) {
		return 0;
	}
	for (GLsizei i = 0; i < count; i++) {
		size_t value;

		switch (type) {
		case GL_UNSIGNED_BYTE:
			value = ((const uint8_t *)indices)[i];
			break;
		case GL_UNSIGNED_SHORT:
			value = ((const uint16_t *)indices)[i];
			break;
		case GL_UNSIGNED_INT:
			value = ((const uint32_t *)indices)[i];
			break;
		default:
			value = (size_t)i;
			break;
		}
		if (value + 1 > max) {
			max = value + 1;
		}
	}
	return max;
}

static void
remote_emit_vertex_data(GLint first, GLsizei count, const void *indices, GLenum type)
{
	size_t elements;

	if (!remote_enabled) {
		return;
	}
	if (indices != NULL) {
		elements = remote_max_index(indices, count, type);
	} else {
		elements = count > 0 ? (size_t)first + (size_t)count : 0;
	}
	if (elements == 0) {
		return;
	}
	for (unsigned index = 0; index < 16; index++) {
		GLsizei stride;
		size_t bytes;

		if (!remote_attrib_enabled[index] || remote_attribs[index].pointer == NULL) {
			continue;
		}
		stride = remote_attribs[index].stride;
		if (stride <= 0) {
			stride = remote_attribs[index].size * wire_type_size(remote_attribs[index].type);
		}
		bytes = elements * (size_t)stride;
		if (bytes == 0 || bytes > (8u << 20)) {
			continue;
		}
		wire_open(&w, 200);
		wire_u32(&w, index);
		wire_i32(&w, remote_attribs[index].size);
		wire_u32(&w, remote_attribs[index].type);
		wire_u8(&w, remote_attribs[index].normalized);
		wire_i32(&w, remote_attribs[index].stride);
		wire_bytes(&w, remote_attribs[index].pointer, (GLsizei)bytes);
		wire_close(&w);
	}
}

static void
remote_note_vertex_pointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void *pointer)
{
	if (index < 16) {
		remote_attribs[index].pointer = pointer;
		remote_attribs[index].size = size;
		remote_attribs[index].type = type;
		remote_attribs[index].stride = stride;
		remote_attribs[index].normalized = normalized;
	}
}

static void
remote_note_attrib_enabled(GLuint index, int enabled)
{
	if (index < 16) {
		remote_attrib_enabled[index] = enabled;
	}
}

/* glPixelStorei's unpack alignment decides how many bytes the last row of a texture takes. */
static void
remote_note_pixel_store(GLenum pname, GLint value)
{
	if (pname == GL_UNPACK_ALIGNMENT && value > 0) {
		wire_unpack_alignment = value;
	}
}

#include "encode.inc"
#include "remote_table.inc"
#include "remote.inc"


/* Called once the EAGL table has been filled: when the host is in use, the calls QuartzCore makes
 * are recorded instead of run here. */
void
eagl_remote_install(void **table)
{
	eagl_remote_begin();
	if (!remote_enabled) {
		return;
	}
	eagl_fill_table_remote(table);
}
