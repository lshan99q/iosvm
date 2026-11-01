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
#define REMOTE_ARENA_BYTES   (4u << 20)

static unsigned char remote_arena[REMOTE_ARENA_BYTES] __attribute__((aligned(4096)));
static wire_buffer w;  /* the generated recorders append here */
static unsigned char *remote_frame;             /* where the host writes the picture */
static size_t remote_frame_bytes;
static int remote_fd = -1;
static int remote_enabled;
static int remote_frames;
static long remote_total_us;

extern int remote_host_port;
extern char remote_host_address[64];

static double
remote_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* The picture the host draws lands directly after the frame's magic word, in guest memory the host
 * has mapped as the RAM file. The guest only has to read it. */
void
eagl_remote_begin(void)
{
	remote_enabled = 1;
	wire_reset(&w, remote_arena + 8, REMOTE_ARENA_BYTES - 8);
	memcpy(remote_arena, &(unsigned long long){REMOTE_MAGIC_RECORDS}, 8);
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
	memcpy(remote_frame, &(unsigned long long){REMOTE_MAGIC_FRAME}, 8);
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
	    (unsigned long long)REMOTE_MAGIC_RECORDS, (unsigned long long)REMOTE_MAGIC_FRAME,
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

#include "encode.inc"
