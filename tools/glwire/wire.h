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
#include <GLES2/gl2.h>
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
} wire_buffer;

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

static inline void
wire_bytes(wire_buffer *w, const void *bytes, GLsizei count)
{
	uint32_t size = count > 0 ? (uint32_t)count : 0;

	wire_u32(w, size);
	if (size != 0) {
		wire_put(w, bytes, size);
	}
}

/* The opcode and length are written by the generator around the payload, so it needs a way to go
 * back and fill the length in once the arguments are known. */
static inline void wire_open(wire_buffer *w, uint32_t opcode)
{
	size_t start = w->at;

	wire_u32(w, opcode);
	wire_u32(w, 0);
	w->at = start + 8;
}

static inline void
wire_close(wire_buffer *w)
{
	size_t end = w->at;
	size_t start = end;
	uint32_t length;

	while (start > 0 && (w->at - start) != 8) {
		start--;
	}
	length = (uint32_t)(end - (start + 8));
	memcpy(w->base + start + 4, &length, 4);
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

#endif
#endif
