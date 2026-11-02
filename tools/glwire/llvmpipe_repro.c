/* llvmpipe_repro: the SIGSEGV after a large texture upload and an FBO attach.
 *
 * The live host crashed in libgallium a few calls after uploading a 1125 x 2436 texture and
 * attaching it to a framebuffer; the last call printed was glClearColor. This program performs just
 * that sequence on a fresh llvmpipe context so it can be run under gdb.
 *
 *   cc -O0 -g -o llvmpipe_repro llvmpipe_repro.c -lEGL -lGL
 *   ./llvmpipe_repro
 */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1125
#define H 2436

static EGLDisplay
display_open(void)
{
	PFNEGLQUERYDEVICESEXTPROC query = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
	    (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDeviceEXT devices[8];
	EGLint count = 0, major = 0, minor = 0;

	if (query != NULL && get_display != NULL && query(8, devices, &count) && count > 0) {
		for (EGLint i = 0; i < count; i++) {
			EGLDisplay d = get_display(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);

			if (d != EGL_NO_DISPLAY && eglInitialize(d, &major, &minor)) {
				return d;
			}
		}
	}
	{
		EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);

		eglInitialize(d, &major, &minor);
		return d;
	}
}

int
main(void)
{
	EGLDisplay display = display_open();
	EGLConfig config;
	EGLint count = 0;
	EGLContext context;
	EGLSurface surface;
	static const EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};
	unsigned char *pixels = malloc((size_t)W * H * 4);
	GLuint texture = 0, client_fbo = 0, rb = 0, guest_fbo = 0;

	if (pixels == NULL) {
		return 2;
	}
	memset(pixels, 0x40, (size_t)W * H * 4);
	eglBindAPI(EGL_OPENGL_API);
	if (!eglChooseConfig(display, attributes, &config, 1, &count) || count == 0) {
		return 2;
	}
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
	surface = eglCreatePbufferSurface(display, config,
	    (EGLint[]){ EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE });
	if (!eglMakeCurrent(display, surface, surface, context)) {
		return 2;
	}
	printf("renderer %s\n", glGetString(GL_RENDERER));

	printf("1. client framebuffer (renderbuffer)\n"); fflush(stdout);
	glGenFramebuffers(1, &client_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, client_fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	printf("   status %#x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER)); fflush(stdout);

	printf("2. big texture upload\n"); fflush(stdout);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	printf("   texture %u, error %#x\n", texture, glGetError()); fflush(stdout);

	printf("3. guest framebuffer, texture attached\n"); fflush(stdout);
	glGenFramebuffers(1, &guest_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, guest_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	printf("   status %#x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER)); fflush(stdout);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	printf("4. clear\n"); fflush(stdout);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	printf("   survived, error %#x\n", glGetError()); fflush(stdout);

	printf("5. second frame: redefine the attached texture\n"); fflush(stdout);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	printf("   redefine done, error %#x\n", glGetError()); fflush(stdout);
	glBindFramebuffer(GL_FRAMEBUFFER, guest_fbo);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	printf("   survived frame 2, error %#x\n", glGetError()); fflush(stdout);
	return 0;
}
