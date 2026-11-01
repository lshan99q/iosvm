/* host_glwire: make CoreAnimation's GL calls on the host's GPU.
 *
 * The guest records the calls QuartzCore makes (62 of them, see spec.def) and this replays them on
 * a real context -- a Tesla V100 here -- then copies the resulting picture straight into the guest's
 * memory, which is mapped as the RAM file. Nothing is read back into the guest's CPU.
 *
 * Each guest process that loads the library is one connection, and each connection gets its own GL
 * context and its own name maps: the guest's object names are per process, and two processes may
 * well both call their first texture 1. The request carries the magic words the guest put in front
 * of its record arena and its picture, so the host can find both without an address travelling.
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
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wire.h"
#include "names.h"

#define MAX_CLIENTS 16

struct client {
	int fd;
	EGLContext context;
	EGLSurface surface;
	GLuint framebuffer;
	int width, height;
	struct glstate names;
	unsigned char *frame_at;        /* the magic word in the RAM file, if seen */
	uint64_t frame_magic;
	int have_frame_at;
};

static EGLDisplay display;
static EGLConfig config;
static unsigned char *ram;
static size_t ram_bytes;
static struct client clients[MAX_CLIENTS];
static unsigned long long frames_replayed;

static double
now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int
make_client_current(struct client *client)
{
	if (client->context == EGL_NO_CONTEXT) {
		client->context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
		client->surface = eglCreatePbufferSurface(display, config,
		    (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });
	}
	if (client->context == EGL_NO_CONTEXT || client->surface == EGL_NO_SURFACE) {
		return 0;
	}
	return eglMakeCurrent(display, client->surface, client->surface, client->context);
}

static void
make_frame_buffer(struct client *client, int width, int height)
{
	GLuint rb = 0;

	if (client->framebuffer != 0) {
		glDeleteFramebuffers(1, &client->framebuffer);
		client->framebuffer = 0;
	}
	glGenFramebuffers(1, &client->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, client->framebuffer);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	client->width = width;
	client->height = height;
}

static uint64_t
read_magic(const unsigned char *at)
{
	uint64_t word;

	memcpy(&word, at, sizeof(word));
	return word;
}

/* The guest's picture buffer has a magic word in front of it; the magic is unique to the process,
 * so more than one guest can be served. The first scan of the 12 GB file is a second or two, after
 * that the address is cached and checked with one load. */
static unsigned char *
find_frame(struct client *client, uint64_t magic)
{
	unsigned char pattern[8];

	if (client->have_frame_at && client->frame_at + 8 <= ram + ram_bytes &&
	    read_magic(client->frame_at) == magic) {
		return client->frame_at;
	}
	for (int i = 0; i < 8; i++) {
		pattern[i] = (unsigned char)(magic >> (8 * i));
	}
	client->frame_at = memmem(ram, ram_bytes, pattern, 8);
	client->have_frame_at = client->frame_at != NULL;
	if (client->frame_at != NULL) {
		printf("host_glwire: picture %016llx at offset %#zx\n",
		    (unsigned long long)magic, (size_t)(client->frame_at - ram));
		fflush(stdout);
	}
	return client->frame_at;
}

static int
dispatch(struct glstate *g, wire_buffer *r, uint32_t opcode)
{
#include "dispatch.inc"
	return 0;
}

static void
reply(struct client *client, const char *line)
{
	send(client->fd, line, strlen(line), 0);
}

/* One frame: "R <records magic> <frame magic> <w> <h> <records> <frame>\n", the records, and the
 * reply once the picture is in the guest's buffer. */
static int
serve_frame(struct client *client)
{
	char line[192];
	int at = 0;
	unsigned long long records_magic = 0, frame_magic = 0;
	int width = 0, height = 0;
	size_t records_bytes = 0, frame_bytes = 0;
	double began, took;
	unsigned char *records;
	unsigned char *frame_at;

	while (at < (int)sizeof(line) - 1) {
		char c;
		ssize_t got = recv(client->fd, &c, 1, 0);

		if (got != 1) {
			return -1;
		}
		if (c == '\n') {
			break;
		}
		line[at++] = c;
	}
	line[at] = '\0';
	if (sscanf(line, "R %llu %llu %d %d %zu %zu", &records_magic, &frame_magic,
		&width, &height, &records_bytes, &frame_bytes) != 6) {
		fprintf(stderr, "host_glwire: bad request '%s'\n", line);
		return -1;
	}
	(void)records_magic;
	if (width <= 0 || height <= 0 || (size_t)width * (size_t)height * 4 != frame_bytes) {
		reply(client, "R badsize\n");
		return 0;
	}
	records = malloc(records_bytes);
	if (records == NULL) {
		reply(client, "R nomem\n");
		return 0;
	}
	if (recv(client->fd, records, records_bytes, MSG_WAITALL) != (ssize_t)records_bytes) {
		free(records);
		return -1;
	}
	began = now_seconds();
	frame_at = find_frame(client, frame_magic);
	if (frame_at == NULL || frame_at + 8 + frame_bytes > ram + ram_bytes) {
		free(records);
		reply(client, "R missing\n");
		return 0;
	}
	if (!make_client_current(client)) {
		free(records);
		reply(client, "R nocontext\n");
		return 0;
	}
	if (client->framebuffer == 0 || client->width != width || client->height != height) {
		make_frame_buffer(client, width, height);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, client->framebuffer);
	glViewport(0, 0, width, height);
	{
		wire_buffer r;
		size_t offset = 0;

		while (offset + 8 <= records_bytes) {
			uint32_t opcode, length;

			memcpy(&opcode, records + offset, 4);
			memcpy(&length, records + offset + 4, 4);
			if (offset + 8 + length > records_bytes) {
				fprintf(stderr, "host_glwire: a record runs past the frame\n");
				break;
			}
			r.base = records + offset + 8;
			r.capacity = length;
			r.at = 0;
			if (dispatch(&client->names, &r, opcode) != 0) {
				break;
			}
			offset += 8 + length;
		}
	}
	glFinish();
	glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, frame_at + 8);
	names_attribs_release(&client->names);
	wire_release();
	took = now_seconds() - began;
	frames_replayed++;
	{
		char done[64];
		int length = snprintf(done, sizeof(done), "R done %.2f\n", took);
		send(client->fd, done, (size_t)length, 0);
	}
	printf("host_glwire: frame %llu, %dx%d, %zu records B, %zu picture B, %.2f ms\n",
	    frames_replayed, width, height, records_bytes, frame_bytes, took);
	fflush(stdout);
	free(records);
	return 0;
}

/* The older raw-record path: an 8-byte header, then one record's payload. Kept for a client that
 * wants to send calls outside a frame. */
static int
serve_record(struct client *client)
{
	uint32_t header[2];
	wire_buffer r;

	if (recv(client->fd, header, sizeof(header), MSG_WAITALL) != (ssize_t)sizeof(header)) {
		return -1;
	}
	if (header[0] == 0xffffffffu) {
		return -1;
	}
	r.base = malloc(header[1]);
	r.capacity = header[1];
	r.at = 0;
	if (r.base == NULL ||
	    recv(client->fd, r.base, header[1], MSG_WAITALL) != (ssize_t)header[1]) {
		free(r.base);
		return -1;
	}
	if (!make_client_current(client)) {
		free(r.base);
		return -1;
	}
	dispatch(&client->names, &r, header[0]);
	free(r.base);
	return 0;
}

static void
close_client(struct client *client)
{
	if (client->fd < 0) {
		return;
	}
	if (client->context != EGL_NO_CONTEXT) {
		eglMakeCurrent(display, client->surface, client->surface, client->context);
		if (client->framebuffer != 0) {
			glDeleteFramebuffers(1, &client->framebuffer);
		}
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (client->surface != EGL_NO_SURFACE) {
			eglDestroySurface(display, client->surface);
		}
		eglDestroyContext(display, client->context);
	}
	names_free(&client->names);
	close(client->fd);
	printf("host_glwire: client %d closed\n", client->fd);
	fflush(stdout);
	client->fd = -1;
	client->context = EGL_NO_CONTEXT;
	client->surface = EGL_NO_SURFACE;
	client->framebuffer = 0;
	client->have_frame_at = 0;
	client->frame_at = NULL;
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
	const char *ram_path = argc > 1 ? argv[1] : "qemu/ram.img";
	int port = argc > 2 ? atoi(argv[2]) : 9110;
	int fd = open(ram_path, O_RDWR);
	struct stat info;
	int listener;
	int count = 0;
	static const EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};

	if (fd < 0 || fstat(fd, &info) != 0) {
		fprintf(stderr, "host_glwire: cannot open %s\n", ram_path);
		return 1;
	}
	ram = mmap(NULL, (size_t)info.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ram == MAP_FAILED) {
		fprintf(stderr, "host_glwire: cannot map %s\n", ram_path);
		return 1;
	}
	ram_bytes = (size_t)info.st_size;
	printf("host_glwire: %s mapped, %zu bytes\n", ram_path, ram_bytes);

	display = make_display();
	eglBindAPI(EGL_OPENGL_API);
	if (display == EGL_NO_DISPLAY ||
	    !eglChooseConfig(display, attributes, &config, 1, &count) || count == 0) {
		fprintf(stderr, "host_glwire: no GL configuration\n");
		return 1;
	}
	/* a context to report what the host's GL is before a guest connects */
	{
		EGLContext probe = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
		EGLSurface surface = eglCreatePbufferSurface(display, config,
		    (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });

		if (probe != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, probe)) {
			printf("host_glwire: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
			eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		}
		if (surface != EGL_NO_SURFACE) {
			eglDestroySurface(display, surface);
		}
		if (probe != EGL_NO_CONTEXT) {
			eglDestroyContext(display, probe);
		}
	}
	fflush(stdout);

	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
		clients[i].context = EGL_NO_CONTEXT;
		clients[i].surface = EGL_NO_SURFACE;
	}

	listener = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
	{
		struct sockaddr_in address;

		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons((unsigned short)port);
		if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
		    listen(listener, 4) != 0) {
			fprintf(stderr, "host_glwire: cannot listen on %d\n", port);
			return 1;
		}
	}
	printf("host_glwire: listening on %d\n", port);
	fflush(stdout);

	for (;;) {
		struct pollfd polled[1 + MAX_CLIENTS];
		int indices[1 + MAX_CLIENTS];
		int nfds = 1;

		polled[0].fd = listener;
		polled[0].events = POLLIN;
		indices[0] = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				polled[nfds].fd = clients[i].fd;
				polled[nfds].events = POLLIN;
				indices[nfds] = i;
				nfds++;
			}
		}
		if (poll(polled, (nfds_t)nfds, -1) < 0) {
			continue;
		}
		if (polled[0].revents & POLLIN) {
			int fd = accept(listener, NULL, NULL);

			if (fd >= 0) {
				int slot;
				for (slot = 0; slot < MAX_CLIENTS && clients[slot].fd >= 0; slot++) {
				}
				if (slot == MAX_CLIENTS) {
					close(fd);
				} else {
					memset(&clients[slot], 0, sizeof(clients[slot]));
					clients[slot].fd = fd;
					clients[slot].context = EGL_NO_CONTEXT;
					clients[slot].surface = EGL_NO_SURFACE;
					names_init(&clients[slot].names);
					setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
					printf("host_glwire: client %d connected\n", fd);
					fflush(stdout);
				}
			}
		}
		for (int i = 1; i < nfds; i++) {
			struct client *client = &clients[indices[i]];
			char peek = 0;
			ssize_t got;

			if (!(polled[i].revents & (POLLIN | POLLHUP | POLLERR))) {
				continue;
			}
			got = recv(client->fd, &peek, 1, MSG_PEEK);
			if (got <= 0) {
				close_client(client);
				continue;
			}
			if (peek == 'R') {
				if (serve_frame(client) != 0) {
					close_client(client);
				}
			} else if (serve_record(client) != 0) {
				close_client(client);
			}
		}
	}
	return 0;
}
