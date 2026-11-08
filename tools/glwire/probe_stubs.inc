/* probe_stubs.inc: the host renderer's stage probes, as no-ops for the replay tests.
 *
 * dispatch.inc is shared by host_glwire (where the probes live) and by the two replay tests
 * (frame_replay, map_test), which only need the name maps and the dispatch itself.  Before this file
 * existed those two stopped compiling as soon as a probe was added to dispatch.inc -- the stage-13
 * uniform probe broke them and nobody rebuilt the tests.  The stubs say "nothing was filled, nothing
 * was logged, the legacy format was not touched", which is exactly the pre-probe behaviour.
 *
 * Included *before* dispatch.inc; not included by host_glwire.c, which defines the real ones.
 */
#ifndef GLWIRE_PROBE_STUBS
#define GLWIRE_PROBE_STUBS

static unsigned char *
host_fill_special(GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	(void)width;
	(void)height;
	(void)format;
	(void)type;
	(void)pixels;
	return NULL;
}

static int
host_legacy_teximage(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
	(void)target;
	(void)level;
	(void)internal;
	(void)width;
	(void)height;
	(void)border;
	(void)format;
	(void)type;
	(void)pixels;
	return 0;
}

static void
host_watch_upload(struct glstate *g, const char *how, GLuint guest, GLint level, GLint internal,
    GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels, GLsizei bytes)
{
	(void)g;
	(void)how;
	(void)guest;
	(void)level;
	(void)internal;
	(void)width;
	(void)height;
	(void)format;
	(void)type;
	(void)pixels;
	(void)bytes;
}

static void
host_log_uniform(struct glstate *g, const char *how, GLint guest_location, GLint host_location,
    const GLfloat *values, int count)
{
	(void)g;
	(void)how;
	(void)guest_location;
	(void)host_location;
	(void)values;
	(void)count;
}

static void
host_note_draw(struct glstate *g, const char *how, GLenum mode, GLsizei count)
{
	(void)g;
	(void)how;
	(void)mode;
	(void)count;
}

static void
host_viewport_log(GLint x, GLint y, GLsizei width, GLsizei height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}

static void
host_fbo_bind(struct glstate *g, GLenum target, GLuint guest, GLuint host)
{
	(void)g;
	(void)target;
	(void)guest;
	(void)host;
}

static void
host_fbo_gen(struct glstate *g, GLsizei n, const GLuint *guest, const GLuint *host)
{
	(void)g;
	(void)n;
	(void)guest;
	(void)host;
}

static void
host_fbo_attach(struct glstate *g, GLenum target, GLenum attachment, GLuint guest_tex, GLint level)
{
	(void)g;
	(void)target;
	(void)attachment;
	(void)guest_tex;
	(void)level;
}

static void
host_fbo_draw(struct glstate *g)
{
	(void)g;
}

/* (stage fifteen: the forced-colour label draw is a host_glwire experiment; the replay tests run
 * the recorded programme, as they always did) */
static int
host_label_begin(struct glstate *g)
{
	(void)g;
	return 0;
}

#endif /* GLWIRE_PROBE_STUBS */
