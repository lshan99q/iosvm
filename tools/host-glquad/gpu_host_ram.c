/* gpu_host_ram: draw a frame that is already in the guest's memory, without copying it.
 *
 * The socket transport measured 6.8 s a frame, all of it copying 21.9 MB through an emulated stack;
 * drawing was 32 ms. With QEMU_RAM_SHARE=on the file QEMU backs the guest's RAM with *is* that
 * memory, so the layer can be read where it lies and the frame written where the display reads it.
 * This is that path, driven by a one-line request instead of a payload:
 *
 *   request   "S <width> <height> <layer offset> <frame offset>\n"   (offsets into the RAM file)
 *   answer    "S done <milliseconds>\n"
 *
 * Layer and frame are BGRA, width*height*4 bytes each.
 *
 *   gpu_host_ram <ram.img> <port>
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

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
		fprintf(stderr, "gpu_host_ram: shader: %s\n", log);
	}
	return shader;
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

int
main(int argc, char **argv)
{
	const char *ram_path = argc > 1 ? argv[1] : "qemu/ram.img";
	int port = argc > 2 ? atoi(argv[2]) : 9101;
	int fd = open(ram_path, O_RDWR);
	struct stat info;
	unsigned char *ram;
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	EGLint config_count = 0;
	GLuint program, texture = 0, framebuffer = 0, rb = 0, vao, vbo;
	int listener, client;
	int texture_width = 0, texture_height = 0;

	if (fd < 0 || fstat(fd, &info) != 0) {
		fprintf(stderr, "gpu_host_ram: cannot open %s\n", ram_path);
		return 1;
	}
	ram = mmap(NULL, (size_t)info.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ram == MAP_FAILED) {
		fprintf(stderr, "gpu_host_ram: cannot map %s\n", ram_path);
		return 1;
	}
	printf("gpu_host_ram: %s mapped, %zu bytes\n", ram_path, (size_t)info.st_size);

	display = make_display();
	eglBindAPI(EGL_OPENGL_API);
	static const EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};
	eglChooseConfig(display, attributes, &config, 1, &config_count);
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
	surface = eglCreatePbufferSurface(display, config, (EGLint[]){EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE});
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
		fprintf(stderr, "gpu_host_ram: no GL context\n");
		return 1;
	}
	printf("gpu_host_ram: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	program = glCreateProgram();
	glAttachShader(program, compile(GL_VERTEX_SHADER,
	    "#version 330 core\n"
	    "layout(location=0) in vec2 p; layout(location=1) in vec2 t; out vec2 uv;\n"
	    "void main() { uv = t; gl_Position = vec4(p, 0.0, 1.0); }\n"));
	glAttachShader(program, compile(GL_FRAGMENT_SHADER,
	    "#version 330 core\n"
	    "in vec2 uv; out vec4 colour; uniform sampler2D layer;\n"
	    "void main() { colour = texture(layer, uv); }\n"));
	glLinkProgram(program);
	glUseProgram(program);
	glUniform1i(glGetUniformLocation(program, "layer"), 0);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	{
		float quad[16] = {-1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, 1, 0, 1};
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
	}

	listener = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((unsigned short)port);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 4) != 0) {
		fprintf(stderr, "gpu_host_ram: cannot listen on %d\n", port);
		return 1;
	}
	printf("gpu_host_ram: listening on %d\n", port);
	fflush(stdout);

	for (;;) {
		char header[128];
		int at = 0, c;
		int width = 0, height = 0;
		unsigned long long layer_at = 0, frame_at = 0;
		double started, elapsed;

		client = accept(listener, NULL, NULL);
		if (client < 0) {
			continue;
		}
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
		while ((c = recv(client, &header[at], 1, 0)) == 1 && header[at] != '\n' &&
		    at < (int)sizeof(header) - 1) {
			at++;
		}
		header[at] = '\0';
		if (header[0] != 'S' ||
		    sscanf(header, "S %d %d %llu %llu", &width, &height, &layer_at, &frame_at) != 4) {
			fprintf(stderr, "gpu_host_ram: bad request '%s'\n", header);
			close(client);
			continue;
		}
		size_t bytes = (size_t)width * height * 4;
		if (layer_at + bytes > (unsigned long long)info.st_size ||
		    frame_at + bytes > (unsigned long long)info.st_size) {
			fprintf(stderr, "gpu_host_ram: request outside the memory file\n");
			close(client);
			continue;
		}
		started = now_ms();
		if (texture == 0 || texture_width != width || texture_height != height) {
			if (texture == 0) {
				glGenTextures(1, &texture);
				glGenFramebuffers(1, &framebuffer);
				glGenRenderbuffers(1, &rb);
			}
			glBindTexture(GL_TEXTURE_2D, texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE,
			    ram + layer_at);
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
			glBindRenderbuffer(GL_RENDERBUFFER, rb);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
			texture_width = width;
			texture_height = height;
		} else {
			glBindTexture(GL_TEXTURE_2D, texture);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE,
			    ram + layer_at);
		}
		glViewport(0, 0, width, height);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
		/* straight into the guest's memory, where the display will read it */
		glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, ram + frame_at);
		elapsed = now_ms() - started;
		{
			char reply[64];
			int length = snprintf(reply, sizeof(reply), "S done %.2f\n", elapsed);
			send(client, reply, (size_t)length, 0);
		}
		printf("gpu_host_ram: %dx%d drawn in place, %.2f ms\n", width, height, elapsed);
		fflush(stdout);
		close(client);
	}
	return 0;
}
