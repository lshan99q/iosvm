/* frame_record: build a frame's worth of glwire records with the guest's own recorders.
 *
 * This is the generated half the guest runs -- encode.inc, unmodified -- built for the host so that
 * a frame can be recorded and replayed without a guest. The calls are the ones CoreAnimation's
 * renderer makes for a full-screen layer: the layer's texture, a program that samples it, and two
 * triangles over the panel.
 */
#define GLWIRE_GUEST
#define GLWIRE_GL_TYPES
#include <GL/gl.h>

#include "wire.h"

/* the buffer the generated recorders append to */
wire_buffer w;

#include "encode.inc"

/* The vertex arrays are not part of glVertexAttribPointer's record (the GL signature carries no
 * size); the guest sends them with the draw, and this test does the same. */
static void
record_vertices(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
    const void *pointer, GLsizei bytes)
{
	wire_open(&w, 200);
	wire_u32(&w, index);
	wire_i32(&w, size);
	wire_u32(&w, type);
	wire_u8(&w, normalized);
	wire_i32(&w, stride);
	wire_bytes(&w, pointer, bytes);
	wire_close(&w);
}

size_t
frame_records(uint8_t *buffer, size_t capacity, GLuint texture, GLuint program,
    GLint texture_uniform, GLint position, GLint texcoord, int width, int height, const void *layer)
{
	static const GLfloat positions[] = {
		-1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f,
	};
	static const GLfloat texcoords[] = {
		0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f,
	};

	wire_reset(&w, buffer, capacity);
	gw_glViewport(0, 0, width, height);
	gw_glDisable(GL_BLEND);
	gw_glDisable(GL_DEPTH_TEST);
	gw_glDisable(GL_SCISSOR_TEST);
	gw_glActiveTexture(GL_TEXTURE0);
	gw_glBindTexture(GL_TEXTURE_2D, texture);
	gw_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gw_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, layer);
	gw_glUseProgram(program);
	gw_glUniform1i(texture_uniform, 0);
	gw_glVertexAttribPointer((GLuint)position, 2, GL_FLOAT, GL_FALSE, 0, positions);
	gw_glEnableVertexAttribArray((GLuint)position);
	record_vertices((GLuint)position, 2, GL_FLOAT, GL_FALSE, 0, positions, sizeof(positions));
	gw_glVertexAttribPointer((GLuint)texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
	gw_glEnableVertexAttribArray((GLuint)texcoord);
	record_vertices((GLuint)texcoord, 2, GL_FLOAT, GL_FALSE, 0, texcoords, sizeof(texcoords));
	gw_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gw_glFlush();
	return w.at;
}
