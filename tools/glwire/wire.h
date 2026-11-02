/* wire.h: the byte buffer both halves speak.
 *
 * One record per GL call: u32 opcode, u32 payload length, payload. Integers as they lie, floats as
 * their bits, pointers as a u32 count followed by the bytes. The guest records into a buffer it
 * owns; the host reads from one it filled from the socket. Same code, two directions -- the compiler
 * decides which by which macros are defined.
 */
#ifndef GLWIRE_WIRE_H
#define GLWIRE_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef GLWIRE_GUEST
#ifndef GLWIRE_GL_TYPES
/* The recorder is also built on the host, against GL/gl.h, so that a frame can be made and replayed
 * without a guest (frame_record.c). The types below are then already there. */
#include <OpenGLES/ES2/gl.h>
#endif
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

typedef struct {
	uint8_t *base;
	size_t capacity;
	size_t at;
	size_t record;   /* where the current record's header starts */
} wire_buffer;

/* One physically contiguous run of a texture the host will read out of the guest's RAM. */
typedef struct {
	uint64_t pa;
	uint32_t len;
	uint32_t reserved;      /* keeps the wire layout the same on both sides */
} wire_segment;

#ifdef GLWIRE_GUEST

/* ---- recording ---- */
static inline void
wire_reset(wire_buffer *w, uint8_t *memory, size_t capacity)
{
	w->base = memory;
	w->capacity = capacity;
	w->at = 0;
}

static inline void
wire_put(wire_buffer *w, const void *bytes, size_t count)
{
	if (w->at + count > w->capacity) {
		return;                 /* the frame is dropped rather than overrunning */
	}
	memcpy(w->base + w->at, bytes, count);
	w->at += count;
}

static inline void wire_u32(wire_buffer *w, uint32_t v) { wire_put(w, &v, 4); }
static inline void wire_i32(wire_buffer *w, int32_t v) { wire_put(w, &v, 4); }
static inline void wire_f32(wire_buffer *w, float v) { wire_put(w, &v, 4); }
static inline void wire_u8(wire_buffer *w, uint8_t v) { wire_put(w, &v, 1); }
static inline void wire_u64(wire_buffer *w, uint64_t v) { wire_put(w, &v, 8); }

static inline void
wire_bytes(wire_buffer *w, const void *bytes, GLsizei count)
{
	uint32_t size = count > 0 ? (uint32_t)count : 0;

	wire_u32(w, size);
	if (size != 0) {
		wire_put(w, bytes, size);
	}
}

/* A NUL-terminated string: the bytes without the NUL, which the host puts back. */
static inline void
wire_string(wire_buffer *w, const char *string)
{
	uint32_t size = string != NULL ? (uint32_t)strlen(string) : 0;

	wire_u32(w, size);
	if (size != 0) {
		wire_put(w, string, size);
	}
}

/*
 * A texture upload can name the memory instead of carrying it. eagl_remote.c sets this while the
 * kernel's hakios_vtop (syscall 556) is available; a test build leaves it NULL and every image
 * travels inline, exactly as before.
 *
 * The answer has to cover every byte: a partial translation would let the host draw a texture with
 * a hole in it, so anything short of the whole buffer falls back to inline bytes.
 */
typedef int (*wire_vtop_fn)(const void *pixels, size_t bytes, wire_segment *segments, unsigned max);
static wire_vtop_fn wire_vtop_hook;
static wire_segment wire_image_segments[2048];
/* Guest-side counters: how many image arguments went by reference and how many inline. */
static unsigned long wire_image_ref_count;
static unsigned long wire_image_inline_count;

static inline void
wire_image(wire_buffer *w, const void *pixels, GLsizei count)
{
	uint32_t size = count > 0 ? (uint32_t)count : 0;
	uint32_t n = 0;

	if (size != 0 && pixels != NULL && wire_vtop_hook != NULL) {
		int got = wire_vtop_hook(pixels, size, wire_image_segments,
		    sizeof(wire_image_segments) / sizeof(wire_image_segments[0]));
		uint64_t covered = 0;

		if (got > 0) {
			for (int i = 0; i < got; i++) {
				covered += wire_image_segments[i].len;
			}
			n = covered == size ? (uint32_t)got : 0;
		}
	}
	wire_u8(w, n != 0 ? 1 : 0);
	if (n != 0) {
		wire_image_ref_count++;
		wire_u32(w, n);
		for (uint32_t i = 0; i < n; i++) {
			wire_u64(w, wire_image_segments[i].pa);
			wire_u32(w, wire_image_segments[i].len);
			wire_u32(w, 0);
		}
	} else {
		if (size != 0) {
			wire_image_inline_count++;
		}
		wire_u32(w, size);
		if (size != 0) {
			wire_put(w, pixels, size);
		}
	}
}

/*
 * How many bytes a pointer argument stands for. The GL signature does not carry a size, so it is
 * worked out from the other arguments -- the count of an index array, the width and height of a
 * texture, the number of uniform vectors. The unpack alignment matters for the last row of a
 * texture: the caller's glPixelStorei has been seen, so the recorder knows it too.
 */
static int wire_unpack_alignment = 4;

static inline GLsizei
wire_type_size(GLenum type)
{
	switch (type) {
	case GL_UNSIGNED_BYTE:
		return 1;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
		return 2;
	case GL_UNSIGNED_INT:
	case GL_INT:
	case GL_FLOAT:
		return 4;
	default:
		return 4;
	}
}

static inline GLsizei
wire_format_bpp(GLenum format, GLenum type)
{
	switch (format) {
	case GL_ALPHA:
	case GL_LUMINANCE:
		return wire_type_size(type);
	case GL_LUMINANCE_ALPHA:
		return 2 * wire_type_size(type);
	case GL_RGB:
		return 3 * wire_type_size(type);
	case GL_RGBA:
	case 0x80E1:                   /* GL_BGRA / GL_BGRA_EXT */
		return 4 * wire_type_size(type);
	default:
		/* the packed 16-bit formats (5_6_5 and friends) are two bytes a pixel */
		switch (type) {
		case GL_UNSIGNED_SHORT_5_6_5:
		case GL_UNSIGNED_SHORT_4_4_4_4:
		case GL_UNSIGNED_SHORT_5_5_5_1:
			return 2;
		default:
			return 4 * wire_type_size(type);
		}
	}
}

static inline GLsizei
wire_index_bytes(GLsizei count, GLenum type)
{
	if (count <= 0) {
		return 0;
	}
	return count * wire_type_size(type);
}

static inline GLsizei
wire_image_bytes(GLsizei width, GLsizei height, GLenum format, GLenum type)
{
	GLsizei bpp, row;

	if (width <= 0 || height <= 0) {
		return 0;
	}
	bpp = wire_format_bpp(format, type);
	row = width * bpp;
	if (wire_unpack_alignment > 1) {
		row = (row + wire_unpack_alignment - 1) & ~(wire_unpack_alignment - 1);
	}
	return row * height;
}

/* The opcode and length are written by the generator around the payload, so it needs a way to go
 * back and fill the length in once the arguments are known. */
static inline void
wire_open(wire_buffer *w, uint32_t opcode)
{
	w->record = w->at;
	wire_u32(w, opcode);
	wire_u32(w, 0);                 /* the payload length, filled in by wire_close */
}

static inline void
wire_close(wire_buffer *w)
{
	uint32_t length = (uint32_t)(w->at - (w->record + 8));

	memcpy(w->base + w->record + 4, &length, 4);
}

#else

/* ---- reading ---- */
static inline uint32_t
wire_get_u32(wire_buffer *r)
{
	uint32_t v = 0;

	if (r->at + 4 <= r->capacity) {
		memcpy(&v, r->base + r->at, 4);
		r->at += 4;
	}
	return v;
}

static inline int32_t wire_get_i32(wire_buffer *r) { return (int32_t)wire_get_u32(r); }

static inline uint64_t
wire_get_u64(wire_buffer *r)
{
	uint64_t v = 0;

	if (r->at + 8 <= r->capacity) {
		memcpy(&v, r->base + r->at, 8);
		r->at += 8;
	}
	return v;
}

static inline float
wire_get_f32(wire_buffer *r)
{
	float v = 0;

	if (r->at + 4 <= r->capacity) {
		memcpy(&v, r->base + r->at, 4);
		r->at += 4;
	}
	return v;
}

static inline uint8_t
wire_get_u8(wire_buffer *r)
{
	uint8_t v = 0;

	if (r->at < r->capacity) {
		v = r->base[r->at++];
	}
	return v;
}

/*
 * The guest's physical memory, as this host has it mapped: QEMU's ram.img with
 * -object memory-backend-file,share=on. A texture record may then name a physical address instead
 * of carrying pixels; wire_take_image copies the bytes out of this mapping.
 */
static unsigned char *wire_ram;
static uint64_t wire_ram_pa_base;
static size_t wire_ram_bytes;
/* How many image arguments this frame named by physical address, and how many bytes that saved. */
static unsigned long wire_image_refs;
static unsigned long wire_image_ref_bytes;

static inline void
wire_set_ram(void *memory, size_t bytes, uint64_t pa_base)
{
	wire_ram = memory;
	wire_ram_bytes = bytes;
	wire_ram_pa_base = pa_base;
}

/* The host owns the copy: the call may keep the pointer after it returns (glTexImage2D copies, but
 * glVertexAttribPointer does not, so it must stay alive until the frame is submitted). */
static inline void *
wire_take(wire_buffer *r, GLsizei *count)
{
	uint32_t size = wire_get_u32(r);
	void *copy = NULL;

	*count = (GLsizei)size;
	if (size == 0) {
		return NULL;
	}
	if (r->at + size > r->capacity) {
		r->at = r->capacity;
		return NULL;
	}
	copy = malloc(size);
	if (copy != NULL) {
		memcpy(copy, r->base + r->at, size);
	}
	r->at += size;
	return copy;
}

/*
 * A texture pointer: either inline bytes (kind 0, the old wire shape) or physical segments
 * (kind 1). The segments are copied into one contiguous buffer, which is what the GL call wants;
 * the caller frees it as it always did. Returns NULL and consumes the record if a segment falls
 * outside the mapped RAM -- a reference that cannot be followed must not be drawn.
 */
static inline void *
wire_take_image(wire_buffer *r, GLsizei *count)
{
	uint8_t kind = wire_get_u8(r);

	*count = 0;
	if (kind == 0) {
		return wire_take(r, count);
	}
	if (kind != 1) {
		r->at = r->capacity;
		return NULL;
	}
	{
		uint32_t n = wire_get_u32(r);
		wire_segment *segments;
		unsigned char *pixels;
		size_t total = 0, at = 0;

		if (n == 0 || n > 8192) {
			r->at = r->capacity;
			return NULL;
		}
		segments = calloc(n, sizeof(*segments));
		if (segments == NULL) {
			r->at = r->capacity;
			return NULL;
		}
		for (uint32_t i = 0; i < n; i++) {
			segments[i].pa = wire_get_u64(r);
			segments[i].len = wire_get_u32(r);
			segments[i].reserved = wire_get_u32(r);
			total += segments[i].len;
		}
		pixels = total != 0 ? malloc(total) : NULL;
		if (pixels == NULL) {
			free(segments);
			r->at = r->capacity;
			return NULL;
		}
		for (uint32_t i = 0; i < n; i++) {
			uint64_t pa = segments[i].pa;
			uint32_t len = segments[i].len;

			if (wire_ram == NULL || pa < wire_ram_pa_base ||
			    pa - wire_ram_pa_base > wire_ram_bytes ||
			    len > wire_ram_bytes - (size_t)(pa - wire_ram_pa_base)) {
				free(pixels);
				free(segments);
				r->at = r->capacity;
				return NULL;
			}
			memcpy(pixels + at, wire_ram + (size_t)(pa - wire_ram_pa_base), len);
			at += len;
		}
		free(segments);
		wire_image_refs++;
		wire_image_ref_bytes += total;
		*count = (GLsizei)total;
		return pixels;
	}
}

/* The same, for a NUL-terminated string: a byte more than the wire carries, and a NUL in it. */
static inline char *
wire_take_string(wire_buffer *r)
{
	uint32_t size = wire_get_u32(r);
	char *copy;

	if (r->at + size > r->capacity) {
		r->at = r->capacity;
		size = 0;
	}
	copy = malloc((size_t)size + 1);
	if (copy == NULL) {
		return NULL;
	}
	if (size != 0) {
		memcpy(copy, r->base + r->at, size);
	}
	copy[size] = '\0';
	r->at += size;
	return copy;
}

/* A pointer the GL keeps after the call returns -- the vertex arrays of glVertexAttribPointer -- must
 * live until the draw that reads it. The dispatcher keeps up to sixteen of them and frees them after
 * the next draw or flush; freeing them with the call itself made the host draw read freed memory. */
#define WIRE_KEPT_MAX 16
static void *wire_kept[WIRE_KEPT_MAX];
static int wire_kept_count;

static inline void
wire_keep(void *pointer)
{
	if (pointer == NULL) {
		return;
	}
	if (wire_kept_count < WIRE_KEPT_MAX) {
		wire_kept[wire_kept_count++] = pointer;
	} else {
		free(pointer);
	}
}

static inline void
wire_release(void)
{
	while (wire_kept_count > 0) {
		free(wire_kept[--wire_kept_count]);
	}
}

#endif
#endif
