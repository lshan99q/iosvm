/*
 * libhakios.ciq.dylib: the IOSurface a client hands to CoreAnimation, said out loud (stage 11, P2).
 *
 * The kernel says what our MiniIOSurface driver makes:
 *
 *   MiniIOSurface: SpringBoard[10] created surface 4: 312x202 LA08, 640 bytes per row
 *
 * and that 312x202 LA08 surface -- 4 bytes per pixel of luminance+alpha, so the text the home
 * screen draws for itself -- is the picture we want on the display. What the kernel's line does
 * not say is which of the two calls below made it, nor which CAImageQueue it is shown in, so this
 * library puts one line on the console for each of them, in SpringBoard and in backboardd (the
 * two processes build.sh's INJECT now gives it to).
 *
 * Two hooks, and the reason for the second one:
 *
 *   CAImageQueueRegisterIOSurfaceBuffer(CAImageQueueRef queue, IOSurfaceRef surface, uint32_t flags)
 *	-> CAImageQueueBufferId
 *	QuartzCore's exported C function. In this system's dyld shared cache (20F66, the cache that
 *	ships here) it is QuartzCore __TEXT,__text, unslid 0x18833c9dc:
 *
 *	  0x18833c9dc  f8 5f bc a9   stp  x24, x23, [sp, #-0x40]!
 *	  0x18833c9f0  c1 04 00 b4   cbz  x1, 0x18833ca88          ; surface == NULL -> return 0
 *	  0x18833c9f4  f3 03 00 aa   mov  x19, x0                  ; x0 = the CAImageQueue
 *	  0x18833c9f8  08 18 40 f9   ldr  x8, [x0, #0x30]          ; queue->...; 0 -> return 0
 *	  0x18833ca00  f4 03 02 aa   mov  x20, x2                  ; x2 = flags, used as bits (below)
 *	  0x18833ca18  02 17 80 52   mov  w2, #0xb8                ; a 184-byte buffer object
 *	  0x18833ca1c  b2 f1 61 95   bl   0x18dbb90e4              ; calloc
 *	  0x18833ca28  ff 02 13 72   tst  w23, #0x2000
 *	  0x18833ca34  23 01 88 1a   csel w3, w9, w8, eq           ; 31 or 29: the buffer's kind
 *	  0x18833ca40  02 00 80 12   mov  w2, #0xffffffff
 *	  0x18833ca44  ...            bl   0x188343658              ; (buffer, surface, -1, kind, 0,0,0)
 *	  0x18833ca54  74 02 00 37   tbnz w20, #0, ...             ; flags bit 0: sets 0x120
 *	  0x18833ca58  f4 02 08 37   tbnz w20, #1, ...             ; flags bit 1: clears them
 *	  0x18833ca5c  94 00 10 36   tbz  w20, #2, 0x18833ca6c      ; flags bit 2: sets 0x40
 *	  0x18833ca6c  e0 03 13 aa   mov  x0, x19                  ; queue
 *	  0x18833ca70  e1 03 15 aa   mov  x1, x21                  ; buffer
 *	  0x18833ca84  b3 24 00 14   b    0x188345d50              ; tail call -> returns the buffer id
 *
 *	so the ABI is fixed: x0 is the CAImageQueue (checked, then used), x1 the IOSurface (checked,
 *	then passed on), x2 a flags word, and x0 on return is whatever the tail call at 0x188345d50
 *	returns -- a buffer id, 0 when the call failed. That is what this file declares: whole
 *	registers as uintptr_t, passed through untouched, and only x1 (an IOSurface, by name and by
 *	the disassembly) is asked about its size.
 *
 *   IOSurfaceCreate(CFDictionaryRef) -> IOSurfaceRef
 *	the call that actually creates a surface, in IOSurface.framework. Hooked because the first
 *	one can be silent: interposition (__DATA,__interpose) only redirects calls that cross an
 *	image boundary, and QuartzCore calls its own CAImageQueueRegisterIOSurfaceBuffer directly --
 *	two BLs to 0x18833c9dc in QuartzCore's __TEXT (0x188386980 and 0x188461ba8) -- which dyld
 *	cannot interpose. The surface in the kernel's line is made in SpringBoard's process by
 *	IOSurfaceCreate, whichever framework calls it, and that call does cross: it leaves the
 *	calling image for IOSurface.framework, so this hook sees it.
 *
 * What to look for in the console, one line per call (the first 200 of each, then a note):
 *
 *   ciq: probe loaded (SpringBoard): CAImageQueueRegisterIOSurfaceBuffer + IOSurfaceCreate
 *   ciq: SpringBoard: create s=0x... id=4 312x202 LA08 bpr=640
 *   ciq: SpringBoard: register q=0x... s=0x... id=4 312x202 LA08 bpr=640 flags=0x0 -> 0x1
 *
 * "id=" is the IOSurfaceID the kernel's line calls "surface 4", so the two logs line up by it;
 * 312x202 LA08 is the one this stage is after.
 *
 * Nothing here can hurt a process that is brought up with it: there is no constructor work beyond
 * one line, no dlsym whose failure could matter, and no allocation. The replacements call the
 * functions they replace by name (as src/inject_app.c does with posix_spawn -- dyld does not
 * interpose the interposing image's own calls), pass every register through, and return exactly
 * what came back.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* how many lines each hook may print: a boot makes thousands of surfaces, and the console is shared */
#define CIQ_LIMIT 200

/*
 * The two functions, declared as the ABI read from the disassembly above (the real headers for
 * these are private; IOSurface's are in <IOSurface/IOSurface.h>, and these are the same types).
 */
extern uintptr_t CAImageQueueRegisterIOSurfaceBuffer(void *queue, void *surface, uintptr_t flags);
extern void *IOSurfaceCreate(const void *properties);
extern uint32_t IOSurfaceGetID(void *surface);
extern size_t IOSurfaceGetWidth(void *surface);
extern size_t IOSurfaceGetHeight(void *surface);
extern uint32_t IOSurfaceGetPixelFormat(void *surface);
extern size_t IOSurfaceGetBytesPerRow(void *surface);

static int console = -1;
static unsigned register_calls;
static unsigned create_calls;

/* where what is said goes: the standard error, which is the console's for SpringBoard and backboardd */
static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	if (write(2, line, (size_t)n) < 0) {
		if (console < 0) {
			console = open("/dev/console", O_WRONLY);
		}
		if (console >= 0) {
			(void)write(console, line, (size_t)n);
		}
	}
}

/* the process name a line is about: SpringBoard or backboardd, and "?" if the system cannot say */
static const char *
process_name(void)
{
	const char *name = getprogname();

	return name != NULL ? name : "?";
}

/* 'LA08' as a printer shows it, and not the little-endian bytes of the OSType: anything unprintable
 * becomes '.', so that a pixel format we do not know cannot put control characters on the console */
static void
four_cc(uint32_t format, char out[5])
{
	int i;

	for (i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)(format >> (24 - 8 * i));

		out[i] = c >= 0x20 && c < 0x7f ? (char)c : '.';
	}
	out[4] = '\0';
}

/* the one line that says what a surface is: the fields the kernel's own "created surface" line has,
 * plus the ID, which is what ties the two lines together */
static void
surface_said(const char *what, void *surface, const void *queue, uintptr_t flags, int have_flags,
    uintptr_t result)
{
	char format[5];
	uint32_t id = 0;
	size_t width = 0, height = 0, bpr = 0;

	if (surface == NULL) {
		say("ciq: %s: %s %s s=0 -> (nothing)\n", process_name(), what,
		    have_flags ? "queue" : "no queue");
		return;
	}
	id = IOSurfaceGetID(surface);
	width = IOSurfaceGetWidth(surface);
	height = IOSurfaceGetHeight(surface);
	bpr = IOSurfaceGetBytesPerRow(surface);
	four_cc(IOSurfaceGetPixelFormat(surface), format);
	if (have_flags) {
		say("ciq: %s: %s q=%p s=%p id=%u %zux%zu %s bpr=%zu flags=%#lx -> %#lx\n", process_name(),
		    what, queue, surface, id, width, height, format, bpr, (unsigned long)flags,
		    (unsigned long)result);
	} else {
		say("ciq: %s: %s s=%p id=%u %zux%zu %s bpr=%zu\n", process_name(), what, surface, id,
		    width, height, format, bpr);
	}
}

/*
 * CAImageQueueRegisterIOSurfaceBuffer: registers a surface the caller already owns with an image
 * queue, and returns the queue's buffer id for it. x1 is the surface here (by name and by the
 * disassembly: it is stored and passed on to the buffer object), so it is the one argument this
 * hook looks into -- and only after the call, so that a call the real function rejects (a NULL
 * queue, an empty one) is not touched.
 */
static uintptr_t
watched_register_iosurface(void *queue, void *surface, uintptr_t flags)
{
	static __thread int inside;
	uintptr_t result;

	/* the call below is the original function (dyld does not interpose the interposing image's own
	 * calls, as src/inject_app.c's posix_spawn relies on); this guard is only so that a dyld that
	 * does would stop here instead of recursing for ever */
	if (inside) {
		return 0;
	}
	inside = 1;
	result = CAImageQueueRegisterIOSurfaceBuffer(queue, surface, flags);
	inside = 0;

	if (__atomic_fetch_add(&register_calls, 1, __ATOMIC_RELAXED) < CIQ_LIMIT) {
		surface_said("register", surface, queue, flags, 1, result);
	} else if (register_calls == CIQ_LIMIT + 1) {
		say("ciq: %s: register: %d calls printed, the rest is not\n", process_name(), CIQ_LIMIT);
	}
	return result;
}

/*
 * IOSurfaceCreate: the surface itself, as the kernel's line sees it. Called for every surface in
 * the process, so the count is throttled the same way.
 */
static void *
watched_surface_create(const void *properties)
{
	static __thread int inside;
	void *surface;

	if (inside) {
		return NULL;            /* see the guard above: it should never be taken */
	}
	inside = 1;
	surface = IOSurfaceCreate(properties);
	inside = 0;

	if (surface != NULL) {
		if (__atomic_fetch_add(&create_calls, 1, __ATOMIC_RELAXED) < CIQ_LIMIT) {
			surface_said("create", surface, NULL, 0, 0, 0);
		} else if (create_calls == CIQ_LIMIT + 1) {
			say("ciq: %s: create: %d calls printed, the rest is not\n", process_name(), CIQ_LIMIT);
		}
	}
	return surface;
}

/* the pair of replacement tables dyld reads: {ours, theirs}, in one array so that both are in the
 * same __DATA,__interpose section (src/inject_app.c does the same for posix_spawn) */
__attribute__((used)) static struct {
	const void *ours, *theirs;
} ciq_interpose[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)watched_register_iosurface, (const void *)CAImageQueueRegisterIOSurfaceBuffer },
	{ (const void *)watched_surface_create, (const void *)IOSurfaceCreate },
};

__attribute__((constructor))
static void
hakios_ciq_init(void)
{
	say("ciq: probe loaded (%s): CAImageQueueRegisterIOSurfaceBuffer + IOSurfaceCreate\n",
	    process_name());
}
