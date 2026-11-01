/* host_glwire: make CoreAnimation's GL calls on the host's GPU.
 *
 * The guest records the calls QuartzCore makes (62 of them, see spec.def) and this replays them on
 * a real context -- a Tesla V100 here -- then copies the resulting picture straight into the guest's
 * memory, which is mapped as the RAM file. Nothing is read back into the guest's CPU.
 *
 *   host_glwire <ram.img> <port>
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
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

#include "wire.h"

static GLuint frame_buffer;

static double
now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
make_frame_buffer(int width, int height)
{
	GLuint rb = 0;

	if (frame_buffer != 0) {
		glDeleteFramebuffers(1, &frame_buffer);
	}
	glGenFramebuffers(1, &frame_buffer);
	glBindFramebuffer(GL_FRAMEBUFFER, frame_buffer);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
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
dispatch(wire_buffer *r, uint32_t opcode)
{
#include "dispatch.inc"
	return 0;
}

int
main(int argc, char **argv)
{
	const char *ram_path = argc > 1 ? argv[1] : "qemu/ram.img";
	int port = argc > 2 ? atoi(argv[2]) : 9110;
	int fd = open(ram_path, O_RDWR);
	struct stat info;
	unsigned char *ram;
	int listener;

	if (fd < 0 || fstat(fd, &info) != 0) {
		fprintf(stderr, "host_glwire: cannot open %s\n", ram_path);
		return 1;
	}
	ram = mmap(NULL, (size_t)info.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ram == MAP_FAILED) {
		fprintf(stderr, "host_glwire: cannot map %s\n", ram_path);
		return 1;
	}
	printf("host_glwire: %s mapped, %zu bytes\n", ram_path, (size_t)info.st_size);
	(void)ram;
	{
		EGLDisplay display = make_display();
		EGLConfig config;
		EGLint config_count = 0;
		EGLContext context;
		EGLSurface surface;
		static const EGLint attributes[] = {
		    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
		};

		eglBindAPI(EGL_OPENGL_API);
		if (!eglChooseConfig(display, attributes, &config, 1, &config_count) || config_count == 0) {
			fprintf(stderr, "host_glwire: no GL configuration\n");
			return 1;
		}
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
		surface = eglCreatePbufferSurface(display, config,
		    (EGLint[]){EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE});
		if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context)) {
			fprintf(stderr, "host_glwire: no GL context\n");
			return 1;
		}
		printf("host_glwire: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	}

	listener = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((unsigned short)port);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 4) != 0) {
		fprintf(stderr, "host_glwire: cannot listen on %d\n", port);
		return 1;
	}
	printf("host_glwire: listening on %d\n", port);
	fflush(stdout);
	for (;;) {
		int client = accept(listener, NULL, NULL);
		if (client < 0) {
			continue;
		}
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
		for (;;) {
			uint32_t header[2];
			/* a frame request: "R <records magic> <frame magic> <w> <h> <records> <frame>\n" */
			{
				char peek = 0;
				if (recv(client, &peek, 1, MSG_PEEK) == 1 && peek == 'R') {
					char line[160];
					int at = 0;
					unsigned long long records_magic = 0, frame_magic = 0;
					int width = 0, height = 0;
					size_t records_bytes = 0, frame_bytes = 0;
					unsigned char *records_at = NULL, *frame_at = NULL;

					while (at < (int)sizeof(line) - 1 &&
					    recv(client, &line[at], 1, 0) == 1 && line[at] != '\n') {
						at++;
					}
					line[at] = '\0';
					if (sscanf(line, "R %llu %llu %d %d %zu %zu", &records_magic, &frame_magic,
					    &width, &height, &records_bytes, &frame_bytes) != 6) {
						fprintf(stderr, "host_glwire: bad request '%s'\n", line);
						break;
					}
					{
						unsigned char pattern[8];
						for (int i = 0; i < 8; i++) {
							pattern[i] = (unsigned char)(records_magic >> (8 * i));
						}
						records_at = memmem(ram, (size_t)info.st_size, pattern, 8);
						for (int i = 0; i < 8; i++) {
							pattern[i] = (unsigned char)(frame_magic >> (8 * i));
						}
						frame_at = memmem(ram, (size_t)info.st_size, pattern, 8);
					}
					if (records_at == NULL || frame_at == NULL) {
						send(client, "R missing\n", 10, 0);
						break;
					}
					records_at += 8;
					frame_at += 8;
					{
						unsigned char *records = malloc(records_bytes);
						double began = now_seconds(), took;

						if (records == NULL ||
						    recv(client, records, records_bytes, MSG_WAITALL) !=
						    (ssize_t)records_bytes) {
							free(records);
							break;
						}
						/* draw into a picture of the panel's size, then put it where the
						 * guest will read it */
						{
							static int have_width, have_height;
							if (frame_buffer == 0 || have_width != width ||
							    have_height != height) {
								make_frame_buffer(width, height);
								have_width = width;
								have_height = height;
							}
						}
						glBindFramebuffer(GL_FRAMEBUFFER, frame_buffer);
						glViewport(0, 0, width, height);
						{
							wire_buffer r;
							size_t at2 = 0;

							while (at2 + 8 <= records_bytes) {
								uint32_t opcode, length;
								memcpy(&opcode, records + at2, 4);
								memcpy(&length, records + at2 + 4, 4);
								if (at2 + 8 + length > records_bytes) {
									break;
								}
								r.base = records + at2 + 8;
								r.capacity = length;
								r.at = 0;
								dispatch(&r, opcode);
								at2 += 8 + length;
							}
						}
						glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, frame_at);
						took = now_seconds() - began;
						{
							char reply[64];
							int length = snprintf(reply, sizeof(reply), "R done %.2f\n", took);
							send(client, reply, (size_t)length, 0);
						}
						free(records);
						printf("host_glwire: frame %dx%d replayed, %.2f ms\n", width, height, took);
						fflush(stdout);
					}
					continue;
				}
			}
			wire_buffer r;

			if (recv(client, header, sizeof(header), MSG_WAITALL) != (ssize_t)sizeof(header)) {
				break;
			}
			if (header[0] == 0xffffffffu) {
				break;      /* end of frame marker */
			}
			r.base = malloc(header[1]);
			r.capacity = header[1];
			r.at = 0;
			if (r.base == NULL ||
			    recv(client, r.base, header[1], MSG_WAITALL) != (ssize_t)header[1]) {
				free(r.base);
				break;
			}
			if (dispatch(&r, header[0]) != 0) {
				free(r.base);
				break;
			}
			free(r.base);
		}
		close(client);
	}
	return 0;
}
