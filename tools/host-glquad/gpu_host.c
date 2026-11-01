/* gpu_host: the host half of the drawing pipe, on a socket, for the prototype.
 *
 * The measured facts it stands on: a panel-sized frame costs the host GPU about 2 ms (see glquad),
 * and the same frame costs the guest about 700 ms. So the host can afford to do the drawing for
 * every frame the guest sends.
 *
 * Protocol, deliberately plain so both sides stay readable while it is a prototype:
 *
 *   client -> host   "F <width> <height> <bytes>\n" then <bytes> of BGRA (the layer to draw)
 *   host -> client   "O <width> <height> <bytes>\n" then <bytes> of BGRA (the composited frame)
 *   client -> host   "Q\n"                     the connection is done
 *
 * The texture bytes are the part that must not stay a copy for long: in the real thing the host
 * maps the guest's RAM (qemu/ram.img) and reads the layer where it already lies.
 *
 *   gpu_host [port]
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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
		fprintf(stderr, "gpu_host: shader: %s\n", log);
	}
	return shader;
}

static int
read_full(int fd, void *buffer, size_t bytes)
{
	size_t done = 0;

	while (done < bytes) {
		ssize_t got = read(fd, (char *)buffer + done, bytes - done);
		if (got <= 0) {
			return 0;
		}
		done += (size_t)got;
	}
	return 1;
}

static int
write_full(int fd, const void *buffer, size_t bytes)
{
	size_t done = 0;

	while (done < bytes) {
		ssize_t put = write(fd, (const char *)buffer + done, bytes - done);
		if (put <= 0) {
			return 0;
		}
		done += (size_t)put;
	}
	return 1;
}

int
main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 9100;
	PFNEGLQUERYDEVICESEXTPROC query_devices;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLDeviceEXT devices[8];
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLint count = 0, major = 0, minor = 0, config_count = 0;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	GLuint texture = 0, framebuffer = 0, rb = 0, program, vao, vbo;
	int width = 0, height = 0, texture_ready = 0;
	unsigned char *layer = NULL, *frame = NULL;
	struct sockaddr_in address;
	int listener, client;

	if ((query_devices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT")) != NULL) {
		if (query_devices(8, devices, &count) && count > 0) {
			get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
			    "eglGetPlatformDisplayEXT");
			for (EGLint i = 0; i < count && display == EGL_NO_DISPLAY; i++) {
				display = get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
				if (display != EGL_NO_DISPLAY && !eglInitialize(display, &major, &minor)) {
					display = EGL_NO_DISPLAY;
				}
			}
		}
	}
	if (display == EGL_NO_DISPLAY) {
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		eglInitialize(display, &major, &minor);
	}
	eglBindAPI(EGL_OPENGL_API);
	static const EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};
	eglChooseConfig(display, attributes, &config, 1, &config_count);
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
	surface = eglCreatePbufferSurface(display, config,
	    (EGLint[]){EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE});
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
		fprintf(stderr, "gpu_host: no GL context\n");
		return 1;
	}
	printf("gpu_host: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	fflush(stdout);

	program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER, VERTEX));
	glAttachShader(program, compile(GL_FRAGMENT_SHADER, FRAGMENT));
	glLinkProgram(program);
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "layer"), 0);

	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	listener = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((unsigned short)port);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 4) != 0) {
		fprintf(stderr, "gpu_host: cannot listen on %d\n", port);
		return 1;
	}
	printf("gpu_host: listening on %d\n", port);
	fflush(stdout);

	for (;;) {
		char header[64];
		int bytes = 0, at = 0, c;
		double started, elapsed;

		client = accept(listener, NULL, NULL);
		if (client < 0) {
			continue;
		}
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
		while ((c = read(client, &header[at], 1)) == 1 && header[at] != '\n' && at < (int)sizeof(header) - 1) {
			at++;
		}
		header[at] = '\0';
		if (header[0] == 'Q') {
			close(client);
			continue;
		}
		if (header[0] != 'F' || sscanf(header, "F %d %d %d", &width, &height, &bytes) != 3) {
			fprintf(stderr, "gpu_host: bad header '%s'\n", header);
			close(client);
			continue;
		}
		free(layer);
		layer = malloc((size_t)bytes);
		if (!read_full(client, layer, (size_t)bytes)) {
			close(client);
			continue;
		}
		if (!texture_ready || texture == 0) {
			/* one texture, refilled the way CoreAnimation refills its layers */
			glGenTextures(1, &texture);
			glBindTexture(GL_TEXTURE_2D, texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glGenFramebuffers(1, &framebuffer);
			glGenRenderbuffers(1, &rb);
			free(frame);
			frame = malloc((size_t)width * height * 4);
			texture_ready = 1;
		}
		started = 0;
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			started = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
		}
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
		    layer);
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, rb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
		glViewport(0, 0, width, height);
		{
			float quad[16] = {-1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, 1, 0, 1};
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
			glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
			    (void *)(2 * sizeof(float)));
		}
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, frame);
		{
			struct timespec ts;
			char reply[64];
			int length;

			clock_gettime(CLOCK_MONOTONIC, &ts);
			elapsed = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6 - started;
			length = snprintf(reply, sizeof(reply), "O %d %d %d\n", width, height, width * height * 4);
			write_full(client, reply, (size_t)length);
			write_full(client, frame, (size_t)width * height * 4);
			printf("gpu_host: %dx%d drawn in %.2f ms\n", width, height, elapsed);
			fflush(stdout);
		}
		close(client);
	}
	return 0;
}
