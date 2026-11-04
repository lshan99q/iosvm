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
 * Stage 11 loaded this library into backboardd and SpringBoard and got the two "probe loaded"
 * lines and not one call line, so stage 12 adds a second mechanism beside the interposing one:
 * the GOT itself (see "the second mechanism" further down). Both are in this file, both are always
 * on, and either one is enough to produce the lines below; the "got rebind" lines say what the
 * second one found.
 *
 * What to look for in the console, one line per call (the first 200 of each, then a note):
 *
 *   ciq: probe loaded (SpringBoard): CAImageQueueRegisterIOSurfaceBuffer + IOSurfaceCreate
 *   ciq: got rebind SpringBoard: 3 slot(s) in 2 image(s) (IOSurfaceCreate=2+1 island, CAImageQueueRegisterIOSurfaceBuffer=0+0 island)
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
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/vm_page_size.h>
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

/*
 * ---------------------------------------------------------------------------------------------
 * The second mechanism: the GOT itself (fishhook's idea, stage 12's P1''''').
 *
 * Interposition is dyld's business: it rewrites binds while it resolves them. By the time a
 * library that was inserted with DYLD_INSERT_LIBRARIES (or dyld itself) runs its initializers,
 * the images it would have patched have had their chained fixups applied, so a bind dyld has
 * already done is not ours to redirect -- which is exactly what stage 11 saw: the two
 * "probe loaded" lines, and not one call line. This pass does not ask dyld for anything. It walks
 * every loaded image's symbol-pointer sections (__got, __la_symbol_ptr, the S_NON_LAZY_SYMBOL_POINTERS
 * and S_LAZY_SYMBOL_POINTERS sections), names every slot from the image's own LC_SYMTAB and
 * LC_DYSYMTAB indirect symbol table, and writes the address of our replacement into the slots
 * whose name is one of the two functions. A call made through such a slot -- which is every call
 * to a function in another image -- then lands here whatever dyld decided before.
 *
 * What keeps it from hurting a process:
 *  - our own image is skipped: the replacements call the original by name through their own
 *    slots, and redirecting those would call them for ever;
 *  - __DATA_CONST is read-only: the page is made writable with VM_PROT_COPY (a private copy, which
 *    is what dyld's own patching does) and put back to the segment's initprot afterwards; a page
 *    that will not take it is skipped with one line;
 *  - __auth_got (arm64e signed pointers) is not touched -- a plain write would leave a pointer that
 *    fails authentication on the first call -- and says so once;
 *  - no allocation, no recursion, every count bounded: at most a few "ciq: got" lines per process.
 *
 * Images loaded after this library (a framework opened lazily by SpringBoard) get the same pass
 * from _dyld_register_func_for_add_image().
 */

struct ciq_target {
	const char *name;		/* the symbol as the indirect symbol table spells it */
	const void *replacement;
	const void *real;		/* the function itself, as this image's own reference sees it */
	unsigned patched;
	unsigned island;		/* slots of the shared cache's stub islands changed */
};

static struct ciq_target ciq_targets[] = {
	{ "_IOSurfaceCreate", (const void *)watched_surface_create, NULL, 0, 0 },
	{ "_CAImageQueueRegisterIOSurfaceBuffer", (const void *)watched_register_iosurface, NULL, 0, 0 },
};

#define CIQ_TARGET_COUNT (sizeof(ciq_targets) / sizeof(ciq_targets[0]))

static uintptr_t ciq_own_image;		/* our own mach_header: its slots are the originals */
static unsigned ciq_patched_images;

/* what to call an image in a log line: the last component of its path, or "?" */
static const char *
got_image_name(const struct mach_header *mh, Dl_info *info)
{
	const char *slash;

	if (dladdr((const void *)mh, info) != 0 && info->dli_fname != NULL) {
		slash = strrchr(info->dli_fname, '/');

		return slash != NULL ? slash + 1 : info->dli_fname;
	}
	return "?";
}

/*
 * Is this section a table of pointers to functions in other images? Both the ordinary form and the
 * one the dyld shared cache is built in: there __got is S_REGULAR, not S_NON_LAZY_SYMBOL_POINTERS,
 * but reserved1 still names its first slot in the indirect symbol table (QuartzCore: __got of 1433
 * slots, reserved1 1225, nindirectsyms 2658 -- 1225 + 1433 == 2658, and the names at 1225 are
 * _CFAbsoluteTimeGetCurrent and on alphabetically), and its slots that import a function are the
 * ones this pass is after.
 */
static int
got_symbol_pointer_section(const struct section_64 *sect)
{
	uint32_t type = sect->flags & SECTION_TYPE;

	if (type == S_NON_LAZY_SYMBOL_POINTERS || type == S_LAZY_SYMBOL_POINTERS) {
		return 1;
	}
	if (type == S_REGULAR && sect->reserved1 != 0 &&
	    (strcmp(sect->sectname, "__got") == 0 || strcmp(sect->sectname, "__la_symbol_ptr") == 0)) {
		return 1;
	}
	return 0;
}

/*
 * __DATA_CONST is read-only once dyld has done its fixups, whatever the load command's initprot
 * says (on disk it says read-write; dyld protects the segment afterwards), so it always needs the
 * copy-on-write trick below. Everything else follows its initprot.
 */
static int
got_read_only(const struct segment_command_64 *seg)
{
	if (strcmp(seg->segname, "__DATA_CONST") == 0 || strcmp(seg->segname, "__AUTH_CONST") == 0) {
		return 1;
	}
	return (seg->initprot & VM_PROT_WRITE) == 0;
}

/* the protection to put a segment's pages back to when they were not writable */
static vm_prot_t
got_put_back(const struct segment_command_64 *seg)
{
	if (strcmp(seg->segname, "__DATA_CONST") == 0 || strcmp(seg->segname, "__AUTH_CONST") == 0) {
		return VM_PROT_READ;
	}
	return seg->initprot != 0 ? (seg->initprot & ~VM_PROT_COPY) : VM_PROT_READ;
}

/* write the replacements into one symbol-pointer section; how many slots changed */
static unsigned
got_patch_section(const struct segment_command_64 *seg, const struct section_64 *sect, intptr_t slide,
    const struct nlist_64 *symbols, uint32_t nsyms, const char *strings, uint32_t strsize,
    const uint32_t *indirect, uint32_t nindirect, const char *image)
{
	uintptr_t addr = (uintptr_t)slide + (uintptr_t)sect->addr;
	size_t slots = (size_t)(sect->size / sizeof(void *));
	uint32_t first = sect->reserved1;
	unsigned changed = 0;
	int writable = !got_read_only(seg);
	uintptr_t page, end;
	size_t i;

	if (sect->size == 0 || sect->size % sizeof(void *) != 0 || first >= nindirect) {
		return 0;
	}
	if (slots > (size_t)(nindirect - first)) {
		slots = (size_t)(nindirect - first);
	}
	page = addr & ~(uintptr_t)(vm_page_size - 1);
	end = (addr + slots * sizeof(void *) + (uintptr_t)vm_page_size - 1) &
	    ~(uintptr_t)(vm_page_size - 1);
	if (!writable) {
		if (vm_protect(mach_task_self(), (vm_address_t)page, (vm_size_t)(end - page), FALSE,
		    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
			say("ciq: got %s: %s,%s cannot be made writable, skipped\n", image,
			    seg->segname, sect->sectname);
			return 0;
		}
	}
	for (i = 0; i < slots; i++) {
		uint32_t index = indirect[first + i];
		const struct nlist_64 *symbol;
		const char *name;
		void **slot = (void **)(addr + i * sizeof(void *));
		size_t t;

		if (index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
			continue;	/* local or absolute: not an import of another image */
		}
		if (index >= nsyms) {
			continue;
		}
		symbol = &symbols[index];
		if (symbol->n_un.n_strx == 0 || symbol->n_un.n_strx >= strsize) {
			continue;
		}
		name = strings + symbol->n_un.n_strx;
		for (t = 0; t < CIQ_TARGET_COUNT; t++) {
			if (strcmp(name, ciq_targets[t].name) != 0) {
				continue;
			}
			if (*slot != ciq_targets[t].replacement) {
				*slot = (void *)ciq_targets[t].replacement;
				ciq_targets[t].patched++;
				changed++;
			}
			break;
		}
	}
	if (!writable) {
		/* __DATA_CONST goes back to the read-only it was, as dyld left it */
		(void)vm_protect(mach_task_self(), (vm_address_t)page, (vm_size_t)(end - page), FALSE,
		    got_put_back(seg));
	}
	return changed;
}

/*
 * The shared cache's stub islands.
 *
 * An image in the dyld shared cache does not go through its own __got for a function in another
 * image: its __stubs entries were rebuilt to load the pointer from a shared "island" page instead
 * (QuartzCore's __stubs: 1225 stubs, 4 of them point into its own __got, the rest into pages
 * around 0x1dade6000). Those pages have no symbol table to name a slot from, so this pass goes
 * the other way: it decodes each stub's adrp+ldr to find the slot that stub will load, and changes
 * the slot when it holds the address of one of our two functions -- which dyld has already
 * resolved by the time this library's initializers run. A slot that holds anything else is left
 * alone, so a table that keeps the same address for its own reasons is not disturbed.
 *
 * A stub is three instructions on arm64:
 *	adrp x16, <page>
 *	ldr  x16, [x16, #<slot>]
 *	br   x16
 * anything else (arm64e's authenticated stubs, a stub that is no longer that shape) is skipped.
 */
static unsigned
got_patch_stubs(const struct section_64 *sect, intptr_t slide, const char *image)
{
	uintptr_t base = (uintptr_t)slide + (uintptr_t)sect->addr;
	size_t entry = sect->reserved2;		/* the size of one stub: 12 on arm64 */
	size_t count, i, t;
	unsigned changed = 0;

	if (entry < 12 || entry > 32 || sect->size == 0 || sect->size % entry != 0) {
		return 0;
	}
	count = (size_t)(sect->size / entry);
	for (i = 0; i < count; i++) {
		const uint32_t *code = (const uint32_t *)(base + i * entry);
		uint32_t adrp = code[0], ldr = code[1], br = code[2];
		uint64_t immlo, immhi;
		int64_t imm;
		uintptr_t island, slot_addr, page;
		void **slot;

		if ((adrp & 0x9f00001f) != 0x90000010 || (ldr & 0xffc00000) != 0xf9400000 ||
		    (ldr & 0x1f) != 16 || ((ldr >> 5) & 0x1f) != 16 || br != 0xd61f0200) {
			continue;	/* not the plain adrp/ldr/br x16 stub */
		}
		immlo = (uint64_t)((adrp >> 29) & 3);
		immhi = (uint64_t)((adrp >> 5) & 0x7ffff);
		imm = (int64_t)((immhi << 2) | immlo);
		if (imm & (1 << 20)) {
			imm -= 1 << 21;
		}
		island = ((base + i * entry) & ~(uintptr_t)0xfff) + (uintptr_t)(imm << 12);
		slot_addr = island + (uintptr_t)((ldr >> 10) & 0xfff) * sizeof(void *);
		slot = (void **)slot_addr;
		for (t = 0; t < CIQ_TARGET_COUNT; t++) {
			if (ciq_targets[t].real == NULL ||
			    ciq_targets[t].real == ciq_targets[t].replacement ||
			    *slot != ciq_targets[t].real) {
				continue;
			}
			page = slot_addr & ~(uintptr_t)(vm_page_size - 1);
			if (vm_protect(mach_task_self(), (vm_address_t)page, (vm_size_t)vm_page_size,
			    FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
				say("ciq: got %s: stub island cannot be made writable, skipped\n", image);
				return changed;
			}
			*slot = (void *)ciq_targets[t].replacement;
			(void)vm_protect(mach_task_self(), (vm_address_t)page, (vm_size_t)vm_page_size,
			    FALSE, VM_PROT_READ);
			ciq_targets[t].island++;
			changed++;
			break;
		}
	}
	return changed;
}

/* one image: find its tables, then every symbol-pointer section it has */
static void
got_scan_image(const struct mach_header *mh, intptr_t slide)
{
	const struct mach_header_64 *header = (const struct mach_header_64 *)mh;
	const struct load_command *cmd;
	const struct segment_command_64 *linkedit = NULL;
	const struct symtab_command *symtab = NULL;
	const struct dysymtab_command *dysymtab = NULL;
	const struct nlist_64 *symbols;
	const char *strings;
	const uint32_t *indirect;
	uintptr_t linkedit_base;
	Dl_info info;
	const char *image;
	uint32_t i;
	unsigned changed = 0;

	if (header == NULL || header->magic != MH_MAGIC_64 || (uintptr_t)header == ciq_own_image) {
		return;
	}
	/* pass one: the tables the slots are named from */
	cmd = (const struct load_command *)(header + 1);
	for (i = 0; i < header->ncmds; i++) {
		if (cmd->cmdsize < sizeof(*cmd)) {
			return;
		}
		switch (cmd->cmd) {
		case LC_SEGMENT_64: {
			const struct segment_command_64 *seg = (const void *)cmd;

			if (strcmp(seg->segname, "__LINKEDIT") == 0) {
				linkedit = seg;
			}
		} break;
		case LC_SYMTAB:
			symtab = (const void *)cmd;
			break;
		case LC_DYSYMTAB:
			dysymtab = (const void *)cmd;
			break;
		}
		cmd = (const struct load_command *)((const char *)cmd + cmd->cmdsize);
	}
	if (linkedit == NULL || symtab == NULL || dysymtab == NULL || symtab->nsyms == 0 ||
	    symtab->strsize == 0 || dysymtab->nindirectsyms == 0) {
		return;
	}
	linkedit_base = (uintptr_t)slide + (uintptr_t)linkedit->vmaddr - (uintptr_t)linkedit->fileoff;
	symbols = (const struct nlist_64 *)(linkedit_base + symtab->symoff);
	strings = (const char *)(linkedit_base + symtab->stroff);
	indirect = (const uint32_t *)(linkedit_base + dysymtab->indirectsymoff);
	image = got_image_name(mh, &info);

	/* pass two: every symbol-pointer section in the image */
	cmd = (const struct load_command *)(header + 1);
	for (i = 0; i < header->ncmds; i++) {
		if (cmd->cmdsize < sizeof(*cmd)) {
			return;
		}
		if (cmd->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *seg = (const void *)cmd;
			const struct section_64 *sect = (const struct section_64 *)(seg + 1);
			uint32_t s;

			for (s = 0; s < seg->nsects; s++, sect++) {
				if ((sect->flags & SECTION_TYPE) == S_SYMBOL_STUBS) {
					changed += got_patch_stubs(sect, slide, image);
					continue;
				}
				if (!got_symbol_pointer_section(sect)) {
					continue;
				}
				if (strcmp(sect->sectname, "__auth_got") == 0 ||
				    strcmp(seg->segname, "__AUTH_CONST") == 0 ||
				    strcmp(seg->segname, "__AUTH") == 0) {
					say("ciq: got %s: %s,%s skipped (arm64e signed pointers)\n",
					    image, seg->segname, sect->sectname);
					continue;
				}
				changed += got_patch_section(seg, sect, slide, symbols, symtab->nsyms,
				    strings, symtab->strsize, indirect, dysymtab->nindirectsyms, image);
			}
		}
		cmd = (const struct load_command *)((const char *)cmd + cmd->cmdsize);
	}
	if (changed != 0) {
		ciq_patched_images++;
		say("ciq: got %s: %u slot(s)\n", image, changed);
	}
}

/* one image has been added (or was here already): the same pass, from dyld's callback */
static void
got_image_added(const struct mach_header *mh, intptr_t slide)
{
	got_scan_image(mh, slide);
}

/*
 * The pass over everything, existing images and the ones that come later. dyld calls the callback
 * for every image that is loaded at registration time, so there is no window between "what is
 * loaded" and "what the callback sees".
 */
static void
got_rebind_everything(void)
{
	Dl_info info;
	unsigned total = 0;
	size_t t;

	if (dladdr((const void *)&got_rebind_everything, &info) != 0) {
		ciq_own_image = (uintptr_t)info.dli_fbase;
	}
	ciq_targets[0].real = (const void *)IOSurfaceCreate;
	ciq_targets[1].real = (const void *)CAImageQueueRegisterIOSurfaceBuffer;
	_dyld_register_func_for_add_image(got_image_added);
	for (t = 0; t < CIQ_TARGET_COUNT; t++) {
		total += ciq_targets[t].patched + ciq_targets[t].island;
	}
	say("ciq: got rebind %s: %u slot(s) in %u image(s) (IOSurfaceCreate=%u+%u island, "
	    "CAImageQueueRegisterIOSurfaceBuffer=%u+%u island)\n", process_name(), total,
	    ciq_patched_images, ciq_targets[0].patched, ciq_targets[0].island,
	    ciq_targets[1].patched, ciq_targets[1].island);
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
	got_rebind_everything();
}
