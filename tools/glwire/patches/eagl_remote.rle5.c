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
/* (eagl.c keeps what is bound; the trampoline tells it, see eagl_note_bind) */
extern void eagl_note_bind(void *context, GLuint framebuffer);

static int remote_fd = -1;
/* option readers are defined below; the init self-report needs them by name */
static int remote_upload_fast(void);
static int remote_upload_565(void);
static int remote_upload_rle(void);
static int remote_dirty_upload(void);
static int remote_ack_timeout_ms(void);
static int remote_upload_scale(void);
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

/*
 * P1'''': the frame's second panel-sized surface. A frame can name more than one surface the size of
 * the panel -- the kernel's swap log takes two in turn (3072 and 3008 bytes a row) -- and if only
 * the first one is written, the other keeps whatever it had, which is the black screen a screenshot
 * catches every other time. It travels exactly as the first one does: named by its rows, written by
 * the host, nothing copied here.
 */
static wire_segment remote_surface2_segments[2048];
static int remote_surface2_segments_count;
static GLsizei remote_surface2_row;

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

/* Bisection: boot-arg glwire_slots=MASK keeps only the recorders whose spec index has its bit set.
 * The others get their original local entry back, so the guest still draws; with half the slots off,
 * a crash that disappears names the half the culprit is in. All bits set when the argument is
 * absent, which is every ordinary boot. */
static unsigned long long remote_slot_mask = ~0ULL;

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
	/* stage eighteen: the array buffer bound when the pointer was given.  A non-zero binding means
	 * the "pointer" is an offset into that buffer, and the bytes read from it at draw time are not
	 * the vertices -- which is what the host saw at the interface's label draws. */
	GLint buffer;
	void *snapshot;
	size_t snapshot_bytes;
} remote_attribs[16];
static int remote_attrib_enabled[16];
static unsigned remote_pointer_said;
static unsigned remote_emit_count;


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
	static char arguments[2048];
	size_t size = sizeof(arguments) - 1;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return NULL;
	}
	return strstr(arguments, wanted);
}

/* Whether the boot keeps the local drawing: a file (so the A/B needs no rebuild) and, for a run that
 * cannot be given a new image, the same thing as a boot-arg. */
static const char *
remote_bootargs_raw(void)
{
	static char arguments[2048];
	size_t size = sizeof(arguments) - 1;

	arguments[0] = 0;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return "";
	}
	arguments[size < sizeof(arguments) ? size : sizeof(arguments) - 1] = 0;
	return arguments;
}

/* /tmp/glwire_opts is the long-lived option channel: one key=value per line, written by the host
 * (or baked into the root), so boot-args no longer have to carry six switches. */
static void
remote_apply_option_line(char *line)
{
	char *space = line;

	while (*space != 0) {
		char *key = space;
		char *end;
		char *value;
		const char *env_key = NULL;

		while (*space == ' ' || *space == '\t') {
			space++;
		}
		if (*space == 0) {
			break;
		}
		key = space;
		while (*space != 0 && *space != ' ' && *space != '\t') {
			space++;
		}
		end = space;
		value = strchr(key, '=');
		if (value == NULL || value >= end) {
			continue;
		}
		*value++ = 0;
		if (end < space) {
			*end = 0;
		}
		if (strcmp(key, "fast") == 0) {
			env_key = "GLWIRE_UPLOAD_FAST";
		} else if (strcmp(key, "565") == 0) {
			env_key = "GLWIRE_UPLOAD_565";
		} else if (strcmp(key, "rle") == 0) {
			env_key = "GLWIRE_UPLOAD_RLE";
		} else if (strcmp(key, "dirty") == 0) {
			env_key = "GLWIRE_DIRTY_UPLOAD";
		} else if (strcmp(key, "ack") == 0) {
			env_key = "GLWIRE_ACK_MS";
		} else if (strcmp(key, "scale") == 0) {
			env_key = "GLWIRE_UPLOAD_SCALE";
		}
		if (env_key != NULL) {
			setenv(env_key, value, 1);
		}
	}
}

static void
remote_load_options(void)
{
	const char *paths[] = { "/tmp/glwire_opts", "/redwood/etc/glwire_opts" };
	int loaded = 0;

	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		FILE *file = fopen(paths[i], "r");
		char line[512];

		if (file == NULL) {
			continue;
		}
		while (fgets(line, sizeof(line), file) != NULL) {
			remote_apply_option_line(line);
		}
		fclose(file);
		loaded = 1;
	}
	if (loaded) {
		remote_say("hakios-remote: options file loaded\n");
	}
}

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

/* Twelfth stage: what the big texture uploads are.  The local entries only run when the guest also
 * draws (glwire_local), so in the host-draws-everything mode the uploads are only ever seen here, in
 * the recorded stream: boot-arg texlog=1 says the sizes of the first forty of them, which is what
 * tells a 312x202 'LA08' glyph overlay (a surface that arrives and is never bound) from no upload at
 * all. */
int remote_texlog;

void
remote_texlog_upload(const char *what, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	static unsigned logged;

	if (!remote_texlog || logged >= 40 || width * height < 8000) {
		return;
	}
	logged++;
	remote_say("hakios-gl: %s %dx%d format %#x type %#x, %s (pid %d)\n", what, (int) width, (int) height, format, type,
	    pixels != NULL ? "filled" : "empty", (int) getpid());
}

/* Fifteenth stage: every record says which EAGL context made the call.  The trampoline receives the
 * context itself as its first argument (the EAGL table's convention), so the recorder interns the
 * pointer into 0..7 and puts that number in the record header; the host replays each context with
 * its own name maps and its own GL state.  A ninth context shares number 0 rather than travelling
 * with an unrepresentable id. */
static void *remote_context_address[8];
static uint32_t remote_context_count;

void
remote_note_context(void *context)
{
	for (uint32_t i = 0; i < remote_context_count; i++) {
		if (remote_context_address[i] == context) {
			wire_set_record_context(i);
			return;
		}
	}
	if (remote_context_count < sizeof(remote_context_address) / sizeof(remote_context_address[0])) {
		remote_context_address[remote_context_count] = context;
		wire_set_record_context(remote_context_count);
		remote_context_count++;
		return;
	}
	wire_set_record_context(0);
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
	remote_load_options();
	/*
	 * The fallback switch: a file, so a boot that goes wrong can be brought back to the inline
	 * wire without a rebuild. And a kernel that does not answer the query keeps the old path by
	 * itself, which is what a boot of an older kernel does.
	 */
	if (access("/redwood/etc/glwire_inline", F_OK) != 0 && remote_vtop_available()) {
		wire_vtop_hook = remote_vtop;
	}
	remote_only = wire_vtop_hook != NULL && !remote_wants_local();
	/* stage fifteen: the record header carries the EAGL context (see remote_note_context) */
	wire_set_record_version(2);
	remote_texlog = remote_boot_arg("texlog=1") != NULL;
	remote_no_flush = remote_boot_arg("glwire_noflush=1") != NULL ||
	    access("/redwood/etc/glwire_noflush", F_OK) == 0;
	{
		const char *slots = remote_boot_arg("glwire_slots=");

		if (slots != NULL) {
			remote_slot_mask = strtoull(slots + strlen("glwire_slots="), NULL, 0);
		}
	}
	remote_say("hakios-remote: the frame is %s%s; recorder mask 0x%llx\n",
	    remote_only ? "drawn by the host alone" : "drawn here as well (glwire_local)",
	    remote_no_flush ? "; the recorders are installed but no frame is sent (glwire_noflush)" : "",
	    remote_slot_mask);
	{
		char line[1600];
		int n = snprintf(line, sizeof(line),
		    "hakios-remote: options raw=\"%s\" parsed local=%d fast=%d 565=%d rle=%d dirty=%d ack=%d scale=%d\n",
		    remote_bootargs_raw(), remote_only, remote_upload_fast(), remote_upload_565(),
		    remote_upload_rle(), remote_dirty_upload(), remote_ack_timeout_ms(), remote_upload_scale());

		if (n > 0) {
			write(2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
		}
	}
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

/*
 * For eagl.c: whether the composed display surface goes on the wire as the panel (0xFFFF0001), which
 * the host draws on screen and hashes against its own picture. glwire_nopanel_up=1 leaves it here and
 * changes nothing else: what the host then has of the frame is the CoreAnimation calls and the layer
 * textures alone, so whether its picture still is the desk answers "who draws the picture".
 * A boot-arg, so one image answers both halves; on unless the boot asks otherwise.
 */
int
eagl_remote_panel_upload_on(void)
{
	static int on = -1;

	if (on < 0) {
		on = remote_boot_arg("glwire_nopanel_up=1") == NULL;
		remote_say("hakios-remote: the panel %s\n",
		    on ? "goes on the wire (0xFFFF0001)" : "stays here (glwire_nopanel_up=1)");
	}
	return on;
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
	/* (the second one is a per-frame decision too: eagl.c names it after this one, if the frame
	 * has a second panel-sized target) */
	remote_surface2_segments_count = 0;
	remote_surface2_row = 0;
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

/*
 * P1'''': the same for the frame's second panel-sized surface (see remote_surface2_segments). Its
 * rows are named in the same request, after the first surface's; the host writes the picture into
 * both, so whichever of the two the display presents next holds the frame.
 */
int
eagl_remote_surface2(GLsizei width, GLsizei height, void *base, GLsizei row_bytes)
{
	size_t bytes, covered = 0;
	int count;

	remote_surface2_segments_count = 0;
	remote_surface2_row = 0;
	if (remote_no_flush || !remote_enabled || wire_vtop_hook == NULL || base == NULL ||
	    width <= 0 || height <= 0 || row_bytes < width * 4) {
		return 0;
	}
	bytes = (size_t)row_bytes * (size_t)height;
	count = wire_vtop_hook(base, bytes, remote_surface2_segments,
	    sizeof(remote_surface2_segments) / sizeof(remote_surface2_segments[0]));
	if (count <= 0) {
		return 0;
	}
	for (int i = 0; i < count; i++) {
		covered += remote_surface2_segments[i].len;
	}
	if (covered != bytes) {
		return 0;
	}
	remote_surface2_segments_count = count;
	remote_surface2_row = row_bytes;
	return 1;
}

/* Stage 19 follow-up: the per-frame picture is 4.9 MB and only TCP_NODELAY was set.  A boot-arg
 * glwire_sockbuf=<bytes> (or GLWIRE_SOCKBUF in the environment) raises SO_RCVBUF/SO_SNDBUF; zero or
 * absent leaves the socket exactly as before, so the default path is unchanged. */
static int
remote_socket_buffer(void)
{
	const char *argument = remote_boot_arg("glwire_sockbuf=");
	const char *env = getenv("GLWIRE_SOCKBUF");
	const char *value = env != NULL && *env != '\0' ? env : argument;

	if (value == NULL) {
		return 0;
	}
	if (value == argument) {
		value += strlen("glwire_sockbuf=");
	}
	{
		char *end = NULL;
		unsigned long parsed = strtoul(value, &end, 0);

		if (end == value || parsed < 65536UL) {
			return 0;
		}
		if (parsed > 64UL * 1024UL * 1024UL) {
			parsed = 64UL * 1024UL * 1024UL;
		}
		return (int)parsed;
	}
}

static void
remote_apply_socket_buffer(int fd)
{
	int bytes = remote_socket_buffer();
	int maxsockbuf = 0;
	size_t length = sizeof(maxsockbuf);

	if (bytes <= 0) {
		return;
	}
	/* iOS's kern.ipc.maxsockbuf is the ceiling the old 8 MB request hit with ENOBUFS (55).  Clamp
	 * instead of requiring a privileged sysctl; the request value and the ceiling are both logged. */
	if (sysctlbyname("kern.ipc.maxsockbuf", &maxsockbuf, &length, NULL, 0) == 0 && maxsockbuf > 0) {
		if (bytes > maxsockbuf) {
			remote_say("hakios-remote: socket buffer request %d clamped to kern.ipc.maxsockbuf %d\n",
			    bytes, maxsockbuf);
			bytes = maxsockbuf;
		}
	} else {
		maxsockbuf = 0;
	}
	for (int attempt = bytes; attempt >= 65536; attempt /= 2) {
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &attempt, sizeof(attempt)) == 0 &&
		    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &attempt, sizeof(attempt)) == 0) {
			remote_say("hakios-remote: socket buffer %d B applied (requested %d, maxsockbuf %d)\n",
			    attempt, remote_socket_buffer(), maxsockbuf);
			return;
		}
	}
	remote_say("hakios-remote: socket buffer request %d B not applied (errno %d, maxsockbuf %d)\n",
	    bytes, errno, maxsockbuf);
}

/* Candidate 2: the guest blocks in the reply read after sending a frame.  glwire_ack_ms=<ms>
 * (or GLWIRE_ACK_MS) bounds that wait; on timeout the socket is closed so a late reply can never be
 * mistaken for the next frame's.  Default 0 = the old infinite wait. */
/* Payload experiment: glwire_upload_scale=2 makes the host return only the top-left quarter and
 * makes frame_end copy only that quarter.  Default 1 sends/copies the whole picture as before. */
static int
remote_upload_scale(void)
{
	static int parsed = -1;
	static int scale = 1;

	if (parsed >= 0) {
		return scale;
	}
	parsed = 1;
	{
		const char *env = getenv("GLWIRE_UPLOAD_SCALE");
		const char *argument = remote_boot_arg("glwire_upload_scale=");
		const char *value = env != NULL && *env != '\0' ? env : argument;

		if (value != NULL) {
			if (value == argument) {
				value += strlen("glwire_upload_scale=");
			}
			{
				int parsed_value = atoi(value);

				if (parsed_value == 2 || parsed_value == 4) {
					scale = parsed_value;
				}
			}
		}
	}
	if (scale > 1) {
		remote_say("hakios-remote: upload scale %d (1/%d of the picture)\\n", scale, scale * scale);
	}
	return scale;
}

int
eagl_remote_upload_scale(void)
{
	return remote_upload_scale();
}

/* Fast full-frame path: receive the host's BGRA rows directly into the display IOSurface instead of
 * remote_frame + a second 4.9 MB copy.  Off unless glwire_upload_fast=1. */
static void *remote_target_base;
static size_t remote_target_row;
static int remote_target_width, remote_target_height;

static int
remote_upload_fast(void)
{
	static int parsed = -1;
	static int on;

	if (parsed >= 0) {
		return on;
	}
	parsed = 1;
	{
		const char *env = getenv("GLWIRE_UPLOAD_FAST");
		const char *argument = remote_boot_arg("glwire_upload_fast=1");

		on = (env != NULL && strcmp(env, "0") != 0) || argument != NULL;
	}
	if (on) {
		remote_say("hakios-remote: upload fast path (direct surface rows)\n");
	}
	return on;
}

int
eagl_remote_fast_upload_on(void)
{
	return remote_upload_fast();
}

void
eagl_remote_set_target(void *base, size_t row, int width, int height)
{
	remote_target_base = base;
	remote_target_row = row;
	remote_target_width = width;
	remote_target_height = height;
}

/* RGB565 experiment: the host sends 2,436,000 B for a 750x1624 frame, the guest expands each row
 * to BGRA (into the target surface when the fast path is on, otherwise into remote_frame). */
static int
remote_upload_565(void)
{
	static int parsed = -1;
	static int on;

	if (parsed >= 0) {
		return on;
	}
	parsed = 1;
	{
		const char *env = getenv("GLWIRE_UPLOAD_565");
		const char *argument = remote_boot_arg("glwire_upload_565=1");

		on = (env != NULL && strcmp(env, "0") != 0) || argument != NULL;
	}
	if (on) {
		remote_say("hakios-remote: upload565=1 (RGB565)\n");
	}
	return on;
}

static int
remote_upload_rle(void)
{
	static int parsed = -1;
	static int on;

	if (parsed >= 0) {
		return on;
	}
	parsed = 1;
	{
		const char *env = getenv("GLWIRE_UPLOAD_RLE");
		const char *argument = remote_boot_arg("glwire_upload_rle=1");

		on = (env != NULL && strcmp(env, "0") != 0) || argument != NULL;
	}
	if (on) {
		remote_say("hakios-remote: upload-rle=1\n");
	}
	return on;
}

static void
expand_565_row(unsigned char *dst, const uint16_t *src, int width)
{
	for (int x = 0; x < width; x++) {
		unsigned v = src[x];
		unsigned r = (v >> 11) & 31;
		unsigned g = (v >> 5) & 63;
		unsigned b = v & 31;

		dst[x * 4 + 0] = (unsigned char)((b << 3) | (b >> 2));
		dst[x * 4 + 1] = (unsigned char)((g << 2) | (g >> 4));
		dst[x * 4 + 2] = (unsigned char)((r << 3) | (r >> 2));
		dst[x * 4 + 3] = 255;
	}
}

/* GLWIRE_DIRTY_UPLOAD=1 / glwire_dirty_upload=1: the host compares 64x64 tiles with the previous
 * frame and replies D <tiles>; the guest patches its full remote_frame.  Off by default. */
static int
remote_dirty_upload(void)
{
	static int parsed = -1;
	static int on;

	if (parsed >= 0) {
		return on;
	}
	parsed = 1;
	{
		const char *env = getenv("GLWIRE_DIRTY_UPLOAD");
		const char *argument = remote_boot_arg("glwire_dirty_upload=1");

		on = (env != NULL && strcmp(env, "0") != 0) || argument != NULL;
	}
	if (on) {
		remote_say("hakios-remote: dirty upload 64x64 tiles\n");
	}
	return on;
}

static int
remote_recv_all(int fd, void *data, size_t bytes)
{
	unsigned char *p = data;

	while (bytes > 0) {
		ssize_t got = recv(fd, p, bytes, 0);

		if (got <= 0) {
			return 0;
		}
		p += got;
		bytes -= (size_t)got;
	}
	return 1;
}

static int
remote_ack_timeout_ms(void)
{
	static int parsed = -1;
	static int milliseconds;

	if (parsed >= 0) {
		return milliseconds;
	}
	parsed = 1;
	{
		const char *env = getenv("GLWIRE_ACK_MS");
		const char *argument = remote_boot_arg("glwire_ack_ms=");
		const char *value = env != NULL && *env != '\0' ? env : argument;

		if (value != NULL) {
			if (value == argument) {
				value += strlen("glwire_ack_ms=");
			}
			milliseconds = atoi(value);
			if (milliseconds < 1 || milliseconds > 60000) {
				milliseconds = 0;
			}
		}
	}
	return milliseconds;
}

/* The first frame is the boot's frame: CoreAnimation has not settled, the panel is still empty,
 * and the host may need a full replay before the first picture.  A 50 ms cap there drops that frame,
 * and the guest then closes the socket while the host is still sending the picture.  Arm the cap
 * only after one complete frame has been received, and disarm it for the rest of the process after
 * three drops.  ack_ms=0 never arms, and the old infinite paths below stay in use. */
static int remote_ack_armed;
static int remote_ack_disabled;
static unsigned remote_ack_complete;
static unsigned remote_ack_drops;
#define REMOTE_ACK_MAX_DROPS 3
#define REMOTE_ACK_ARM_AFTER 5

static int
remote_ack_effective_ms(void)
{
	int milliseconds = remote_ack_timeout_ms();

	return milliseconds > 0 && remote_ack_armed && !remote_ack_disabled ? milliseconds : 0;
}

static void
remote_apply_ack_timeout(int fd)
{
	int milliseconds = remote_ack_effective_ms();

	if (milliseconds > 0) {
		struct timeval timeout;

		timeout.tv_sec = milliseconds / 1000;
		timeout.tv_usec = (milliseconds % 1000) * 1000;
		if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
			remote_say("hakios-remote: ack timeout %d ms not applied (errno %d)\n", milliseconds, errno);
		} else {
			remote_say("hakios-remote: ack timeout %d ms applied after the first frame\n", milliseconds);
		}
	}
}

static void
remote_ack_success(int fd)
{
	if (remote_ack_timeout_ms() <= 0 || remote_ack_disabled) {
		return;
	}
	if (!remote_ack_armed) {
		remote_ack_complete++;
		if (remote_ack_complete < REMOTE_ACK_ARM_AFTER) {
			return;
		}
		remote_ack_armed = 1;
		remote_apply_ack_timeout(fd);
		remote_say("hakios-remote: ack timeout armed after %u complete frames\n", remote_ack_complete);
	}
}

static void
remote_ack_note_drop(void)
{
	if (remote_ack_timeout_ms() <= 0) {
		return;
	}
	remote_ack_drops++;
	if (remote_ack_drops >= REMOTE_ACK_MAX_DROPS && !remote_ack_disabled) {
		remote_ack_disabled = 1;
		remote_ack_armed = 0;
		remote_say("hakios-remote: ack timeout disabled after %u drops; using the old infinite wait\n",
		    remote_ack_drops);
	}
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
		remote_apply_socket_buffer(remote_fd);
		remote_apply_ack_timeout(remote_fd);
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
	/* (a second surface is only meaningful when the first target is one) */
	if (remote_picture_row <= 0) {
		remote_surface2_segments_count = 0;
		remote_surface2_row = 0;
	}
	snprintf(request, sizeof(request), "R %d %d %zu %zu %d %d %d %d %d %d %d %d\n",
	    width, height, w.at, (size_t)width * height * 4, remote_picture_segments_count,
	    remote_picture_row, remote_surface2_segments_count, remote_surface2_row,
	    2 /* the record header names its context */, remote_dirty_upload(), remote_upload_565(),
	    remote_upload_rle());
	if (send(remote_fd, request, strlen(request), 0) != (ssize_t)strlen(request) ||
	    (remote_picture_segments_count > 0 &&
	     send(remote_fd, remote_picture_segments,
	     (size_t)remote_picture_segments_count * sizeof(wire_segment), 0) !=
	     (ssize_t)((size_t)remote_picture_segments_count * sizeof(wire_segment))) ||
	    (remote_surface2_segments_count > 0 &&
	     send(remote_fd, remote_surface2_segments,
	     (size_t)remote_surface2_segments_count * sizeof(wire_segment), 0) !=
	     (ssize_t)((size_t)remote_surface2_segments_count * sizeof(wire_segment))) ||
	    send(remote_fd, w.base, w.at, 0) != (ssize_t)w.at) {
		close(remote_fd);
		remote_fd = -1;
		return 0;
	}
	if (remote_ack_effective_ms() == 0) {
		/* the pre-ack path, unchanged: first frame, ack off, or cap disabled */
		while (at < (int)sizeof(reply) - 2 && recv(remote_fd, &reply[at], 1, 0) == 1 &&
		    reply[at] != '\n') {
			at++;
		}
		reply[at] = '\0';
	} else {
		int failed = 0;
		int ack_ms = remote_ack_effective_ms();

		while (at < (int)sizeof(reply) - 2) {
			ssize_t got = recv(remote_fd, &reply[at], 1, 0);

			if (got == 1) {
				if (reply[at] == '\n') {
					break;
				}
				at++;
				continue;
			}
			failed = 1;
			break;
		}
		reply[at] = '\0';
		if (failed) {
			remote_ack_note_drop();
			remote_say("hakios-remote: ack timeout after %d ms, frame dropped %u/%u (errno %d)\n",
			    ack_ms, remote_ack_drops, REMOTE_ACK_MAX_DROPS, errno);
			close(remote_fd);
			remote_fd = -1;
			wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
			return 0;
		}
	}
	if (reply[0] == 'D') {
		int tiles = 0;
		size_t total = 0, got_total = 0;

		if (sscanf(reply, "D %d %zu", &tiles, &total) != 2 || tiles < 0 || tiles > 1024 ||
		    total > (size_t)width * height * 4) {
			remote_say("hakios-remote: bad dirty reply '%s'\n", reply);
			close(remote_fd);
			remote_fd = -1;
			wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
			return 0;
		}
		for (int i = 0; i < tiles; i++) {
			uint32_t header[4];
			uint32_t x, y, tw, th;

			if (!remote_recv_all(remote_fd, header, sizeof(header))) {
				goto dirty_failed;
			}
			x = header[0];
			y = header[1];
			tw = header[2];
			th = header[3];
			if (x >= (uint32_t)width || y >= (uint32_t)height || tw == 0 || th == 0 ||
			    x + tw > (uint32_t)width || y + th > (uint32_t)height) {
				goto dirty_failed;
			}
			for (uint32_t row = 0; row < th; row++) {
				unsigned char *dst = remote_frame + 8 +
				    ((size_t)(y + row) * (size_t)width + x) * 4;

				if (!remote_recv_all(remote_fd, dst, (size_t)tw * 4)) {
					goto dirty_failed;
				}
				got_total += (size_t)tw * 4;
			}
		}
		if (got_total != total) {
			goto dirty_failed;
		}
		remote_say("hakios-remote: dirty %d tiles, %zu B\n", tiles, total);
		remote_ack_success(remote_fd);
		remote_frames++;
		remote_total_us += (long)(remote_now_ms() - started) * 1000.0;
		wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
		return 1;
dirty_failed:
		remote_say("hakios-remote: dirty upload truncated (%zu of %zu), frame dropped\n", got_total, total);
		close(remote_fd);
		remote_fd = -1;
		wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
		return 0;
	}
	if (reply[0] == 'Z') {
		size_t raw = 0, packed_size = 0;
		unsigned char *packed_bytes;
		unsigned char *pixels;
		size_t out = 0, in = 0;
		int pixel_bytes;
		long began;

		if (sscanf(reply, "Z %zu %zu", &raw, &packed_size) != 2) {
			goto bad_z;
		}
		if (raw == (size_t)width * (size_t)height * 2) {
			pixel_bytes = 2;
		} else if (raw == (size_t)width * (size_t)height * 4) {
			pixel_bytes = 4;
		} else {
			goto bad_z;
		}
		if (packed_size > raw + raw / 64 + 32) {
			goto bad_z;
		}
		packed_bytes = malloc(packed_size);
		pixels = malloc(raw);
		if (packed_bytes == NULL || pixels == NULL ||
		    !remote_recv_all(remote_fd, packed_bytes, packed_size)) {
			free(packed_bytes);
			free(pixels);
			goto bad_z;
		}
		began = remote_now_ms();
		while (in < packed_size) {
			unsigned code = packed_bytes[in++];

			if ((code & 0x80) != 0) {
				size_t run = (code & 0x7f) + 3;
				uint32_t value = 0;

				if (in + pixel_bytes > packed_size) {
					break;
				}
				if (out + run * (size_t)pixel_bytes > raw) {
					run = (raw - out) / (size_t)pixel_bytes;
				}
				memcpy(&value, packed_bytes + in, (size_t)pixel_bytes);
				in += (size_t)pixel_bytes;
				if (pixel_bytes == 4) {
					if (value == 0 || value == 0xffffffffu) {
						memset(pixels + out, (int)(value & 0xff), run * 4);
					} else {
						uint32_t *dst = (uint32_t *)(void *)(pixels + out);

						for (size_t i = 0; i < run; i++) {
							dst[i] = value;
						}
					}
				} else {
					uint16_t *dst = (uint16_t *)(void *)(pixels + out);

					for (size_t i = 0; i < run; i++) {
						dst[i] = (uint16_t)value;
					}
				}
				out += run * (size_t)pixel_bytes;
			} else {
				size_t literal = code + 1;

				if (in + literal * (size_t)pixel_bytes > packed_size) {
					break;
				}
				if (out + literal * (size_t)pixel_bytes > raw) {
					break;
				}
				memcpy(pixels + out, packed_bytes + in, literal * (size_t)pixel_bytes);
				out += literal * (size_t)pixel_bytes;
				in += literal * (size_t)pixel_bytes;
			}
		}
		if (out != raw) {
			remote_say("hakios-remote: RLE short %zu of %zu bytes\n", out, raw);
			free(packed_bytes);
			free(pixels);
			goto bad_z;
		}
		{
			long unpack_ms = remote_now_ms() - began;

			for (int y = 0; y < height; y++) {
				unsigned char *dst;

				if (remote_upload_fast() && remote_target_base != NULL &&
				    remote_target_width == width && remote_target_height == height &&
				    remote_target_row >= (size_t)width * 4) {
					dst = (unsigned char *)remote_target_base + remote_target_row * (size_t)y;
				} else {
					dst = remote_frame + 8 + (size_t)y * (size_t)width * 4;
				}
				if (pixel_bytes == 4) {
					memcpy(dst, pixels + (size_t)y * (size_t)width * 4, (size_t)width * 4);
				} else {
					expand_565_row(dst, (const uint16_t *)(pixels + (size_t)y * (size_t)width * 2), width);
				}
			}
			remote_say("hakios-remote: upload-rle %s raw %zu -> packed %zu B (%zu%%), unpack %ld ms\n",
			    pixel_bytes == 4 ? "bgra" : "565", raw, packed_size,
			    raw ? packed_size * 100 / raw : 0, unpack_ms);
		}
		free(packed_bytes);
		free(pixels);
		remote_ack_success(remote_fd);
		remote_frames++;
		remote_total_us += (long)(remote_now_ms() - started) * 1000.0;
		wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
		return remote_upload_fast() ? 3 : 1;
bad_z:
		remote_say("hakios-remote: bad RLE reply '%s'\n", reply);
		close(remote_fd);
		remote_fd = -1;
		wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
		return 0;
	}

	{
		size_t picture_bytes = 0;
		int received = 0;
		int in_place = 0;
		int scale = remote_upload_scale();
		size_t scaled_bytes = (size_t)width * (size_t)height * 4;

		if (scale > 1 && width % scale == 0 && height % scale == 0) {
			scaled_bytes = (size_t)(width / scale) * (size_t)(height / scale) * 4;
		} else {
			scale = 1;
		}

		if (sscanf(reply, "R %zu", &picture_bytes) == 1 &&
		    picture_bytes == 0 && remote_picture_segments_count > 0) {
			/*
			 * The host already wrote the picture into the memory this frame named: the packed
			 * buffer (1: copy it out here), or the display's surface itself (2: nothing to copy).
			 */
			received = (int)((size_t)width * height * 4);
			in_place = remote_picture_row > 0 ? 2 : 1;
		} else if (sscanf(reply, "R %zu", &picture_bytes) == 1 &&
		    picture_bytes == (size_t)width * (size_t)height * 2 && remote_upload_565()) {
			uint16_t *row = malloc((size_t)width * 2);
			long began = remote_now_ms();
			int ok = row != NULL;

			for (int y = 0; y < height && ok; y++) {
				unsigned char *dst;

				if (!remote_recv_all(remote_fd, row, (size_t)width * 2)) {
					ok = 0;
					break;
				}
				if (remote_upload_fast() && remote_target_base != NULL &&
				    remote_target_width == width && remote_target_height == height &&
				    remote_target_row >= (size_t)width * 4) {
					dst = (unsigned char *)remote_target_base + remote_target_row * (size_t)y;
				} else {
					dst = remote_frame + 8 + (size_t)y * (size_t)width * 4;
				}
				expand_565_row(dst, row, width);
			}
			free(row);
			if (!ok) {
				close(remote_fd);
				remote_fd = -1;
				wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
				return 0;
			}
			{
				long took = remote_now_ms() - began;

				remote_say("hakios-remote: upload565 %zu B in %ld ms (%.1f MB/s)\n",
				    picture_bytes, took, took > 0 ? (double)picture_bytes / (double)took / 1000.0 : 0.0);
			}
			remote_ack_success(remote_fd);
			remote_frames++;
			remote_total_us += (long)(remote_now_ms() - started) * 1000.0;
			wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
			return remote_upload_fast() ? 3 : 1;
		} else if (sscanf(reply, "R %zu", &picture_bytes) == 1 &&
		    picture_bytes == scaled_bytes) {
			int ack_ms = remote_ack_effective_ms();

			if (remote_upload_fast() && remote_target_base != NULL &&
			    remote_target_width == width && remote_target_height == height &&
			    remote_target_row >= (size_t)width * 4 && picture_bytes == (size_t)width * height * 4) {
				long began = remote_now_ms();
				int ok = 1;

				for (int y = 0; y < height && ok; y++) {
					if (!remote_recv_all(remote_fd,
					    (unsigned char *)remote_target_base + remote_target_row * (size_t)y,
					    (size_t)width * 4)) {
						ok = 0;
					}
				}
				if (ok) {
					long took = remote_now_ms() - began;
					remote_say("hakios-remote: upload %zu B in %ld ms (%.1f MB/s)\n",
					    picture_bytes, took, took > 0 ? (double)picture_bytes / (double)took / 1000.0 : 0.0);
					remote_ack_success(remote_fd);
					remote_frames++;
					remote_total_us += (long)(remote_now_ms() - started) * 1000.0;
					wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
					return 3;
				}
				close(remote_fd);
				remote_fd = -1;
				wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
				return 0;
			}
			while (received < (int)picture_bytes) {
				ssize_t got = recv(remote_fd, remote_frame + 8 + received,
				    picture_bytes - (size_t)received, 0);

				if (got <= 0) {
					if (ack_ms > 0) {
						remote_ack_note_drop();
						remote_say("hakios-remote: picture timeout after %d ms (%d of %zu), "
						    "frame dropped %u/%u\n", ack_ms, received, picture_bytes,
						    remote_ack_drops, REMOTE_ACK_MAX_DROPS);
						close(remote_fd);
						remote_fd = -1;
						wire_reset(&w, remote_arena, REMOTE_ARENA_BYTES);
						return 0;
					}
					break;
				}
				received += (int)got;
			}
		}
		remote_ack_success(remote_fd);
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

/* Stage nineteen, P3: a widget surface, opcode 201 with the screen rectangle attached.  Kept apart
 * from eagl_remote_upload() so the ordinary texture path cannot grow a rectangle by accident. */
void
eagl_remote_widget_upload(GLuint texture, GLenum target, GLint internal, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const void *pixels, GLsizei row_bytes,
    GLsizei pixel_bytes, GLint x, GLint y, GLsizei rect_width, GLsizei rect_height)
{
	size_t bytes;

	if (!remote_enabled || pixels == NULL || width <= 0 || height <= 0 || row_bytes <= 0 ||
	    pixel_bytes <= 0 || rect_width <= 0 || rect_height <= 0) {
		return;
	}
	bytes = (size_t)row_bytes * (size_t)height;
	if (bytes == 0 || bytes > (24u << 20)) {
		return;
	}
	wire_open(&w, 202);
	wire_u32(&w, texture);
	wire_u32(&w, target);
	wire_i32(&w, internal);
	wire_i32(&w, width);
	wire_i32(&w, height);
	wire_u32(&w, format);
	wire_u32(&w, type);
	wire_i32(&w, row_bytes);
	wire_i32(&w, pixel_bytes);
	wire_i32(&w, x);
	wire_i32(&w, y);
	wire_i32(&w, rect_width);
	wire_i32(&w, rect_height);
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
	remote_emit_count++;
	for (unsigned index = 0; index < 16; index++) {
		GLsizei stride;
		size_t bytes;

		if (!remote_attrib_enabled[index] || remote_attribs[index].pointer == NULL) {
			continue;
		}
		/*
		 * Stage eighteen/nineteen: the first ten draws' arrays -- where do they live -- and then every
		 * five-hundredth draw the raw first eight bytes of attrib 3, which is the texcoord pair the
		 * host read as 0/4.7969, 0/nan, 6.1e28/0 at the interface.  Those bytes at the interface's
		 * own draws are what says whether CoreAnimation put texcoords there at all.
		 */
		if (remote_pointer_said < 10) {
			remote_pointer_said++;
			remote_say("hakios-remote: attrib %u pointer %p buffer %d size %d type %#x stride %d\n",
			    index, remote_attribs[index].pointer, (int)remote_attribs[index].buffer,
			    (int)remote_attribs[index].size, (unsigned)remote_attribs[index].type,
			    (int)remote_attribs[index].stride);
		}
		if (remote_emit_count % 500 == 0 && index == 3 && remote_attribs[index].pointer != NULL &&
		    remote_attribs[index].buffer == 0 &&
		    (uintptr_t)remote_attribs[index].pointer >= 0x100000000ULL) {
			/* (only a client array -- a non-zero binding means the pointer is an offset, and reading
			 * it would fault this process, which is what the first version of this diagnostic did) */
			const unsigned char *at = (const unsigned char *)remote_attribs[index].pointer;
			float f0 = 0.0f, f1 = 0.0f;
			unsigned char raw[8];

			memcpy(raw, at, sizeof(raw));
			memcpy(&f0, at, 4);
			memcpy(&f1, at + 4, 4);
			remote_say("hakios-remote: emit %u attrib 3 buffer %d stride %d raw %02x%02x%02x%02x "
			    "%02x%02x%02x%02x float %.4f %.4f\n", remote_emit_count,
			    (int)remote_attribs[index].buffer, (int)remote_attribs[index].stride,
			    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
			    (double)f0, (double)f1);
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
		GLint binding = 0;

		glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &binding);
		remote_attribs[index].pointer = pointer;
		remote_attribs[index].size = size;
		remote_attribs[index].type = type;
		remote_attribs[index].stride = stride;
		remote_attribs[index].normalized = normalized;
		remote_attribs[index].buffer = binding;
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
	/*
	 * glwire_slots=MASK: put back the slots the mask leaves out, so the guest draws through its
	 * own entry as if that recorder had never been installed. The recorded calls keep their
	 * local_entry, which is harmless -- nothing calls a trampoline that is not in the table.
	 */
	for (unsigned i = 0; i < sizeof(remote_slot_offset) / sizeof(remote_slot_offset[0]); i++) {
		if ((remote_slot_mask & (1ULL << i)) == 0) {
			table[remote_slot_offset[i]] = local_entry[i];
		}
	}
}
