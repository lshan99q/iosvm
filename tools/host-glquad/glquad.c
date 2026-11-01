/* glquad: render a textured, blended quad into an offscreen surface on the host GPU.
 *
 * The prototype's question is whether the host can do the drawing CoreAnimation asks for (it is
 * six triangles and one texture a frame) and how long a frame takes there. This is the smallest
 * program that answers both: an EGL context without a window, chosen device, an FBO the size of
 * the panel, one quad with the panel's texture, read back and timed.
 *
 *   glquad [width] [height] [frames]
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const char *VERTEX =
    "#version 330 core\n"
    "layout(location = 0) in vec2 p;\n"
    "layout(location = 1) in vec2 t;\n"
    "out vec2 uv;\n"
    "void main() { uv = t; gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *FRAGMENT =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "out vec4 colour;\n"
    "uniform sampler2D layer;\n"
    "void main() { colour = texture(layer, uv); }\n";

static GLuint
compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		printf("glquad: shader: %s\n", log);
	}
	return shader;
}

int
main(int argc, char **argv)
{
	int width = argc > 1 ? atoi(argv[1]) : 1125;
	int height = argc > 2 ? atoi(argv[2]) : 2436;
	int frames = argc > 3 ? atoi(argv[3]) : 10;
	PFNEGLQUERYDEVICESEXTPROC query_devices;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLDeviceEXT devices[8];
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLint count = 0, major = 0, minor = 0, config_count = 0;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	GLuint texture, framebuffer, vertex, fragment, program, vao, vbo, rb;
	float vertices[16];
	unsigned char *pixels;
	double started, elapsed, total = 0;

	if ((query_devices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT")) != NULL) {
		if (query_devices(8, devices, &count) && count > 0) {
			get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
			    "eglGetPlatformDisplayEXT");
			for (EGLint i = 0; i < count && display == EGL_NO_DISPLAY; i++) {
				display = get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
				if (display == EGL_NO_DISPLAY) {
					continue;
				}
				if (!eglInitialize(display, &major, &minor)) {
					display = EGL_NO_DISPLAY;
				}
			}
		}
	}
	if (display == EGL_NO_DISPLAY) {
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		eglInitialize(display, &major, &minor);
	}
	if (!eglBindAPI(EGL_OPENGL_API)) {
		printf("glquad: cannot bind OpenGL\n");
		return 1;
	}
	static const EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
	    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
	    EGL_NONE,
	};
	if (!eglChooseConfig(display, attributes, &config, 1, &config_count) || config_count == 0) {
		printf("glquad: no configuration\n");
		return 1;
	}
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
	if (context == EGL_NO_CONTEXT) {
		printf("glquad: no context (0x%x)\n", eglGetError());
		return 1;
	}
	surface = eglCreatePbufferSurface(display, config,
	    (EGLint[]){EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE});
	if (!eglMakeCurrent(display, surface, surface, context)) {
		printf("glquad: cannot make current (0x%x)\n", eglGetError());
		return 1;
	}
	printf("glquad: renderer %s | %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VENDOR),
	    glGetString(GL_VERSION));

	/* A 1125x2436 BGRA layer, the size CoreAnimation asks the display for. */
	pixels = calloc((size_t)width * height * 4, 1);
	for (int y = 0; y < height; y += 8) {
		for (int x = 0; x < width; x += 8) {
			size_t at = ((size_t)y * width + x) * 4;
			pixels[at + 0] = (unsigned char)(x * 255 / width);
			pixels[at + 1] = (unsigned char)(y * 255 / height);
			pixels[at + 2] = 200;
			pixels[at + 3] = 255;
		}
	}

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
	    GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("glquad: framebuffer incomplete\n");
		return 1;
	}

	program = glCreateProgram();
	vertex = compile(GL_VERTEX_SHADER, VERTEX);
	fragment = compile(GL_FRAGMENT_SHADER, FRAGMENT);
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "layer"), 0);

	/* Six triangles worth of layout, done as two triangles over the whole panel: the draw count
	 * CoreAnimation shows is small, so this is representative enough for a timing floor. */
	vertices[0] = -1; vertices[1] = -1; vertices[2] = 0; vertices[3] = 0;
	vertices[4] = 1; vertices[5] = -1; vertices[6] = 1; vertices[7] = 0;
	vertices[8] = 1; vertices[9] = 1; vertices[10] = 1; vertices[11] = 1;
	vertices[12] = -1; vertices[13] = 1; vertices[14] = 0; vertices[15] = 1;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
	    (void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	unsigned char *read = malloc((size_t)width * height * 4);
	glViewport(0, 0, width, height);
	for (int i = 0; i < frames; i++) {
		started = now_ms();
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, read);
		elapsed = now_ms() - started;
		total += elapsed;
	}
	printf("glquad: %dx%d, %d frames, %.2f ms a frame (draw + readback)\n", width, height, frames,
	    total / frames);
	glFinish();
	printf("glquad: after glFinish, %.2f ms a frame\n", (now_ms() - started));
	free(read);
	free(pixels);
	return 0;
}
