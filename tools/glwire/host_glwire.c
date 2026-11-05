/* host_glwire: make CoreAnimation's GL calls on the host's GPU.
 *
 * The guest records the calls QuartzCore makes (62 of them, see spec.def) and this replays them on
 * a real context -- a Tesla V100 here -- then sends the picture back over the same socket. The guest
 * copies it into the surface its display reads; the host needs no mapping of the guest's RAM, so a
 * guest with anonymous RAM is enough.
 *
 * Each guest process that loads the library is one connection, and each connection gets its own GL
 * context and its own name maps: the guest's object names are per process, and two processes may
 * well both call their first texture 1.
 *
 *   host_glwire [port]          (default 9110)
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wire.h"
#include "names.h"

#define GLWIRE_AFTER_DRAW(g) host_after_draw(g)

#define MAX_CLIENTS 16

struct client {
	int fd;
	EGLContext context;
	EGLSurface surface;
	GLuint framebuffer;
	int width, height;
	struct glstate names;
};

static EGLDisplay display;
static EGLConfig config;
static struct client clients[MAX_CLIENTS];
static unsigned long long frames_replayed;
/* The picture of the frame before: the guest's uploaded panel is the surface as of the previous
 * present (measured: picture(N) == panel(N+1) for every animating frame of a --no-panel run), so
 * the acceptance that "the host drew what the guest drew" is read against this one. */
static uint64_t previous_picture_hash;
static int have_previous_picture;
/* GLWIRE_NO_INPLACE: take the picture's segments but do not write through them (the reply then
 * carries the picture, as it did before). One process's frames are one experiment. */
static int no_in_place;
/* GLWIRE_NO_PANEL (or --no-panel): do not draw the panel the guest uploaded -- replay the
 * recorded calls instead. The question it answers is whether the host can produce the frame
 * from CoreAnimation's own stream, which is what the guest would have to stop doing. */
static int no_panel;
/* A picture of the last frame's record stream: which opcodes it held, and how many (the
 * GLWIRE_NO_PANEL path logs one line per frame so a displayed stream can be read). */
static unsigned long long replay_ops[256];
static int replay_used;
/* GLWIRE_DUMP_DRAWS: after every draw of one chosen frame, save the framebuffer and print the
 * state the draw saw. One frame of a home screen is nine draws; the pictures say which draw put a
 * wrong pixel down, and the state says why. */
static int dump_draws;
static int draws_seen;
static int dump_draws_runtime;
static int dump_frame_runtime;
static int trace_width, trace_height;
static struct client *trace_client;
static const char *save_dir;
/* The guest's physical memory: ram.img, mapped so a texture record can name its pixels instead of
 * carrying them, and so the picture can be written back into the guest's own buffer. */
static unsigned char *ram_memory;
static size_t ram_bytes;
static uint64_t ram_pa_base = 0x40000000;
static int ram_writable;

static int
map_ram(const char *path)
{
	struct stat information;
	int fd;

	fd = open(path, O_RDWR);
	ram_writable = fd >= 0;
	if (fd < 0) {
		fd = open(path, O_RDONLY);
	}
	if (fd < 0) {
		fprintf(stderr, "host_glwire: cannot open %s: %m\n", path);
		return 0;
	}
	if (fstat(fd, &information) != 0 || information.st_size <= 0) {
		fprintf(stderr, "host_glwire: cannot size %s\n", path);
		close(fd);
		return 0;
	}
	ram_memory = mmap(NULL, (size_t)information.st_size,
	    ram_writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (ram_memory == MAP_FAILED) {
		ram_memory = NULL;
		fprintf(stderr, "host_glwire: cannot map %s: %m\n", path);
		return 0;
	}
	ram_bytes = (size_t)information.st_size;
	wire_set_ram(ram_memory, ram_bytes, ram_pa_base);
	printf("host_glwire: %s mapped, %zu bytes, physical base 0x%llx%s\n", path, ram_bytes,
	    (unsigned long long)ram_pa_base, ram_writable ? "" : " (read-only)");
	return 1;
}

static double now_seconds(void);

/* Pixels to a file, and a line saying so; nothing if the file cannot be made. */
static int
write_pixels(const char *path, const void *pixels, size_t bytes)
{
	FILE *file = fopen(path, "wb");

	if (file == NULL) {
		return 0;
	}
	fwrite(pixels, 1, bytes, file);
	fclose(file);
	printf("host_glwire: wrote %s\n", path);
	return 1;
}

/* A byte-for-byte copy of what the host drew, for the acceptance check against the guest's own
 * pixels: the panel is RGBA, the picture BGRA. GLWIRE_DUMP=<prefix> writes them once.
 *
 * GLWIRE_DUMP_PAIR=1: the frame after that one as well, as <prefix>-n1-panel-WxH.rgba and
 * <prefix>-n1-picture-WxH.bgra, once. The panel the guest uploads is the surface as of the
 * *previous* present -- picture(N) == panel(N+1), see previous_picture_hash -- so comparing what
 * the host drew with what the guest showed takes a pair of frames, not one. The pair is written
 * with the next frame that arrives after the dump and then never again. */
static void
dump_once(struct client *client, const unsigned char *picture, int width, int height, int number)
{
	static int dumped, paired;
	static double first;
	const char *prefix = getenv("GLWIRE_DUMP");
	const char *from = getenv("GLWIRE_DUMP_FROM");
	const char *at = getenv("GLWIRE_DUMP_AT");
	const char *pair = getenv("GLWIRE_DUMP_PAIR");
	char path[512];

	if (first == 0) {
		first = now_seconds();
	}
	/* The frame after the one dumped: the guest's panel of it beside the host's picture of it. */
	if (dumped && !paired && prefix != NULL && pair != NULL && strtoul(pair, NULL, 10) != 0) {
		paired = 1;
		if (client->names.panel != NULL) {
			snprintf(path, sizeof(path), "%s-n1-panel-%dx%d.rgba", prefix, width, height);
			write_pixels(path, client->names.panel, (size_t)width * height * 4);
		}
		snprintf(path, sizeof(path), "%s-n1-picture-%dx%d.bgra", prefix, width, height);
		write_pixels(path, picture, (size_t)width * height * 4);
		fflush(stdout);
	}
	/* GLWIRE_DUMP_AT=<seconds>: the first frame that arrives that long after the first one, which is
	 * how a dump is aimed at a moment (the interface is up eleven minutes into a boot here) rather
	 * than at a frame number that no two boots share. */
	if (at != NULL && now_seconds() - first < atof(at)) {
		return;
	}
	if (dumped || prefix == NULL || (at == NULL && number < (from != NULL ? atoi(from) : 1))) {
		return;
	}
	dumped = 1;
	if (client->names.panel != NULL) {
		snprintf(path, sizeof(path), "%s-panel-%dx%d.rgba", prefix, width, height);
		write_pixels(path, client->names.panel, (size_t)width * height * 4);
	}
	snprintf(path, sizeof(path), "%s-picture-%dx%d.bgra", prefix, width, height);
	write_pixels(path, picture, (size_t)width * height * 4);
	fflush(stdout);
}

static double
now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint64_t
fnv_update(uint64_t hash, const unsigned char *bytes, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		hash ^= bytes[i];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/* The panel upload is RGBA (names.h swizzled it); the read-back picture is BGRA. Hash the same
 * logical (R, G, B, A) order so a match means the two pictures are identical. */
static uint64_t
hash_panel(const unsigned char *pixels, size_t count)
{
	return fnv_update(0xcbf29ce484222325ULL, pixels, count * 4);
}

static uint64_t
hash_picture(const unsigned char *pixels, size_t count)
{
	uint64_t hash = 0xcbf29ce484222325ULL;

	for (size_t i = 0; i < count; i++) {
		unsigned char rgba[4] = { pixels[i * 4 + 2], pixels[i * 4 + 1], pixels[i * 4], pixels[i * 4 + 3] };

		hash = fnv_update(hash, rgba, 4);
	}
	return hash;
}

static int
send_all(int fd, const void *data, size_t bytes)
{
	const char *p = data;

	while (bytes > 0) {
		ssize_t sent = send(fd, p, bytes, 0);

		if (sent <= 0) {
			return 0;
		}
		p += sent;
		bytes -= (size_t)sent;
	}
	return 1;
}

static int
recv_all(int fd, void *data, size_t bytes)
{
	char *p = data;

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
make_client_current(struct client *client)
{
	if (client->context == EGL_NO_CONTEXT) {
		client->context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
		client->surface = eglCreatePbufferSurface(display, config,
		    (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });
	}
	if (client->context == EGL_NO_CONTEXT || client->surface == EGL_NO_SURFACE) {
		return 0;
	}
	return eglMakeCurrent(display, client->surface, client->surface, client->context);
}

static void
make_frame_buffer(struct client *client, int width, int height)
{
	GLuint rb = 0;

	if (client->framebuffer != 0) {
		glDeleteFramebuffers(1, &client->framebuffer);
		client->framebuffer = 0;
	}
	glGenFramebuffers(1, &client->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, client->framebuffer);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	client->width = width;
	client->height = height;
}

/* The panel-sized texture eagl.c uploaded is the picture the guest composed. Draw it as a quad;
 * the CA draw stream's framebuffer state is not needed (and llvmpipe faulted replaying it after
 * the upload). */
static void
draw_panel(struct client *client, const unsigned char *panel, int width, int height)
{
	GLuint texture = 0;

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, panel);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_TEXTURE_2D);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
	glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, -1.0f);
	glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, 1.0f);
	glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, 1.0f);
	glEnd();
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &texture);
}

/*
 * Twelfth stage, and the reason the home screen had no app labels: CoreAnimation uploads its glyph
 * atlases as GL_ALPHA (0x1906) textures -- 149x90, 152x90, ... -- and a desktop GL context here takes
 * that as an internal format not at all (GL_INVALID_OPERATION, 0x502: frames of the interface replay
 * with it).  The guest's GL ES accepted the call and drew nothing with it on this side, so the text
 * simply was not there.  Expand the three legacy formats to RGBA on the way in; the alpha becomes the
 * alpha, luminance its three channels.
 */
static int
host_legacy_teximage(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
	enum { LEGACY_ALPHA = 0x1906, LEGACY_LUMINANCE = 0x1909, LEGACY_LUMINANCE_ALPHA = 0x190a };
	unsigned char *expanded;
	size_t count, i;
	int kind;
	static unsigned said;

	if (level != 0 || type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0) {
		return 0;
	}
	/*
	 * Off by default: with it on, the eight frames a boot replays with GL_INVALID_OPERATION (0x502,
	 * the upload of a legacy format this context will not take) go through, but what the screen shows
	 * of it is the widget boxes losing their rounded corners -- 14,934 pixels, measured -- and the app
	 * labels are unchanged, so this is not the missing piece.  GLWIRE_LEGACY_FORMATS=1 turns it on for
	 * whoever picks this thread up.
	 */
	{
		static int wanted = -1;

		if (wanted < 0) {
			const char *value = getenv("GLWIRE_LEGACY_FORMATS");

			wanted = value != NULL && value[0] == '1';
		}
		if (!wanted) {
			return 0;
		}
	}
	if (internal == LEGACY_ALPHA && format == LEGACY_ALPHA) {
		kind = 1;
	} else if (internal == LEGACY_LUMINANCE && format == LEGACY_LUMINANCE) {
		kind = 2;
	} else if (internal == LEGACY_LUMINANCE_ALPHA && format == LEGACY_LUMINANCE_ALPHA) {
		kind = 3;
	} else {
		return 0;
	}
	count = (size_t) width * (size_t) height;
	expanded = malloc(count * 4);
	if (expanded == NULL) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		const unsigned char *source = pixels != NULL ? (const unsigned char *) pixels : NULL;
		unsigned char luma = 0, alpha = 0;

		if (source != NULL) {
			if (kind == 1) {
				alpha = source[i];
			} else if (kind == 2) {
				luma = source[i];
			} else {
				luma = source[2 * i];
				alpha = source[2 * i + 1];
			}
		}
		/*
		 * GL_ALPHA samples as (0, 0, 0, a) on the guest's GL, and CoreAnimation's mask shaders read the
		 * three channels as well as the alpha: expanding it to white turned every rounded corner into a
		 * square (measured: 14,934 pixels of the widget boxes changed).  Keep the legacy sample.
		 */
		expanded[4 * i + 0] = kind == 1 ? 0 : luma;
		expanded[4 * i + 1] = kind == 1 ? 0 : luma;
		expanded[4 * i + 2] = kind == 1 ? 0 : luma;
		expanded[4 * i + 3] = kind == 1 ? alpha : (kind == 2 ? 255 : alpha);
	}
	if (said < 8) {
		said++;
		printf("glwire: legacy texture %dx%d internal %#x format %#x: expanded to RGBA\n", (int) width, (int) height,
		    internal, format);
		fflush(stdout);
	}
	glTexImage2D(target, level, GL_RGBA8, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, expanded);
	free(expanded);
	return 1;
}

/*
 * Stage thirteen, the experiment that tells whether the text's colour really is the 64x19 texture:
 * GLWIRE_FILL=white replaces an empty 64x19 RGBA8 allocation with white pixels.  If the labels appear,
 * that texture is the colour side; if they do not, it is not what the text shader is missing.
 */
static unsigned char *
host_fill_special(GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	const char *what = getenv("GLWIRE_FILL");
	unsigned char *fill;
	size_t bytes = (size_t) width * (size_t) height * 4;

	if (what == NULL || pixels != NULL || format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
		return NULL;
	}
	if (width != 64 || height != 19) {
		return NULL;
	}
	fill = malloc(bytes);
	if (fill != NULL) {
		memset(fill, what[0] == 'w' ? 0xff : 0x40, bytes);
		if (what[0] == 'r') {
			for (size_t i = 0; i < bytes; i += 4) {
				fill[i] = 0xff;
			}
		}
	}
	return fill;
}

static void host_dump_uniforms(struct glstate *g, const char *tag);

/*
 * Twelfth stage, the question the texture dump leaves open: are the glyph tiles (184x90, 136x42, ...)
 * ever sampled by a draw?  The replay's draw carries no texture in its signature -- the binding does --
 * so this asks the host which textures are bound to the first eight units at the moment of a draw, and
 * which guest name each one is.  GLWIRE_DRAW_LOG=1 writes the first 300 of them.
 */
static void
host_note_draw(struct glstate *g, const char *how, GLenum mode, GLsizei count)
{
	static unsigned logged;
	GLint program = 0, active = 0, glyph_height = 0;

	if (logged >= 20000 || getenv("GLWIRE_DRAW_LOG") == NULL) {
		return;
	}
	logged++;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	{
		GLint viewport[4] = { 0, 0, 0, 0 };
		GLint framebuffer = 0;

		GLint scissor[4] = { 0, 0, 0, 0 };

		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		glGetIntegerv(GL_SCISSOR_BOX, scissor);
		printf("glwire: draw#%u %s mode %#x count %d program %d (guest %u) fbo %d viewport %d,%d %dx%d "
		    "scissor %s %d,%d %dx%d", logged, how, mode, (int) count, (int) program,
		    (unsigned) g->current_program, (int) framebuffer, (int) viewport[0], (int) viewport[1],
		    (int) viewport[2], (int) viewport[3], glIsEnabled(GL_SCISSOR_TEST) ? "on" : "off",
		    (int) scissor[0], (int) scissor[1], (int) scissor[2], (int) scissor[3]);
	}
	for (unsigned unit = 0; unit < 8; unit++) {
		GLint bound = 0, height = 0;

		glActiveTexture(GL_TEXTURE0 + unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		if (bound == 0) {
			continue;
		}
		printf(" u%u:host%u", unit, (unsigned) bound);
		for (size_t i = 0; i < g->textures.capacity; i++) {
			struct name_entry *entry = &g->textures.entries[i];

			if (entry->state == 1 && (GLuint) entry->host == (GLuint) bound) {
				printf("(guest%llu)", (unsigned long long) entry->guest);
				break;
			}
		}
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		if (height == 90 || height == 42) {
			glyph_height = height;
		}
	}
	printf("\n");
	fflush(stdout);
	glActiveTexture((GLenum) active);
	/* (stage thirteen: a draw that binds a glyph tile is a text draw -- say what its programme holds) */
	if (glyph_height != 0) {
		host_dump_uniforms(g, "text-draw");
	}
}

/*
 * Twelfth stage, the last stretch: the text drawer's colour side is a 64x19 RGBA8 texture that arrives
 * at the host as zeros.  This says how it is made -- GLWIRE_WATCH=64x19 names the size, and every
 * upload of a texture that size is said with the guest name, the format, whether the pixels came at
 * all and a hash of them, so an empty upload (or none) is told from an upload whose bytes are black.
 */
static void
host_watch_upload(struct glstate *g, const char *how, GLuint guest, GLint level, GLint internal, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const void *pixels, GLsizei bytes)
{
	const char *watch = getenv("GLWIRE_WATCH");
	char want[32];
	static unsigned said;
	uint32_t hash = 2166136261u;
	const unsigned char *data = pixels;

	if (watch == NULL || level != 0 || said >= 40 || bytes < 0) {
		return;
	}
	snprintf(want, sizeof(want), "%dx%d", (int) width, (int) height);
	if (strcmp(watch, want) != 0) {
		return;
	}
	said++;
	for (GLsizei i = 0; data != NULL && i < bytes; i++) {
		hash = (hash ^ data[i]) * 16777619u;
	}
	printf("glwire: watch %s host%u %dx%d internal %#x format %#x type %#x %s %d B hash %08x first %02x%02x%02x%02x\n",
	    how, (unsigned) guest, (int) width, (int) height, internal, format, type,
	    pixels != NULL ? "pixels" : "no-pixels", bytes, hash,
	    data != NULL && bytes >= 1 ? data[0] : 0, data != NULL && bytes >= 2 ? data[1] : 0,
	    data != NULL && bytes >= 3 ? data[2] : 0, data != NULL && bytes >= 4 ? data[3] : 0);
	fflush(stdout);
	(void) g;
}

/*
 * Stage thirteen, P1: where the text drawer's colour is.  Two lines of evidence --
 * GLWIRE_UNIFORM_LOG=1 says every glUniform the stream carries (guest location, host location, value)
 * and GLWIRE_UNIFORM_PROGRAM=<guest programme> makes each of that programme's draws say the uniforms
 * its host programme actually holds, so a value that arrives on the wrong location (or none) is seen.
 */
static void
host_log_uniform(struct glstate *g, const char *how, GLint guest_location, GLint host_location,
    const GLfloat *values, int count)
{
	static unsigned said;

	if (getenv("GLWIRE_UNIFORM_LOG") == NULL || said >= 300) {
		return;
	}
	said++;
	printf("glwire: uniform %s programme(guest) %u location guest %d host %d value", how,
	    (unsigned) g->current_program, (int) guest_location, (int) host_location);
	for (int i = 0; i < count; i++) {
		printf(" %.4f", (double) values[i]);
	}
	printf("\n");
	fflush(stdout);
}

static void
host_dump_uniforms(struct glstate *g, const char *tag)
{
	const char *wanted = getenv("GLWIRE_UNIFORM_PROGRAM");
	static unsigned said;
	GLint count = 0, max_length = 0;
	GLuint program = g->current_host_program;

	if (wanted == NULL || program == 0 || said >= 60) {
		return;
	}
	{
		unsigned long named = strtoul(wanted, NULL, 10);

		/* (the draw log names host programmes; either number is accepted, so the two can be matched) */
		if (named != (unsigned long) g->current_program && named != (unsigned long) program) {
			return;
		}
	}
	said++;
	glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
	glGetProgramiv(program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max_length);
	printf("glwire: %s programme(guest) %u host %u active %d uniforms:", tag, (unsigned) g->current_program,
	    (unsigned) program, (int) count);
	for (GLint i = 0; i < count && i < 16; i++) {
		char name[128] = "?";
		GLint size = 0, location = -1;
		GLenum type = 0;
		GLfloat value[16] = { 0 };

		glGetActiveUniform(program, (GLuint) i, (GLsizei) sizeof(name), NULL, &size, &type, name);
		location = glGetUniformLocation(program, name);
		if (location >= 0) {
			glGetUniformfv(program, location, value);
		}
		printf(" %s(loc %d)=%.3f,%.3f,%.3f,%.3f", name, (int) location, (double) value[0],
		    (double) value[1], (double) value[2], (double) value[3]);
	}
	printf("\n");
	fflush(stdout);
}

static void host_after_draw(struct glstate *g);

static int
dispatch(struct glstate *g, wire_buffer *r, uint32_t opcode)
{
	const char *watch_wanted = getenv("GLWIRE_WATCH");

#include "dispatch.inc"
	return 0;
}


/* The state a draw saw, and the framebuffer as it stood after it. GLWIRE_DUMP_DRAWS=<prefix>
 * writes <prefix>-draw-N-fbo-F-program-P.bgra (bottom row first, as glReadPixels returns it). */
static void
host_after_draw(struct glstate *g)
{
	GLint bound = 0, view[4] = { 0, 0, 0, 0 }, scissor[4] = { 0, 0, 0, 0 };
	GLint program = 0, active = 0, blend[2] = { 0, 0 }, equation = 0;
	const char *prefix = getenv("GLWIRE_DUMP_DRAWS");
	int number = draws_seen++;
	unsigned char *pixels;

	if (!dump_draws || prefix == NULL || trace_client == NULL || trace_width <= 0 || trace_height <= 0) {
		return;
	}
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
	glGetIntegerv(GL_VIEWPORT, view);
	glGetIntegerv(GL_SCISSOR_BOX, scissor);
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	printf("host_glwire: draw %d fbo %d program %d viewport %d,%d %dx%d scissor %d,%d %dx%d "
	    "blend %s %d %d eq %#x", number, bound, program, view[0], view[1], view[2], view[3],
	    scissor[0], scissor[1], scissor[2], scissor[3], glIsEnabled(GL_BLEND) ? "on" : "off",
	    blend[0], blend[1], equation);
	for (GLint unit = 0; unit < 8; unit++) {
		GLint texture = 0, width = 0, height = 0, filter = 0, wrap = 0;

		glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		if (texture == 0) {
			continue;
		}
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap);
		printf(" | unit %d texture %d %dx%d min %#x wrap %#x", unit, texture, width, height,
		    filter, wrap);
	}
	glActiveTexture((GLenum)active);
	printf("\n");
	fflush(stdout);
	if (bound == 0) {
		return;
	}
	pixels = malloc((size_t)trace_width * (size_t)trace_height * 4);
	if (pixels == NULL) {
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
	glReadPixels(0, 0, trace_width, trace_height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	{
		char path[512];
		FILE *file;

		snprintf(path, sizeof(path), "%s-draw-%02d-fbo-%d-program-%d.bgra", prefix, number, bound,
		    program);
		file = fopen(path, "wb");
		if (file != NULL) {
			fwrite(pixels, 1, (size_t)trace_width * (size_t)trace_height * 4, file);
			fclose(file);
			printf("host_glwire: wrote %s\n", path);
			fflush(stdout);
		}
	}
	free(pixels);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
}

static void
reply(struct client *client, const char *line)
{
	send_all(client->fd, line, strlen(line));
}

/*
 * The picture's rows into a buffer that is wider than the picture: the display's own IOSurface has
 * 3072 bytes to a row for a 3000-byte row of picture, and the guest would otherwise copy the whole
 * picture into it after the frame (48 ms of a 77 ms frame). The segments cover the strided range,
 * in order, so a cursor walks them as the rows go by.
 */
struct guest_cursor {
	wire_segment *segments;
	int count;
	int index;
	size_t at;              /* the guest offset segments[index] starts at */
};

static unsigned char *
cursor_at(struct guest_cursor *cursor, size_t offset, size_t *available)
{
	while (cursor->index < cursor->count &&
	    cursor->at + cursor->segments[cursor->index].len <= offset) {
		cursor->at += cursor->segments[cursor->index].len;
		cursor->index++;
	}
	if (cursor->index >= cursor->count || offset < cursor->at) {
		return NULL;
	}
	*available = (size_t)cursor->segments[cursor->index].len - (offset - cursor->at);
	return ram_memory + (size_t)(cursor->segments[cursor->index].pa - ram_pa_base) +
	    (offset - cursor->at);
}

/* Stage 11, P1''''/P0'''': is what the host drew what the guest's display surface now holds? The
 * same five rows and five pixels a row are sampled on both sides -- here from the picture (packed,
 * `used` bytes a row) and from the guest's RAM (strided by row_bytes, through the same segments the
 * write used); the guest samples its own surface the same way (userland/src/eagl.c,
 * surface_row_hash). Equal hashes mean the rows landed; different ones say the picture never got
 * there, and which of the two it is can be answered from one pair of logs. */
static uint32_t
sample_row_hash(const unsigned char *row, size_t used)
{
	uint32_t hash = 2166136261u;

	if (row == NULL || used < 4) {
		return hash;
	}
	for (int i = 0; i < 5; i++) {
		size_t pixel = ((used / 4) - 1) * (size_t)i / 4 * 4;

		for (int b = 0; b < 4; b++) {
			hash = (hash ^ row[pixel + (size_t)b]) * 16777619u;
		}
	}
	return hash;
}

static void
stage11_report(const char *which, const unsigned char *picture, int width, int height, size_t row_bytes,
    wire_segment *segments, int count, unsigned long long frame)
{
	uint32_t picture_hash[5], surface_hash[5];
	int rows[5];
	uint32_t first[2] = { 0, 0 };

	rows[0] = 0;
	rows[1] = height / 4;
	rows[2] = height / 2;
	rows[3] = (3 * height) / 4;
	rows[4] = height - 1;
	for (int i = 0; i < 5; i++) {
		struct guest_cursor cursor = { segments, count, 0, 0 };
		size_t available = 0;
		unsigned char *where = cursor_at(&cursor, (size_t)rows[i] * row_bytes, &available);

		picture_hash[i] = sample_row_hash(picture + (size_t)rows[i] * (size_t)width * 4,
		    (size_t)width * 4);
		surface_hash[i] = where != NULL && available >= (size_t)width * 4
		    ? sample_row_hash(where, (size_t)width * 4) : 0;
		if (i == 2 && where != NULL && available >= (size_t)width * 4) {
			memcpy(first, where, sizeof(first));
		}
	}
	printf("host_glwire: stage11 %s frame %llu surface %zu B, %zu B a row: picture %08x %08x %08x "
	    "%08x %08x surface %08x %08x %08x %08x %08x px %08x %08x\n", which, frame,
	    row_bytes * (size_t)height, row_bytes, picture_hash[0], picture_hash[1], picture_hash[2],
	    picture_hash[3], picture_hash[4], surface_hash[0], surface_hash[1], surface_hash[2],
	    surface_hash[3], surface_hash[4], first[0], first[1]);
	fflush(stdout);
}

static int
write_picture_rows(const unsigned char *picture, int width, int height, size_t row_bytes,
    wire_segment *segments, int count)
{
	struct guest_cursor cursor = { segments, count, 0, 0 };
	size_t used = (size_t)width * 4;

	for (int y = 0; y < height; y++) {
		size_t offset = (size_t)y * row_bytes;
		size_t done = 0;

		while (done < used) {
			size_t available = 0;
			unsigned char *destination = cursor_at(&cursor, offset + done, &available);

			if (destination == NULL) {
				return 0;
			}
			if (available > used - done) {
				available = used - done;
			}
			memcpy(destination, picture + (size_t)y * used + done, available);
			done += available;
		}
	}
	return 1;
}


/* GLWIRE_DUMP_TEXTURES=<prefix>: every host texture after the frame's upload pass, with the
 * parameters the replay will sample it with. The files are level 0 as RGBA; the small text line
 * beside each says which guest name made it and how it is filtered and wrapped. */
static void
dump_textures(struct client *client, const char *prefix)
{
	GLint active = 0, previous = 0;

	if (prefix == NULL) {
		return;
	}
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	for (size_t i = 0; i < client->names.textures.capacity; i++) {
		struct name_entry *entry = &client->names.textures.entries[i];
		GLint width = 0, height = 0, internal = 0, min_filter = 0, mag_filter = 0;
		GLint wrap_s = 0, wrap_t = 0;
		unsigned char *pixels;
		char path[512];
		FILE *file, *meta;

		if (entry->state != 1) {
			continue;
		}
		glBindTexture(GL_TEXTURE_2D, (GLuint)entry->host);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);
		snprintf(path, sizeof(path), "%s-guest-%llu-host-%llu-%dx%d.rgba", prefix,
		    (unsigned long long)entry->guest, (unsigned long long)entry->host, width, height);
		meta = fopen(path, "ab");
		if (meta != NULL) {
			fprintf(meta, "guest %llu host %llu %dx%d internal %#x min %#x mag %#x wrap %#x %#x\n",
			    (unsigned long long)entry->guest, (unsigned long long)entry->host, width, height,
			    internal, min_filter, mag_filter, wrap_s, wrap_t);
			fclose(meta);
		}
		if (width <= 0 || height <= 0 || (size_t)width * (size_t)height * 4 > (64u << 20)) {
			continue;
		}
		pixels = malloc((size_t)width * (size_t)height * 4);
		if (pixels == NULL) {
			continue;
		}
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		snprintf(path, sizeof(path), "%s-guest-%llu-host-%llu-%dx%d.pixels", prefix,
		    (unsigned long long)entry->guest, (unsigned long long)entry->host, width, height);
		file = fopen(path, "wb");
		if (file != NULL) {
			fwrite(pixels, 1, (size_t)width * (size_t)height * 4, file);
			fclose(file);
		}
		free(pixels);
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
	glActiveTexture((GLenum)active);
	printf("host_glwire: textures written with prefix %s\n", prefix);
	fflush(stdout);
}

/* One frame: "R <w> <h> <records bytes> <picture bytes>\n", the records, then the picture:
 * "R <picture bytes>\n" and the BGRA pixels. */
static int
serve_frame(struct client *client)
{
	char line[192];
	int at = 0;
	int width = 0, height = 0, segment_count = 0, row_bytes = 0;
	/* P1'''': a frame may name a second panel-sized surface (the display presents two in turn). */
	int segment_count2 = 0, row_bytes2 = 0;
	size_t records_bytes = 0, frame_bytes = 0;
	double began, took;
	unsigned char *records;
	unsigned char *picture;

	wire_segment *segments = NULL;
	wire_segment *segments2 = NULL;
	int in_place = 0;
	int replayed = 0;
	/* Taken before the frame's release frees the panel the guest uploaded: the release runs
	 * first, so the log line used to read the panel afterwards and find nothing. */
	uint64_t panel_hash = 0, picture_hash = 0;
	uint64_t last_picture_hash = have_previous_picture ? previous_picture_hash : 0;
	int panel_known = 0;

	while (at < (int)sizeof(line) - 1) {
		char c;
		ssize_t got = recv(client->fd, &c, 1, 0);

		if (got != 1) {
			return -1;
		}
		if (c == '\n') {
			break;
		}
		line[at++] = c;
	}
	line[at] = '\0';
	if (sscanf(line, "R %d %d %zu %zu %d %d %d %d", &width, &height, &records_bytes, &frame_bytes,
	    &segment_count, &row_bytes, &segment_count2, &row_bytes2) < 4) {
		fprintf(stderr, "host_glwire: bad request '%s'\n", line);
		return -1;
	}
	if (width <= 0 || height <= 0 || (size_t)width * (size_t)height * 4 != frame_bytes) {
		reply(client, "R badsize\n");
		return 0;
	}
	if (segment_count < 0 || segment_count > 2048 || segment_count2 < 0 || segment_count2 > 2048) {
		reply(client, "R badsize\n");
		return 0;
	}
	if ((row_bytes != 0 && (row_bytes < width * 4 || row_bytes > (1 << 20))) ||
	    (row_bytes2 != 0 && (row_bytes2 < width * 4 || row_bytes2 > (1 << 20)))) {
		reply(client, "R badsize\n");
		return 0;
	}
	segments = segment_count > 0 ? calloc((size_t)segment_count, sizeof(*segments)) : NULL;
	if (segment_count > 0 && (segments == NULL ||
	    !recv_all(client->fd, segments, (size_t)segment_count * sizeof(*segments)))) {
		free(segments);
		free(segments2);
		return -1;
	}
	segments2 = segment_count2 > 0 ? calloc((size_t)segment_count2, sizeof(*segments2)) : NULL;
	if (segment_count2 > 0 && (segments2 == NULL ||
	    !recv_all(client->fd, segments2, (size_t)segment_count2 * sizeof(*segments2)))) {
		free(segments);
		free(segments2);
		return -1;
	}
	records = malloc(records_bytes);
	picture = malloc(frame_bytes);
	if (records == NULL || picture == NULL) {
		free(records);
		free(picture);
		free(segments);
		free(segments2);
		reply(client, "R nomem\n");
		return 0;
	}
	if (!recv_all(client->fd, records, records_bytes)) {
		free(records);
		free(picture);
		free(segments);
		free(segments2);
		return -1;
	}
	began = now_seconds();
	{
		/*
		 * GLWIRE_SAVE_RECORDS=<prefix>: the first frames' record streams as they came, so a
		 * frame's calls can be read one by one on their own (opcode, length, payload). The
		 * header is the request line's numbers, so a reader does not have to remember them.
		 */
		static int saved;
		const char *prefix = getenv("GLWIRE_SAVE_RECORDS");
		const char *at = getenv("GLWIRE_SAVE_AT");
		int wanted = at != NULL ? atoi(at) : 0;
		int keep = at != NULL ? ((int)(frames_replayed + 1) == wanted) : (saved < 3);

		if (prefix != NULL && keep) {
			char path[512];
			FILE *file;

			snprintf(path, sizeof(path), "%s-%d-%dx%d-%d.records", prefix,
			    at != NULL ? wanted : saved + 1, width, height, (int)records_bytes);
			file = fopen(path, "wb");
			if (file != NULL) {
				fwrite(records, 1, records_bytes, file);
				fclose(file);
				printf("host_glwire: saved %s\n", path);
				fflush(stdout);
			}
			saved++;
		}
	}
	wire_image_refs = 0;
	wire_image_ref_bytes = 0;
	if (!make_client_current(client)) {
		free(records);
		free(picture);
		free(segments);
		free(segments2);
		reply(client, "R nocontext\n");
		return 0;
	}
	if (client->framebuffer == 0 || client->width != width || client->height != height) {
		make_frame_buffer(client, width, height);
	}
	/*
	 * Uploads first, with no framebuffer bound: redefining a texture that an FBO has attached
	 * corrupts llvmpipe's state tracker (the crash came a few calls later, in glClearColor).
	 * The stream's later glGenTextures must not replace the uploaded texture (names_*_put_new).
	 */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	{
		wire_buffer upload;
		size_t offset = 0;

		while (offset + 8 <= records_bytes) {
			uint32_t opcode, length;

			memcpy(&opcode, records + offset, 4);
			memcpy(&length, records + offset + 4, 4);
			if (offset + 8 + length > records_bytes) {
				break;
			}
			if (opcode == 201) {
				upload.base = records + offset + 8;
				upload.capacity = length;
				upload.at = 0;
				dispatch(&client->names, &upload, opcode);
			}
			offset += 8 + length;
		}
	}
	{
		const char *from = getenv("GLWIRE_DUMP_FROM");

		trace_width = width;
		trace_height = height;
		trace_client = client;
		draws_seen = 0;
		dump_frame_runtime = from != NULL ? atoi(from) : 1;
		dump_draws_runtime = (int)(frames_replayed + 1) == dump_frame_runtime;
		dump_draws = getenv("GLWIRE_DUMP_DRAWS") != NULL && dump_draws_runtime;
	}
	if (dump_draws_runtime && (int)(frames_replayed + 1) == dump_frame_runtime) {
		dump_textures(client, getenv("GLWIRE_DUMP_TEXTURES"));
	}
	glBindFramebuffer(GL_FRAMEBUFFER, client->framebuffer);
	glViewport(0, 0, width, height);
	if (!no_panel && client->names.panel != NULL && client->names.panel_width == width &&
	    client->names.panel_height == height) {
		draw_panel(client, client->names.panel, width, height);
	} else {
		wire_buffer r;
		size_t offset = 0;

		memset(replay_ops, 0, sizeof(replay_ops));
		client->names.bound_framebuffer = client->framebuffer;
		client->names.drawn_framebuffer = 0;
		replay_used = 1;
		replayed = 1;
		while (offset + 8 <= records_bytes) {
			uint32_t opcode, length;

			memcpy(&opcode, records + offset, 4);
			memcpy(&length, records + offset + 4, 4);
			if (offset + 8 + length > records_bytes) {
				fprintf(stderr, "host_glwire: a record runs past the frame\n");
				break;
			}
			if (opcode >= 256) {
				fprintf(stderr, "host_glwire: opcode %u out of range\n", opcode);
				break;
			}
			if (opcode == 201) {
				offset += 8 + length;
				continue;       /* already uploaded before the framebuffer was bound */
			}
			replay_ops[opcode]++;
			r.base = records + offset + 8;
			r.capacity = length;
			r.at = 0;
			if (dispatch(&client->names, &r, opcode) != 0) {
				fprintf(stderr, "host_glwire: dispatch stopped at opcode %u\n", opcode);
				break;
			}
			offset += 8 + length;
		}
	}
	glFinish();
	if (replay_used) {
		/*
		 * What the replay was made of: the opcodes and how many of each. A frame that draws
		 * nothing is then a stream with no draws in it, and a frame that draws wrong is a
		 * stream with the wrong ones -- either way, this line says which.
		 */
		char histogram[512];
		int at = 0;

		histogram[0] = '\0';
		for (unsigned i = 0; i < 256; i++) {
			if (replay_ops[i] != 0 && at < (int)sizeof(histogram) - 16) {
				at += snprintf(histogram + at, sizeof(histogram) - (size_t)at, " %u:%llu",
				    i, replay_ops[i]);
			}
		}
		printf("host_glwire: replay %zu records B, opcodes%s, glerror 0x%x\n",
		    records_bytes, histogram, glGetError());
		fflush(stdout);
		replay_used = 0;
	}
	{
		/*
		 * Where the picture comes from: the replay leaves whatever framebuffer the stream bound
		 * last, and glReadPixels reads that one -- not the one this renderer made. CoreAnimation
		 * binds the framebuffer of the surface it is about to draw, so the line says whether the
		 * read-back target is the guest's surface (a nonzero name this side mapped) or 0 (the
		 * EGL surface, which is a 16x16 pbuffer here and holds nothing).
		 */
		GLint bound = -1, box[4] = { 0, 0, 0, 0 };
		GLint kind = 0;

		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
		glGetIntegerv(GL_VIEWPORT, box);
		if (bound != 0) {
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
		}
		printf("host_glwire: read-back framebuffer %d (attachment %s), viewport %d,%d %dx%d, "
		    "last drawn into %u\n", bound, bound == 0 ? "none" : kind == GL_TEXTURE ? "texture" :
		    kind == GL_RENDERBUFFER ? "renderbuffer" : "other", box[0], box[1], box[2], box[3],
		    client->names.drawn_framebuffer);
		/*
		 * And read the picture out of the framebuffer the stream last drew into: the guest reads
		 * its own picture back from the texture that was drawn into, not from "whatever was bound
		 * when the frame ended" (CoreAnimation unbinds before the flush: measured framebuffer 0,
		 * attachment none, viewport 0x0 -- the EGL surface is a 16x16 pbuffer and holds nothing).
		 */
		if (replayed && client->names.drawn_framebuffer != 0 &&
		    client->names.drawn_framebuffer != (GLuint)bound) {
			glBindFramebuffer(GL_FRAMEBUFFER, client->names.drawn_framebuffer);
			glGetIntegerv(GL_VIEWPORT, box);
			printf("host_glwire: reading from %u instead, viewport %d,%d %dx%d\n",
			    client->names.drawn_framebuffer, box[0], box[1], box[2], box[3]);
		}
		fflush(stdout);
	}
	glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, picture);
	/*
	 * When the guest named its picture buffer's physical memory, write the picture there: the
	 * reply is then two bytes and the guest reads the picture where it already is. The same
	 * validation as a texture reference: the segments must cover every byte and lie in the RAM
	 * file. If anything is off, the old path sends the picture back instead.
	 */
	if (!no_in_place && segments != NULL && ram_memory != NULL) {
		/* The rows of the display's surface (row_bytes: the picture is written row by row and the
		 * guest has nothing left to copy), or the packed picture buffer it asked for. */
		size_t target_bytes = row_bytes > 0 ? (size_t)row_bytes * (size_t)height : frame_bytes;
		size_t at = 0;

		in_place = 1;
		for (int i = 0; i < segment_count; i++) {
			uint64_t pa = segments[i].pa;
			uint32_t len = segments[i].len;

			if (pa < ram_pa_base || pa - ram_pa_base > ram_bytes ||
			    len > ram_bytes - (size_t)(pa - ram_pa_base)) {
				in_place = 0;
				break;
			}
			at += len;
		}
		if (at != target_bytes) {
			in_place = 0;
		}
		/* The second surface of the frame: same picture, its own stride and its own segments. If
		 * it cannot be written the picture travels back, or the surface the display presents next
		 * would be the only one with anything in it -- and the other one is what makes the screen
		 * black every other time. */
		if (in_place && segment_count2 > 0) {
			size_t target2 = row_bytes2 > 0 ? (size_t)row_bytes2 * (size_t)height : frame_bytes;
			size_t at2 = 0;

			if (row_bytes2 <= 0) {
				in_place = 0;
			}
			for (int i = 0; in_place && i < segment_count2; i++) {
				uint64_t pa = segments2[i].pa;
				uint32_t len = segments2[i].len;

				if (pa < ram_pa_base || pa - ram_pa_base > ram_bytes ||
				    len > ram_bytes - (size_t)(pa - ram_pa_base)) {
					in_place = 0;
					break;
				}
				at2 += len;
			}
			if (in_place && at2 != target2) {
				in_place = 0;
			}
		}
		if (in_place && row_bytes > 0) {
			in_place = write_picture_rows(picture, width, height, (size_t)row_bytes,
			    segments, segment_count);
			if (in_place && segment_count2 > 0) {
				in_place = write_picture_rows(picture, width, height, (size_t)row_bytes2,
				    segments2, segment_count2);
			}
			/* P1'''': five lines a boot would be enough to read, but a stall's frame and the
			 * frame after a Today rebuild are not the first five -- one every hundred frames
			 * leaves forty lines in a 4,000-frame boot and still dates the change. */
			if (in_place && (frames_replayed + 1 <= 5 || (frames_replayed + 1) % 100 == 0)) {
				stage11_report("first", picture, width, height, (size_t)row_bytes, segments,
				    segment_count, frames_replayed + 1);
				if (segment_count2 > 0) {
					stage11_report("second", picture, width, height, (size_t)row_bytes2,
					    segments2, segment_count2, frames_replayed + 1);
				}
			}
		} else if (in_place) {
			at = 0;
			for (int i = 0; i < segment_count; i++) {
				memcpy(ram_memory + (size_t)(segments[i].pa - ram_pa_base),
				    picture + at, segments[i].len);
				at += segments[i].len;
			}
		}
	}
	dump_once(client, picture, width, height, (int)frames_replayed + 1);
	if (save_dir != NULL && ((frames_replayed + 1) <= 20 || (frames_replayed + 1) % 100 == 0)) {
		char path[512];
		FILE *file;

		snprintf(path, sizeof(path), "%s/frame-%llu-%dx%d.bgra", save_dir, frames_replayed + 1,
		    width, height);
		file = fopen(path, "wb");
		if (file != NULL) {
			fwrite(picture, 1, frame_bytes, file);
			fclose(file);
			printf("host_glwire: saved %s\n", path);
		}
	}
	/* Before the release: the panel is the guest's own pixels of this frame, and the picture is
	 * what the host drew. Equal hashes are the acceptance: the host added nothing and lost
	 * nothing. */
	if (client->names.panel != NULL && client->names.panel_width == width &&
	    client->names.panel_height == height) {
		panel_hash = hash_panel(client->names.panel, (size_t)width * height);
		picture_hash = hash_picture(picture, (size_t)width * height);
		panel_known = 1;
	}
	previous_picture_hash = picture_hash;
	have_previous_picture = 1;
	names_attribs_release(&client->names);
	wire_release();
	took = now_seconds() - began;
	frames_replayed++;
	{
		char done[64];
		int length = in_place ? snprintf(done, sizeof(done), "R 0\n")
		    : snprintf(done, sizeof(done), "R %zu\n", frame_bytes);

		if (!send_all(client->fd, done, (size_t)length) ||
		    (!in_place && !send_all(client->fd, picture, frame_bytes))) {
			free(records);
			free(picture);
			free(segments);
		free(segments2);
			return -1;
		}
	}
	{
		static double last_frame;
		double now = now_seconds();
		double interval = last_frame > 0 ? now - last_frame : 0;

		last_frame = now;
		if (panel_known) {
			printf("host_glwire: frame %llu, %dx%d, %zu records B, %zu picture B, %.1f ms, "
			    "interval %.2f s (%.2f fps), %lu image refs (%lu B), panel %016llx "
			    "picture %016llx %s%s\n",
			    frames_replayed, width, height, records_bytes, frame_bytes, took * 1000.0, interval,
			    interval > 0 ? 1.0 / interval : 0.0, wire_image_refs,
			    wire_image_ref_bytes, (unsigned long long)panel_hash,
			    (unsigned long long)picture_hash,
			    panel_hash == picture_hash ? "MATCH" : "DIFF",
			    in_place ? (row_bytes > 0 ? " in-place rows" : " in-place") : "");
		} else {
			printf("host_glwire: frame %llu, %dx%d, %zu records B, %zu picture B, %.1f ms, "
			    "interval %.2f s (%.2f fps), %lu image refs (%lu B)%s\n",
			    frames_replayed, width, height, records_bytes, frame_bytes, took * 1000.0, interval,
			    interval > 0 ? 1.0 / interval : 0.0, wire_image_refs,
			    wire_image_ref_bytes,
			    in_place ? (row_bytes > 0 ? " in-place rows" : " in-place") : "");
		}
	}
	fflush(stdout);
	free(records);
	free(picture);
	free(segments);
	free(segments2);
	return 0;
}

/* The older raw-record path: an 8-byte header, then one record's payload. Kept for a client that
 * wants to send calls outside a frame. */
static int
serve_record(struct client *client)
{
	uint32_t header[2];
	wire_buffer r;

	if (recv(client->fd, header, sizeof(header), MSG_WAITALL) != (ssize_t)sizeof(header)) {
		return -1;
	}
	if (header[0] == 0xffffffffu) {
		return -1;
	}
	r.base = malloc(header[1]);
	r.capacity = header[1];
	r.at = 0;
	if (r.base == NULL || !recv_all(client->fd, r.base, header[1])) {
		free(r.base);
		return -1;
	}
	if (!make_client_current(client)) {
		free(r.base);
		return -1;
	}
	dispatch(&client->names, &r, header[0]);
	free(r.base);
	return 0;
}

static void
close_client(struct client *client)
{
	if (client->fd < 0) {
		return;
	}
	if (client->context != EGL_NO_CONTEXT) {
		eglMakeCurrent(display, client->surface, client->surface, client->context);
		if (client->framebuffer != 0) {
			glDeleteFramebuffers(1, &client->framebuffer);
		}
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (client->surface != EGL_NO_SURFACE) {
			eglDestroySurface(display, client->surface);
		}
		eglDestroyContext(display, client->context);
	}
	names_free(&client->names);
	close(client->fd);
	printf("host_glwire: client %d closed\n", client->fd);
	fflush(stdout);
	client->fd = -1;
	client->context = EGL_NO_CONTEXT;
	client->surface = EGL_NO_SURFACE;
	client->framebuffer = 0;
}

static EGLDisplay
make_display(void)
{
	PFNEGLQUERYDEVICESEXTPROC query = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
	    (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDeviceEXT devices[8];
	EGLint count = 0, major = 0, minor = 0;

	if (query != NULL && get_display != NULL && query(8, devices, &count) && count > 0) {
		for (EGLint i = 0; i < count; i++) {
			EGLDisplay candidate = get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);

			if (candidate != EGL_NO_DISPLAY && eglInitialize(candidate, &major, &minor)) {
				return candidate;
			}
		}
	}
	{
		EGLDisplay fallback = eglGetDisplay(EGL_DEFAULT_DISPLAY);

		eglInitialize(fallback, &major, &minor);
		return fallback;
	}
}

int
main(int argc, char **argv)
{
	const char *ram_path = getenv("GLWIRE_RAM");
	int port = 9110;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
			save_dir = argv[++i];
		} else if (strcmp(argv[i], "--no-panel") == 0) {
			no_panel = 1;
		} else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
			ram_path = argv[++i];
		} else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
			port = atoi(argv[i]);
		} else {
			ram_path = argv[i];
		}
	}
	int listener;
	int count = 0;
	static const EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};

	no_in_place = getenv("GLWIRE_NO_INPLACE") != NULL;
	/* (the flag the argument loop set stays set: this used to overwrite it, so "--no-panel" on the
	 * command line silently did nothing and every acceptance run drew the guest's own panel) */
	no_panel = no_panel || getenv("GLWIRE_NO_PANEL") != NULL;
	if (ram_path != NULL) {
		map_ram(ram_path);
	} else {
		printf("host_glwire: no RAM file: texture references will fall back to inline bytes\n");
	}
	display = make_display();
	eglBindAPI(EGL_OPENGL_API);
	if (display == EGL_NO_DISPLAY ||
	    !eglChooseConfig(display, attributes, &config, 1, &count) || count == 0) {
		fprintf(stderr, "host_glwire: no GL configuration\n");
		return 1;
	}
	{
		EGLContext probe = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
		EGLSurface surface = eglCreatePbufferSurface(display, config,
		    (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });

		if (probe != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, probe)) {
			printf("host_glwire: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
			eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		}
		if (surface != EGL_NO_SURFACE) {
			eglDestroySurface(display, surface);
		}
		if (probe != EGL_NO_CONTEXT) {
			eglDestroyContext(display, probe);
		}
	}
	fflush(stdout);

	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
		clients[i].context = EGL_NO_CONTEXT;
		clients[i].surface = EGL_NO_SURFACE;
	}

	listener = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
	{
		struct sockaddr_in address;

		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons((unsigned short)port);
		if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
		    listen(listener, 4) != 0) {
			fprintf(stderr, "host_glwire: cannot listen on %d\n", port);
			return 1;
		}
	}
	printf("host_glwire: listening on %d%s%s\n", port,
	    no_in_place ? " (GLWIRE_NO_INPLACE: the picture travels back instead)" : "",
	    no_panel ? " (no panel: the recorded calls are replayed instead)" : "");
	fflush(stdout);

	for (;;) {
		struct pollfd polled[1 + MAX_CLIENTS];
		int indices[1 + MAX_CLIENTS];
		int nfds = 1;

		polled[0].fd = listener;
		polled[0].events = POLLIN;
		indices[0] = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				polled[nfds].fd = clients[i].fd;
				polled[nfds].events = POLLIN;
				indices[nfds] = i;
				nfds++;
			}
		}
		if (poll(polled, (nfds_t)nfds, -1) < 0) {
			continue;
		}
		if (polled[0].revents & POLLIN) {
			int fd = accept(listener, NULL, NULL);

			if (fd >= 0) {
				int slot;
				for (slot = 0; slot < MAX_CLIENTS && clients[slot].fd >= 0; slot++) {
				}
				if (slot == MAX_CLIENTS) {
					close(fd);
				} else {
					memset(&clients[slot], 0, sizeof(clients[slot]));
					clients[slot].fd = fd;
					clients[slot].context = EGL_NO_CONTEXT;
					clients[slot].surface = EGL_NO_SURFACE;
					names_init(&clients[slot].names);
					setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
					printf("host_glwire: client %d connected\n", fd);
					fflush(stdout);
				}
			}
		}
		for (int i = 1; i < nfds; i++) {
			struct client *client = &clients[indices[i]];
			char peek = 0;
			ssize_t got;

			if (!(polled[i].revents & (POLLIN | POLLHUP | POLLERR))) {
				continue;
			}
			got = recv(client->fd, &peek, 1, MSG_PEEK);
			if (got <= 0) {
				close_client(client);
				continue;
			}
			if (peek == 'R') {
				if (serve_frame(client) != 0) {
					close_client(client);
				}
			} else if (serve_record(client) != 0) {
				close_client(client);
			}
		}
	}
	return 0;
}
