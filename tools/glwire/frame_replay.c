/* frame_replay: draw a frame on the host's GPU from glwire records, in the panel's own pixels.
 *
 * The layer is read from the guest's framebuffer in the RAM file, recorded into the wire format by
 * the same generated recorders the guest runs (frame_record.c), replayed through the same dispatcher
 * host_glwire uses, drawn on the V100, read back and written into that framebuffer again. With the
 * guest paused, the next screendump is the host's drawing -- and the pixels can be compared with
 * the guest's own, byte for byte.
 *
 *   frame_replay <ram.img> <framebuffer physical address> [--no-write] [--out FILE]
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wire.h"
#include "names.h"

#define PANEL_WIDTH  1125
#define PANEL_HEIGHT 2436
#define GUEST_RAM_BASE 0x40000000ULL

static GLuint frame_buffer;

static double
now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
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
			EGLDisplay display = get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
			if (display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor)) {
				return display;
			}
		}
	}
	EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(display, &major, &minor);
	return display;
}

static int
dispatch(struct glstate *g, wire_buffer *r, uint32_t opcode)
{
#include "dispatch.inc"
	return 0;
}

static GLuint
make_program(GLint *texture_uniform)
{
	static const char *vertex =
	    "attribute vec2 position;\n"
	    "attribute vec2 texcoord;\n"
	    "varying vec2 coordinate;\n"
	    "void main() { coordinate = texcoord; gl_Position = vec4(position, 0.0, 1.0); }\n";
	static const char *fragment =
	    "uniform sampler2D layer;\n"
	    "varying vec2 coordinate;\n"
	    "void main() { gl_FragColor = texture2D(layer, coordinate); }\n";
	GLuint program = glCreateProgram();
	GLuint shaders[2];
	const char *sources[2] = { vertex, fragment };
	GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };

	for (int i = 0; i < 2; i++) {
		shaders[i] = glCreateShader(types[i]);
		glShaderSource(shaders[i], 1, &sources[i], NULL);
		glCompileShader(shaders[i]);
		{
			GLint good = 0;
			glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &good);
			if (!good) {
				char said[512] = "";
				glGetShaderInfoLog(shaders[i], sizeof(said), NULL, said);
				fprintf(stderr, "frame_replay: shader does not compile: %s\n", said);
				return 0;
			}
		}
	}
	glAttachShader(program, shaders[0]);
	glAttachShader(program, shaders[1]);
	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "texcoord");
	glLinkProgram(program);
	{
		GLint good = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &good);
		if (!good) {
			char said[512] = "";
			glGetProgramInfoLog(program, sizeof(said), NULL, said);
			fprintf(stderr, "frame_replay: program does not link: %s\n", said);
			return 0;
		}
	}
	*texture_uniform = glGetUniformLocation(program, "layer");
	return program;
}

size_t frame_records(uint8_t *buffer, size_t capacity, GLuint texture, GLuint program,
    GLint texture_uniform, GLint position, GLint texcoord, int width, int height, const void *layer);

int
main(int argc, char **argv)
{
	const char *ram_path = argc > 1 ? argv[1] : "qemu/ram.img";
	uint64_t framebuffer = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x33b9eb000ULL;
	int write_back = 1;
	const char *out_path = NULL;
	struct stat info;
	unsigned char *ram;
	size_t frame_bytes;
	unsigned char *layer, *picture, *records;
	size_t records_bytes;
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	EGLint config_count = 0;
	GLuint texture, program;
	GLint texture_uniform = -1, position = 0, texcoord = 1;
	struct glstate names;
	double began, recorded, replayed;

	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--no-write") == 0) {
			write_back = 0;
		} else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
			out_path = argv[++i];
		}
	}
	if (framebuffer < GUEST_RAM_BASE) {
		fprintf(stderr, "frame_replay: %#llx is below the guest's RAM\n", (unsigned long long)framebuffer);
		return 1;
	}

	int fd = open(ram_path, write_back ? O_RDWR : O_RDONLY);
	if (fd < 0 || fstat(fd, &info) != 0) {
		fprintf(stderr, "frame_replay: cannot open %s\n", ram_path);
		return 1;
	}
	ram = mmap(NULL, (size_t)info.st_size, write_back ? (PROT_READ | PROT_WRITE) : PROT_READ,
	    MAP_SHARED, fd, 0);
	if (ram == MAP_FAILED) {
		fprintf(stderr, "frame_replay: cannot map %s\n", ram_path);
		return 1;
	}
	if (framebuffer - GUEST_RAM_BASE + (uint64_t)PANEL_WIDTH * PANEL_HEIGHT * 4 > (uint64_t)info.st_size) {
		fprintf(stderr, "frame_replay: the framebuffer is outside %s\n", ram_path);
		return 1;
	}
	layer = ram + (framebuffer - GUEST_RAM_BASE);
	frame_bytes = (size_t)PANEL_WIDTH * PANEL_HEIGHT * 4;
	picture = malloc(frame_bytes);
	records = malloc(32u << 20);
	if (picture == NULL || records == NULL) {
		fprintf(stderr, "frame_replay: no memory\n");
		return 1;
	}

	display = make_display();
	if (display == EGL_NO_DISPLAY) {
		fprintf(stderr, "frame_replay: no EGL display\n");
		return 1;
	}
	{
		static const EGLint attributes[] = {
		    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
		};

		eglBindAPI(EGL_OPENGL_API);
		if (!eglChooseConfig(display, attributes, &config, 1, &config_count) || config_count == 0) {
			fprintf(stderr, "frame_replay: no GL configuration\n");
			return 1;
		}
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
		surface = eglCreatePbufferSurface(display, config,
		    (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });
		if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
			fprintf(stderr, "frame_replay: no GL context\n");
			return 1;
		}
	}
	printf("frame_replay: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	/*
	 * The objects the record stream uses are made here, the way the guest's own library makes the
	 * objects whose names stay local (glCreateProgram and friends answer a value, so they do not
	 * travel). The stream then names them exactly as this context did.
	 */
	program = make_program(&texture_uniform);
	if (program == 0) {
		return 1;
	}
	glGenTextures(1, &texture);
	/* The stream names the objects this context has just made, as the guest names its own. The
	 * identity mapping says so explicitly, instead of relying on both starting at 1. */
	names_init(&names);
	names_program_put(&names, program, program);
	names_texture_put(&names, texture, texture);
	glDisable(GL_DITHER);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	glGenFramebuffers(1, &frame_buffer);
	glBindFramebuffer(GL_FRAMEBUFFER, frame_buffer);
	{
		GLuint colour = 0;

		glGenRenderbuffers(1, &colour);
		glBindRenderbuffer(GL_RENDERBUFFER, colour);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, PANEL_WIDTH, PANEL_HEIGHT);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colour);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			fprintf(stderr, "frame_replay: the framebuffer is not complete\n");
			return 1;
		}
	}

	began = now_seconds();
	records_bytes = frame_records(records, 32u << 20, texture, program, texture_uniform, position,
	    texcoord, PANEL_WIDTH, PANEL_HEIGHT, layer);
	recorded = now_seconds();
	{
		wire_buffer r;
		size_t at = 0;

		while (at + 8 <= records_bytes) {
			uint32_t opcode, length;

			memcpy(&opcode, records + at, 4);
			memcpy(&length, records + at + 4, 4);
			if (at + 8 + length > records_bytes) {
				fprintf(stderr, "frame_replay: a record runs past the frame at %zu\n", at);
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
		(void)r;
	}
	glFinish();
	replayed = now_seconds();
	glReadPixels(0, 0, PANEL_WIDTH, PANEL_HEIGHT, GL_BGRA, GL_UNSIGNED_BYTE, picture);

	{
		double sum = 0;
		size_t different = 0, worst = 0;

		for (size_t i = 0; i < frame_bytes; i += 4) {
			unsigned a = picture[i] - layer[i], b = picture[i + 1] - layer[i + 1];
			unsigned c = picture[i + 2] - layer[i + 2];
			unsigned d = (a > 127 ? 256 - a : a) + (b > 127 ? 256 - b : b) + (c > 127 ? 256 - c : c);

			if (picture[i] != layer[i] || picture[i + 1] != layer[i + 1] ||
			    picture[i + 2] != layer[i + 2]) {
				different++;
			}
			sum += d;
			if (d > worst) {
				worst = d;
			}
		}
		printf("frame_replay: %zu B of records for %zu B of layer; recorded in %.1f ms, drawn in %.1f ms; "
		    "%zu pixels differ (%.3f%%), mean %.3f, worst %zu\n", records_bytes, frame_bytes,
		    (recorded - began) * 1000.0, (replayed - recorded) * 1000.0, different,
		    100.0 * (double)different / (double)(frame_bytes / 4), sum / (double)(frame_bytes / 4), worst);
	}
	if (out_path != NULL) {
		FILE *file = fopen(out_path, "wb");

		if (file != NULL) {
			fwrite(picture, 1, frame_bytes, file);
			fclose(file);
			printf("frame_replay: picture written to %s (%zu bytes, BGRA)\n", out_path, frame_bytes);
		}
	}
	if (write_back) {
		memcpy(layer, picture, frame_bytes);
		msync(layer, frame_bytes, MS_SYNC);
		printf("frame_replay: frame written into the panel at %#llx\n", (unsigned long long)framebuffer);
	}
	munmap(ram, (size_t)info.st_size);
	close(fd);
	names_free(&names);
	free(picture);
	free(records);
	return 0;
}
