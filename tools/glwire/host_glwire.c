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
