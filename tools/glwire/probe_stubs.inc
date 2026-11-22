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

static int
host_program_skip(struct glstate *g)
{
	(void)g;
	return 0;
}

static void
host_widget_rect(struct glstate *g, GLuint guest, GLint x, GLint y, GLsizei w, GLsizei h, int blank)
{
	(void)g;
	(void)guest;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)blank;
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

static void
host_note_uniform(struct glstate *g, const char *kind, GLint guest_location, GLint host_location,
    const GLfloat *values)
{
	(void)g;
	(void)kind;
	(void)guest_location;
	(void)host_location;
	(void)values;
}

static int
host_widget_blank(const unsigned char *data, GLsizei bytes, GLint w, GLint h, GLsizei row_bytes,
    GLsizei pixel_bytes)
{
	(void)data;
	(void)bytes;
	(void)w;
	(void)h;
	(void)row_bytes;
	(void)pixel_bytes;
	/* not blank: the replay draws the surface it uploaded, as it did before the widget probe */
	return 0;
}

/*
 * The replay switches, as "every recorded call is replayed": the same idea as the probe stubs above.
 * GLWIRE_SKIP_CLEAR, GLWIRE_SKIP_THIN_LINES, GLWIRE_FORCE_LINEAR, GLWIRE_LABEL_ROW_SCRUB and the
 * RLE request field are host_glwire dash experiments, and dispatch.inc reads them (and the
 * per-frame upload bookkeeping and the last text draw's scissor) directly. Defining them here keeps
 * the two replay tests compiling as the host grows switches -- the stage-26 thin-line default broke
 * them because dispatch.inc learned a new global and this file did not.
 */
int host_skip_clear;
int host_skip_thin_lines;
int host_force_linear;
static int label_row_scrub;
static int rle_wanted;
static int trace_width, trace_height;
static unsigned long long frames_replayed;
static GLuint panel_upload_texture;
static int panel_upload_frame;
static GLuint panel_upload_texture2;
static int panel_upload_frame2;
static unsigned char *panel_upload_copy;
static size_t panel_upload_copy_bytes;
static struct {
	int scissor_on;
	GLint scissor[4];
} text_snap;

#endif /* GLWIRE_PROBE_STUBS */
