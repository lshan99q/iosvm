/* map_stream: build a record stream whose object names are not the host's.
 *
 * The names here (program 77, shader 88, textures 101 and 102, framebuffer 303, uniform
 * locations 5 and 6) are deliberately unlike the ones a fresh host context makes. map_test.c
 * replays the stream after making a few dummy objects, so the host's names are unlike these too;
 * the picture only comes out right if the dispatcher translates every one of them.
 */
#define GLWIRE_GUEST
#define GLWIRE_GL_TYPES
#include <GL/gl.h>

#include "wire.h"

wire_buffer w;

#include "encode.inc"

static const char *vertex_source =
    "attribute vec2 position;\n"
    "varying vec2 uv;\n"
    "void main() { uv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }\n";
static const char *fragment_source =
    "uniform sampler2D layer;\n"
    "uniform vec4 tint;\n"
    "varying vec2 uv;\n"
    "void main() { gl_FragColor = texture2D(layer, uv) * tint; }\n";

static void
vertices(GLuint index, GLint size, const GLfloat *data, GLsizei bytes)
{
	wire_open(&w, 200);
	wire_u32(&w, index);
	wire_i32(&w, size);
	wire_u32(&w, GL_FLOAT);
	wire_u8(&w, GL_FALSE);
	wire_i32(&w, 0);
	wire_bytes(&w, data, bytes);
	wire_close(&w);
}

size_t
map_records(uint8_t *buffer, size_t capacity)
{
	static const GLfloat positions[] = {
		-1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f,
	};
	static const GLfloat texcoords[] = {
		0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f,
	};
	static const unsigned char layer_pixels[2 * 2 * 4] = {
		51, 102, 204, 255,  51, 102, 204, 255,
		51, 102, 204, 255,  51, 102, 204, 255,
	};
	static const unsigned char target_pixels[4 * 4 * 4] = { 0 };
	GLuint textures[2] = { 101, 102 };
	GLuint framebuffer = 303;

	wire_reset(&w, buffer, capacity);
	gw_glViewport(0, 0, 4, 4);
	gw_glDisable(GL_BLEND);
	gw_glDisable(GL_DEPTH_TEST);
	gw_glDisable(GL_SCISSOR_TEST);
	gw_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	/* the program and its shader, by the guest's names */
	gw_glCreateProgram(77);
	gw_glCreateShader(GL_VERTEX_SHADER, 88);
	gw_glShaderSource(88, 1, &vertex_source, NULL);
	gw_glCompileShader(88);
	gw_glAttachShader(77, 88);
	gw_glCreateShader(GL_FRAGMENT_SHADER, 89);
	gw_glShaderSource(89, 1, &fragment_source, NULL);
	gw_glCompileShader(89);
	gw_glAttachShader(77, 89);
	gw_glBindAttribLocation(77, 0, "position");
	gw_glLinkProgram(77);

	/* two textures: the layer that is sampled, and the target the framebuffer draws into */
	gw_glGenTextures(2, textures);
	gw_glBindTexture(GL_TEXTURE_2D, 101);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gw_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, layer_pixels);
	gw_glBindTexture(GL_TEXTURE_2D, 102);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gw_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gw_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, target_pixels);

	/* the framebuffer, by the guest's name, over the guest's target texture */
	gw_glGenFramebuffers(1, &framebuffer);
	gw_glBindFramebuffer(GL_FRAMEBUFFER, 303);
	gw_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 102, 0);

	/* uniform locations 5 and 6 are the guest's answers; the host's will differ */
	/* unit 0 samples the layer, not the texture the framebuffer draws into */
	gw_glBindTexture(GL_TEXTURE_2D, 101);
	gw_glUseProgram(77);
	gw_glGetUniformLocation(77, "layer", 5);
	gw_glGetUniformLocation(77, "tint", 6);
	gw_glUniform1i(5, 0);
	gw_glUniform4f(6, 1.0f, 0.5f, 0.0f, 1.0f);

	gw_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
	gw_glEnableVertexAttribArray(0);
	vertices(0, 2, positions, sizeof(positions));
	gw_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
	gw_glEnableVertexAttribArray(1);
	vertices(1, 2, texcoords, sizeof(texcoords));

	gw_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	/* and a fence, whose guest pointer is not a host pointer either */
	gw_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0, (void *)(uintptr_t)0x1122334455667788ULL);
	gw_glClientWaitSync((void *)(uintptr_t)0x1122334455667788ULL, GL_SYNC_FLUSH_COMMANDS_BIT, 0, 0);
	gw_glFlush();
	return w.at;
}
