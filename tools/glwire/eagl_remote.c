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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "wire.h"

/* A frame's texture upload is the size of the panel (10.96 MB), and a page can upload more than one
 * layer; 4 MiB silently dropped the rest of the frame, so the host was sent a truncated record. */
#define REMOTE_ARENA_BYTES   (64u << 20)

static unsigned char remote_arena[REMOTE_ARENA_BYTES] __attribute__((aligned(4096)));
static wire_buffer w;  /* the generated recorders append here */
static unsigned char *remote_frame;             /* where the host writes the picture */
static size_t remote_frame_bytes;
static int remote_fd = -1;
static int remote_enabled;
static int remote_frames;
static long remote_total_us;
/* The picture the host draws: instead of sending it back, the host writes it into this buffer at
 * the physical addresses below. 512 segments covers a 4.8 MB picture even at one per page. */
static wire_segment remote_frame_segments[2048];
static int remote_frame_segments_count;

/*
 * W3: where the picture goes. The display reads an IOSurface, whose rows are usually wider than the
 * picture (3072 bytes a row for a 3000-byte picture here), and this side copies the picture into it
 * every frame -- 48 ms of a 77 ms frame. The surface's memory can be named the same way the packed
 * buffer's is, so the host writes the rows where the display already looks and the copy goes away.
 * The packed buffer stays as the fallback: a surface that is not fully resident (the compressor is
 * allowed to move it) cannot be written through.
 */
static wire_segment remote_surface_segments[2048];
static int remote_surface_segments_count;
static GLsizei remote_surface_row;              /* the surface's bytes a row */

/* Which of the two the frame's request names, and how many bytes it covers. */
static wire_segment *remote_picture_segments;
static int remote_picture_segments_count;
static GLsizei remote_picture_row;
static size_t remote_picture_bytes;

/* The calls spec.def marks `!local` are the ones the host draws the frame with: a texture upload or
 * a draw in Mesa's software rasteriser is the whole cost of a frame, and the host already has those
 * pixels by reference. Non-zero = do not run them here as well (the generated trampolines ask this).
 * Off when the inline wire is in use -- nothing would draw the frame then -- and off when
 * /redwood/etc/glwire_local exists or a glwire_local=1 boot-arg says so, which is the A/B for it. */
int remote_only;

/* The A/B between "the 62 slots are replaced" and "the frame goes on the wire": with
 * /redwood/etc/glwire_noflush or boot-arg glwire_noflush=1 the recorders are installed and every
 * call still records, but eagl_remote_flush() drops the frame and returns 0 at once -- no socket,
 * no address translation, no picture copy. If backboardd still dies, the slots are the problem. */
static int remote_no_flush;

#ifndef SYS_hakios_vtop
#define SYS_hakios_vtop 556
#endif
extern int syscall(int, ...);

/* The kernel's user address -> physical address query. Returns the number of segments, or 0 when
 * this kernel has no such call (the old, inline wire is then used). */
static int
remote_vtop(const void *pixels, size_t bytes, wire_segment *segments, unsigned max)
{
	uint32_t count = 0;

	if (pixels == NULL || bytes == 0 || max == 0) {
		return 0;
	}
	if (syscall(SYS_hakios_vtop, (uintptr_t)pixels, (uintptr_t)bytes,
	    (uintptr_t)segments, (uintptr_t)max, (uintptr_t)&count) != 0) {
		return 0;
	}
	return count <= max ? (int)count : 0;
}

/* A page of our own, so the probe does not depend on any other buffer being resident. */
static int
remote_vtop_available(void)
{
	static unsigned char probe[16384];
	wire_segment segments[8];
	int n, covered = 0;

	probe[0] = 1;
	probe[sizeof(probe) - 1] = 2;
	n = remote_vtop(probe, sizeof(probe), segments, sizeof(segments) / sizeof(segments[0]));
	for (int i = 0; i < n; i++) {
		covered += (int)segments[i].len;
	}
	return n > 0 && covered == (int)sizeof(probe);
}
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

/* The console this port reads is the processes' standard error (eagl.c does the same). */
static void
remote_say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static double
remote_now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* The kernel's boot-args, for a switch that a run without a new image can still set. */
static const char *
remote_boot_arg(const char *wanted)
{
	static char arguments[512];
	size_t size = sizeof(arguments) - 1;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return NULL;
	}
	return strstr(arguments, wanted);
}

/* Whether the boot keeps the local drawing: a file (so the A/B needs no rebuild) and, for a run that
 * cannot be given a new image, the same thing as a boot-arg. */
static int
remote_wants_local(void)
{
	if (remote_boot_arg("glwire_local=1") != NULL) {
		return 1;
	}
	if (remote_boot_arg("glwire_local=0") != NULL) {
		return 0;
	}
	return access("/redwood/etc/glwire_local", F_OK) == 0;
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
	/*
	 * The fallback switch: a file, so a boot that goes wrong can be brought back to the inline
	 * wire without a rebuild. And a kernel that does not answer the query keeps the old path by
	 * itself, which is what a boot of an older kernel does.
	 */
	if (access("/redwood/etc/glwire_inline", F_OK) != 0 && remote_vtop_available()) {
		wire_vtop_hook = remote_vtop;
	}
	remote_only = wire_vtop_hook != NULL && !remote_wants_local();
	remote_no_flush = remote_boot_arg("glwire_noflush=1") != NULL ||
	    access("/redwood/etc/glwire_noflush", F_OK) == 0;
	remote_say("hakios-remote: the frame is %s%s\n",
	    remote_only ? "drawn by the host alone" : "drawn here as well (glwire_local)",
	    remote_no_flush ? "; the recorders are installed but no frame is sent (glwire_noflush)" : "");
	wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
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
	remote_frame_bytes = bytes;
	/* The host will write the picture here; touch the pages once so the first query can find them. */
	memset(remote_frame + 8, 0, bytes);
	/*
	 * And keep them where they are. The host writes at the physical addresses this side named;
	 * if the compressor moves a page afterwards, that write lands in whatever took its place.
	 * mlock is allowed up to the process's own limit, so the answer is logged rather than
	 * assumed -- an unwired buffer still works, it is just the risk this is here to remove.
	 */
	if (remote_boot_arg("glwire_mlock=0") == NULL) {
		int locked = mlock(remote_frame, bytes + 8);

		remote_say("hakios-remote: picture buffer %zu B + 8, mlock %d%s\n", bytes, locked,
		    locked == 0 ? " (wired)" : " (NOT wired: a page that moves afterwards can be written)");
	}
}

int
eagl_remote_is_on(void)
{
	return remote_enabled;
}

/* For eagl.c: with this on, composing the frame here (the IOSurface refills, the panel upload)
 * copies pixels that were never drawn here, so it is not done. */
int
eagl_remote_no_local(void)
{
	return remote_only;
}

size_t
eagl_remote_pending(void)
{
	return w.at;
}

/* Hand the frame to the host and wait for the picture. The host replays the records on its own GL
 * context and sends the panel's pixels back; this side copies them into the display's surface. No
 * address travels, and the host needs no access to the guest's RAM. */
/*
 * Point the frame's picture at the display's surface. Returns 1 when the host can write it there
 * (every byte translated, in order), 0 when the packed buffer has to carry it after all -- in which
 * case this also clears a target a previous frame set.
 */
int
eagl_remote_surface(GLsizei width, GLsizei height, void *base, GLsizei row_bytes)
{
	size_t bytes, covered = 0;
	int count;

	remote_surface_segments_count = 0;
	remote_surface_row = 0;
	if (remote_no_flush || !remote_enabled || wire_vtop_hook == NULL || base == NULL ||
	    width <= 0 || height <= 0 || row_bytes < width * 4) {
		return 0;
	}
	bytes = (size_t)row_bytes * (size_t)height;
	count = wire_vtop_hook(base, bytes, remote_surface_segments,
	    sizeof(remote_surface_segments) / sizeof(remote_surface_segments[0]));
	if (count <= 0) {
		return 0;
	}
	for (int i = 0; i < count; i++) {
		covered += remote_surface_segments[i].len;
	}
	if (covered != bytes) {
		return 0;
	}
	remote_surface_segments_count = count;
	remote_surface_row = row_bytes;
	return 1;
}

int
eagl_remote_flush(int width, int height)
{
	struct sockaddr_in address;
	char request[96], reply[64];
	double started;
	int at = 0;

	if (remote_no_flush) {
		/*
		 * The recorders stay installed and every call is still recorded (that is the variable
		 * under test); the frame simply never leaves the machine. Drop the records so the arena
		 * cannot fill, and answer exactly as a frame that was sent and not drawn would.
		 */
		wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
		return 0;
	}
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
	/*
	 * The surface when it could be translated (W3: the host writes the rows itself), otherwise the
	 * packed buffer. The request line says which by its row stride: 0 means the picture is packed.
	 */
	remote_picture_segments = remote_frame_segments;
	remote_picture_segments_count = 0;
	remote_picture_row = 0;
	remote_picture_bytes = (size_t)width * height * 4;
	remote_frame_segments_count = 0;
	if (remote_surface_segments_count > 0) {
		remote_picture_segments = remote_surface_segments;
		remote_picture_segments_count = remote_surface_segments_count;
		remote_picture_row = remote_surface_row;
		remote_picture_bytes = (size_t)remote_surface_row * (size_t)height;
	} else if (wire_vtop_hook != NULL && remote_frame != NULL) {
		remote_picture_segments_count = wire_vtop_hook(remote_frame + 8,
		    (size_t)width * height * 4, remote_frame_segments,
		    sizeof(remote_frame_segments) / sizeof(remote_frame_segments[0]));
	}
	remote_frame_segments_count = remote_picture_segments_count;
	snprintf(request, sizeof(request), "R %d %d %zu %zu %d %d\n",
	    width, height, w.at, (size_t)width * height * 4, remote_picture_segments_count,
	    remote_picture_row);
	if (send(remote_fd, request, strlen(request), 0) != (ssize_t)strlen(request) ||
	    (remote_picture_segments_count > 0 &&
	     send(remote_fd, remote_picture_segments,
	     (size_t)remote_picture_segments_count * sizeof(wire_segment), 0) !=
	     (ssize_t)((size_t)remote_picture_segments_count * sizeof(wire_segment))) ||
	    send(remote_fd, w.base, w.at, 0) != (ssize_t)w.at) {
		close(remote_fd);
		remote_fd = -1;
		return 0;
	}
	while (at < (int)sizeof(reply) - 2 && recv(remote_fd, &reply[at], 1, 0) == 1 && reply[at] != '\n') {
		at++;
	}
	reply[at] = '\0';
	{
		size_t picture_bytes = 0;
		int received = 0;
		int in_place = 0;

		if (sscanf(reply, "R %zu", &picture_bytes) == 1 &&
		    picture_bytes == 0 && remote_picture_segments_count > 0) {
			/*
			 * The host already wrote the picture into the memory this frame named: the packed
			 * buffer (1: copy it out here), or the display's surface itself (2: nothing to copy).
			 */
			received = (int)((size_t)width * height * 4);
			in_place = remote_picture_row > 0 ? 2 : 1;
		} else if (sscanf(reply, "R %zu", &picture_bytes) == 1 &&
		    picture_bytes == (size_t)width * height * 4) {
			while (received < (int)picture_bytes) {
				ssize_t got = recv(remote_fd, remote_frame + 8 + received,
				    picture_bytes - (size_t)received, 0);

				if (got <= 0) {
					break;
				}
				received += (int)got;
			}
		}
		remote_total_us += (long)((remote_now_ms() - started) * 1000.0);
		remote_frames++;
		if ((remote_frames % 30) == 1) {
			printf("glwire: %lu image uploads by reference, %lu inline\n",
			    wire_image_ref_count, wire_image_inline_count);
		}
		wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
		if (in_place != 0) {
			return in_place;
		}
		return (received == (int)picture_bytes && picture_bytes != 0) ? 1 : 0;
	}
}

/*
 * An IOSurface's pixels, for the host. eagl.c uploads these with its own glTexImage2D -- outside
 * the EAGL table, so the ordinary recorder never sees them. Without this the host has the texture's
 * name but no pixels, and every frame it draws is black. The row stride travels because an IOSurface
 * is usually wider than the picture (4608 bytes a row for a 1125-pixel panel).
 */
void
eagl_remote_upload(GLuint texture, GLenum target, GLint internal, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *pixels, GLsizei row_bytes, GLsizei pixel_bytes)
{
	size_t bytes;

	if (!remote_enabled || pixels == NULL || width <= 0 || height <= 0 ||
	    row_bytes <= 0 || pixel_bytes <= 0) {
		return;
	}
	bytes = (size_t)row_bytes * (size_t)height;
	if (bytes == 0 || bytes > (24u << 20)) {
		return;
	}
	wire_open(&w, 201);
	wire_u32(&w, texture);
	wire_u32(&w, target);
	wire_i32(&w, internal);
	wire_i32(&w, width);
	wire_i32(&w, height);
	wire_u32(&w, format);
	wire_u32(&w, type);
	wire_i32(&w, row_bytes);
	wire_i32(&w, pixel_bytes);
	wire_image(&w, pixels, (GLsizei)bytes);
	wire_close(&w);
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
