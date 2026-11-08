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
		if (client->names.panel != NULL && client->names.panel_width == width &&
	    client->names.panel_height == height) {
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
	if (client->names.panel != NULL && client->names.panel_width == width &&
	    client->names.panel_height == height) {
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
	/* (the context's drawable is a 16x16 pbuffer, so its default viewport is not the frame's; this
	 * is the one reset -- the replayed stream keeps whatever viewport the guest left behind) */
	glViewport(0, 0, width, height);
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
	static int mode = -1;          /* -1 unread: 0 native first, 1 expand, 2 native only */

	if (level != 0 || type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0) {
		return 0;
	}
	/*
	 * GLWIRE_LEGACY_FORMATS: the legacy formats (GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA) were
	 * expanded to RGBA because an earlier host context refused them with GL_INVALID_OPERATION.  It
	 * does not: measured on this one (4.6 compatibility, V100) all three upload natively with no
	 * error, and a native upload samples exactly as the guest's GL ES did.  The expansion is still
	 * what the widget's rounded corners lost with (14,934 pixels of the widget boxes changed), so
	 * the native upload is tried first now and the expansion is the fallback:
	 *
	 *   unset / 0   native first, expand only if the native upload is refused
	 *   1           always expand (the stage twelve behaviour, for comparison)
	 *   2           native only: a refusal is left to the caller's own glTexImage2D
	 */
	if (mode < 0) {
		const char *value = getenv("GLWIRE_LEGACY_FORMATS");

		mode = value == NULL ? 0 : (value[0] == '1' ? 1 : value[0] == '2' ? 2 : 0);
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
	if (mode != 1) {
		GLenum error = GL_NO_ERROR;

		while (glGetError() != GL_NO_ERROR) {
		}
		glTexImage2D(target, level, internal, width, height, border, format, type, pixels);
		error = glGetError();
		if (error == GL_NO_ERROR || mode == 2) {
			if (said < 8) {
				said++;
				printf("glwire: legacy texture %dx%d internal %#x format %#x: taken natively\n",
				    (int)width, (int)height, internal, format);
				fflush(stdout);
			}
			return 1;
		}
		printf("glwire: legacy texture %dx%d internal %#x format %#x: native upload refused %#x, "
		    "expanding\n", (int)width, (int)height, internal, format, error);
		fflush(stdout);
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
/* ---- stage nineteen: the last uniform writes, to say what put a NaN in texmat0 ---- */
#define GLRING 64
struct uniform_write {
	uint32_t program;
	GLint guest_location;
	GLint host_location;
	const char *kind;
	GLfloat values[4];
};

static struct uniform_write uniform_ring[GLRING];
static unsigned uniform_ring_at;

static void
host_note_uniform(struct glstate *g, const char *kind, GLint guest_location, GLint host_location,
    const GLfloat *values)
{
	struct uniform_write *slot = &uniform_ring[uniform_ring_at++ % GLRING];

	slot->program = (uint32_t)g->current_program;
	slot->guest_location = guest_location;
	slot->host_location = host_location;
	slot->kind = kind;
	for (int i = 0; i < 4; i++) {
		slot->values[i] = values != NULL ? values[i] : 0.0f;
	}
}

static int uniform_ring_dumped;

static void
host_dump_uniform_ring(GLint host_location)
{
	if (uniform_ring_dumped >= 3) {
		return;
	}
	uniform_ring_dumped++;
	printf("glwire: uniform writes to host location %d, newest first:\n", (int)host_location);
	for (unsigned i = 0; i < GLRING; i++) {
		struct uniform_write *slot = &uniform_ring[(uniform_ring_at + GLRING - 1 - i) % GLRING];

		if (slot->kind == NULL || slot->host_location != host_location) {
			continue;
		}
		printf("glwire:   programme %u guest location %d (%s): %.4f %.4f %.4f %.4f\n",
		    (unsigned)slot->program, (int)slot->guest_location, slot->kind,
		    (double)slot->values[0], (double)slot->values[1], (double)slot->values[2],
		    (double)slot->values[3]);
	}
	fflush(stdout);
}

static int last_draw_was_glyph;

/*
 * Stage nineteen, P0/v3: the text-draw line must describe the state the draw *started* with.
 * names_attribs_release() frees the CPU copies of the vertex arrays before host_after_draw() runs,
 * so a line built from the after-draw state reported "attrib -1" for every draw and never saw a
 * vertex.  host_note_draw() now takes this snapshot after it has decided the draw is a glyph and
 * before dispatch() calls glDraw*; host_after_draw() only reads the target back and prints it.
 */
struct text_draw_snapshot {
	int valid;
	unsigned long long frame;         /* (frames are per record; the draw number is not) */
	unsigned long long draw_number;   /* global draw serial, never reset per frame */
	GLint program;
	GLint fbo, kind, name, width, height;
	GLint view[4];
	int scissor_on;
	GLint scissor[4];
	int blend_on;
	GLint blend_src, blend_dst;
	GLint attrib_index, attrib_type;
	int vertices;
	float xs[4], ys[4], x0, y0, x1, y1;
	int tc_valid;
	float tcs[4][2];
	uint32_t tc_raw[2];
	uint32_t texmat[4];
	int matrix_valid;
	uint32_t vertex_matrix[16];
	/* (unit 0's sampler input: an empty tile and a feedback loop both look like "tc0 is right and
	 * nothing landed", so the line carries the texture's own size and whether it is the target) */
	int u0_valid;
	GLint u0_tex, u0_w, u0_h;
};

static struct text_draw_snapshot text_snap;
static unsigned long long draw_serial;

/* (defined next to host_attrib_xy(), which it uses; forward-declared for host_note_draw()) */
static void host_text_snapshot(struct glstate *g, unsigned long long draw_number);

/*
 * Stage nineteen, P0/v4: the text draw's own target is 64x19 or 750x200, but the stream leaves the
 * viewport at the panel's size (750x1624) when the draw arrives, so the quad is clipped/scaled out
 * of the small attachment.  GLWIRE_VIEWPORT_TO_TARGET=1 sets the viewport to the attachment for
 * glyph draws only, then restores the stream's viewport in host_after_draw().
 */
static struct {
	int active;
	GLint fbo;
	GLint view[4];
} viewport_override;

static void
host_viewport_to_target(void)
{
	GLint view[4], fbo = 0, kind = -1, name = 0, width = 0, height = 0;

	viewport_override.active = 0;
	if (!last_draw_was_glyph || getenv("GLWIRE_VIEWPORT_TO_TARGET") == NULL) {
		return;
	}
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	if (fbo == 0) {
		/* (the default framebuffer's size is the drawable; the panel viewport is right for it) */
		return;
	}
	glGetIntegerv(GL_VIEWPORT, view);
	glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
	glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	if (kind == GL_TEXTURE && name != 0) {
		GLint previous_texture = 0;

		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
		glBindTexture(GL_TEXTURE_2D, (GLuint)name);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
	} else if (kind == GL_RENDERBUFFER && name != 0) {
		GLint previous_rb = 0;

		glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_rb);
		glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
		glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
		glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
		glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)previous_rb);
	}
	if (width <= 0 || height <= 0) {
		return;
	}
	if (view[0] == 0 && view[1] == 0 && view[2] == width && view[3] == height) {
		return;
	}
	viewport_override.active = 1;
	viewport_override.fbo = fbo;
	memcpy(viewport_override.view, view, sizeof(view));
	glViewport(0, 0, width, height);
	{
		static unsigned said;

		if (said < 20) {
			said++;
			printf("glwire: viewport override fbo %d %dx%d (was %d,%d %dx%d)\n", (int)fbo,
			    (int)width, (int)height, (int)view[0], (int)view[1], (int)view[2],
			    (int)view[3]);
			fflush(stdout);
		}
	}
}

static void
host_viewport_restore(void)
{
	if (!viewport_override.active) {
		return;
	}
	glViewport(viewport_override.view[0], viewport_override.view[1],
	    viewport_override.view[2], viewport_override.view[3]);
	viewport_override.active = 0;
}

/* (one line per glViewport: it says whether the small target's viewport is set before or after
 * the text draw that needs it) */
static void
host_viewport_log(GLint x, GLint y, GLsizei width, GLsizei height)
{
	GLint fbo = 0;

	if (getenv("GLWIRE_VIEWPORT_LOG") == NULL) {
		return;
	}
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	printf("glwire: viewport frame %llu fbo %d %d,%d %dx%d\n",
	    (unsigned long long)(frames_replayed + 1), (int)fbo, (int)x, (int)y, (int)width,
	    (int)height);
	fflush(stdout);
}

/* (stage sixteen: programme 13's 64x19 backdrop is created empty here -- the wire records no fill for
 * it -- so the darken pass multiplies the text to zero.  GLWIRE_BACKDROP=1 fills that texture from
 * the panel once per frame, and the darken pass then has something to darken.) */
static GLuint panel_fbo, backdrop_fbo, backdrop_filled;
static GLsizei panel_width, panel_height;
static unsigned long long backdrop_frame;
static void host_backdrop_fill(struct glstate *g);
static int fbo_trace;
static int record_context_known;
static uint32_t record_context;
static unsigned fbo_trace_said;

/*
 * Stage fifteen: what does the text sampler actually hold?  At a glyph draw, read the texture bound
 * to unit 0 back through a scratch framebuffer and say how much of it is non-zero and what its alpha
 * looks like.  An all-zero atlas is an upload problem; a full atlas with an empty draw is a sampling
 * or a programme problem -- the two were indistinguishable from the FBO dumps alone.
 */
static GLuint label_sample_fbo;

static void
host_label_sample(struct glstate *g)
{
	static unsigned said;
	GLint previous_fbo = 0, texture = 0, width = 0, height = 0, internal = 0, active = 0;
	GLenum error;

	if (said >= 8) {
		return;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	if (texture == 0) {
		return;
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
	if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
		glActiveTexture((GLenum)active);
		return;
	}
	if (label_sample_fbo == 0) {
		glGenFramebuffers(1, &label_sample_fbo);
	}
	{
		size_t bytes = (size_t)width * (size_t)height * 4;
		unsigned char *pixels = calloc(1, bytes);
		unsigned nonzero = 0, opaque = 0;

		glBindFramebuffer(GL_FRAMEBUFFER, label_sample_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)texture, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && pixels != NULL) {
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
			for (size_t i = 0; i + 3 < bytes; i += 4) {
				if (pixels[i] | pixels[i + 1] | pixels[i + 2] | pixels[i + 3]) {
					nonzero++;
				}
				if (pixels[i + 3] > 127) {
					opaque++;
				}
			}
		}
		error = glGetError();
		if (getenv("GLWIRE_ATLAS_DUMP") != NULL && pixels != NULL && nonzero > 0) {
			/* stage nineteen: the texture the text draw samples -- if the glyphs are in it, this
			 * is the picture that should land on the screen */
			char path[512];
			FILE *file;

			snprintf(path, sizeof(path), "%s-%dx%d.bgra", getenv("GLWIRE_ATLAS_DUMP"),
			    (int)width, (int)height);
			file = fopen(path, "wb");
			if (file != NULL) {
				fwrite(pixels, 1, (size_t)width * (size_t)height * 4, file);
				fclose(file);
				printf("glwire: atlas dump %s %dx%d\n", path, (int)width, (int)height);
			}
		}
		printf("glwire: label sample texture %d %dx%d internal %#x: %u nonzero pixels, %u with alpha, "
		    "glerror %#x\n", (int)texture, (int)width, (int)height, internal, nonzero, opaque, error);
		fflush(stdout);
		free(pixels);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_fbo);
	glActiveTexture((GLenum)active);
	said++;
	(void)g;
}

static void host_program_source(struct glstate *g, GLuint program);
static void host_attrib_state(struct glstate *g);

/*
 * Stage sixteen: when a label pass renders into a small texture, does any later draw sample that
 * texture?  Host names are recycled, so the ring below keys on the *guest* name that the attachment
 * maps to; a match within a couple of frames is the composite we are looking for.
 */
static uint64_t guest_of_host(const struct name_map *map, uint64_t host);

struct tile_note {
	unsigned long long guest;
	unsigned long long frame;
};
static struct tile_note tile_notes[16];
static unsigned tile_next;
static unsigned tile_said;

static void
tile_note_add(struct glstate *g, GLuint host_texture)
{
	uint64_t guest = 0;

	guest = guest_of_host(&g->textures, (uint64_t)host_texture);
	if (guest != 0) {
		tile_notes[tile_next] = (struct tile_note) { guest, frames_replayed };
		tile_next = (tile_next + 1) % (sizeof(tile_notes) / sizeof(tile_notes[0]));
	}
}

static void
tile_note_check(const char *where, GLuint unit, unsigned long long guest, GLint program, GLint fbo)
{
	if (getenv("GLWIRE_TILE_TRACE") == NULL || tile_said >= 200) {
		return;
	}
	for (unsigned i = 0; i < sizeof(tile_notes) / sizeof(tile_notes[0]); i++) {
		struct tile_note *note = &tile_notes[i];

		if (note->guest != guest || frames_replayed < note->frame || frames_replayed - note->frame > 3) {
			continue;
		}
		printf("glwire: tile guest %llu drawn at frame %llu, sampled %s u%u in frame %llu "
		    "(program %d fbo %d)\n", guest, note->frame, where, unit, frames_replayed,
		    (int)program, (int)fbo);
		fflush(stdout);
		tile_said++;
		break;
	}
}

/*
 * Stage eighteen, P0's next layer: a text draw whose sampler matrix is NaN.  Measured (s16tile):
 * every label draw of programme 13 has `texmat0 nan nan nan 0.0000`, and the vertex shader computes
 * texcoord0_0 = vertex_texcoord0 * texmat0.xy + texmat0.zw -- so every glyph sample of every label is
 * undefined.  This is the experiment: replace a NaN texmat with the identity (1,1,0,0) so the sample
 * lands somewhere defined; if the labels then draw, the NaN is what held them back and the recording
 * of glUniform4fv is where the real fix goes.
 */
static GLfloat
half_to_float(uint16_t half)
{
	uint32_t sign = (half >> 15) & 1;
	uint32_t exponent = (half >> 10) & 0x1f;
	uint32_t mantissa = half & 0x3ff;
	float value;

	if (exponent == 0) {
		value = (float)mantissa / 1024.0f * (float)(1u << 14);
	} else if (exponent == 31) {
		value = 0.0f;           /* inf/nan: not a texel coordinate */
	} else if (exponent > 15) {
		value = (float)(mantissa + 1024) / 1024.0f * (float)(1u << (exponent - 15));
	} else {
		value = (float)(mantissa + 1024) / 1024.0f / (float)(1u << (15 - exponent));
	}
	return sign ? -value : value;
}

static void
host_fix_nan_texmat(GLuint program)
{
	static const char *names[3] = { "texmat0", "texmat1", "texmat2" };
	static unsigned said;

	for (unsigned i = 0; i < 3; i++) {
		GLint location = glGetUniformLocation(program, names[i]);
		GLfloat value[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		int nan = 0;

		if (location < 0) {
			continue;
		}
		glGetUniformfv(program, location, value);
		if (said < 3) {
			said++;
			printf("glwire: texmat check programme %u %s location %d: %.4f %.4f %.4f %.4f\n",
			    (unsigned)program, names[i], (int)location, (double)value[0], (double)value[1],
			    (double)value[2], (double)value[3]);
			fflush(stdout);
		}
		for (int c = 0; c < 4; c++) {
			if (value[c] != value[c]) {
				nan = 1;
			}
		}
		if (!nan) {
			continue;
		}
		glUniform4f(location, 1.0f, 1.0f, 0.0f, 0.0f);
		{
			printf("glwire: %s was NaN for programme %u, set to (1,1,0,0)\n", names[i],
			    (unsigned)program);
			fflush(stdout);
		}
	}
}

/*
 * Stage nineteen fix: recognising the text draw and repairing a NaN sampler matrix are probes, not
 * debug prints.  Both used to sit behind GLWIRE_DRAW_LOG / GLWIRE_LABEL_DUMP, so a run that asked
 * for the text-draw line but not the draw log saw nothing: host_note_draw() returned before the
 * glyph scan and last_draw_was_glyph stayed 0.  These two run first; the logging below is still
 * gated exactly as before.
 */
static int
host_draw_binds_glyph(void)
{
	GLint active = 0, glyph_height = 0;

	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	for (unsigned unit = 0; unit < 8; unit++) {
		GLint bound = 0, height = 0;

		glActiveTexture(GL_TEXTURE0 + unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		if (bound == 0) {
			continue;
		}
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		if (height == 90 || height == 42) {
			glyph_height = height;
		}
	}
	glActiveTexture((GLenum)active);
	return glyph_height != 0;
}

static void
host_texmat_nan_to_identity(GLint program)
{
	GLint location = glGetUniformLocation((GLuint)program, "texmat0");
	GLfloat texmat[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t bits[4];
	int nan = 0;

	if (location < 0) {
		return;
	}
	glGetUniformfv((GLuint)program, location, texmat);
	memcpy(bits, texmat, sizeof(bits));
	for (int i = 0; i < 4; i++) {
		if (((bits[i] >> 23) & 0xff) == 0xff) {
			nan = 1;
		}
	}
	if (nan) {
		static unsigned said_nan;

		glUniform4f(location, 1.0f, 1.0f, 0.0f, 0.0f);
		if (said_nan < 5) {
			said_nan++;
			printf("glwire: texmat0 was a NaN (programme %d), set to (1,1,0,0)\n", (int)program);
			fflush(stdout);
		}
	}
}

static void
host_note_draw(struct glstate *g, const char *how, GLenum mode, GLsizei count)
{
	static unsigned logged;
	GLint program = 0, active = 0, glyph_height = 0;
	unsigned long long serial = ++draw_serial;
	int text_line = getenv("GLWIRE_TEXT_DRAW_LINE") != NULL;

	/* (a stale snapshot from the previous text draw must not be printed for a draw that is not one) */
	text_snap.valid = 0;
	/* (the glyph probe and the NaN repair run before this gate: they are what the text-draw line
	 * needs, and a run that does not want the draw log still needs them) */
	last_draw_was_glyph = host_draw_binds_glyph();
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	if (last_draw_was_glyph && getenv("GLWIRE_TEXMAT_NAN_TO_IDENTITY") != NULL) {
		host_texmat_nan_to_identity(program);
	}
	if (logged >= 20000 || getenv("GLWIRE_DRAW_LOG") == NULL) {
		/* (the draw-log path is the common one to skip, but the text-draw line still has to see
		 * this draw's state before glDraw* -- the arrays are freed right after it) */
		if (text_line && last_draw_was_glyph) {
			host_text_snapshot(g, serial);
		}
		host_viewport_to_target();
		return;
	}
	logged++;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	{
		GLint viewport[4] = { 0, 0, 0, 0 };
		GLint framebuffer = 0;

		GLint scissor[4] = { 0, 0, 0, 0 };

		glGetIntegerv(GL_VIEWPORT, viewport);
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		glGetIntegerv(GL_SCISSOR_BOX, scissor);
		GLint drawbuffer = 0;

		glGetIntegerv(GL_DRAW_BUFFER, &drawbuffer);
		printf("glwire: draw#%u", logged);
		if (record_context_known) {
			printf(" ctx %u", record_context);
		}
		printf(" %s mode %#x count %d program %d (guest %u) fbo %d viewport %d,%d %dx%d "
		    "scissor %s %d,%d %dx%d status %#x drawbuffer %#x", how, mode, (int) count, (int) program,
		    (unsigned) g->current_program, (int) framebuffer, (int) viewport[0], (int) viewport[1],
		    (int) viewport[2], (int) viewport[3], glIsEnabled(GL_SCISSOR_TEST) ? "on" : "off",
		    (int) scissor[0], (int) scissor[1], (int) scissor[2], (int) scissor[3],
		    (unsigned)glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER),
		    (unsigned)drawbuffer);
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
				tile_note_check("draw", unit, entry->guest, (GLint) program, (GLint)g->bound_framebuffer);
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
	last_draw_was_glyph = glyph_height != 0;
	if (last_draw_was_glyph && getenv("GLWIRE_FIX_TEXMAT") != NULL) {
		host_fix_nan_texmat((GLuint)program);
	}
	if (last_draw_was_glyph && getenv("GLWIRE_TEXMAT_FROM_ATTRIB") != NULL) {
		/*
		 * The third form of the same experiment, and the only one that can be right: derive the
		 * sampler matrix from the draw's own vertices.  A text quad carries two 2D texcoord arrays
		 * -- the unit square and the atlas subrectangle the glyph lives in -- and programme 13 maps
		 * the first into the second with texmat0 = (u1-u0, v1-v0, u0, v0).
		 *
		 * Measured (s18t6): at the interface's draws the subrectangle array arrives corrupt -- the
		 * three corners read 0/4.7969, 0/nan and 6.1e28/0 -- while the same array is sane earlier in
		 * the boot (0.125 .. 0.5).  Those numbers are what half-floats look like read as floats, so
		 * each vertex is tried both ways and the plausible reading (finite, within a texture's 0..1)
		 * wins.  If that reading makes the labels draw, the guest's recording of the texcoord array
		 * is where the real fix goes.
		 */
		GLfloat u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
		int found = 0, half_used = 0;

		for (GLint i = 0; i < 16 && !found; i++) {
			GLint enabled = 0, size = 0, type = 0, stride = 0;
			const unsigned char *base;
			int unit_square = 1, plausible_float = 1, plausible_half = 1;
			GLfloat fu0 = 1e30f, fv0 = 1e30f, fu1 = -1e30f, fv1 = -1e30f;
			GLfloat hu0 = 1e30f, hv0 = 1e30f, hu1 = -1e30f, hv1 = -1e30f;

			glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
			if (!enabled || g->attribs[i].data == NULL) {
				continue;
			}
			glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
			glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
			glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
			if (size != 2 || stride <= 0) {
				continue;
			}
			base = (const unsigned char *)g->attribs[i].data;
			/* (a quad is four vertices: this array's first three all sit on v = 0.125, and the
			 * subrectangle's v span only appears at the fourth) */
			for (int vertex = 0; vertex < 4; vertex++) {
				const unsigned char *at = base + (size_t)vertex * (size_t)stride;
				GLfloat u = 0.0f, t = 0.0f;
				uint16_t hu = 0, ht = 0;

				memcpy(&u, at, 4);
				memcpy(&t, at + 4, 4);
				memcpy(&hu, at, 2);
				memcpy(&ht, at + 2, 2);
				if (!(u >= -8.0f && u <= 8.0f) || !(t >= -8.0f && t <= 8.0f)) {
					plausible_float = 0;
				}
				{
					/* half-float: sign/exponent/mantissa, the same decode host_attrib_vertex uses */
					GLfloat dh = half_to_float(hu), dv = half_to_float(ht);

					if (!(dh >= -8.0f && dh <= 8.0f) || !(dv >= -8.0f && dv <= 8.0f)) {
						plausible_half = 0;
					}
					if (dh < hu0) hu0 = dh;
					if (dh > hu1) hu1 = dh;
					if (dv < hv0) hv0 = dv;
					if (dv > hv1) hv1 = dv;
				}
				if (u < fu0) fu0 = u;
				if (u > fu1) fu1 = u;
				if (t < fv0) fv0 = t;
				if (t > fv1) fv1 = t;
			}
			if (plausible_float) {
				u0 = fu0; v0 = fv0; u1 = fu1; v1 = fv1;
				half_used = 0;
			} else if (plausible_half) {
				u0 = hu0; v0 = hv0; u1 = hu1; v1 = hv1;
				half_used = 1;
			} else {
				continue;
			}
			/* the unit square is the array to skip: it is what texmat0 maps *from* */
			if (!((u0 == 0.0f || u0 == 1.0f) && (u1 == 0.0f || u1 == 1.0f) &&
			    (v0 == 0.0f || v0 == 1.0f) && (v1 == 0.0f || v1 == 1.0f))) {
				found = 1;
			}
			(void)type;
		}
		if (found) {
			GLint location = glGetUniformLocation((GLuint)program, "texmat0");

			if (location >= 0) {
				static unsigned said_float, said_half;

				glUniform4f(location, u1 - u0, v1 - v0, u0, v0);
				if (said_float < 3) {
					GLint bound = 0;
					GLfloat back[4] = { 0, 0, 0, 0 };

					glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
					glGetUniformfv((GLuint)program, location, back);
					printf("glwire: texmat0 write programme %d location %d bound %d -> %.4f %.4f "
					    "%.4f %.4f (wrote %.4f %.4f %.4f %.4f)\n", (int)program,
					    (int)location, (int)bound, (double)back[0], (double)back[1],
					    (double)back[2], (double)back[3], (double)(u1 - u0), (double)(v1 - v0),
					    (double)u0, (double)v0);
					fflush(stdout);
				}
				if ((half_used && said_half < 5) || (!half_used && said_float < 3)) {
					if (half_used) {
						said_half++;
					} else {
						said_float++;
					}
					printf("glwire: texmat0 derived from the vertices (%s): %.4f %.4f %.4f %.4f "
					    "for programme %u\n", half_used ? "half-float" : "float",
					    (double)(u1 - u0), (double)(v1 - v0), (double)u0, (double)v0,
					    (unsigned)program);
					fflush(stdout);
				}
			}
		}
	}
	if (last_draw_was_glyph && getenv("GLWIRE_TEXMAT_IDENTITY") != NULL) {
		/* the stronger half of the same experiment: never mind what the stream set, force the
		 * sampler matrix of the text draw to the identity and see whether the glyph changes */
		GLint location = glGetUniformLocation((GLuint)program, "texmat0");

		if (location >= 0) {
			static unsigned said;

			glUniform4f(location, 1.0f, 1.0f, 0.0f, 0.0f);
			if (said < 5) {
				said++;
				printf("glwire: texmat0 forced to (1,1,0,0) for programme %u\n", (unsigned)program);
				fflush(stdout);
			}
		}
	}
	if (last_draw_was_glyph) {
		host_backdrop_fill(g);
	}
	if (glyph_height != 0) {
		if (getenv("GLWIRE_LABEL_DUMP") != NULL) {
			GLint matrix_location = glGetUniformLocation((GLuint)program, "vertex_matrix");
			GLint texmat_location = glGetUniformLocation((GLuint)program, "texmat0");
			GLfloat texmat[4] = { 0, 0, 0, 0 };
			GLfloat matrix[16] = { 0 };

			host_label_sample(g);
			if (texmat_location >= 0) {
				glGetUniformfv((GLuint)program, texmat_location, texmat);
			}
			/*
			 * Stage nineteen: the comparison form (x != x) never fired while the print showed nan,
			 * so this reads the bits themselves -- a float that is a NaN has to look like one here.
			 */
			{
				static unsigned bits_said;

				if (bits_said < 3) {
					uint32_t bits[4];

					bits_said++;
					memcpy(bits, texmat, sizeof(bits));
					printf("glwire: texmat0 bits %08x %08x %08x %08x location %d programme %d "
					    "error %#x\n", bits[0], bits[1], bits[2], bits[3],
					    (int)texmat_location, (int)program, glGetError());
					fflush(stdout);
					host_dump_uniform_ring(texmat_location);
				}
			}
			if (matrix_location >= 0) {
				glGetUniformfv((GLuint)program, matrix_location, matrix);
			}
			{
				GLboolean mask[4] = { 0, 0, 0, 0 };
				GLint blend_src = 0, blend_dst = 0, colour = 0;

				glGetBooleanv(GL_COLOR_WRITEMASK, mask);
				glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src);
				glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst);
				glGetIntegerv(GL_CURRENT_COLOR, &colour);
				printf("glwire: label state blend %s src %#x dst %#x colormask %d%d%d%d\n",
				    glIsEnabled(GL_BLEND) ? "on" : "off", blend_src, blend_dst, (int)mask[0],
				    (int)mask[1], (int)mask[2], (int)mask[3]);
			}
			{
				GLint d_location = glGetUniformLocation((GLuint)program, "texmatD");
				GLfloat dmat[4] = { 0, 0, 0, 0 };
				GLint u1 = 0, u1w = 0, u1h = 0, previous_unit = 0;

				if (d_location >= 0) {
					glGetUniformfv((GLuint)program, d_location, dmat);
				}
				glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_unit);
				glActiveTexture(GL_TEXTURE1);
				glGetIntegerv(GL_TEXTURE_BINDING_2D, &u1);
				if (u1 != 0) {
					GLint previous_texture = 0;

					glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
					glBindTexture(GL_TEXTURE_2D, (GLuint)u1);
					glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &u1w);
					glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &u1h);
					glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
				}
				glActiveTexture((GLenum)previous_unit);
				printf("glwire: label state texmatD %.4f %.4f %.4f %.4f backdrop texture %d %dx%d\n",
				    (double)dmat[0], (double)dmat[1], (double)dmat[2], (double)dmat[3], (int)u1,
				    (int)u1w, (int)u1h);
			}
			{
				uint32_t here[4];

				memcpy(here, texmat, sizeof(here));
				/* (stage nineteen: the NaN repair now runs in host_note_draw, before the
				 * draw, so it is independent of GLWIRE_LABEL_DUMP; this block only records
				 * the bits the draw actually used) */
				if (uniform_ring_dumped < 4) {
					uniform_ring_dumped++;
					printf("glwire: texmat0 bits at the state line %08x %08x %08x %08x\n",
					    here[0], here[1], here[2], here[3]);
				}
			}
			printf("glwire: label state program %d texmat0 %.4f %.4f %.4f %.4f matrix %.4f %.4f %.4f %.4f / "
			    "%.4f %.4f %.4f %.4f / "
			    "%.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f\n", (int)program, (double)texmat[0],
			    (double)texmat[1], (double)texmat[2], (double)texmat[3], (double)matrix[0],
			    (double)matrix[1], (double)matrix[2], (double)matrix[3], (double)matrix[4],
			    (double)matrix[5], (double)matrix[6], (double)matrix[7], (double)matrix[8],
			    (double)matrix[9], (double)matrix[10], (double)matrix[11], (double)matrix[12],
			    (double)matrix[13], (double)matrix[14], (double)matrix[15]);
			host_attrib_state(g);
			fflush(stdout);
		}
		host_dump_uniforms(g, "text-draw");
		/* (stage fourteen: what the text draw actually asks the GPU to do) */
		if (getenv("GLWIRE_PROGRAM_SOURCE") != NULL) {
			host_program_source(g, (GLuint) program);
			host_attrib_state(g);
		}
	}
	/* (the last thing before dispatch() calls glDraw*: the state this draw is about to use) */
	if (text_line && last_draw_was_glyph) {
		host_text_snapshot(g, serial);
	}
	host_viewport_to_target();
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
		if (type == GL_FLOAT_MAT4 && size == 1) {
			GLfloat matrix[16] = { 0 };

			glGetUniformfv(program, location, matrix);
			printf(" matrix4x4 %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / "
			    "%.4f %.4f %.4f %.4f", (double)matrix[0], (double)matrix[1], (double)matrix[2],
			    (double)matrix[3], (double)matrix[4], (double)matrix[5], (double)matrix[6],
			    (double)matrix[7], (double)matrix[8], (double)matrix[9], (double)matrix[10],
			    (double)matrix[11], (double)matrix[12], (double)matrix[13], (double)matrix[14],
			    (double)matrix[15]);
		}
	}
	printf("\n");
	fflush(stdout);
}

/*
 * Stage fourteen: the text draw's outputs are all zero even when its colour texture is filled white, so
 * the question moves to the draw itself -- what shader does it run, and what vertices does it have?
 * GLWIRE_PROGRAM_SOURCE=1 prints the vertex and fragment source of every programme a glyph draw runs
 * (once each) with the shader type and compile status; host_attrib_state() prints every enabled
 * vertex attribute with the first vertices of the CPU copy the wire carried (opcode 200), so a
 * degenerate quad (all positions zero) is told from a shader that samples a transparent texel.
 */
static void
host_program_source(struct glstate *g, GLuint program)
{
	static GLuint dumped[32];
	static unsigned said;
	GLuint shaders[4] = { 0, 0, 0, 0 };
	GLint count = 0;
	unsigned limit = getenv("GLWIRE_PROGRAM_ALL") != NULL ? 32 : 8;

	(void)g;
	if (program == 0 || said >= limit) {
		return;
	}
	for (unsigned i = 0; i < said; i++) {
		if (dumped[i] == program) {
			return;
		}
	}
	dumped[said++] = program;
	glGetAttachedShaders(program, 4, &count, shaders);
	for (GLint i = 0; i < count && i < 4; i++) {
		char source[8192];
		GLint length = 0, type = 0, compiled = 0;

		glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
		glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
		glGetShaderSource(shaders[i], (GLsizei)sizeof(source) - 1, &length, source);
		source[length > 0 ? length : 0] = '\0';
		printf("glwire: programme %u shader %u type %#x compiled %d source %d B:\n%s\n---\n", program,
		    shaders[i], type, (int)compiled, (int)length, source);
		fflush(stdout);
	}
}

/* One attribute of one vertex, decoded by its type: the wire's copy carries the values the draw
 * actually uses, and the array is interleaved, so reading it without the stride was unreadable. */
static void
host_attrib_vertex(const struct attrib_state *attrib, int vertex, GLint type, GLint size)
{
	const unsigned char *at = (const unsigned char *)attrib->data +
	    (size_t)vertex * (size_t)(attrib->stride > 0 ? attrib->stride : 0);

	printf(" v%d:", vertex);
	for (GLint c = 0; c < size && c < 4; c++) {
		if (type == GL_FLOAT) {
			float value = 0;

			memcpy(&value, at + c * 4, 4);
			printf(" %.4f", (double)value);
		} else if (type == 0x140b) {    /* GL_HALF_FLOAT */
			uint16_t half = 0;
			float value = 0;
			uint32_t sign, exponent, mantissa;

			memcpy(&half, at + c * 2, 2);
			sign = (half >> 15) & 1;
			exponent = (half >> 10) & 0x1f;
			mantissa = half & 0x3ff;
			if (exponent == 0) {
				value = (float)mantissa / 1024.0f * (float)(1u << 14);
			} else if (exponent != 31) {
				value = (float)(mantissa + 1024) / 1024.0f *
				    (float)(1u << (exponent > 15 ? exponent - 15 : 0));
				if (exponent <= 15) {
					value = (float)(mantissa + 1024) / 1024.0f / (float)(1u << (15 - exponent));
				}
			}
			printf(" %.4f", (double)(sign ? -value : value));
		} else if (type == GL_UNSIGNED_BYTE) {
			printf(" %u", (unsigned)at[c]);
		} else {
			printf(" ?");
		}
	}
}

static void
host_attrib_state(struct glstate *g)
{
	static unsigned said;

	if (said >= 20) {
		return;
	}
	for (GLint i = 0; i < 16; i++) {
		GLint enabled = 0, size = 0, type = 0, stride = 0;

		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
		if (!enabled) {
			continue;
		}
		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
		printf("glwire: attrib %d size %d type %#x stride %d data %s", (int)i, (int)size, type,
		    (int)stride, g->attribs[i].data != NULL ? "yes" : "no");
		if (g->attribs[i].data != NULL && g->attribs[i].size > 0) {
			for (int vertex = 0; vertex < 3; vertex++) {
				host_attrib_vertex(&g->attribs[i], vertex, type, size);
			}
		}
		printf("\n");
	}
	said++;
	fflush(stdout);
}

/*
 * Stage fifteen: the label pass's geometry is right (its matrix maps the quad onto the scissor box)
 * but every dump is zero, and the shader by itself cannot say whether that is "nothing was covered"
 * or "the sampled glyph alpha is zero".  GLWIRE_LABEL_WHITE=1 answers it by drawing the same vertices
 * with a trivial programme that writes opaque magenta: whatever the quad covers turns magenta, so a
 * still-zero dump means no coverage and a magenta block means the glyph sampling is the question.
 */
static GLuint label_debug_program;
static GLint label_debug_matrix;
static int label_debug_tried;

/*
 * Stage sixteen: programme 13 samples a 64x19 texture as its backdrop and multiplies its text colour
 * by that pixel's luminance -- with the texture empty (nothing in the wire ever fills it) the product
 * is zero and no label is drawn.  Fill it, once per frame per texture, by blitting the last
 * panel-sized framebuffer into it.  This is an experiment (GLWIRE_BACKDROP=1), not yet the default.
 */
static void
host_backdrop_fill(struct glstate *g)
{
	GLint active = 0, texture = 0, width = 0, height = 0, previous = 0, saved = 0;
	GLint saved_unit7 = 0;

	if (getenv("GLWIRE_BACKDROP") == NULL || panel_fbo == 0 || frames_replayed == backdrop_frame) {
		return;
	}
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE1);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved);
	if (texture == 0) {
		glActiveTexture((GLenum)active);
		return;
	}
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	if (width <= 0 || height <= 0 || width > 256 || height > 256) {
		glActiveTexture((GLenum)active);
		return;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	if (backdrop_fbo == 0) {
		glGenFramebuffers(1, &backdrop_fbo);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, panel_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, backdrop_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)texture, 0);
	if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
		glBlitFramebuffer(0, 0, panel_width, panel_height, 0, 0, width, height,
		    GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	glActiveTexture(GL_TEXTURE7);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_unit7);
	glBindTexture(GL_TEXTURE_2D, (GLuint)saved);
	glActiveTexture((GLenum)active);
	backdrop_filled = (GLuint)texture;
	backdrop_frame = frames_replayed;
	(void)g;
	(void)saved_unit7;
}

static int
host_label_begin(struct glstate *g)
{
	GLint program = 0, position_index = -1;
	const char *want = getenv("GLWIRE_LABEL_WHITE");

	if (want == NULL || !last_draw_was_glyph) {
		return 0;
	}
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	if (program == 0) {
		return 0;
	}
	for (int i = 0; i < 16; i++) {
		GLint enabled = 0;

		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
		if (enabled && g->attribs[i].data != NULL && g->attribs[i].size == 4 &&
		    g->attribs[i].type == GL_FLOAT) {
			position_index = i;
			break;
		}
	}
	if (position_index < 0) {
		return 0;
	}
	if (!label_debug_tried) {
		static const char *vertex_source =
		    "uniform mat4 vertex_matrix;\nattribute vec4 vertex_position;\n"
		    "void main() { gl_Position = vertex_matrix * vertex_position; }\n";
		static const char *alpha_vertex_source =
		    "uniform mat4 vertex_matrix;\nattribute vec4 vertex_position;\nattribute vec2 uv;\n"
		    "varying vec2 v;\n"
		    "void main() { v = uv; gl_Position = vertex_matrix * vertex_position; }\n";
		static const char *fragment_source =
		    "void main() { gl_FragData[0] = vec4(1.0, 0.0, 1.0, 1.0); }\n";
		static const char *alpha_fragment_source =
		    "varying vec2 v;\nuniform sampler2D texture0;\n"
		    "void main() { gl_FragData[0] = vec4(texture2D(texture0, v).a, 0.0, 0.0, 1.0); }\n";
		int want_alpha = getenv("GLWIRE_LABEL_ALPHA") != NULL;
		GLint texcoord_index = -1;
		GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
		GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
		const char *vertex_text = want_alpha ? alpha_vertex_source : vertex_source;
		const char *fragment_text = want_alpha ? alpha_fragment_source : fragment_source;
		GLint good = 0;

		for (int i = 0; i < 16; i++) {
			GLint enabled = 0;

			glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
			if (enabled && g->attribs[i].data != NULL && g->attribs[i].size == 2 &&
			    g->attribs[i].type == GL_FLOAT) {
				texcoord_index = i;
				break;
			}
		}
		label_debug_tried = 1;
		glShaderSource(vertex, 1, &vertex_text, NULL);
		glCompileShader(vertex);
		glShaderSource(fragment, 1, &fragment_text, NULL);
		glCompileShader(fragment);
		label_debug_program = glCreateProgram();
		glAttachShader(label_debug_program, vertex);
		glAttachShader(label_debug_program, fragment);
		glBindAttribLocation(label_debug_program, (GLuint)position_index, "vertex_position");
		if (texcoord_index >= 0) {
			glBindAttribLocation(label_debug_program, (GLuint)texcoord_index, "uv");
		}
		glLinkProgram(label_debug_program);
		glGetProgramiv(label_debug_program, GL_LINK_STATUS, &good);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		{
			GLint log_length = 0;
			char log[512] = "";

			glGetProgramiv(label_debug_program, GL_INFO_LOG_LENGTH, &log_length);
			if (log_length > 1) {
				glGetProgramInfoLog(label_debug_program, (GLsizei)sizeof(log) - 1, NULL, log);
			}
			printf("glwire: label debug %s: link %d position %d texcoord %d alpha %d %s\n",
			    want_alpha ? "alpha" : "magenta", (int)good, (int)position_index, (int)texcoord_index,
			    want_alpha, log);
			fflush(stdout);
		}
		if (!good) {
			glDeleteProgram(label_debug_program);
			label_debug_program = 0;
			return 0;
		}
		label_debug_matrix = glGetUniformLocation(label_debug_program, "vertex_matrix");
	}
	if (label_debug_program == 0) {
		return 0;
	}
	{
		GLint location = glGetUniformLocation((GLuint)program, "vertex_matrix");
		GLfloat matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

		if (location >= 0) {
			glGetUniformfv((GLuint)program, location, matrix);
		}
		glUseProgram(label_debug_program);
		glUniformMatrix4fv(label_debug_matrix, 1, GL_FALSE, matrix);
	}
	return program;
}

/*
 * Stage fourteen, P0: what is "fbo 1"?  A framebuffer name in the host's context means nothing on its
 * own -- make_frame_buffer() takes the first name the context hands out (1), the guest names its own
 * objects, and two clients have two contexts.  GLWIRE_FBO_LOG=1 says, once for every framebuffer the
 * stream binds, which guest name made it, what its colour attachment is (texture or renderbuffer, the
 * attachment's guest name, its size and internal format) and whether it is complete; the frame summary
 * says which framebuffer this client reads its picture from and which one the stream last drew into.
 * glGenFramebuffers and glFramebufferTexture2D are said too, so an attachment that never gets bound is
 * still accounted for.
 */
#define FBO_SEEN_MAX 64

struct fbo_note {
	int fd;
	uint64_t guest, host;
	unsigned binds, draws;
	int kind;                       /* GL_TEXTURE, GL_RENDERBUFFER or -1 */
	uint64_t attach;
	GLsizei width, height;
	GLint internal;
};

static int fbo_log;
static int probe_errors;
static int keep_binding;
static int read_panel;              /* (stage 17: read the picture from the panel-sized target) */
static GLuint panel_drawn_fbo;      /* the panel-sized target this frame last had a draw */
static int panel_drawn_frame = -1;
static int frame_delay_ms;          /* (stage 15: a deliberate per-frame delay, for the timing question) */
static int fbo_client_fd = -1;
static struct fbo_note fbo_notes[FBO_SEEN_MAX];
static unsigned fbo_note_count;

static uint64_t
guest_of_host(const struct name_map *map, uint64_t host)
{
	for (size_t i = 0; i < map->capacity; i++) {
		const struct name_entry *entry = &map->entries[i];

		if (entry->state == 1 && entry->host == host) {
			return entry->guest;
		}
	}
	return 0;
}

static struct fbo_note *
host_fbo_note(int fd, uint64_t guest, uint64_t host)
{
	for (unsigned i = 0; i < fbo_note_count; i++) {
		if (fbo_notes[i].fd == fd && fbo_notes[i].host == host && fbo_notes[i].guest == guest) {
			return &fbo_notes[i];
		}
	}
	if (fbo_note_count < FBO_SEEN_MAX) {
		struct fbo_note *note = &fbo_notes[fbo_note_count++];

		memset(note, 0, sizeof(*note));
		note->fd = fd;
		note->guest = guest;
		note->host = host;
		note->kind = -1;
		return note;
	}
	return NULL;
}

/* The attachment's size, without leaving the unit's texture binding changed. */
static void
fbo_texture_size(struct fbo_note *note)
{
	GLint previous = 0;

	if (note->kind != GL_TEXTURE || note->attach == 0) {
		return;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glBindTexture(GL_TEXTURE_2D, (GLuint)note->attach);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &note->width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &note->height);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &note->internal);
	glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
}

static void
host_fbo_bind(struct glstate *g, GLenum target, GLuint guest, GLuint host)
{
	struct fbo_note *note;
	GLenum query = target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER;
	GLint kind = -1, name = 0, complete = 0;

	if (!fbo_log || host == 0) {
		return;
	}
	note = host_fbo_note(fbo_client_fd, guest, host);
	if (note == NULL) {
		return;
	}
	note->binds++;
	glGetFramebufferAttachmentParameteriv(query, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
	glGetFramebufferAttachmentParameteriv(query, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	note->kind = kind;
	note->attach = (uint64_t)(GLuint)name;
	fbo_texture_size(note);
	complete = (GLint)glCheckFramebufferStatus(query);
	if (fbo_trace && fbo_trace_said < 3000) {
		fbo_trace_said++;
		printf("glwire: fbo trace #%u frame %llu guest %u host %u attachment %s %u %dx%d complete %#x\n",
		    fbo_trace_said, (unsigned long long)frames_replayed, guest, host,
		    kind == GL_TEXTURE ? "texture" : kind == GL_RENDERBUFFER ? "renderbuffer" : "none",
		    (unsigned)note->attach, (int)note->width, (int)note->height, complete);
		fflush(stdout);
	}
	if (note->binds > 1) {
		return;
	}
	printf("glwire: fbo bind client=%d target %#x guest %u host %u attachment %s name %u guest %llu "
	    "%dx%d internal %#x complete %#x drawn-so-far %u\n", fbo_client_fd, target, guest, host,
	    kind == GL_TEXTURE ? "texture" : kind == GL_RENDERBUFFER ? "renderbuffer" : "none",
	    (unsigned)note->attach,
	    (unsigned long long)guest_of_host(&g->textures, note->attach), (int)note->width,
	    (int)note->height, note->internal, complete, (unsigned)g->drawn_framebuffer);
	fflush(stdout);
}

static void
host_fbo_gen(struct glstate *g, GLsizei n, const GLuint *guest, const GLuint *host)
{
	(void)g;
	if (!fbo_log || n <= 0 || guest == NULL || host == NULL) {
		return;
	}
	printf("glwire: fbo gen client=%d n=%d:", fbo_client_fd, (int)n);
	for (GLsizei i = 0; i < n && i < 8; i++) {
		printf(" guest %u -> host %u", guest[i], host[i]);
	}
	printf("\n");
	fflush(stdout);
}

static void
host_fbo_attach(struct glstate *g, GLenum target, GLenum attachment, GLuint guest_tex, GLint level)
{
	GLint fbo = 0, kind = -1, name = 0, width = 0, height = 0, previous = 0, internal = 0;

	if (!fbo_log) {
		return;
	}
	glGetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	if (fbo == 0) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glBindTexture(GL_TEXTURE_2D, names_texture(g, guest_tex));
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
	glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
	printf("glwire: fbo attach client=%d fbo host %d guest %llu attachment %#x kind %#x name %d "
	    "texture guest %u host %u level %d %dx%d internal %#x\n", fbo_client_fd, fbo,
	    (unsigned long long)guest_of_host(&g->framebuffers, (uint64_t)fbo), attachment, kind, name,
	    guest_tex, names_texture(g, guest_tex), (int)level, width, height, internal);
	fflush(stdout);
}

static void
host_fbo_draw(struct glstate *g)
{
	struct fbo_note *note;

	if (g->bound_framebuffer == 0) {
		return;
	}
	note = host_fbo_note(fbo_client_fd, guest_of_host(&g->framebuffers, g->bound_framebuffer),
	    g->bound_framebuffer);
	if (note != NULL && (note->width == 0 || note->kind == -1)) {
		/* (stage seventeen: every drawn target's size is needed, not only the logged ones: the
		 * picture is read from the last *panel-sized* target, see serve_frame) */
		GLint kind = -1, name = 0;

		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
		note->kind = kind;
		note->attach = (uint64_t)(GLuint)name;
		fbo_texture_size(note);
	}
	if (note != NULL && note->width == 750 && note->height == 1624) {
		/* (stage sixteen: what programme 13's backdrop is downsampled from) */
		panel_fbo = g->bound_framebuffer;
		panel_width = note->width;
		panel_height = note->height;
	}
	if (note != NULL && trace_width > 0 && note->width == trace_width && note->height == trace_height) {
		/* (stage seventeen: the target the picture has to come from even when the frame's last
		 * draw was a small tile -- the size the frame asked for, not a hard-coded one) */
		panel_drawn_fbo = g->bound_framebuffer;
		panel_drawn_frame = (int)frames_replayed + 1;
	}
	if (fbo_log && note != NULL) {
		note->draws++;
	}
}

/*
 * Stage seventeen, P0: the label is composited into the panel-sized target and then the rest of the
 * frame rebuilds that target.  Two probes, both host-side and both off unless asked for:
 *
 *   GLWIRE_PANEL_PROBE=1  hashes the label's screen rectangle in a panel-sized target after every
 *       draw of the frame, so "the composite wrote, then the rebuild erased" is read off the hashes
 *       rather than argued from the draw order.  The rectangle is the scissor box of the last glyph
 *       draw -- CoreAnimation sets the scissor to the layer's own rect for the text pass.
 *
 *   GLWIRE_PANEL_SHADOW=1 keeps a 750x1624 shadow of that target as it stood right after a draw
 *       that sampled a texture some framebuffer had drawn into (a layer composite).  At the end of
 *       the frame the shadow, not the target, supplies the picture (GLWIRE_SHADOW_MODE=1, the whole
 *       target) or only the label's rectangle is patched back into it (mode 2).  This is the stage's
 *       experiment: the risk is that the shadow freezes a mid-frame state that the guest would have
 *       painted over.
 *
 * Both keep every piece of GL state they touch -- the framebuffer binding, the texture binding of
 * the active unit, the pack alignment -- because they run in the middle of the guest's stream.
 */
static int panel_probe_on, panel_shadow_on;
static int shadow_mode = 1;
static int shadow_apply = 1;
static GLuint shadow_fbo, shadow_texture;
static int shadow_width, shadow_height;
static int shadow_frame = -1;          /* the frame whose composite is in the shadow */
static int shadow_used;
static unsigned long long shadow_captures, shadow_uses;
static int shadow_dump_every;
static const char *shadow_dump_prefix;
static int label_rect[4] = { 0, 0, 0, 0 };
static int label_rect_valid;
static int label_rect_frame = -1;
static GLuint drawn_targets[32];
static unsigned drawn_target_count;
static GLuint label_tiles[16];      /* textures a text pass has drawn into (the label tile) */
static unsigned label_tile_count;
static int label_composites;        /* draws that put such a tile back into the panel */
static unsigned probe_said;

static int
panel_target_size(GLuint fbo, GLsizei *width, GLsizei *height)
{
	GLint kind = -1, name = 0, previous = 0, w = 0, h = 0;

	*width = 0;
	*height = 0;
	if (fbo == 0) {
		return 0;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	if (kind == GL_TEXTURE && name != 0) {
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
		glBindTexture(GL_TEXTURE_2D, (GLuint)name);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
	} else if (kind == GL_RENDERBUFFER && name != 0) {
		GLint previous_renderbuffer = 0;

		glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_renderbuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
		glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w);
		glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h);
		glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)previous_renderbuffer);
	}
	*width = (GLsizei)w;
	*height = (GLsizei)h;
	return w > 0 && h > 0;
}

/* Which textures have been drawn into: a draw that samples one of them is compositing a layer. */
static void
remember_drawn_target(GLuint fbo)
{
	GLint kind = -1, name = 0;

	if (fbo == 0) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
	if (kind != GL_TEXTURE) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
	if (name == 0) {
		return;
	}
	for (unsigned i = 0; i < drawn_target_count; i++) {
		if (drawn_targets[i] == (GLuint)name) {
			return;
		}
	}
	if (drawn_target_count < sizeof(drawn_targets) / sizeof(drawn_targets[0])) {
		drawn_targets[drawn_target_count++] = (GLuint)name;
	}
}

/* Does this draw sample a texture that a text pass drew into?  That is the label composite. */
static int
draw_samples_label_tile(void)
{
	for (GLint unit = 0; unit < 8; unit++) {
		GLint texture = 0;

		glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		if (texture == 0) {
			continue;
		}
		for (unsigned i = 0; i < label_tile_count; i++) {
			if (label_tiles[i] == (GLuint)texture) {
				glActiveTexture(GL_TEXTURE0);
				return 1;
			}
		}
	}
	glActiveTexture(GL_TEXTURE0);
	return 0;
}

/* The rectangle CoreAnimation scissored for the text pass, in screen coordinates. */
static void
label_rect_capture(void)
{
	GLint viewport[4] = { 0, 0, 0, 0 }, scissor[4] = { 0, 0, 0, 0 };

	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_SCISSOR_BOX, scissor);
	if (glIsEnabled(GL_SCISSOR_TEST) && scissor[2] > 0 && scissor[3] > 0) {
		label_rect[0] = scissor[0];
		label_rect[1] = scissor[1];
		label_rect[2] = scissor[2];
		label_rect[3] = scissor[3];
	} else {
		label_rect[0] = viewport[0];
		label_rect[1] = viewport[1];
		label_rect[2] = viewport[2];
		label_rect[3] = viewport[3];
	}
	label_rect_valid = label_rect[2] > 0 && label_rect[3] > 0;
	label_rect_frame = (int)frames_replayed + 1;
}

static void
rect_clip(int *x, int *y, int *w, int *h, int width, int height)
{
	if (*x < 0) {
		*w += *x;
		*x = 0;
	}
	if (*y < 0) {
		*h += *y;
		*y = 0;
	}
	if (*x + *w > width) {
		*w = width - *x;
	}
	if (*y + *h > height) {
		*h = height - *y;
	}
	if (*w < 0) {
		*w = 0;
	}
	if (*h < 0) {
		*h = 0;
	}
}

/* Hash and a little colour, of one rectangle of one framebuffer. */
static uint64_t
region_state(GLuint fbo, int x, int y, int w, int h, unsigned *nonzero, unsigned *bright)
{
	unsigned char *pixels;
	size_t bytes, i;
	uint64_t hash = 1469598103934665603ULL;
	GLint previous = 0, pack = 4;
	unsigned nz = 0, bt = 0;

	if (nonzero != NULL) {
		*nonzero = 0;
	}
	if (bright != NULL) {
		*bright = 0;
	}
	if (w <= 0 || h <= 0) {
		return 0;
	}
	bytes = (size_t)w * (size_t)h * 4;
	pixels = malloc(bytes);
	if (pixels == NULL) {
		return 0;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glReadPixels(x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	glPixelStorei(GL_PACK_ALIGNMENT, pack);
	for (i = 0; i + 3 < bytes; i += 4) {
		hash = fnv_update(hash, pixels + i, 4);
		if (pixels[i] | pixels[i + 1] | pixels[i + 2] | pixels[i + 3]) {
			nz++;
		}
		if (pixels[i] >= 200 && pixels[i + 1] >= 200 && pixels[i + 2] >= 200) {
			bt++;
		}
	}
	free(pixels);
	if (nonzero != NULL) {
		*nonzero = nz;
	}
	if (bright != NULL) {
		*bright = bt;
	}
	return hash;
}

static void
shadow_ensure(int width, int height)
{
	GLint active = 0, bound = 0, previous = 0;

	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glGenTextures(1, &shadow_texture);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, shadow_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &shadow_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_texture, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	glActiveTexture((GLenum)active);
	shadow_width = width;
	shadow_height = height;
}

static void
shadow_capture(GLuint from, int width, int height)
{
	GLint previous = 0;

	if (shadow_fbo != 0 && (shadow_width != width || shadow_height != height)) {
		glDeleteFramebuffers(1, &shadow_fbo);
		glDeleteTextures(1, &shadow_texture);
		shadow_fbo = 0;
		shadow_texture = 0;
	}
	if (shadow_fbo == 0) {
		shadow_ensure(width, height);
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, shadow_fbo);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	shadow_frame = (int)frames_replayed + 1;
	shadow_captures++;
}

/* The stage's probe block: runs after every draw, but only inside a label frame. */
static void
panel_probe_after_draw(void)
{
	GLint fbo = 0, program = 0;
	GLsizei width = 0, height = 0;
	int composite;
	unsigned nonzero = 0, bright = 0;
	uint64_t hash;
	int x, y, w, h;

	if (!panel_probe_on && !panel_shadow_on) {
		return;
	}
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
	if (fbo == 0) {
		return;
	}
	remember_drawn_target((GLuint)fbo);
	if (!panel_target_size((GLuint)fbo, &width, &height) || width != trace_width ||
	    height != trace_height) {
		return;
	}
	composite = draw_samples_label_tile();
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	/*
	 * A label composite is hashed over the text's own rectangle (small read-back), and the shadow
	 * keeps the panel as it stood right after it.  Every other draw of the frame is hashed over its
	 * own scissor box, if it is small, so the sequence says which draw wrote the text and which one
	 * painted over it.  Nothing here reads the whole panel: a stall in the host is what makes the
	 * guest's display lock time out.
	 */
	if (panel_probe_on && label_rect_valid) {
		x = label_rect[0];
		y = label_rect[1];
		w = label_rect[2];
		h = label_rect[3];
		rect_clip(&x, &y, &w, &h, width, height);
		if (w > 0 && h > 0 && (size_t)w * (size_t)h <= 200000 && probe_said < 4000) {
			probe_said++;
			hash = region_state((GLuint)fbo, x, y, w, h, &nonzero, &bright);
			printf("glwire: panel probe frame %llu draw %d program %d fbo %d rect %d,%d %dx%d "
			    "%s hash %016llx nonzero %u bright %u\n",
			    (unsigned long long)frames_replayed + 1, draws_seen - 1, (int)program, (int)fbo,
			    x, y, w, h, composite ? "label-composite" : "draw", (unsigned long long)hash,
			    nonzero, bright);
			fflush(stdout);
		}
	}
	if (composite) {
		label_composites++;
		/* the first composite of the frame is the one that puts the text in: a later draw that
		 * samples the same tile again is the frame rebuilding over it */
		if (panel_shadow_on && shadow_frame != (int)frames_replayed + 1) {
			shadow_capture((GLuint)fbo, width, height);
		}
		return;
	}
	if (!panel_probe_on || probe_said >= 6000 || !glIsEnabled(GL_SCISSOR_TEST)) {
		return;
	}
	{
		GLint scissor[4] = { 0, 0, 0, 0 };

		glGetIntegerv(GL_SCISSOR_BOX, scissor);
		x = scissor[0];
		y = scissor[1];
		w = scissor[2];
		h = scissor[3];
	}
	rect_clip(&x, &y, &w, &h, width, height);
	if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > 200000) {
		return;
	}
	probe_said++;
	hash = region_state((GLuint)fbo, x, y, w, h, &nonzero, &bright);
	printf("glwire: panel probe frame %llu draw %d program %d fbo %d rect %d,%d %dx%d "
	    "hash %016llx nonzero %u bright %u\n",
	    (unsigned long long)frames_replayed + 1, draws_seen - 1, (int)program, (int)fbo,
	    x, y, w, h, (unsigned long long)hash, nonzero, bright);
	fflush(stdout);
}

static void
host_fbo_summary(const char *tag, unsigned long long frame, struct client *client)
{
	printf("glwire: fbo summary %s frame %llu client %d own %u drawn %u readback-ready %u:",
	    tag, frame, client->fd, (unsigned)client->framebuffer, (unsigned)client->names.drawn_framebuffer,
	    (unsigned)client->names.bound_framebuffer);
	for (unsigned i = 0; i < fbo_note_count; i++) {
		struct fbo_note *note = &fbo_notes[i];

		if (note->fd != client->fd) {
			continue;
		}
		printf(" [guest %llu host %llu %s %llu %dx%d binds %u draws %u]", (unsigned long long)note->guest,
		    (unsigned long long)note->host,
		    note->kind == GL_TEXTURE ? "texture" : note->kind == GL_RENDERBUFFER ? "renderbuffer" : "none",
		    (unsigned long long)note->attach, (int)note->width, (int)note->height, note->binds,
		    note->draws);
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


/*
 * Stage nineteen, P0/v3: one line per text draw, so the draws that land pixels and the ones that
 * land none are compared field by field.  The fields are what the two groups can differ in -- the
 * target and its own size (which need not be the viewport's), the viewport, the scissor test and its
 * box, the four vertices of the position attribute and their bounding box, the four texcoords and
 * the first vertex's raw bytes, the vertex matrix, the sampler matrix bits, the blend state, the
 * frame and the draw's global serial, and the read-back counts inside the scissor rectangle.
 *
 * The state is the snapshot host_note_draw() took before dispatch() calls glDraw*; this line only
 * reads the target back.  That ordering is the point: names_attribs_release() frees the CPU copies
 * of the vertex arrays before host_after_draw() runs, so an after-draw line could never see them.
 *
 * GLWIRE_TEXT_DRAW_LINE=1 turns it on.  GLWIRE_TEXT_DRAW_LIMIT caps the lines (default 20000), and
 * GLWIRE_TEXT_DRAW_FROM_FRAME=<n> starts at that frame (default: every frame).  Off by default, so a
 * measurement that does not ask for it is unchanged.
 */
static int
host_attrib_xy(const struct attrib_state *attrib, GLint type, int vertex, float *x, float *y)
{
	const unsigned char *at = (const unsigned char *)attrib->data +
	    (size_t)vertex * (size_t)(attrib->stride > 0 ? attrib->stride : 0);

	if (type == GL_FLOAT) {
		memcpy(x, at, 4);
		memcpy(y, at + 4, 4);
		return 1;
	}
	if (type == 0x140b) {   /* GL_HALF_FLOAT: what the guest records for some arrays */
		uint16_t hx = 0, hy = 0;

		memcpy(&hx, at, 2);
		memcpy(&hy, at + 2, 2);
		*x = half_to_float(hx);
		*y = half_to_float(hy);
		return 1;
	}
	return 0;
}

/*
 * The draw's state, taken before it runs.  Every query here is a read except the temporary binding
 * of the draw framebuffer and of an attachment name to get its size; both are put back, and the
 * draw framebuffer binding is the one the draw is about to use anyway.
 */
static void
host_text_snapshot(struct glstate *g, unsigned long long draw_number)
{
	struct text_draw_snapshot *s = &text_snap;

	memset(s, 0, sizeof(*s));
	s->valid = 1;
	s->frame = frames_replayed + 1;
	s->draw_number = draw_number;
	s->kind = -1;

	glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->fbo);
	glGetIntegerv(GL_VIEWPORT, s->view);
	s->scissor_on = glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0;
	glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
	s->blend_on = glIsEnabled(GL_BLEND) ? 1 : 0;
	glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src);
	glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst);

	/* the position array: attribute 0 when it is the 2+-wide float/half array, else the first one */
	s->attrib_index = -1;
	for (GLint i = 0; i < 16; i++) {
		GLint enabled = 0, size = 0, type = 0;

		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
		if (!enabled || g->attribs[i].data == NULL) {
			continue;
		}
		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
		glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
		if (size < 2 || (type != GL_FLOAT && type != 0x140b)) {
			continue;
		}
		if (s->attrib_index < 0 || i == 0) {
			s->attrib_index = i;
			s->attrib_type = type;
			if (i == 0) {
				break;
			}
		}
	}
	if (s->attrib_index >= 0) {
		s->x0 = s->y0 = 1e30f;
		s->x1 = s->y1 = -1e30f;
		for (int v = 0; v < 4; v++) {
			float x = 0, y = 0;

			/* (a quad is four vertices: the bbox is what the scissor has to contain) */
			if (!host_attrib_xy(&g->attribs[s->attrib_index], s->attrib_type, v, &x, &y)) {
				break;
			}
			s->xs[v] = x;
			s->ys[v] = y;
			if (x < s->x0) s->x0 = x;
			if (x > s->x1) s->x1 = x;
			if (y < s->y0) s->y0 = y;
			if (y > s->y1) s->y1 = y;
			s->vertices++;
		}
	}

	/* attrib 3 is texcoord0 in the guest's stream: four vertices of two floats, plus the first
	 * vertex's 8 raw bytes so it can be compared with the guest's own emit line word by word */
	{
		GLint enabled3 = 0, size3 = 0, type3 = 0;

		glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled3);
		if (enabled3 && g->attribs[3].data != NULL) {
			glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size3);
			glGetVertexAttribiv(3, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type3);
		}
		if (enabled3 && g->attribs[3].data != NULL && size3 >= 2 &&
		    (type3 == GL_FLOAT || type3 == 0x140b)) {
			size_t avail = g->attribs[3].stride > 0 ? (size_t)g->attribs[3].stride :
			    (size_t)size3 * (type3 == 0x140b ? 2u : 4u);

			memcpy(s->tc_raw, g->attribs[3].data, avail < sizeof(s->tc_raw) ? avail : sizeof(s->tc_raw));
			s->tc_valid = 1;
			for (int v = 0; v < 4; v++) {
				if (!host_attrib_xy(&g->attribs[3], type3, v, &s->tcs[v][0], &s->tcs[v][1])) {
					break;
				}
			}
		}
	}

	/* bits, not printed floats: a NaN or a sign flip has to be visible, not rounded away */
	{
		GLint location = glGetUniformLocation((GLuint)s->program, "vertex_matrix");

		if (location >= 0) {
			GLfloat matrix[16] = { 0 };

			glGetUniformfv((GLuint)s->program, location, matrix);
			memcpy(s->vertex_matrix, matrix, sizeof(s->vertex_matrix));
			s->matrix_valid = 1;
		}
	}
	{
		GLint location = glGetUniformLocation((GLuint)s->program, "texmat0");

		if (location >= 0) {
			GLfloat texmat[4] = { 0, 0, 0, 0 };

			glGetUniformfv((GLuint)s->program, location, texmat);
			memcpy(s->texmat, texmat, sizeof(s->texmat));
		}
	}

	/* the target's own size: the attachment's, not the viewport's, is what clips the quad */
	if (s->fbo != 0) {
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->fbo);
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &s->kind);
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &s->name);
		if (s->kind == GL_TEXTURE && s->name != 0) {
			GLint previous_texture = 0;

			glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
			glBindTexture(GL_TEXTURE_2D, (GLuint)s->name);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &s->width);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &s->height);
			glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
		} else if (s->kind == GL_RENDERBUFFER && s->name != 0) {
			GLint previous_rb = 0;

			glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_rb);
			glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)s->name);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &s->width);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &s->height);
			glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)previous_rb);
		}
		if (s->width <= 0 || s->height <= 0) {
			s->width = s->view[2];
			s->height = s->view[3];
		}
	} else {
		s->width = s->view[2] > 0 ? s->view[2] : trace_width;
		s->height = s->view[3] > 0 ? s->view[3] : trace_height;
	}

	/* unit 0's sampled texture: size only (no read-back -- an empty tile and a feedback loop are
	 * both "tc0 is right, nothing landed" until the texture itself is named) */
	{
		GLint previous_active = 0;

		glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->u0_tex);
		if (s->u0_tex != 0) {
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &s->u0_w);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &s->u0_h);
			s->u0_valid = 1;
		}
		glActiveTexture((GLenum)previous_active);
	}
}

/* The line itself: read the snapshot's target rectangle back, then print the snapshot.  The
 * snapshot is the draw's own state, so nothing here queries or infers the draw. */
static void
host_text_draw_line(const struct text_draw_snapshot *snap)
{
	static unsigned lines_said;
	unsigned limit = getenv("GLWIRE_TEXT_DRAW_LIMIT") != NULL ?
	    (unsigned)atoi(getenv("GLWIRE_TEXT_DRAW_LIMIT")) : 20000;
	const char *from_text = getenv("GLWIRE_TEXT_DRAW_FROM_FRAME");
	unsigned long long from_frame = from_text != NULL ? strtoull(from_text, NULL, 10) : 0;
	unsigned long long frame = snap->frame, draw_number = snap->draw_number;
	GLint read_fbo = 0, draw_fbo = 0, read_buffer = 0, draw_buffer = 0;
	int rect_valid = 0, rx = 0, ry = 0, rw = 0, rh = 0;
	unsigned nonzero = 0, alpha = 0;
	char vtext[256], mtext[200], tctext[256], nztext[32], atext[32], utext[64];

	if (!snap->valid) {
		return;
	}
	if (from_frame != 0 && frame < from_frame) {
		return;
	}
	/* (a boot has 1600+ label draws: cap the lines so one measurement cannot fill a disk,
	 * and say once where it stopped) */
	if (lines_said >= limit) {
		if (lines_said == limit) {
			lines_said++;
			printf("glwire: text draw line limit reached %u (GLWIRE_TEXT_DRAW_LIMIT)\n", limit);
			fflush(stdout);
		}
		return;
	}
	lines_said++;

	if (snap->vertices > 0) {
		snprintf(vtext, sizeof(vtext), "%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f bbox %.1f,%.1f %.1f,%.1f",
		    (double)snap->xs[0], (double)snap->ys[0], (double)snap->xs[1], (double)snap->ys[1],
		    (double)snap->xs[2], (double)snap->ys[2], (double)snap->xs[3], (double)snap->ys[3],
		    (double)snap->x0, (double)snap->y0, (double)snap->x1, (double)snap->y1);
	} else {
		snprintf(vtext, sizeof(vtext), "none");
	}
	if (snap->tc_valid) {
		int at = 0;

		for (int v = 0; v < 4; v++) {
			at += snprintf(tctext + at, sizeof(tctext) - (size_t)at, "%s%.3f,%.3f",
			    at == 0 ? "" : " ", (double)snap->tcs[v][0], (double)snap->tcs[v][1]);
		}
		snprintf(tctext + at, sizeof(tctext) - (size_t)at, " raw %08x %08x",
		    snap->tc_raw[0], snap->tc_raw[1]);
	} else {
		snprintf(tctext, sizeof(tctext), "-");
	}
	if (snap->matrix_valid) {
		int at = 0;

		at += snprintf(mtext + at, sizeof(mtext) - (size_t)at, "matrix");
		for (int i = 0; i < 16 && at < (int)sizeof(mtext) - 10; i++) {
			at += snprintf(mtext + at, sizeof(mtext) - (size_t)at, " %08x", snap->vertex_matrix[i]);
		}
	} else {
		snprintf(mtext, sizeof(mtext), "matrix -");
	}
	if (snap->u0_valid) {
		snprintf(utext, sizeof(utext), "tex %d %dx%d%s", (int)snap->u0_tex, (int)snap->u0_w,
		    (int)snap->u0_h, snap->name != 0 && snap->u0_tex == snap->name ? " u0==target" : "");
	} else {
		snprintf(utext, sizeof(utext), "tex 0 -");
	}

	/* (the read-back bookkeeping is the only state this line touches: it is restored below) */
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
	glGetIntegerv(GL_READ_BUFFER, &read_buffer);
	glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);

	/*
	 * Only the scissor rectangle is read: it is the draw's own clip, so a count inside it is the
	 * "did this draw land a pixel" answer, and it is small (a glyph box, a tile).  With the scissor
	 * off the whole target is read only when it is at most a million pixels; a 750x1624 panel is
	 * not, and one full read per draw is what would stall the guest's display lock.
	 */
	if (trace_client != NULL && snap->width > 0 && snap->height > 0) {
		if (snap->scissor_on) {
			rx = snap->scissor[0];
			ry = snap->scissor[1];
			rw = snap->scissor[2];
			rh = snap->scissor[3];
			rect_clip(&rx, &ry, &rw, &rh, snap->width, snap->height);
		} else {
			rx = 0;
			ry = 0;
			rw = snap->width;
			rh = snap->height;
		}
		if (rw > 0 && rh > 0 && (uint64_t)rw * (uint64_t)rh <= 1000000) {
			size_t bytes = (size_t)rw * (size_t)rh * 4;
			unsigned char *pixels = calloc(1, bytes);

			if (pixels != NULL) {
				GLint pack = 4;

				rect_valid = 1;
				glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)snap->fbo);
				glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glReadPixels(rx, ry, rw, rh, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
				glPixelStorei(GL_PACK_ALIGNMENT, pack);
				for (size_t i = 0; i + 3 < bytes; i += 4) {
					if (pixels[i] | pixels[i + 1] | pixels[i + 2] | pixels[i + 3]) {
						nonzero++;
					}
					if (pixels[i + 3] != 0) {
						alpha++;
					}
				}
				free(pixels);
			}
		}
	}
	if (rect_valid) {
		snprintf(nztext, sizeof(nztext), "%u", nonzero);
		snprintf(atext, sizeof(atext), "%u", alpha);
	} else {
		rx = ry = rw = rh = 0;
		snprintf(nztext, sizeof(nztext), "-");
		snprintf(atext, sizeof(atext), "-");
	}

	/* put the bindings and the buffer selections back exactly as the draw left them, so a
	 * GLWIRE_LABEL_DUMP read-back that follows sees the same read/draw framebuffers it would
	 * have seen without this line (GL_READ_BUFFER/GL_DRAW_BUFFER are per-framebuffer, so they
	 * are restored after their framebuffer is bound again) */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
	glReadBuffer((GLenum)read_buffer);
	glDrawBuffer((GLenum)draw_buffer);

	printf("glwire: text draw frame %llu #%llu program %d fbo %d %s %d %dx%d viewport "
	    "%d,%d %dx%d scissor %s %d,%d %dx%d blend %s %#x %#x attrib %d %s texmat "
	    "%08x %08x %08x %08x readrect %d,%d %dx%d scissor-nonzero %s alpha %s tc0 %s %s u0 %s\n",
	    frame, draw_number, (int)snap->program, (int)snap->fbo,
	    snap->kind == GL_TEXTURE ? "texture" :
	    snap->kind == GL_RENDERBUFFER ? "renderbuffer" : (snap->fbo != 0 ? "other" : "default"),
	    (int)snap->name, (int)snap->width, (int)snap->height, (int)snap->view[0],
	    (int)snap->view[1], (int)snap->view[2], (int)snap->view[3],
	    snap->scissor_on ? "on" : "off", (int)snap->scissor[0], (int)snap->scissor[1],
	    (int)snap->scissor[2], (int)snap->scissor[3], snap->blend_on ? "on" : "off",
	    snap->blend_src, snap->blend_dst, snap->attrib_index, vtext, snap->texmat[0],
	    snap->texmat[1], snap->texmat[2], snap->texmat[3], rx, ry, rw, rh, nztext, atext,
	    tctext, mtext, utext);
	fflush(stdout);
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

	/* (the draw is over: put the stream's viewport back before any probe reads it) */
	host_viewport_restore();

	/*
	 * Stage seventeen: the label's own rectangle first (the dump below clears last_draw_was_glyph),
	 * then the two probes.  They run for every draw of a frame that had a text pass, so the hash
	 * list reads: the composite wrote, then whichever draw the frame ended with.
	 */
	if (last_draw_was_glyph && (panel_probe_on || panel_shadow_on)) {
		GLint label_fbo = 0;

		label_rect_capture();
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &label_fbo);
		if (label_fbo != 0) {
			GLint kind = -1, name = 0;

			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
			if (kind == GL_TEXTURE && name != 0) {
				unsigned at;

				for (at = 0; at < label_tile_count; at++) {
					if (label_tiles[at] == (GLuint)name) {
						break;
					}
				}
				if (at == label_tile_count && label_tile_count < 16) {
					label_tiles[label_tile_count++] = (GLuint)name;
				}
			}
		}
	}
	if (label_rect_frame == (int)frames_replayed + 1) {
		panel_probe_after_draw();
	}
	if (getenv("GLWIRE_PROGRAM_ALL") != NULL) {
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		host_program_source(g, (GLuint)program);
	}

	/*
	 * Stage nineteen: what does the text draw actually leave in its tile?  The counters from stage
	 * fourteen cannot say -- the backdrop fill makes every pixel of a 64x19 tile non-zero -- but the
	 * pixels themselves can: a glyph strip has dark and light columns, a wipe has one colour.  Each
	 * dumped tile is written as its own small file, and the minimum and maximum say at a glance
	 * whether the draw wrote anything at all.
	 */
	if (getenv("GLWIRE_TILE_DUMP") != NULL && last_draw_was_glyph) {
		static unsigned dumped;
		GLint tile_fbo = 0;
		int from = getenv("GLWIRE_TILE_DUMP_FROM") != NULL ? atoi(getenv("GLWIRE_TILE_DUMP_FROM")) : 0;

		if ((int)frames_replayed + 1 >= from && dumped < 200 && (int)frames_replayed % 10 == 0) {
			GLint kind = -1, name = 0, previous = 0, width = 0, height = 0;

			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &tile_fbo);
			if (tile_fbo != 0) {
				glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)tile_fbo);
				glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
				glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
				if (kind == GL_TEXTURE && name != 0) {
					glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
					glBindTexture(GL_TEXTURE_2D, (GLuint)name);
					glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
					glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
					glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
					if (width > 0 && height > 0 && width <= 512 && height <= 512) {
						size_t bytes = (size_t)width * (size_t)height * 4;
						unsigned char *pixels = malloc(bytes);
						unsigned char lo = 255, hi = 0;
						unsigned nonzero = 0;

						if (pixels != NULL) {
							GLint pack = 4;

							glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
							glPixelStorei(GL_PACK_ALIGNMENT, 1);
							glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
							glPixelStorei(GL_PACK_ALIGNMENT, pack);
							for (size_t i = 0; i + 3 < bytes; i += 4) {
								unsigned char v = (unsigned char)((pixels[i] + pixels[i + 1] +
								    pixels[i + 2]) / 3);

								if (v < lo) lo = v;
								if (v > hi) hi = v;
								if (pixels[i] | pixels[i + 1] | pixels[i + 2] | pixels[i + 3]) nonzero++;
							}
							{
								char path[512];
								FILE *file;

								snprintf(path, sizeof(path), "%s-f%llu-t%u-%dx%d.bgra",
								    getenv("GLWIRE_TILE_DUMP"), frames_replayed + 1,
								    (unsigned)name, (int)width, (int)height);
								file = fopen(path, "wb");
								if (file != NULL) {
									fwrite(pixels, 1, bytes, file);
									fclose(file);
									dumped++;
									printf("glwire: label tile dump %s %dx%d nonzero %u "
									    "grey %u..%u\n", path, (int)width, (int)height,
									    nonzero, lo, hi);
									fflush(stdout);
								}
							}
							free(pixels);
						}
					}
				}
				glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)tile_fbo);
			}
		}
	}
	/* (the draw's own state, taken by host_note_draw() before glDraw*: read back and print it) */
	if (getenv("GLWIRE_TEXT_DRAW_LINE") != NULL && text_snap.valid) {
		host_text_draw_line(&text_snap);
	}
	/*
	 * Stage fourteen: every label draw (the one that binds a glyph tile) into the framebuffer the
	 * host stands in for FBO 0 -- how many pixels it actually covered, and to a file the first few
	 * times, so "the shader wrote transparent black" is told from "the quad missed the scissor".
	 */
	if (getenv("GLWIRE_LABEL_DUMP") != NULL && last_draw_was_glyph) {
		static unsigned labels, wrote;
		GLint label_fbo = 0;

		last_draw_was_glyph = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &label_fbo);
		if (label_fbo != 0 && getenv("GLWIRE_TILE_TRACE") != NULL) {
			GLint kind = -1, name = 0;

			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
			if (kind == GL_TEXTURE && name != 0) {
				tile_note_add(g, (GLuint)name);
			}
		}
		if (trace_client != NULL && label_fbo != 0) {
			GLint kind = -1, name = 0, read_width = trace_width, read_height = trace_height;
			GLint previous = 0;
			size_t bytes;
			unsigned char *shot;
			unsigned nonzero = 0, alpha = 0;

			glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)label_fbo);
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
			if (kind == GL_TEXTURE && name != 0) {
				GLint texture_width = 0, texture_height = 0;

				glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
				glBindTexture(GL_TEXTURE_2D, (GLuint)name);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texture_width);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texture_height);
				glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
				if (texture_width > 0 && texture_height > 0) {
					read_width = texture_width;
					read_height = texture_height;
				}
			}
			if (read_width <= 0 || read_height <= 0) {
				read_width = trace_width;
				read_height = trace_height;
			}
			if (read_width <= 0 || read_height <= 0) {
				read_width = 1;
				read_height = 1;
			}
			bytes = (size_t)read_width * (size_t)read_height * 4;
			shot = malloc(bytes);
			if (shot != NULL) {
				memset(shot, 0, bytes);
				glReadPixels(0, 0, read_width, read_height, GL_BGRA, GL_UNSIGNED_BYTE, shot);
				for (size_t i = 0; i + 3 < bytes; i += 4) {
					if (shot[i] | shot[i + 1] | shot[i + 2] | shot[i + 3]) {
						nonzero++;
					}
					if (shot[i + 3] != 0) {
						alpha++;
					}
				}
				labels++;
				printf("glwire: label draw #%u framebuffer %d attachment %s %d %dx%d: %u nonzero pixels, "
				    "%u with alpha, glerror %#x\n", labels, (int)label_fbo,
				    kind == GL_TEXTURE ? "texture" : kind == GL_RENDERBUFFER ? "renderbuffer" : "none",
				    name, (int)read_width, (int)read_height, nonzero, alpha, glGetError());
				fflush(stdout);
				if (labels <= 4) {
					char path[512];

					snprintf(path, sizeof(path), "%s-label-%u-fbo-%d-%dx%d.bgra",
					    getenv("GLWIRE_LABEL_DUMP"), labels, (int)label_fbo, (int)read_width,
					    (int)read_height);
					if (write_pixels(path, shot, bytes)) {
						wrote++;
					}
				}
				free(shot);
			}
		}
	}
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
	int version = 0;                /* the record layout the guest speaks (stage fifteen: 2 = with context) */
	/* P1'''': a frame may name a second panel-sized surface (the display presents two in turn). */
	int segment_count2 = 0, row_bytes2 = 0;
	size_t records_bytes = 0, frame_bytes = 0;
	double began, took;
	unsigned char *records;
	unsigned char *picture;
	GLint saved_viewport[4] = { 0, 0, 0, 0 };

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
	if (sscanf(line, "R %d %d %zu %zu %d %d %d %d %d", &width, &height, &records_bytes, &frame_bytes,
	    &segment_count, &row_bytes, &segment_count2, &row_bytes2, &version) < 4) {
		fprintf(stderr, "host_glwire: bad request '%s'\n", line);
		return -1;
	}
	/* (an older guest sends eight fields: its records keep the 8-byte header, context 0) */
	wire_set_record_version(version);
	if (version >= 2 && getenv("GLWIRE_CONTEXT_LOG") != NULL) {
		printf("host_glwire: client %d speaks record version %d (context in every header)\n", client->fd,
		    version);
		fflush(stdout);
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
	fbo_client_fd = client->fd;
	probe_errors = (getenv("GLWIRE_ERROR_LOG") != NULL && frames_replayed < 12) || dump_draws_runtime;
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

		while (offset + wire_header_bytes(version) <= records_bytes) {
			uint32_t opcode, length;

			memcpy(&opcode, records + offset, 4);
			memcpy(&length, records + offset + 4, 4);
			if (offset + wire_header_bytes(version) + length > records_bytes) {
				break;
			}
			if (opcode == 201) {
				upload.base = records + offset + wire_header_bytes(version);
				upload.capacity = length;
				upload.at = 0;
				dispatch(&client->names, &upload, opcode);
			}
			offset += wire_header_bytes(version) + length;
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
	/*
	 * Stage fifteen: eagl.c ends a frame on glFlush/glFinish, and a CoreAnimation pass need not end
	 * there -- the label pass continues into the next frame record with the framebuffer its previous
	 * frame bound.  Resetting the binding to this renderer's own framebuffer made those draws land
	 * on a 750x1624 stand-in while they were aimed at a 184x90 glyph surface (their scissor says so).
	 * GLWIRE_KEEP_BINDING=1 keeps the guest's binding across the boundary; a binding of 0 is the
	 * drawable, which is what the stand-in is for.
	 *
	 * Stage nineteen, v5: the viewport is continuous across the boundary in the same way.  The
	 * guest leaves a glyph target's viewport (64x19/750x200) set at the previous frame's end, and
	 * a per-frame glViewport(0,0,width,height) threw it away before the next frame's first text
	 * draw -- whose matrix is built for that small target.  Keep the guest's value; only the
	 * host's own panel picture needs the panel viewport, and it is put back immediately.
	 * GLWIRE_RESET_VIEWPORT=1 restores the old unconditional reset for an A/B run.
	 */
	glGetIntegerv(GL_VIEWPORT, &saved_viewport[0]);
	client->names.stand_in = client->framebuffer;
	panel_drawn_fbo = 0;             /* (stage seventeen: the frame's panel target starts unknown) */
	shadow_frame = -1;
	shadow_used = 0;
	if (keep_binding && client->names.framebuffer_known && client->names.bound_framebuffer != 0) {
		GLint kind = -1;

		glBindFramebuffer(GL_FRAMEBUFFER, client->names.bound_framebuffer);
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
		if (kind == GL_NONE || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			glBindFramebuffer(GL_FRAMEBUFFER, client->framebuffer);
			client->names.bound_framebuffer = client->framebuffer;
		}
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, client->framebuffer);
		client->names.bound_framebuffer = client->framebuffer;
	}
	if (getenv("GLWIRE_RESET_VIEWPORT") != NULL) {
		glViewport(0, 0, width, height);
		saved_viewport[0] = 0;
		saved_viewport[1] = 0;
		saved_viewport[2] = width;
		saved_viewport[3] = height;
	}
	if (!no_panel && client->names.panel != NULL && client->names.panel_width == width &&
	    client->names.panel_height == height) {
		glViewport(0, 0, width, height);
		draw_panel(client, client->names.panel, width, height);
		glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2],
		    saved_viewport[3]);
	} else {
		wire_buffer r;
		size_t offset = 0;

		memset(replay_ops, 0, sizeof(replay_ops));
		client->names.drawn_framebuffer = 0;
		replay_used = 1;
		replayed = 1;
		while (offset + wire_header_bytes(version) <= records_bytes) {
			uint32_t opcode, length, context;
			static unsigned last_context;
			static int context_known;

			memcpy(&opcode, records + offset, 4);
			memcpy(&length, records + offset + 4, 4);
			if (offset + wire_header_bytes(version) + length > records_bytes) {
				fprintf(stderr, "host_glwire: a record runs past the frame\n");
				break;
			}
			if (opcode >= 256) {
				fprintf(stderr, "host_glwire: opcode %u out of range\n", opcode);
				break;
			}
			if (opcode == 201) {
				offset += wire_header_bytes(version) + length;
				continue;       /* already uploaded before the framebuffer was bound */
			}
			context = wire_header_context(records + offset, version);
			record_context = context;
			record_context_known = version >= 2;
			if (getenv("GLWIRE_CONTEXT_LOG") != NULL && (!context_known || context != last_context)) {
				static unsigned said;

				if (said < 4000) {
					said++;
					printf("glwire: context %u at opcode %u, record %zu of frame %llu\n", context,
					    opcode, offset, (unsigned long long)frames_replayed + 1);
					fflush(stdout);
				}
				context_known = 1;
				last_context = context;
			}
			replay_ops[opcode]++;
			r.base = records + offset + wire_header_bytes(version);
			r.capacity = length;
			r.at = 0;
			{
				static unsigned glerrors;
				int failed = dispatch(&client->names, &r, opcode);
				GLenum error = probe_errors ? glGetError() : GL_NO_ERROR;

				if (error != GL_NO_ERROR && glerrors < 20) {
					glerrors++;
					printf("glwire: glerror %#x after opcode %u (%d records in)\n", error,
					    opcode, (int)(offset + wire_header_bytes(version) + length));
					fflush(stdout);
				}
				if (failed != 0) {
					fprintf(stderr, "host_glwire: dispatch stopped at opcode %u\n", opcode);
					break;
				}
			}
			offset += wire_header_bytes(version) + length;
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
		 *
		 * Stage seventeen: "the last draw" is not the panel.  CoreAnimation ends a fair share of
		 * its frames with the text pass into a 64x19 tile, and the picture read back from that
		 * framebuffer is 64x19 pixels and black everywhere else -- the panel the frame composed
		 * one draw earlier is never read.  GLWIRE_READ_PANEL=1 reads the last *panel-sized* target
		 * the frame drew into instead; GLWIRE_READ_PANEL=0 restores the old choice.
		 */
		{
			GLuint read_from = client->names.drawn_framebuffer;

			if (read_panel && panel_drawn_fbo != 0 && panel_drawn_frame == (int)frames_replayed + 1) {
				read_from = panel_drawn_fbo;
			}
			if (replayed && read_from != 0 && read_from != (GLuint)bound) {
				glBindFramebuffer(GL_FRAMEBUFFER, read_from);
				glGetIntegerv(GL_VIEWPORT, box);
				printf("host_glwire: reading from %u instead, viewport %d,%d %dx%d%s\n",
				    read_from, box[0], box[1], box[2], box[3],
				    read_from == panel_drawn_fbo && read_from != client->names.drawn_framebuffer
					? " (panel, not the frame's last draw)" : "");
			}
		}
		fflush(stdout);
	}
	if (fbo_log && (frames_replayed + 1 <= 2 || (frames_replayed + 1) % 100 == 0 || dump_draws_runtime)) {
		host_fbo_summary("frame", frames_replayed + 1, client);
	}
	glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, picture);
	/*
	 * Stage seventeen, P0: the label composite is in the shadow.  Either it supplies the picture
	 * (mode 1, the whole target as it stood right after the composite) or only the text's own
	 * rectangle is patched back into the frame the stream ended with (mode 2).  Both are the
	 * experiment; which one is right is what the three run's screenshots say.
	 */
	if (panel_shadow_on && shadow_apply && shadow_frame == (int)frames_replayed + 1 && shadow_fbo != 0) {
		GLint previous = 0;

		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
		glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
		if (shadow_mode == 2 && label_rect_valid) {
			int x = label_rect[0], y = label_rect[1], w = label_rect[2], h = label_rect[3];

			rect_clip(&x, &y, &w, &h, width, height);
			if (w > 0 && h > 0) {
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glReadPixels(x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE,
				    picture + (size_t)y * (size_t)width * 4 + (size_t)x * 4);
			}
		} else {
			glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, picture);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
		shadow_used = 1;
		shadow_uses++;
	}
	if (shadow_dump_prefix != NULL && panel_shadow_on && shadow_fbo != 0 &&
	    shadow_frame == (int)frames_replayed + 1 &&
	    shadow_dump_every > 0 && ((int)frames_replayed + 1) % shadow_dump_every == 0) {
		char path[512];
		FILE *file;

		snprintf(path, sizeof(path), "%s-shadow-%llu-%dx%d.bgra", shadow_dump_prefix,
		    frames_replayed + 1, width, height);
		file = fopen(path, "wb");
		if (file != NULL) {
			unsigned char *shadow_pixels = malloc(frame_bytes);

			if (shadow_pixels != NULL) {
				GLint previous = 0;

				glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
				glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, shadow_pixels);
				glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
				fwrite(shadow_pixels, 1, frame_bytes, file);
				free(shadow_pixels);
				printf("host_glwire: saved %s\n", path);
				fflush(stdout);
			}
			fclose(file);
		}
		snprintf(path, sizeof(path), "%s-picture-%llu-%dx%d.bgra", shadow_dump_prefix,
		    frames_replayed + 1, width, height);
		file = fopen(path, "wb");
		if (file != NULL) {
			fwrite(picture, 1, frame_bytes, file);
			fclose(file);
		}
	}
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
	if (frame_delay_ms > 0) {
		/* (stage 15: the guest waits for this reply; a delay here is a knob on the frame period) */
		usleep((useconds_t)frame_delay_ms * 1000);
	}
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

	/*
	 * Stage nineteen: the card is not always ours.  GLWIRE_SOFTWARE_GL=1 skips the device
	 * enumeration and takes the default display, which with LIBGL_ALWAYS_SOFTWARE=1 is Mesa's
	 * llvmpipe -- slower, but it leaves the GPU to whatever else is running on it.
	 */
	if (getenv("GLWIRE_SOFTWARE_GL") != NULL) {
		EGLDisplay software = eglGetDisplay(EGL_DEFAULT_DISPLAY);

		if (software != EGL_NO_DISPLAY && eglInitialize(software, &major, &minor)) {
			return software;
		}
	}
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
	fbo_log = getenv("GLWIRE_FBO_LOG") != NULL;
	{
		/* (ADR-012: the guest's binding survives eagl.c's frame boundary; "=0" restores stage 14) */
		const char *keep = getenv("GLWIRE_KEEP_BINDING");

		keep_binding = keep == NULL || keep[0] != '0';
	}
	frame_delay_ms = getenv("GLWIRE_FRAME_DELAY_MS") != NULL ? atoi(getenv("GLWIRE_FRAME_DELAY_MS")) : 0;
	fbo_trace = getenv("GLWIRE_FBO_TRACE") != NULL;
	/* (stage seventeen: the two probes for "the label composites and is then rebuilt over") */
	panel_probe_on = getenv("GLWIRE_PANEL_PROBE") != NULL;
	panel_shadow_on = getenv("GLWIRE_PANEL_SHADOW") != NULL;
	shadow_mode = getenv("GLWIRE_SHADOW_MODE") != NULL ? atoi(getenv("GLWIRE_SHADOW_MODE")) : 1;
	shadow_dump_every = getenv("GLWIRE_SHADOW_EVERY") != NULL ? atoi(getenv("GLWIRE_SHADOW_EVERY")) : 0;
	shadow_dump_prefix = getenv("GLWIRE_SHADOW_DUMP");
	{
		const char *value = getenv("GLWIRE_SHADOW_APPLY");

		shadow_apply = value == NULL || value[0] != '0';
	}
	{
		const char *value = getenv("GLWIRE_READ_PANEL");

		read_panel = value == NULL || value[0] != '0';
	}
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
	printf("host_glwire: build marker uniform-ring-v2\n");
	fflush(stdout);
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
