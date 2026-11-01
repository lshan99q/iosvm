/* map_test: replay a stream whose names are all different on the host.
 *
 * This is the name mapping's own acceptance test, without a guest. map_stream.c makes a record
 * stream that names its objects 77 (program), 88/89 (shaders), 101/102 (textures), 303 (framebuffer)
 * and its uniforms 5/6. This program first makes dummy objects, so the host's own names are
 * different numbers again, then replays through the same dispatch.inc and names.h host_glwire uses.
 *
 * The picture is the proof: the layer is (0.2, 0.4, 0.8) and the tint uniform is (1, 0.5, 0, 1), so
 * the centre of the 4x4 target has to be about (51, 51, 0). If a texture, the framebuffer, the
 * program, the shaders or either uniform location went across untranslated, that pixel is wrong.
 *
 *   ./map_test
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire.h"
#include "names.h"

size_t map_records(uint8_t *buffer, size_t capacity);

static int
dispatch(struct glstate *g, wire_buffer *r, uint32_t opcode)
{
#include "dispatch.inc"
	return 0;
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
	EGLDisplay display;
	EGLConfig config;
	EGLint config_count = 0;
	EGLContext context;
	EGLSurface surface;
	static const EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};
	uint8_t *records = malloc(4u << 20);
	size_t bytes;
	struct glstate names;
	unsigned char pixel[4] = { 0, 0, 0, 0 };
	int failed = 0;

	if (records == NULL) {
		fprintf(stderr, "map_test: no memory\n");
		return 2;
	}
	bytes = map_records(records, 4u << 20);
	if (argc > 2 && strcmp(argv[1], "--dump") == 0) {
		FILE *file = fopen(argv[2], "wb");

		if (file == NULL) {
			return 2;
		}
		fwrite(records, 1, bytes, file);
		fclose(file);
		printf("map_test: %zu bytes of records written to %s\n", bytes, argv[2]);
		return 0;
	}
	display = make_display();
	eglBindAPI(EGL_OPENGL_API);
	if (display == EGL_NO_DISPLAY ||
	    !eglChooseConfig(display, attributes, &config, 1, &config_count) || config_count == 0) {
		fprintf(stderr, "map_test: no GL configuration\n");
		return 2;
	}
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
	surface = eglCreatePbufferSurface(display, config,
	    (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
		fprintf(stderr, "map_test: no GL context\n");
		return 2;
	}
	printf("map_test: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	/* Make the host's names different from the guest's on purpose. */
	{
		GLuint dummy_textures[7];
		GLuint dummy_framebuffer = 0;
		GLuint dummy_program, dummy_shader;

		glGenTextures(7, dummy_textures);
		glGenFramebuffers(1, &dummy_framebuffer);
		dummy_program = glCreateProgram();
		dummy_shader = glCreateShader(GL_VERTEX_SHADER);
		printf("map_test: host names start after textures ..%u, framebuffer %u, program %u, shader %u\n",
		    dummy_textures[6], dummy_framebuffer, dummy_program, dummy_shader);
	}

	names_init(&names);
	printf("map_test: %zu bytes of records\n", bytes);
	{
		wire_buffer r;
		size_t at = 0;

		while (at + 8 <= bytes) {
			uint32_t opcode, length;

			memcpy(&opcode, records + at, 4);
			memcpy(&length, records + at + 4, 4);
			if (at + 8 + length > bytes) {
				fprintf(stderr, "map_test: a record runs past the stream\n");
				return 1;
			}
			r.base = records + at + 8;
			r.capacity = length;
			r.at = 0;
			if (dispatch(&names, &r, opcode) != 0) {
				return 1;
			}
			at += 8 + length;
		}
	}
	glFinish();
	{
		GLint program = 0, framebuffer = 0, linked = 0, loc;
		GLfloat tint[4] = { 0, 0, 0, 0 };
		GLuint shaders[4] = { 0 };
		GLsizei shader_count = 0;

		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		glGetAttachedShaders(program, 4, &shader_count, shaders);
		printf("map_test: current program %d (linked %d), framebuffer %d (status %#x), %d shaders:",
		    program, linked, framebuffer, glCheckFramebufferStatus(GL_FRAMEBUFFER), shader_count);
		for (GLsizei i = 0; i < shader_count; i++) {
			GLint good = 0;
			glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &good);
			printf(" %u(compile %d)", shaders[i], good);
		}
		printf("\n");
		loc = glGetUniformLocation(program, "tint");
		glGetUniformfv(program, loc, tint);
		printf("map_test: tint at %d is %g %g %g %g\n", loc, tint[0], tint[1], tint[2], tint[3]);
		{
			GLint unit = -1;

			loc = glGetUniformLocation(program, "layer");
			glGetUniformiv(program, loc, &unit);
			printf("map_test: layer unit at %d is %d; position attribute %d; error %#x\n",
			    loc, unit, glGetAttribLocation(program, "position"), glGetError());
		}
	}
	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	printf("map_test: centre pixel %u %u %u %u; maps: %zu textures, %zu framebuffers, "
	    "%zu programs, %zu shaders, %zu syncs, %zu uniform locations\n",
	    pixel[0], pixel[1], pixel[2], pixel[3],
	    names.textures.used, names.framebuffers.used, names.programs.used,
	    names.shaders.used, names.syncs.used, names.uniforms.used);

	if (pixel[0] < 49 || pixel[0] > 53 || pixel[1] < 49 || pixel[1] > 53 ||
	    pixel[2] > 2 || pixel[3] < 253) {
		fprintf(stderr, "map_test: FAIL: expected about 51 51 0 255\n");
		failed = 1;
	}
	if (names.textures.used != 2 || names.framebuffers.used != 1 ||
	    names.programs.used != 1 || names.shaders.used != 2 || names.syncs.used != 1 ||
	    names.uniforms.used != 2) {
		fprintf(stderr, "map_test: FAIL: the maps did not see every name\n");
		failed = 1;
	}
	if (glGetError() != GL_NO_ERROR) {
		fprintf(stderr, "map_test: FAIL: the GL reported an error\n");
		failed = 1;
	}
	names_free(&names);
	printf("map_test: %s\n", failed ? "FAIL" : "PASS");
	return failed;
}
