/* names.h: the host's map from the guest's GL object names to its own.
 *
 * The guest runs every call locally too (the trampoline calls the slot's old entry first), so the
 * names CoreAnimation sees are the guest's. The host's context makes its own names, which need not
 * be the same numbers -- and on a context that has already made objects they are not. Every call
 * that produces a name (glGenTextures, glGenFramebuffers, glCreateShader, glCreateProgram,
 * glGetUniformLocation, glFenceSync) carries the guest's answer at the end of its record; this
 * header keeps the mapping, and every call that consumes a name translates it first.
 *
 * Uniform locations are per program, so they are keyed by (guest program, guest location): two
 * programs may both call their texture sampler location 3, and they are different locations on the
 * host as well.
 */
#ifndef GLWIRE_NAMES_H
#define GLWIRE_NAMES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire.h"

struct name_entry {
	uint64_t guest;
	uint64_t host;
	int state;                      /* 0 empty, 1 used, 2 tombstone */
};

struct name_map {
	struct name_entry *entries;
	size_t capacity;
	size_t used;
	size_t graves;
};

struct attrib_state {
	GLint size;
	GLenum type;
	GLsizei stride;
	GLboolean normalized;
	GLuint buffer;                  /* the vertex data, in a buffer object (llvmpipe faults on
	                                 * half-float client arrays; the guest's bytes are copied here) */
	void *data;                     /* the same bytes on the CPU, freed after the draw */
};

struct glstate {
	struct name_map textures, framebuffers, programs, shaders, syncs, uniforms;
	uint32_t current_program;       /* the guest's, for the uniform-location key */
	GLuint current_host_program;
	struct attrib_state attribs[16];
	unsigned char *panel;           /* the last 1125x2436 BGRA upload, packed to 4500 */
	int panel_width, panel_height;
	/* The framebuffer the stream bound last, and the one it last drew into: the guest reads its
	 * picture back from the texture that was drawn into, and after the stream has ended this side
	 * has to bind that same framebuffer before it reads (CoreAnimation unbinds before the frame
	 * ends, and the EGL surface here is a 16x16 pbuffer that holds nothing). */
	GLuint bound_framebuffer;
	GLuint drawn_framebuffer;
};

static uint64_t
names_hash(uint64_t x)
{
	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

static void
name_map_init(struct name_map *map)
{
	map->capacity = 64;
	map->entries = calloc(map->capacity, sizeof(*map->entries));
	map->used = 0;
	map->graves = 0;
	if (map->entries == NULL) {
		map->capacity = 0;
	}
}

static void
name_map_free(struct name_map *map)
{
	free(map->entries);
	map->entries = NULL;
	map->capacity = 0;
	map->used = 0;
	map->graves = 0;
}

static struct name_entry *
name_map_slot(struct name_map *map, uint64_t guest)
{
	size_t at = (size_t)(names_hash(guest) & (map->capacity - 1));

	for (;;) {
		struct name_entry *entry = &map->entries[at];

		if (entry->state != 1 || entry->guest == guest) {
			return entry;
		}
		at = (at + 1) & (map->capacity - 1);
	}
}

static void
name_map_grow(struct name_map *map)
{
	struct name_entry *old = map->entries;
	size_t old_capacity = map->capacity;

	map->capacity = old_capacity ? old_capacity * 2 : 64;
	map->entries = calloc(map->capacity, sizeof(*map->entries));
	map->used = 0;
	map->graves = 0;
	if (map->entries == NULL) {
		map->entries = old;
		map->capacity = old_capacity;
		return;
	}
	for (size_t i = 0; i < old_capacity; i++) {
		if (old[i].state == 1) {
			struct name_entry *entry = name_map_slot(map, old[i].guest);

			*entry = old[i];
			map->used++;
		}
	}
	free(old);
}

static void
name_map_put(struct name_map *map, uint64_t guest, uint64_t host)
{
	struct name_entry *entry;

	if (guest == 0 || map->capacity == 0) {
		return;
	}
	if ((map->used + map->graves + 1) * 10 > map->capacity * 7) {
		name_map_grow(map);
		if (map->capacity == 0) {
			return;
		}
	}
	entry = name_map_slot(map, guest);
	if (entry->state == 1) {
		entry->host = host;
		return;
	}
	if (entry->state == 2) {
		map->graves--;
	}
	entry->guest = guest;
	entry->host = host;
	entry->state = 1;
	map->used++;
}

static int
name_map_get(struct name_map *map, uint64_t guest, uint64_t *host)
{
	size_t at;

	if (guest == 0 || map->capacity == 0) {
		return 0;
	}
	at = (size_t)(names_hash(guest) & (map->capacity - 1));
	for (;;) {
		struct name_entry *entry = &map->entries[at];

		if (entry->state == 0) {
			return 0;
		}
		if (entry->state == 1 && entry->guest == guest) {
			*host = entry->host;
			return 1;
		}
		at = (at + 1) & (map->capacity - 1);
	}
}

static uint64_t
name_map_take(struct name_map *map, uint64_t guest)
{
	size_t at;
	uint64_t host = 0;

	if (guest == 0 || map->capacity == 0) {
		return 0;
	}
	at = (size_t)(names_hash(guest) & (map->capacity - 1));
	for (;;) {
		struct name_entry *entry = &map->entries[at];

		if (entry->state == 0) {
			return guest;           /* never seen: the stream's own name */
		}
		if (entry->state == 1 && entry->guest == guest) {
			host = entry->host;
			entry->state = 2;
			map->used--;
			map->graves++;
			return host;
		}
		at = (at + 1) & (map->capacity - 1);
	}
}

static void
names_init(struct glstate *g)
{
	memset(g, 0, sizeof(*g));
	name_map_init(&g->textures);
	name_map_init(&g->framebuffers);
	name_map_init(&g->programs);
	name_map_init(&g->shaders);
	name_map_init(&g->syncs);
	name_map_init(&g->uniforms);
}

static void
names_free(struct glstate *g)
{
	free(g->panel);
	g->panel = NULL;
	for (unsigned i = 0; i < 16; i++) {
		if (g->attribs[i].buffer != 0) {
			glDeleteBuffers(1, &g->attribs[i].buffer);
			g->attribs[i].buffer = 0;
		}
		free(g->attribs[i].data);
		g->attribs[i].data = NULL;
	}
	name_map_free(&g->textures);
	name_map_free(&g->framebuffers);
	name_map_free(&g->programs);
	name_map_free(&g->shaders);
	name_map_free(&g->syncs);
	name_map_free(&g->uniforms);
}

/* ---- the object maps: 0 is the GL's "no object" and stays 0 ---- */

static GLuint
names_texture(struct glstate *g, GLuint guest)
{
	uint64_t host;

	return name_map_get(&g->textures, guest, &host) ? (GLuint)host : guest;
}

static void
names_texture_put(struct glstate *g, uint64_t guest, uint64_t host)
{
	name_map_put(&g->textures, guest, host);
}

/* An upload may have created the texture before the stream's glGenTextures record is replayed;
 * that record must not throw the uploaded host texture away. */
static void
names_texture_put_new(struct glstate *g, uint64_t guest, uint64_t host)
{
	uint64_t present;

	if (!name_map_get(&g->textures, guest, &present)) {
		name_map_put(&g->textures, guest, host);
	}
}

static GLuint
names_texture_take(struct glstate *g, GLuint guest)
{
	return (GLuint)name_map_take(&g->textures, guest);
}

/* An IOSurface upload from eagl.c: create the host texture if the guest made it outside the table,
 * then give it the pixels with the surface's own row stride. */
static void
names_texture_upload(struct glstate *g, GLuint guest, GLenum target, GLint internal,
    GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei row_bytes,
    GLsizei pixel_bytes, void *data)
{
	uint64_t mapped = 0;
	GLuint host;
	GLint previous = 0, previous_alignment = 4, previous_row_length = 0;

	if (data == NULL || width <= 0 || height <= 0) {
		free(data);
		return;
	}
	/* GL_BGRA is a format, not a desktop internal format; the guest's ES driver accepts it. */
	if (internal == GL_BGRA) {
		internal = GL_RGBA;
	}
	/*
	 * Save the unit's binding *before* the creation branch binds the new texture: it used to be
	 * read afterwards, so a texture this upload created stayed bound when the replay began and a
	 * later draw with no glBindTexture sampled the wrong one.
	 */
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &previous_alignment);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &previous_row_length);
	if (!name_map_get(&g->textures, guest, &mapped)) {
		glGenTextures(1, &host);
		names_texture_put(g, guest, host);
		glBindTexture(target, host);
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	} else {
		host = (GLuint)mapped;
	}
	{
		/* The texture may be attached to the framebuffer that is bound; redefining it there
		 * makes llvmpipe 25.2.8 fault. Detach the framebuffer for the upload and put it back. */
		GLsizei row = pixel_bytes > 0 ? width * pixel_bytes : row_bytes;
		unsigned char *packed = malloc((size_t)row * (size_t)height);

		/* llvmpipe also mishandles a large GL_UNPACK_ROW_LENGTH: pack the rows first. */
		if (packed != NULL && row_bytes >= row) {
			for (GLsizei y = 0; y < height; y++) {
				memcpy(packed + (size_t)y * row, (const unsigned char *)data + (size_t)y * row_bytes, row);
			}
			/* eagl.c's composed panel is texture 0xFFFF0001; any full-panel upload is one too.
			 * Keeping it at its real size is what lets the host draw the panel the guest made,
			 * and what the acceptance check compares against. */
			if (guest == 0xFFFF0001u || (width == 1125 && height == 2436)) {
				free(g->panel);
				g->panel = malloc((size_t)row * (size_t)height);
				if (g->panel != NULL) {
					memcpy(g->panel, packed, (size_t)row * (size_t)height);
					g->panel_width = width;
					g->panel_height = height;
				}
			}
			if (format == GL_BGRA) {
				/* llvmpipe's BGRA upload path faulted on the second large texture; hand it RGBA. */
				for (size_t at = 0; at + 4 <= (size_t)row * (size_t)height; at += 4) {
					unsigned char b = packed[at];

					packed[at] = packed[at + 2];
					packed[at + 2] = b;
				}
				if (g->panel != NULL) {
					for (size_t at = 0; at + 4 <= (size_t)row * (size_t)height; at += 4) {
						unsigned char b = g->panel[at];

						g->panel[at] = g->panel[at + 2];
						g->panel[at + 2] = b;
					}
				}
				format = GL_RGBA;
			}
		} else {
			free(packed);
			packed = NULL;
		}
		glBindTexture(target, host);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glTexImage2D(target, 0, internal, width, height, 0, format, type,
		    packed != NULL ? packed : data);
		glPixelStorei(GL_UNPACK_ALIGNMENT, (GLint)previous_alignment);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)previous_row_length);
		glBindTexture(target, (GLuint)previous);
		if (packed != NULL) {
			free(data);
			data = packed;  /* kept until the frame's release */
		}
	}
	/* llvmpipe may keep the client pointer until the next draw/flush; freeing it here was a
	 * use-after-free (the crash came on the next GL call). The frame's release frees it. */
	wire_keep(data);
}

static GLuint
names_framebuffer(struct glstate *g, GLuint guest)
{
	uint64_t host;

	return name_map_get(&g->framebuffers, guest, &host) ? (GLuint)host : guest;
}

static void
names_framebuffer_put(struct glstate *g, uint64_t guest, uint64_t host)
{
	name_map_put(&g->framebuffers, guest, host);
}

static GLuint
names_framebuffer_take(struct glstate *g, GLuint guest)
{
	return (GLuint)name_map_take(&g->framebuffers, guest);
}

static GLuint
names_program(struct glstate *g, GLuint guest)
{
	uint64_t host;

	return name_map_get(&g->programs, guest, &host) ? (GLuint)host : guest;
}

static void
names_program_put(struct glstate *g, uint64_t guest, uint64_t host)
{
	name_map_put(&g->programs, guest, host);
}

static GLuint
names_program_take(struct glstate *g, GLuint guest)
{
	return (GLuint)name_map_take(&g->programs, guest);
}

static GLuint
names_shader(struct glstate *g, GLuint guest)
{
	uint64_t host;

	return name_map_get(&g->shaders, guest, &host) ? (GLuint)host : guest;
}

static void
names_shader_put(struct glstate *g, uint64_t guest, uint64_t host)
{
	name_map_put(&g->shaders, guest, host);
}

static GLsync
names_sync(struct glstate *g, uint64_t guest)
{
	uint64_t host;

	return name_map_get(&g->syncs, guest, &host) ? (GLsync)(uintptr_t)host : (GLsync)(uintptr_t)guest;
}

static void
names_sync_put(struct glstate *g, uint64_t guest, uint64_t host)
{
	name_map_put(&g->syncs, guest, host);
}

static GLsync
names_sync_take(struct glstate *g, uint64_t guest)
{
	return (GLsync)(uintptr_t)name_map_take(&g->syncs, guest);
}

/* ---- uniform locations, keyed by the guest's program ---- */

static void
names_uniform_put(struct glstate *g, uint32_t guest_program, GLint guest_location, GLint host_location)
{
	if (guest_location < 0 || host_location < 0) {
		return;
	}
	name_map_put(&g->uniforms,
	    ((uint64_t)guest_program << 32) | (uint32_t)guest_location,
	    (uint64_t)(uint32_t)host_location);
}

static GLint
names_uniform(struct glstate *g, GLint guest_location)
{
	uint64_t host;

	if (guest_location < 0) {
		return guest_location;
	}
	if (name_map_get(&g->uniforms,
		((uint64_t)g->current_program << 32) | (uint32_t)guest_location, &host)) {
		return (GLint)(uint32_t)host;
	}
	return guest_location;          /* frame_record's own names: this context made them */
}

/* ---- vertex arrays: the data travels just before the draw, not with the pointer call ---- */

static void
names_vertex_pointer(struct glstate *g, GLuint index, GLint size, GLenum type,
    GLboolean normalized, GLsizei stride)
{
	if (index >= 16) {
		return;
	}
	free(g->attribs[index].data);
	g->attribs[index].data = NULL;
	g->attribs[index].size = size;
	g->attribs[index].type = type;
	g->attribs[index].stride = stride;
	g->attribs[index].normalized = normalized;
}

static void
names_attrib_data(struct glstate *g, GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, GLsizei bytes, void *data)
{
	GLint previous = 0;

	if (index >= 16) {
		free(data);
		return;
	}
	free(g->attribs[index].data);
	g->attribs[index].data = data;
	g->attribs[index].size = size;
	g->attribs[index].type = type;
	g->attribs[index].stride = stride;
	g->attribs[index].normalized = normalized;
	/*
	 * llvmpipe 25.2.8 SIGSEGVs drawing a client-side GL_HALF_FLOAT array (CoreAnimation's colours
	 * travel that way). The same bytes in a buffer object draw correctly, and a buffer is what the
	 * driver is fastest at anyway. The binding is saved and put back: CoreAnimation's stream does
	 * not name buffers, so this side must not disturb whatever state it expects.
	 */
	if (g->attribs[index].buffer == 0) {
		glGenBuffers(1, &g->attribs[index].buffer);
	}
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
	glBindBuffer(GL_ARRAY_BUFFER, g->attribs[index].buffer);
	glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_STREAM_DRAW);
	glVertexAttribPointer(index, size, type, normalized, stride, (const void *)0);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous);
	glEnableVertexAttribArray(index);
}

static void
names_attrib_enabled(struct glstate *g, GLuint index, int enabled)
{
	(void)g;
	if (index >= 16) {
		return;
	}
	if (enabled) {
		glEnableVertexAttribArray(index);
	} else {
		glDisableVertexAttribArray(index);
	}
}

static void
names_attribs_release(struct glstate *g)
{
	/*
	 * The panel is not an attribute: it used to be freed here as well, and every draw in the
	 * replayed stream released it -- so in the --no-panel path the frame ended with no panel to
	 * compare against and the acceptance check silently said nothing (measured: 146 of 147 frames
	 * had no panel hash, the one that had it was a frame with no draws).
	 */
	for (unsigned i = 0; i < 16; i++) {
		free(g->attribs[i].data);
		g->attribs[i].data = NULL;
	}
}

/* ---- shader source: GLSL ES has qualifiers the host's GLSL does not ---- */

static char *
names_clean_shader(const char *text, size_t length)
{
	char *out = malloc(length + 1);
	size_t at = 0, line = 0;

	if (out == NULL) {
		return NULL;
	}
	while (line < length) {
		size_t end = line;

		while (end < length && text[end] != '\n') {
			end++;
		}
		{
			size_t first = line;

			while (first < end && (text[first] == ' ' || text[first] == '\t')) {
				first++;
			}
			/* The host is a desktop GL context: ES's version and precision/extension
			 * declarations are not its language. The rest of a GLES2 shader
			 * (attribute, varying, gl_FragColor, texture2D) is GLSL 1.10, which is
			 * what a source with no #version line gets. */
			if (first < end && text[first] == '#') {
				if (end - first >= 8 && strncmp(text + first, "#version", 8) == 0) {
					line = end + 1;
					continue;
				}
				if (end - first >= 10 && strncmp(text + first, "#extension", 10) == 0) {
					line = end + 1;
					continue;
				}
			}
			if (end - first >= 9 && strncmp(text + first, "precision", 9) == 0) {
				line = end + 1;
				continue;
			}
		}
		for (size_t i = line; i < end; i++) {
			/* lowp/mediump/highp are qualifiers of the ES language; drop the word. */
			static const char *words[] = { "lowp", "mediump", "highp" };
			int dropped = 0;

			for (unsigned w = 0; w < 3; w++) {
				size_t n = strlen(words[w]);

				if (i + n <= end && strncmp(text + i, words[w], n) == 0 &&
				    (i == line || !((text[i - 1] >= 'a' && text[i - 1] <= 'z') ||
					(text[i - 1] >= 'A' && text[i - 1] <= 'Z') || text[i - 1] == '_')) &&
				    (i + n == end || !((text[i + n] >= 'a' && text[i + n] <= 'z') ||
					(text[i + n] >= 'A' && text[i + n] <= 'Z') || text[i + n] == '_'))) {
					i += n - 1;
					dropped = 1;
					break;
				}
			}
			if (!dropped) {
				out[at++] = text[i];
			}
		}
		if (line + (end - line) < length) {
			out[at++] = '\n';
		}
		line = end + 1;
	}
	out[at] = '\0';
	return out;
}

static void
names_shader_source(GLuint shader, GLsizei count, const GLchar *const *texts, const GLint *lengths)
{
	const GLchar **clean = NULL;

	if (texts == NULL) {
		count = 0;
	}
	if (count > 0 && texts != NULL) {
		clean = calloc((size_t)count, sizeof(*clean));
	}
	if (clean != NULL) {
		for (GLsizei i = 0; i < count; i++) {
			size_t length = lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] :
			    (texts[i] != NULL ? strlen(texts[i]) : 0);

			clean[i] = names_clean_shader(texts[i] != NULL ? texts[i] : "", length);
		}
	}
	glShaderSource(shader, count, clean != NULL ? clean : texts, NULL);
	if (clean != NULL) {
		for (GLsizei i = 0; i < count; i++) {
			free((void *)clean[i]);
		}
		free(clean);
	}
}

#endif
