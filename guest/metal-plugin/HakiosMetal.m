/* SPDX-License-Identifier: MIT */
/*
 * HakiosMetal: the metalwire guest plug-in — a Metal device that lives on the host.
 *
 * What this is
 * ------------
 * Metal finds a GPU through IOKit: it walks the services of class IOAcceleratorES, reads their
 * MetalPluginName/MetalPluginClassName properties, loads /System/Library/Extensions/<name>.bundle,
 * takes the class, requires it to be a subclass of Metal's private _MTLDevice, and sends it
 * -initWithAcceleratorPort: (see the HakiOS tree's docs/ru/metal.md section 1 and metal_test.m).
 * The port publishes an empty IOAcceleratorES; the boot-arg metal=N makes it name this bundle. That
 * discovery chain is the technique this file follows, as the HakiOS reference plug-in
 * (userland/src/HakiosMetal.m) worked it out; none of its source is copied here.
 *
 * The class is made at run time because _MTLDevice is not in the SDK's Metal.tbd:
 * objc_allocateClassPair on the _MTLDevice the running Metal.framework has. The class declares
 * MTLDevice and the private MTLDeviceSPI — MTLAddDevice asserts conformance to both — and every
 * message Metal's runtime sends that this file does not implement is logged once ("asked and not
 * implemented") and answered with zero through forwardInvocation:, so the device can be asked
 * anything without the process dying.
 *
 * What it does
 * ------------
 * Nothing is executed in the guest. The device hands out buffers in process memory, textures that
 * are only descriptors (the pixels live on the host), a command queue, command buffers, and one
 * blit encoder. A frame is recorded the way Metal records it, and at -[commit] the plug-in writes
 * the frame down the wire (mw_wire.c, host/metalwire/metalwire_protocol.h, TCP 10.0.2.2:9111):
 *
 *   HELLO (once per connection)
 *   FRAME_BEGIN(seq, guest begin time)
 *     CREATE_BUFFER / CREATE_TEXTURE   the first frame that names each object
 *     UPLOAD_BUFFER                    the buffer's bytes as they are at commit
 *     COPY_BUFFER_TO_TEXTURE           the recorded blit
 *     PRESENT                          the drawable's texture
 *   FRAME_END(seq, encoded time, CRC-32, size)
 *   ... the host executes on the V100 through Vulkan, reads the picture back and publishes it, and
 *   answers with one struct mw_ack; -waitUntilCompleted reads that ack and reports its status.
 *
 * The object graph covered is deliberately small: device, queue, command buffer, blit encoder,
 * buffer, texture, drawable (MWDrawable below), plus the methods those need. Render/compute
 * encoders, libraries and pipelines do not exist here; a Metal program that asks for them gets
 * nil and a log line. That is the honest limit of the experiment: the test client is not
 * CoreAnimation (the port has no Metal CoreAnimation path at all — no default.metallib and no
 * AIR compiler — see the repo's ADR-002 and docs/research).
 *
 * What it must not do
 * -------------------
 * Hang a boot or crash a process that is not the experiment's. The device is shown only to the
 * processes the process gate allows (boot-arg metal= level, HakiosMetal.clients beside the bundle,
 * plus this experiment's own metal_frame/metal_probe), and the socket is made lazily, on the first
 * frame, with a bounded connect: if the host is not there the frame becomes a no-op that reports an
 * error, every 5 s a reconnect is tried, and the process runs on.
 *
 * Deliberate dealloc limitation
 * -----------------------------
 * An object whose class is built on one of Metal's private base classes (_MTLDevice,
 * _MTLCommandBuffer, _MTLLibrary, ...) never has the base's -dealloc called: Apple's implementation
 * dereferences dispatch state that a plain -init never sets up, which killed the ca-metal3-shim1
 * boot inside libdispatch (x1 = @selector(dealloc), x0 = NULL). The instance is freed with
 * object_dispose() after the plug-in's own state, so whatever the base's -init allocated is leaked;
 * this is small (a command buffer is a few KB) and is a deliberate limitation of the stub. The
 * class -> base pairs are logged once at registration ("its _MTLx -dealloc is skipped ...").
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The port's user address -> physical address query (bsd/dev/hakios.vtop.c, syscall 556). It is
 * how the GL replay path hands a texture to the host without copying it, and it is how this plug-in
 * can hand its picture over: the host has QEMU's RAM file mapped, so a list of physical segments
 * is all it needs. Not in the SDK, so the number is spelled here as eagl_remote.c spells it.
 */
#ifndef SYS_hakios_vtop
#define SYS_hakios_vtop 556
#endif
/* One list is enough for a buffer, a texture region or an IOSurface: the port's vtop call fills
 * 16-byte runs, and a run is at least a page, so 512 runs cover 2 MiB+ at the worst granularity
 * and the guest's 4.87 MB surface in the usual (contiguous) case. */
#define MW_VTOP_SEGMENTS 512
extern int syscall(int, ...);

#include "mw_metal_extras.h"
#include "mw_wire.h"

/* -------------------------------------------------------------------------------------------------
 * The log. The serial console is the only window this experiment has into the guest, so every
 * interesting decision is one "metalwire:" line. stderr is unbuffered and reaches the console even
 * when the program dies later without flushing stdout.
 * ---------------------------------------------------------------------------------------------- */

static void mw_log(const char *fmt, ...)
{
    va_list ap;
    char text[768];

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    fprintf(stderr, "metalwire: %s\n", text);
    fflush(stderr);
}

/* -------------------------------------------------------------------------------------------------
 * Globals. There is one device per process (Metal's model), one connection, one frame in flight
 * (the v1 wire is synchronous).
 * ---------------------------------------------------------------------------------------------- */

struct mw_buffer_state {
    uint32_t remote_id;
    uint32_t created_generation;
    uint64_t length;
    unsigned long options;
    void *memory;
    id label;
    uint64_t uploaded_seq; /* the last frame this buffer's bytes went to the host */
    /* WP-4 4.1d: whether the buffer may be handed out again. CoreAnimation keeps the buffers it
     * creates (boot wp4-soak-fix2: 750 `newBufferWithLength:262144` calls, one per frame, and the
     * lifetime counters show it never releases them), so the pool can never wait for
     * `-retainCount == 1`; a buffer becomes available once the frame that used it has been sent and
     * acknowledged. */
    int available;
};

/* -------------------------------------------------------------------------------------------------
 * 4.1 Object lifecycle counters and the per-frame instance pools.
 *
 * The stub classes are built on Metal's private bases (_MTLCommandBuffer, _MTLCommandQueue, ...).
 * Their base -dealloc dereferences dispatch state that a plain -init never creates; for the command
 * queue the research (docs/research/ca-metallib-url.md §10.1) pins the exact site: ivar 0xb0
 * `_submittedGroup`, released without a nil check. Two things keep the process from leaking a base
 * allocation per frame:
 *
 *   1. the queue's dealloc now finds a real dispatch group in that ivar and the base -dealloc runs
 *      (the same shape the analysis recommends: "create _submittedGroup with
 *      dispatch_group_create()"), so the queue's own allocation is freed;
 *   2. the command buffer and the encoders -- the objects CoreAnimation creates per frame and per
 *      render pass -- are reused from a fixed pool instead of being allocated and thrown away. The
 *      pool keeps one reference (so -dealloc never runs) and hands the instance out again once the
 *      frame's autorelease pool released it; the state struct is rebuilt for the new frame.
 *
 * The counters below make the accounting visible in the serial log: one line per class at exit and
 * on SIGTERM/SIGINT, plus a periodic line while frames are running. They are the soak's evidence:
 * `alloc` must stop growing once the pool is warm, `base_dealloc` covers the queue (or
 * `skipped` says why not), and `reused` should track the frames.
 * ---------------------------------------------------------------------------------------------- */

#define MW_LIFECYCLE_MAX 16
enum {
    MW_LIFE_ALLOC = 0,
    MW_LIFE_DEALLOC,
    MW_LIFE_BASE_DEALLOC,
    MW_LIFE_BASE_SKIPPED,
    MW_LIFE_REUSED,
    MW_LIFE_SAFE_RELEASE,
    MW_LIFE_COUNT
};
struct mw_lifecycle_entry {
    char name[48];
    uint64_t counts[MW_LIFE_COUNT];
};
static struct mw_lifecycle_entry gLifecycle[MW_LIFECYCLE_MAX];
static int gLifecycleCount;
static pthread_mutex_t gLifecycleLock = PTHREAD_MUTEX_INITIALIZER;
static int gLifecycleInstalled;

static struct mw_lifecycle_entry *mw_lifecycle_entry(Class cls)
{
    const char *name = cls != Nil ? class_getName(cls) : "?";
    struct mw_lifecycle_entry *entry = NULL;

    pthread_mutex_lock(&gLifecycleLock);
    for (int i = 0; i < gLifecycleCount; i++) {
        if (strcmp(gLifecycle[i].name, name) == 0) {
            entry = &gLifecycle[i];
            break;
        }
    }
    if (entry == NULL && gLifecycleCount < MW_LIFECYCLE_MAX) {
        entry = &gLifecycle[gLifecycleCount++];
        snprintf(entry->name, sizeof(entry->name), "%s", name);
    }
    pthread_mutex_unlock(&gLifecycleLock);
    return entry;
}

static void mw_lifecycle_note(Class cls, int kind)
{
    struct mw_lifecycle_entry *entry;

    if (cls == Nil || kind < 0 || kind >= MW_LIFE_COUNT) {
        return;
    }
    entry = mw_lifecycle_entry(cls);
    pthread_mutex_lock(&gLifecycleLock);
    if (entry != NULL) {
        entry->counts[kind]++;
    }
    pthread_mutex_unlock(&gLifecycleLock);
}

static void mw_lifecycle_dump(const char *why)
{
    uint64_t allocs = 0, deallocs = 0, taker = 0, skipped = 0, reused = 0, safe = 0;

    pthread_mutex_lock(&gLifecycleLock);
    mw_log("lifecycle (%s): %d class(es) tracked", why != NULL ? why : "?", gLifecycleCount);
    for (int i = 0; i < gLifecycleCount; i++) {
        struct mw_lifecycle_entry *entry = &gLifecycle[i];

        mw_log("lifecycle: %-32s alloc=%llu dealloc=%llu base_dealloc=%llu base_skipped=%llu "
               "reused=%llu safe_release=%llu", entry->name,
               (unsigned long long)entry->counts[MW_LIFE_ALLOC],
               (unsigned long long)entry->counts[MW_LIFE_DEALLOC],
               (unsigned long long)entry->counts[MW_LIFE_BASE_DEALLOC],
               (unsigned long long)entry->counts[MW_LIFE_BASE_SKIPPED],
               (unsigned long long)entry->counts[MW_LIFE_REUSED],
               (unsigned long long)entry->counts[MW_LIFE_SAFE_RELEASE]);
        allocs += entry->counts[MW_LIFE_ALLOC];
        deallocs += entry->counts[MW_LIFE_DEALLOC];
        taker += entry->counts[MW_LIFE_BASE_DEALLOC];
        skipped += entry->counts[MW_LIFE_BASE_SKIPPED];
        reused += entry->counts[MW_LIFE_REUSED];
        safe += entry->counts[MW_LIFE_SAFE_RELEASE];
    }
    pthread_mutex_unlock(&gLifecycleLock);
    mw_log("lifecycle: totals alloc=%llu dealloc=%llu base_dealloc=%llu base_skipped=%llu "
           "reused=%llu safe_release=%llu", (unsigned long long)allocs,
           (unsigned long long)deallocs, (unsigned long long)taker, (unsigned long long)skipped,
           (unsigned long long)reused, (unsigned long long)safe);
}

static void mw_lifecycle_signal(int signal_number)
{
    mw_lifecycle_dump("signal");
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void mw_lifecycle_atexit(void)
{
    mw_lifecycle_dump("exit");
}

static void mw_lifecycle_install(void)
{
    atexit(mw_lifecycle_atexit);
    signal(SIGTERM, mw_lifecycle_signal);
    signal(SIGINT, mw_lifecycle_signal);
}

/* The instance pools for the per-frame objects. An entry is handed out again only when the pool is
 * its last owner (`-retainCount == 1`), so an object CoreAnimation still holds is never rewritten;
 * in steady state the alloc counts stop at the pool size and `reused` grows with the frames. */
#define MW_POOL_OBJECTS 16
struct mw_object_pool {
    id objects[MW_POOL_OBJECTS];
    uint64_t sizes[MW_POOL_OBJECTS]; /* the buffer pool matches on length; others ignore it */
    int count;
    int capacity;
};
static struct mw_object_pool gCommandBufferPool = {.capacity = 4};
static struct mw_object_pool gEncoderPool = {.capacity = MW_POOL_OBJECTS};
static struct mw_object_pool gBufferPool = {.capacity = 8};

static id mw_pool_take_free(struct mw_object_pool *pool, Class cls)
{
    for (int i = 0; i < pool->count; i++) {
        id object = pool->objects[i];

        /* The encoder pool carries two classes: only an instance of the class being asked for may
         * come back, and only when the pool is its last owner. */
        if (object != nil && object_getClass(object) == cls && [object retainCount] == 1) {
            return object;
        }
    }
    return nil;
}

static void mw_pool_remember(struct mw_object_pool *pool, id object)
{
    if (object != nil && pool->count < pool->capacity && pool->count < MW_POOL_OBJECTS) {
        pool->objects[pool->count] = [object retain];
        pool->sizes[pool->count] = 0;
        pool->count++;
    }
}

/* Buffers are matched on their length: a buffer CoreAnimation asks for this frame is only served
 * from the pool when the pooled one has exactly that length (its Metal buffer object already lives
 * on the host with that id, so the frame's upload refreshes it instead of creating another). */
static struct mw_buffer_state *mw_buffer_state_of(id object);

static id mw_pool_take_free_buffer(struct mw_object_pool *pool, Class cls, uint64_t length)
{
    for (int i = 0; i < pool->count; i++) {
        id object = pool->objects[i];

        /* A buffer is reusable when the frame that last used it has been acknowledged
         * (`available`), or -- for a buffer CoreAnimation did release -- when the pool is its last
         * owner. CoreAnimation keeps the per-frame 256 KiB buffers (boot wp4-soak-fix2: 750
         * newBufferWithLength:262144 calls, one per frame, never released), so the retainCount test
         * alone never matched and the pool never hit. */
        if (object != nil && object_getClass(object) == cls && pool->sizes[i] == length) {
            struct mw_buffer_state *state = mw_buffer_state_of(object);

            if ((state != NULL && state->available) || [object retainCount] == 1) {
                return object;
            }
        }
    }
    return nil;
}

/* The buffers handed out in the current frame. CoreAnimation creates them before it commits, and
 * they may be handed out again only once that frame is on the wire and acknowledged; the list is
 * small (one 256 KiB buffer per frame plus a few 272-byte ones at start-up). */
#define MW_FRAME_BUFFERS 64
static id gFrameBuffers[MW_FRAME_BUFFERS];
static int gFrameBufferCount;
static int gBufferLog = -1; /* MW_BUFFER_LOG=1: rate-limited pool hit/miss lines */
static uint64_t gBufferHits;
static uint64_t gBufferMisses;

static int mw_buffer_log_enabled(void)
{
    if (gBufferLog < 0) {
        const char *env = getenv("MW_BUFFER_LOG");

        gBufferLog = env != NULL && env[0] != '\0' && atoi(env) != 0;
    }
    return gBufferLog;
}

static void mw_frame_buffer_track(id buffer)
{
    if (buffer != nil && gFrameBufferCount < MW_FRAME_BUFFERS) {
        gFrameBuffers[gFrameBufferCount++] = buffer;
    }
}

static void mw_frame_buffers_mark_available(void)
{
    for (int i = 0; i < gFrameBufferCount; i++) {
        struct mw_buffer_state *state = mw_buffer_state_of(gFrameBuffers[i]);

        if (state != NULL) {
            state->available = 1;
        }
        gFrameBuffers[i] = nil;
    }
    gFrameBufferCount = 0;
}

static void mw_pool_remember_buffer(struct mw_object_pool *pool, id object, uint64_t length)
{
    if (object != nil && pool->count < pool->capacity && pool->count < MW_POOL_OBJECTS) {
        pool->objects[pool->count] = [object retain];
        pool->sizes[pool->count] = length;
        pool->count++;
    }
}

static Class gDeviceBase;             /* Metal's private _MTLDevice */
static Class gQueueClass;             /* HakiosMetalQueue, a _MTLCommandQueue */
static Class gCommandBufferClass;     /* HakiosMetalCommandBuffer, a _MTLCommandBuffer */
static Class gBlitEncoderClass;       /* HakiosMetalBlitEncoder, a _MTLCommandEncoder */
static Class gBufferClass;            /* HakiosMetalBuffer, a _MTLResource */
static Class gTextureClass;           /* HakiosMetalTexture, a _MTLResource or NSObject */
static Class gLibraryClass;           /* HakiosMetalLibrary, a _MTLLibrary or NSObject */
static Class gFunctionClass;          /* HakiosMetalFunction, a _MTLFunction or NSObject */
static Class gPipelineStateClass;     /* HakiosMetalRenderPipelineState, a _MTLRenderPipelineState */
static Class gDepthStencilStateClass; /* HakiosMetalDepthStencilState, a _MTLDepthStencilState */
static Class gSamplerStateClass;      /* HakiosMetalSamplerState, a _MTLSamplerState */
static Class gPipelineLibraryClass;   /* HakiosMetalPipelineLibrary, a _MTLPipelineLibrary */
static Class gRenderEncoderClass;     /* HakiosMetalRenderCommandEncoder, a _MTLRenderCommandEncoder */
static id gDevice;                    /* the one device, once made */
static unsigned int gAcceleratorPort; /* the IOAcceleratorES service it was made for */

#define MW_MAX_PROTOCOLS 96
static Protocol *gProtocols[MW_MAX_PROTOCOLS];
static unsigned gProtocolCount;

/* /redwood/etc/metal-census: when the image carries that file, every method our classes inherit
 * from Metal's private base classes is replaced by a forwarding trampoline, so the first inherited
 * method CoreAnimation calls is named in the serial log instead of dying inside Apple's code. */
static int gCensusEnabled;

/* WP-4 experiment switch, parsed from the kernel's boot-args (byname=nil): the by-name pipeline
 * factory answers nil, so CoreAnimation's runtime-specialisation path is exercised instead. Off
 * unless the boot says so; the delivered/experiment default is the placeholder state. */
static int gBynameFail;

/* wire state */
static struct mw_conn gWire;
static int gWireReady;            /* mw_conn_init has run */
static int gLastFd = -1;          /* the socket the object-generation below belongs to */
static uint32_t gGeneration = 1;  /* a new connection: host has no objects, resend them */
static uint64_t gHelloGeneration; /* the connection generation HELLO was sent on */
static uint32_t gNextObjectId = 1;
static uint64_t gFrameSeq;
static id gAwaitingCommandBuffer; /* retained: its FRAME_END has no ack read yet */
static pthread_mutex_t gWireLock = PTHREAD_MUTEX_INITIALIZER;

/* the numbers of the summary line */
static struct {
    uint64_t frames;
    uint64_t ok;
    uint64_t failed;
    uint64_t crc_mismatches;
    uint64_t bytes;      /* picture bytes that went down the socket (fallback uploads) */
    uint64_t phys_bytes; /* picture bytes the host read out of guest RAM by reference */
    uint64_t phys_frames;
    uint64_t encoded_sum, encoded_min, encoded_max;
    uint64_t wait_sum, wait_min, wait_max;
} gStats = {
    .encoded_min = UINT64_MAX,
    .wait_min = UINT64_MAX,
};

/* -------------------------------------------------------------------------------------------------
 * Object state. Every instance of a run-time class made below has one ivar, _mw, pointing at the
 * C struct for it. Nothing is kept in Metal's own base classes.
 * ---------------------------------------------------------------------------------------------- */

enum {
    MW_OP_COPY_BUFFER_TO_TEXTURE = 1,
    MW_OP_PRESENT = 2,
};

struct mw_op {
    uint16_t kind;
    id buffer; /* not retained: Metal requires the resource to outlive the frame */
    id texture;
    uint64_t buffer_offset;
    uint32_t bytes_per_row;
    uint32_t bytes_per_image;
    uint32_t width, height, depth;
    uint32_t origin_x, origin_y, origin_z;
    uint32_t slice, level;
};


struct mw_texture_state {
    uint32_t remote_id;
    uint32_t created_generation;
    uint32_t width, height, depth;
    uint32_t array_length, mipmaps, samples, texture_type;
    uint32_t pixel_format;
    unsigned long usage;
    unsigned long storage_mode;
    id label;
    /* WP-3: an IOSurface-backed texture carries the real surface, not just its geometry. The
     * surface object is CoreAnimation's (not retained here); the base is the address the port's
     * IOSurface implementation mapped for this process, and it is what hakios_vtop resolves. */
    void *surface;
    uint32_t surface_id;
    uintptr_t surface_base;
    uint64_t surface_length;
    uint32_t surface_bytes_per_row;
    int has_surface;
    uint32_t surface_generation; /* connection whose wire has the binding, 0 for none */
    /* WP-3: the last -replaceRegion:... content, kept until the wire is online and the texture
     * is first named in a frame. CA uploads a layer image once and samples it; the host only
     * needs the newest content. */
    void *replace_bytes;
    uint32_t replace_length;
    uint32_t replace_bytes_per_row;
    uint32_t replace_bytes_per_image;
    uint32_t replace_mipmap_level;
    uint32_t replace_slice;
    uint32_t replace_origin[3];
    uint32_t replace_width;
    uint32_t replace_height;
    uint32_t replace_depth;
    uint64_t replace_serial;      /* bumped on every replaceRegion: */
    uint64_t replace_sent_serial; /* the serial the wire last carried */
};

/* One recorded render-encoder call, replayed on the wire in order (stage C). The object fields are
 * retained so a frame's Metal objects outlive the encoder that bound them; the bytes field is a
 * malloc'd copy for setVertexBytes:/setFragmentBytes:. stage is MW_STAGE_VERTEX/FRAGMENT for the
 * two-stage calls and 0 otherwise. */
enum {
    MWR_OP_BEGIN = 1,
    MWR_OP_SET_PIPELINE,
    MWR_OP_SET_DEPTH_STENCIL,
    MWR_OP_SET_VERTEX_BUFFER,
    MWR_OP_SET_VERTEX_BYTES,
    MWR_OP_SET_VERTEX_TEXTURE,
    MWR_OP_SET_VERTEX_SAMPLER,
    MWR_OP_SET_FRAGMENT_BUFFER,
    MWR_OP_SET_FRAGMENT_BYTES,
    MWR_OP_SET_FRAGMENT_TEXTURE,
    MWR_OP_SET_FRAGMENT_SAMPLER,
    MWR_OP_SET_VIEWPORT,
    MWR_OP_SET_SCISSOR,
    MWR_OP_SET_CULL_MODE,
    MWR_OP_SET_WINDING,
    MWR_OP_SET_FILL_MODE,
    MWR_OP_SET_BLEND_COLOR,
    MWR_OP_SET_STENCIL_REFERENCE,
    MWR_OP_SET_VERTEX_BUFFER_OFFSET,
    MWR_OP_SET_FRAGMENT_BUFFER_OFFSET,
    MWR_OP_DRAW,
    MWR_OP_END,
};

struct mw_render_op {
    uint16_t kind;
    uint16_t stage;
    uint32_t index;
    id object;  /* retained: buffer, texture, pipeline, sampler, depth-stencil */
    id object2; /* retained: the draw's index buffer */
    uint64_t value64;
    int64_t signed64;
    uint32_t values[6];
    float fvalues[6];
    void *bytes; /* malloc'd copy, for MWR_OP_SET_*_BYTES */
    uint32_t length;
    struct mw_render_draw draw; /* MWR_OP_DRAW */
};

#define MW_MAX_RENDER_ENCODERS 8
#define MW_RENDER_MAX_OPS 256

struct mw_command_buffer_state {
    int committed;
    int sent;
    int awaiting_ack;
    uint32_t status; /* MTLCommandBufferStatus */
    uint64_t seq;
    uint64_t t_begin_ns;
    uint64_t t_commit_ns;
    uint64_t encoded_ns;
    id error; /* NSError, retained */
    id queue; /* retained */
    id label;
    id handlers; /* NSMutableArray of copied completed handlers, retained */
    int op_count;
    struct mw_op ops[8];
    const char *local_error; /* a static string, or NULL */
    /* CoreAnimation's render path: each ended render encoder is kept here (retained) until the
     * command buffer commits, then its recorded calls are serialized as protocol-2 render
     * messages. The synthetic blit client never fills this array. */
    int render_encoder_count;
    int render_encoder_stored;
    id render_encoders[MW_MAX_RENDER_ENCODERS];
    uint64_t render_draw_count;
    int profiling_enabled;
};

struct mw_encoder_state {
    id command_buffer; /* not retained: the encoder dies before its command buffer */
    int ended;
    id label;
    /* CoreAnimation's texture-level blits: counted, never sent (record-only, like the render
     * path; the wire's v1 subset has no texture-to-texture command). */
    uint64_t texture_copy_count;
    uint64_t mipmap_count;
};

/*
 * The placeholder objects of the library/pipeline phase (CoreAnimation's renderer). None of them
 * carries anything Metal could execute: a library knows the name it was asked for and the function
 * names it has been asked about, a function knows its name and the vertex/fragment guess, and the
 * pipeline/depth-stencil/sampler states hold only the label their descriptor had.
 */
/* WP-4: one Metal library as the wire knows it. Both a library (-newLibraryWith*:) and a pipeline
 * library carry one; a render pipeline quotes the two its functions came from. The remote id is
 * assigned the first time the record goes on the wire, so a boot that never reports a pipeline
 * pays nothing for it. kind is enum mw_library_kind from the protocol header. */
struct mw_wire_library {
    uint32_t remote_id;      /* wire id, 0 until first sent */
    uint32_t kind;           /* enum mw_library_kind */
    uint64_t hash;           /* FNV-1a/64 of the bytes, when the guest could read them */
    uint64_t byte_length;    /* the bytes' length, 0 when unknown */
    int hash_known;
    uint32_t sent_generation; /* the connection generation MW_CREATE_LIBRARY went out on */
    NSMutableArray *names;   /* borrowed: the function names asked of this library */
};

struct mw_library_state {
    uint32_t stub_id;
    id install_name;       /* NSString from the URL/file, retained; nil when unknown */
    id label;              /* NSString, retained */
    NSMutableArray *names; /* the function names asked for, retained */
    struct mw_wire_library wire;
};

struct mw_function_state {
    id name;           /* NSString, retained */
    id label;          /* NSString, retained */
    int function_type; /* MTLFunctionType, guessed from the name */
    /* WP-4: the library this function was made from (borrowed; nil for a specialised function,
     * whose provenance is its base function). */
    struct mw_wire_library *library;
};

/* The three empty states (render pipeline, depth-stencil, sampler) plus what the protocol-2 render
 * messages need from them: a render pipeline state carries its name, its vertex/fragment base
 * functions, the color attachment's format and blend state, and the vertex descriptor; a sampler
 * state carries its descriptor; a depth-stencil state carries the depth half. kind: 1 pipeline,
 * 2 depth-stencil, 3 sampler. */
struct mw_stub_state {
    id label; /* NSString, retained; the descriptor's label, if it had one */
    uint32_t kind;
    uint32_t remote_id;
    uint32_t created_generation;
    int captured;
    struct mw_create_render_pipeline pipeline;
    struct mw_vertex_attribute attributes[MW_MAX_RENDER_ATTRIBUTES];
    struct mw_vertex_layout layouts[MW_MAX_RENDER_LAYOUTS];
    struct mw_create_sampler sampler;
    struct mw_create_depth_stencil depth_stencil;
    /* WP-4: the wire libraries behind the two functions of a render pipeline; borrowed. */
    struct mw_wire_library *vertex_library;
    struct mw_wire_library *fragment_library;
};

/* The pipeline library CoreAnimation builds from QuartzCore.framework/default.pipelinelib. It is a
 * placeholder: the file path is logged, the file is never opened, and the pipeline states it hands
 * out by name are this experiment's placeholders. */
struct mw_pipeline_library_state {
    uint32_t stub_id;
    id file_path; /* NSString, retained; nil when unknown */
    id label;     /* NSString, retained */
    int disable_run_time_compilation; /* -setDisableRunTimeCompilation: */
    struct mw_wire_library wire;      /* WP-4: hash of the pipelinelib file */
};

/* The render command encoder: the calls CoreAnimation makes are recorded here, in order, and the
 * command buffer serializes them at commit (protocol 2). BEGIN is kept as the frame's render-pass
 * record rather than in the op array. */
struct mw_render_encoder_state {
    id command_buffer; /* not retained: the encoder dies before its command buffer */
    int ended;
    id label;
    id pipeline;      /* not retained: the client keeps it alive while it binds it */
    id target;        /* retained: the render pass's color attachment 0 texture */
    id depth_target;  /* retained: its depth attachment, when it has one */
    uint64_t set_count;
    uint64_t draw_count;
    uint32_t encoder_index;
    struct mw_render_begin begin;
    int op_count;
    int dropped_count;
    struct mw_render_op ops[MW_RENDER_MAX_OPS];
};

static uint32_t mw_next_object_id(void)
{
    return __atomic_add_fetch(&gNextObjectId, 1, __ATOMIC_RELAXED);
}

/* -------------------------------------------------------------------------------------------------
 * Shared plumbing for the placeholder objects (libraries, functions, pipelines, pipeline
 * libraries, the record-only render encoder). One lock guards the log-once sets below and the
 * function-name lists; the helpers read foreign objects (descriptors, pipeline states) defensively
 * because the arguments come from CoreAnimation, not from this experiment.
 * ---------------------------------------------------------------------------------------------- */

static pthread_mutex_t gStubLock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t gNextStubId;

static uint32_t mw_next_stub_id(void)
{
    return __atomic_add_fetch(&gNextStubId, 1, __ATOMIC_RELAXED);
}

/* WP-4: FNV-1a/64, the protocol's library/function hash. The host never sees the AIR bytes; this
 * and the entry-function names are the provenance it can print for a pipeline it cannot execute. */
#define MW_LIBRARY_HASH_MAX (64u * 1024u * 1024u)

static uint64_t mw_fnv1a64_update(uint64_t hash, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;

    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

static uint64_t mw_fnv1a64(const void *data, size_t length)
{
    return mw_fnv1a64_update(0xcbf29ce484222325ull, data, length);
}

/* Dispatch data may be many regions; hash them without joining them (the AIR container can be a
 * few hundred KiB). Stops at MW_LIBRARY_HASH_MAX and reports hash_known = 0 then. */
static int mw_fnv1a64_dispatch(dispatch_data_t data, uint64_t *hash, uint64_t *length)
{
    __block uint64_t hash_value = 0xcbf29ce484222325ull;
    __block uint64_t total = 0;
    __block int complete = 1;

    if (data == NULL) {
        return 0;
    }
    dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t offset, const void *buffer,
                                    size_t size) {
        (void)region;
        (void)offset;
        if (total + size > MW_LIBRARY_HASH_MAX) {
            complete = 0;
            return false;
        }
        hash_value = mw_fnv1a64_update(hash_value, buffer, size);
        total += size;
        return true;
    });
    *hash = hash_value;
    *length = total;
    return complete;
}

/* A file the guest can read (a library path, or the pipelinelib). Read-only; capped. */
static int mw_fnv1a64_file(const char *path, uint64_t *hash, uint64_t *length)
{
    unsigned char buffer[65536];
    uint64_t hash_value = 0xcbf29ce484222325ull;
    uint64_t total = 0;
    FILE *file;
    size_t got;

    if (path == NULL) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (total + got > MW_LIBRARY_HASH_MAX) {
            fclose(file);
            return 0;
        }
        hash_value = mw_fnv1a64_update(hash_value, buffer, got);
        total += got;
    }
    fclose(file);
    *hash = hash_value;
    *length = total;
    return 1;
}

/* WP-4: the wire record of a library. Sent lazily, once per connection generation, and only for
 * libraries a pipeline actually names. */
static int mw_wire_ensure_library(struct mw_wire_library *library)
{
    struct {
        struct mw_create_library header;
        struct mw_library_function functions[MW_MAX_LIBRARY_FUNCTIONS];
    } message;
    NSArray *names = nil;
    uint32_t count = 0;
    pthread_mutex_t *lock = &gStubLock;

    if (library == NULL || !gWireReady) {
        return 0;
    }
    if (library->remote_id == 0) {
        library->remote_id = mw_next_stub_id();
    }
    if (library->sent_generation == gGeneration) {
        return 0;
    }
    pthread_mutex_lock(lock);
    if (library->names != nil) {
        names = [NSArray arrayWithArray:library->names];
    }
    pthread_mutex_unlock(lock);
    for (id name in names) {
        const char *utf8;

        if (count == MW_MAX_LIBRARY_FUNCTIONS || ![name isKindOfClass:[NSString class]]) {
            continue;
        }
        utf8 = [(NSString *)name UTF8String];
        if (utf8 == NULL) {
            continue;
        }
        memset(&message.functions[count], 0, sizeof(message.functions[count]));
        strlcpy(message.functions[count].name, utf8, MW_NAME_MAX);
        message.functions[count].name_hash = mw_fnv1a64(utf8, strlen(utf8));
        count++;
    }
    memset(&message.header, 0, sizeof(message.header));
    message.header.id = library->remote_id;
    message.header.kind = library->kind;
    message.header.function_count = count;
    message.header.hash = library->hash_known ? library->hash : 0;
    message.header.byte_length = library->hash_known ? library->byte_length : 0;
    if (mw_send(&gWire, MW_CREATE_LIBRARY, &message,
                (uint32_t)(sizeof(message.header) + (size_t)count * sizeof(message.functions[0]))) !=
        0) {
        return -1;
    }
    library->sent_generation = gGeneration;
    mw_log("library %u: %s %llu bytes hash %016llx -> the wire (%u function name(s))",
           library->remote_id,
           library->kind == MW_LIBRARY_PIPELINE
               ? "pipeline library"
               : (library->kind == MW_LIBRARY_DATA ? "data" : "library"),
           (unsigned long long)(library->hash_known ? library->byte_length : 0),
           (unsigned long long)(library->hash_known ? library->hash : 0), count);
    return 0;
}

/* A C string for the log: "(nil)" for nil, and only NSStrings are read (an object whose
 * -description might touch Apple's private state is not). */
static const char *mw_text(id object)
{
    if (object != nil && [object isKindOfClass:[NSString class]]) {
        const char *text = [(NSString *)object UTF8String];

        if (text != NULL) {
            return text;
        }
    }
    return "(nil)";
}

static void mw_clear_error(NSError **error)
{
    if (error != NULL) {
        *error = nil;
    }
}

static NSError *mw_nserror(const char *description)
{
    NSString *text = [[NSString alloc] initWithFormat:@"metalwire: %s", description];
    NSError *error = [NSError errorWithDomain:@"com.iosvm.metalwire"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey : text}];

    [text release];
    return error;
}

/* The descriptor's label, read defensively: a foreign descriptor may raise where a real Metal
 * descriptor would not, and a label is only wanted for the log. */
static id mw_safe_label(id object)
{
    if (object == nil) {
        return nil;
    }
    @try {
        if ([object respondsToSelector:@selector(label)]) {
            return ((id(*)(id, SEL))objc_msgSend)(object, @selector(label));
        }
    } @catch (NSException *exception) {
        mw_log("could not read a descriptor's label (%s)", mw_text([exception reason]));
    }
    return nil;
}

/* The function's name, same defensive read: the target is CoreAnimation's argument, not ours. */
static id mw_safe_function_name(id function)
{
    if (function == nil) {
        return nil;
    }
    @try {
        if ([function respondsToSelector:@selector(name)]) {
            return ((id(*)(id, SEL))objc_msgSend)(function, @selector(name));
        }
    } @catch (NSException *exception) {
        mw_log("could not read a pipeline function's name (%s)", mw_text([exception reason]));
    }
    return nil;
}

/* -------------------------------------------------------------------------------------------------
 * Run-time classes.
 *
 * A class is made on Metal's private base where this system has it (and on NSObject where it does
 * not, with a line in the log). Method signatures for messages the class does not implement are
 * read from the protocols it declares, and forwardInvocation: answers zero; that is what keeps a
 * real Metal runtime from ending the process over a selector this experiment never needed.
 * ---------------------------------------------------------------------------------------------- */

static void **mw_slot(id object, Class cls)
{
    Ivar ivar = class_getInstanceVariable(cls, "_mw");

    return ivar != NULL ? (void **)((char *)(void *)object + ivar_getOffset(ivar)) : NULL;
}

static id mw_object_description(id self, SEL _cmd);
static void mw_free_buffer(struct mw_buffer_state *state);
static void mw_free_texture(struct mw_texture_state *state);
static void mw_free_command_buffer(struct mw_command_buffer_state *state);
static void mw_free_encoder(struct mw_encoder_state *state);
static void mw_free_library(struct mw_library_state *state);
static void mw_free_function(struct mw_function_state *state);
static void mw_free_stub(struct mw_stub_state *state);
static void mw_free_pipeline_library(struct mw_pipeline_library_state *state);
static struct mw_stub_state *mw_stub_state_of_object(id object);
static int mw_send_render_frame(struct mw_command_buffer_state *state);
static void *mw_iosurface_base(void *surface);
static uint64_t mw_iosurface_alloc_size(void *surface);
static uint32_t mw_iosurface_bytes_per_row(void *surface);
static void mw_render_capture_pipeline(struct mw_stub_state *state, id descriptor);
static void mw_render_capture_sampler(struct mw_stub_state *state, id descriptor);
static void mw_render_capture_depth_stencil(struct mw_stub_state *state, id descriptor);
static void mw_free_render_encoder(struct mw_render_encoder_state *state);

/*
 * 4.1: -[_MTLCommandQueue dealloc] releases the dispatch group at ivar 0xb0 (`_submittedGroup`)
 * without a nil check (docs/research/ca-metallib-url.md §10.1: `ldr x0,[x19,#0xb0]; bl
 * _dispatch_release`, everything around it cbz-guarded). A plain -init never creates that group, so
 * the base dealloc is only safe after one is put there. This creates it when the slot is empty; the
 * base dealloc's release then balances this create and the object's own allocation is freed.
 * Classes without such a known-safe repair keep the old rule (free the instance, skip the base).
 */
/*
 * `/redwood/etc/metal-base-dealloc` (same marker pattern as the inject shims) now selects the
 * *safe release* of the leak, not the inherited -dealloc chain. Boot wp4-soak-fix (t30, 1500 s)
 * ran the chain once and the guest stopped rendering entirely -- 0 frames, the guest clock 2.8x
 * behind the wall clock, no panic -- so that path is recorded as unusable on a stub and is no
 * longer reachable from here.
 *
 * What the marker does enable is the part the analysis actually pins down: -[_MTLCommandQueue
 * dealloc] releases the dispatch group at ivar 0xb0 (`_submittedGroup`) without a nil check
 * (docs/research/ca-metallib-url.md §10.1). A stub whose ivar is nil cannot survive that release,
 * so instead of calling the chain this creates a group, releases it in the same breath and clears
 * the slot (balanced: +1 create, -1 release), and logs the pre/post state. The instance memory is
 * then freed with object_dispose() as before; the inherited chain is never called and Metal's own
 * allocation for the object still leaks (per process, kilobytes). The default boot is unchanged.
 */
static int mw_base_dealloc_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = access("/redwood/etc/metal-base-dealloc", F_OK) == 0;
        mw_log("lifecycle: queue safe-release probe %s (/redwood/etc/metal-base-dealloc); the "
               "inherited -dealloc chain is never called", enabled ? "enabled" : "disabled");
    }
    return enabled;
}

static int mw_base_safe_release(id self, Class base)
{
    Ivar ivar;
    void **slot;

    if (base == Nil || strcmp(class_getName(base), "_MTLCommandQueue") != 0) {
        return 0;
    }
    ivar = class_getInstanceVariable(base, "_submittedGroup");
    if (ivar == NULL) {
        mw_log("queue safe release: this _MTLCommandQueue has no _submittedGroup ivar; nothing to "
               "do");
        return 0;
    }
    slot = (void **)((char *)self + ivar_getOffset(ivar));
    if (*slot != NULL) {
        mw_log("queue safe release: _submittedGroup already %p; leaving it alone", *slot);
        return 1;
    }
    {
        dispatch_group_t group = dispatch_group_create();

        mw_log("queue safe release: pre: _submittedGroup is NULL, ivar offset %td, instance %p",
               ivar_getOffset(ivar), self);
        if (group == NULL) {
            mw_log("queue safe release: dispatch_group_create returned NULL; nothing released");
            return 0;
        }
        *slot = group;
        dispatch_release(group);
        *slot = NULL;
        mw_log("queue safe release: post: created %p and released it; the slot is NULL again "
               "(create/release balanced, inherited -dealloc not called)", group);
    }
    return 1;
}

static void mw_object_dealloc(id self, SEL _cmd)
{
    Class cls = object_getClass(self);
    Class base = class_getSuperclass(cls);
    void **slot = mw_slot(self, cls);

    if (slot != NULL && *slot != NULL) {
        if (cls == gBufferClass) {
            mw_free_buffer(*slot);
        } else if (cls == gTextureClass) {
            mw_free_texture(*slot);
        } else if (cls == gCommandBufferClass) {
            mw_free_command_buffer(*slot);
        } else if (cls == gBlitEncoderClass) {
            mw_free_encoder(*slot);
        } else if (cls == gLibraryClass) {
            mw_free_library(*slot);
        } else if (cls == gFunctionClass) {
            mw_free_function(*slot);
        } else if (cls == gPipelineStateClass || cls == gDepthStencilStateClass ||
                   cls == gSamplerStateClass) {
            mw_free_stub(*slot);
        } else if (cls == gPipelineLibraryClass) {
            mw_free_pipeline_library(*slot);
        } else if (cls == gRenderEncoderClass) {
            mw_free_render_encoder(*slot);
        }
        *slot = NULL;
    }
    mw_lifecycle_note(cls, MW_LIFE_DEALLOC);
    if (base != Nil && strncmp(class_getName(base), "_MTL", 4) == 0) {
        if (mw_base_dealloc_enabled() && mw_base_safe_release(self, base)) {
            /* The safe part of the queue's leak ran (the dispatch group exists and is balanced).
             * The inherited chain is proven unusable on these stubs (t30) and is never called. */
            mw_lifecycle_note(cls, MW_LIFE_SAFE_RELEASE);
            mw_lifecycle_note(cls, MW_LIFE_BASE_SKIPPED);
            object_dispose(self);
            return;
        }
        /* Apple's private -dealloc assumes the concrete class behind the base initialized the
         * base's internal dispatch objects; these stubs are made with a plain -init, so that
         * dealloc is the null-libdispatch fault ca-metal3-shim1 died in (x1 = @selector(dealloc),
         * pc = libdispatch, x0 = NULL). Free the instance ourselves and never call it; the rule
         * and the leak are logged once at registration (mw_make_class). */
        mw_lifecycle_note(cls, MW_LIFE_BASE_SKIPPED);
        object_dispose(self);
        return;
    }
    {
        struct objc_super super = {self, base};

        ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&super, _cmd);
    }
}

static NSMethodSignature *mw_signature_from_protocols(SEL selector)
{
    for (unsigned i = 0; i < gProtocolCount; i++) {
        for (int required = 1; required >= 0; required--) {
            struct objc_method_description description =
                protocol_getMethodDescription(gProtocols[i], selector, required, YES);

            if (description.name != NULL && description.types != NULL) {
                return [NSMethodSignature signatureWithObjCTypes:description.types];
            }
        }
    }
    return nil;
}

/* The last resort of a message no protocol of ours describes: any protocol in the process that
 * does describe it, so a private Metal selector still gets a signature (and then a logged zero)
 * instead of "unrecognized selector". */
static NSMethodSignature *mw_signature_from_any_protocol(SEL selector)
{
    unsigned count = 0;
    Protocol *__unsafe_unretained *all = objc_copyProtocolList(&count);
    NSMethodSignature *signature = nil;

    for (unsigned i = 0; i < count && signature == nil; i++) {
        for (int required = 1; required >= 0; required--) {
            struct objc_method_description description =
                protocol_getMethodDescription(all[i], selector, required, YES);

            if (description.name != NULL && description.types != NULL) {
                signature = [NSMethodSignature signatureWithObjCTypes:description.types];
                break;
            }
        }
    }
    free(all);
    return signature;
}

static id mw_method_signature(id self, SEL _cmd, SEL selector)
{
    NSMethodSignature *signature = mw_signature_from_protocols(selector);
    struct objc_super super;
    id super_signature;

    if (signature != nil) {
        return signature;
    }
    signature = mw_signature_from_any_protocol(selector);
    if (signature != nil) {
        static NSMutableSet *undeclared;
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        NSString *name = NSStringFromSelector(selector);

        pthread_mutex_lock(&lock);
        if (undeclared == nil) {
            undeclared = [[NSMutableSet alloc] init];
        }
        if (![undeclared containsObject:name]) {
            [undeclared addObject:name];
            mw_log("answered -%s from a protocol this class does not declare (zero)",
                   sel_getName(selector));
        }
        pthread_mutex_unlock(&lock);
        return signature;
    }
    super = (struct objc_super){self, class_getSuperclass(object_getClass(self))};
    super_signature =
        ((id(*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&super, _cmd, selector);
    if (super_signature == nil) {
        mw_log("no method signature anywhere for -%s: a send of it will raise",
               sel_getName(selector));
    }
    return super_signature;
}

static void mw_forward_invocation(id self, SEL _cmd, NSInvocation *invocation)
{
    static NSMutableSet *seen;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    SEL selector = [invocation selector];
    NSMethodSignature *signature = [invocation methodSignature];
    NSUInteger length = [signature methodReturnLength];
    const char *return_type = [signature methodReturnType];
    NSString *name = NSStringFromSelector(selector);

    (void)_cmd;
    pthread_mutex_lock(&lock);
    if (seen == nil) {
        seen = [[NSMutableSet alloc] init];
    }
    if (gCensusEnabled) {
        /* census mode: every inherited Apple-base method this plug-in does not implement itself is
         * installed as a forwarding trampoline, and its first call says which class/selector it
         * was. Keys are class+selector so a selector trapped on two classes is logged twice. */
        NSString *key = [[NSString alloc] initWithFormat:@"%s/%s",
                                                         class_getName(object_getClass(self)),
                                                         sel_getName(selector)];

        if (![seen containsObject:key]) {
            Class cls = object_getClass(self);
            Class declarer = Nil;

            [seen addObject:key];
            for (Class candidate = class_getSuperclass(cls);
                 candidate != Nil && candidate != objc_getClass("NSObject");
                 candidate = class_getSuperclass(candidate)) {
                if (class_getInstanceMethod(candidate, selector) != NULL) {
                    declarer = candidate;
                    break;
                }
            }
            mw_log("census: -[%s %s] (inherited from %s; answered zero)", class_getName(cls),
                   sel_getName(selector),
                   declarer != Nil ? class_getName(declarer) : "an Apple base");
        }
        [key release];
    } else if (![seen containsObject:name]) {
        [seen addObject:name];
        mw_log("asked and not implemented: -%s (returns %lu bytes, type %s; answered with zero)",
               sel_getName(selector), (unsigned long)length,
               return_type != NULL ? return_type : "?");
    }
    pthread_mutex_unlock(&lock);
    if (length != 0) {
        void *zero = calloc(1, length);

        if (zero != NULL) {
            [invocation setReturnValue:zero];
            free(zero);
        }
    }
}

/* A public protocol is preferred as the running Metal.framework's own protocol object, looked up
 * by name; when that lookup fails, the compile-time reference the caller passed is used. Darwin's
 * optimized shared cache keeps some protocols *unrealized*, and objc_getProtocol returns NULL for
 * one until some loaded code references it: the first guest boot found MTLDevice, MTLCommandBuffer,
 * MTLTexture and MTLBuffer by name, but not MTLCommandQueue or MTLBlitCommandEncoder. Passing
 * @protocol(...) makes the reference exist, which is how the HakiOS plug-in declared its
 * protocols. [The framework's object is tried first because the first boot's working device was
 * registered with it.] */
static void mw_add_public_protocol(Class cls, Protocol *compiled, const char *name)
{
    Protocol *protocol = objc_getProtocol(name);

    if (protocol == NULL) {
        protocol = compiled;
    }
    if (protocol == NULL) {
        mw_log("the protocol %s is not available (neither in this process's Metal nor compiled in)",
               name);
        return;
    }
    class_addProtocol(cls, protocol);
    for (unsigned i = 0; i < gProtocolCount; i++) {
        if (gProtocols[i] == protocol) {
            return;
        }
    }
    if (gProtocolCount < MW_MAX_PROTOCOLS) {
        gProtocols[gProtocolCount++] = protocol;
    }
}

static void mw_add_private_protocol(Class cls, const char *name)
{
    Protocol *protocol = objc_getProtocol(name);

    if (protocol == NULL) {
        return; /* not on this system: normal for a private protocol */
    }
    mw_add_public_protocol(cls, protocol, name);
}

/* Declares `prefix` and every protocol in the process whose name begins with it — Metal's private
 * SPI protocols share the public name as a prefix (MTLCommandQueueSPI and MTLDeviceSPI are the two
 * the HakiOS runs found), and there is no other way to find them from the SDK. The names found go
 * to the log. */
static void mw_add_protocol_family(Class cls, const char *prefix)
{
    unsigned count = 0;
    Protocol *__unsafe_unretained *all = objc_copyProtocolList(&count);
    size_t length = strlen(prefix);

    for (unsigned i = 0; i < count; i++) {
        const char *name = protocol_getName(all[i]);

        if (name != NULL && strncmp(name, prefix, length) == 0) {
            class_addProtocol(cls, all[i]);
            int known = 0;

            for (unsigned j = 0; j < gProtocolCount; j++) {
                if (gProtocols[j] == all[i]) {
                    known = 1;
                    break;
                }
            }
            if (!known && gProtocolCount < MW_MAX_PROTOCOLS) {
                gProtocols[gProtocolCount++] = all[i];
            }
            mw_log("declares the protocol %s", name);
        }
    }
    free(all);
}

/* A class with the generic stub machinery and, when slot is not 0, the _mw ivar. It is not
 * registered yet: the caller adds its own methods and then calls objc_registerClassPair. */
static Class mw_make_class(const char *name, const char *base_name, int slot)
{
    Class base = base_name != NULL ? objc_getClass(base_name) : Nil;
    Class cls;

    if (base == Nil) {
        mw_log("%s: this system has no %s; NSObject is used", name,
               base_name != NULL ? base_name : "NSObject");
        base = objc_getClass("NSObject");
    }
    cls = objc_allocateClassPair(base, name, 0);
    if (cls == Nil) {
        mw_log("could not make the class %s", name);
        return Nil;
    }
    if (base != Nil && strncmp(class_getName(base), "_MTL", 4) == 0) {
        /* The dealloc rule, said once at registration: Apple's private -dealloc dereferences
         * dispatch state a plain -init never creates (the ca-metal3-shim1 fault), so
         * mw_object_dealloc frees the instance itself and never calls the base's dealloc. The
         * base allocation is leaked: a deliberate limitation of the stub. */
        mw_log("%s: its %s -dealloc is skipped and the base allocation is leaked (deliberate: the "
               "private dealloc dereferences state a plain -init never sets up)",
               name, class_getName(base));
    }
    if (slot != 0 &&
        !class_addIvar(cls, "_mw", sizeof(void *), sizeof(void *) == 8 ? 3 : 2, "^v")) {
        mw_log("%s: the _mw ivar could not be added", name);
    }
    class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)mw_method_signature,
                    "@@::");
    class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)mw_forward_invocation,
                    "v@:@");
    class_addMethod(cls, sel_registerName("dealloc"), (IMP)mw_object_dealloc, "v@:");
    /* Apple's own description/debugDescription may read internal state this stub never set up */
    class_addMethod(cls, sel_registerName("description"), (IMP)mw_object_description, "@@:");
    return cls;
}

static id mw_object_description(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return @"<metalwire object>";
}

static id mw_alloc_init(Class cls, const char *what)
{
    id object;
    id initialized;

    if (cls == Nil) {
        return nil;
    }
    object = ((id(*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc"));
    if (object != nil) {
        mw_lifecycle_note(cls, MW_LIFE_ALLOC);
    }
    if (object == nil) {
        mw_log("%s: +alloc gave nil", what);
        return nil;
    }
    initialized = ((id(*)(id, SEL))objc_msgSend)(object, sel_registerName("init"));
    if (initialized == nil) {
        /* Metal's -init gave nil (unlikely, and it may have released the allocation): a fresh
         * allocation without -init still has our ivar and is safe for our methods. */
        mw_log("%s: the base's -init gave nil; a fresh, un-inited object is used", what);
        return ((id(*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc"));
    }
    return initialized;
}

static struct mw_buffer_state *mw_buffer_state_of(id object)
{
    void **slot;

    if (object == nil || gBufferClass == Nil || object_getClass(object) != gBufferClass) {
        return NULL;
    }
    slot = mw_slot(object, gBufferClass);
    return slot != NULL ? *slot : NULL;
}

static struct mw_texture_state *mw_texture_state_of(id object)
{
    void **slot;

    if (object == nil || gTextureClass == Nil || object_getClass(object) != gTextureClass) {
        return NULL;
    }
    slot = mw_slot(object, gTextureClass);
    return slot != NULL ? *slot : NULL;
}

static struct mw_command_buffer_state *mw_command_buffer_state_of(id object)
{
    void **slot;

    if (object == nil || gCommandBufferClass == Nil ||
        object_getClass(object) != gCommandBufferClass) {
        return NULL;
    }
    slot = mw_slot(object, gCommandBufferClass);
    return slot != NULL ? *slot : NULL;
}

static struct mw_encoder_state *mw_encoder_state_of(id object)
{
    void **slot;

    if (object == nil || gBlitEncoderClass == Nil || object_getClass(object) != gBlitEncoderClass) {
        return NULL;
    }
    slot = mw_slot(object, gBlitEncoderClass);
    return slot != NULL ? *slot : NULL;
}

/* -------------------------------------------------------------------------------------------------
 * The drawable: the one object a client gets without asking Metal for it (see mw_metal_extras.h).
 * It is a plain Objective-C class, because it needs no part of Metal's private class tree: the
 * command buffer's presentDrawable: is ours and only reads the texture out of it.
 * ---------------------------------------------------------------------------------------------- */

@interface MWDrawable : NSObject <MWRemoteDrawable>
@property(nonatomic, readonly, retain) id<MTLTexture> texture;
- (instancetype)initWithTexture:(id<MTLTexture>)texture;
@end

static NSUInteger gDrawableIds;

@implementation MWDrawable {
    id<MTLTexture> _texture;
    NSUInteger _drawableID;
}

- (instancetype)initWithTexture:(id<MTLTexture>)texture
{
    self = [super init];
    if (self != nil) {
        _texture = [(id)texture retain];
        _drawableID = ++gDrawableIds;
    }
    return self;
}

- (void)dealloc
{
    [_texture release];
    [super dealloc];
}

- (id<MTLTexture>)texture
{
    return _texture;
}

- (NSUInteger)drawableID
{
    return _drawableID;
}

- (CFTimeInterval)presentedTime
{
    return 0;
}

- (void)present
{
    /* presentDrawable: is the command buffer's; this is the drawable's own, unused in v1 */
}

- (void)presentAtTime:(CFTimeInterval)presentation_time
{
    (void)presentation_time;
}

- (void)presentAfterMinimumDuration:(CFTimeInterval)duration
{
    (void)duration;
}

- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block
{
    (void)block;
}

@end

/* -------------------------------------------------------------------------------------------------
 * The wire, one frame at a time. All of it runs under gWireLock; the ack for a frame is read by
 * the command buffer's -waitUntilCompleted (or, if the client commits another frame first, by the
 * next -commit, so the stream cannot fall out of step).
 * ---------------------------------------------------------------------------------------------- */

static void mw_cb_set_error(struct mw_command_buffer_state *state, int code, const char *fmt, ...)
{
    va_list ap;
    char text[512];
    NSString *description;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    description = [[NSString alloc] initWithFormat:@"metalwire: %s", text];
    [state->error release];
    state->error = [[NSError alloc] initWithDomain:@"com.iosvm.metalwire"
                                              code:code
                                          userInfo:@{NSLocalizedDescriptionKey : description}];
    [description release];
    state->status = MTLCommandBufferStatusError;
    mw_log("command buffer seq %llu failed: %s", (unsigned long long)state->seq, text);
}

static int mw_wire_online(void)
{
    int fd = mw_conn_ensure(&gWire, mw_now_ns());

    if (fd < 0) {
        return -1;
    }
    if (fd != gLastFd) {
        /* a new connection: the host has none of this process's objects */
        gLastFd = fd;
        gGeneration++;
        gHelloGeneration = 0;
    }
    return fd;
}

static void mw_wire_offline(const char *why)
{
    gLastFd = -1;
    mw_conn_close(&gWire, why);
}

static int mw_wire_ensure_buffer(struct mw_buffer_state *buffer)
{
    struct mw_create_buffer message;

    if (buffer == NULL) {
        return -1;
    }
    if (buffer->created_generation == gGeneration) {
        return 0;
    }
    message.id = buffer->remote_id;
    message.options = (uint32_t)buffer->options;
    message.length = buffer->length;
    if (mw_send(&gWire, MW_CREATE_BUFFER, &message, sizeof(message)) != 0) {
        return -1;
    }
    buffer->created_generation = gGeneration;
    return 0;
}

static int mw_wire_ensure_texture(struct mw_texture_state *texture)
{
    struct mw_create_texture message;

    if (texture == NULL) {
        return -1;
    }
    if (texture->created_generation != gGeneration) {
        message.id = texture->remote_id;
        message.width = texture->width;
        message.height = texture->height;
        message.pixel_format = texture->pixel_format;
        message.usage = (uint32_t)texture->usage;
        message.reserved = 0;
        if (mw_send(&gWire, MW_CREATE_TEXTURE, &message, sizeof(message)) != 0) {
            return -1;
        }
        texture->created_generation = gGeneration;
    }
    if (texture->has_surface && texture->surface_generation != gGeneration) {
        struct {
            struct mw_texture_set_iosurface request;
            struct mw_segment segments[MW_VTOP_SEGMENTS];
        } iosurface;
        uint32_t count = 0;
        uint64_t covered = 0;
        int complete = 0;

        memset(&iosurface, 0, sizeof(iosurface));
        if (texture->surface_base != 0 && texture->surface_length != 0 &&
            texture->surface_length <= UINT32_MAX) {
            /* The surface's pages may be mapped but not yet faulted in: hakios_vtop answers
             * EFAULT for a page the pmap has no translation for, so touch one byte per page
             * first. This does not change the picture; it makes the mapping resident. */
            volatile const unsigned char *touch =
                (volatile const unsigned char *)texture->surface_base;

            for (uint64_t at = 0; at < texture->surface_length; at += 16384) {
                (void)touch[at];
            }
            if (syscall(SYS_hakios_vtop, texture->surface_base,
                        (uintptr_t)texture->surface_length, (uintptr_t)iosurface.segments,
                        (uintptr_t)MW_VTOP_SEGMENTS, (uintptr_t)&count) == 0 &&
                count != 0 && count <= MW_VTOP_SEGMENTS) {
                for (uint32_t i = 0; i < count; i++) {
                    covered += iosurface.segments[i].len;
                }
                complete = covered == texture->surface_length;
            }
        }
        if (!complete) {
            count = 0;
            mw_log("texture %u: IOSurface %u has no usable physical segments; the host keeps its "
                   "own read-back for the picture",
                   texture->remote_id, texture->surface_id);
        }
        iosurface.request.texture_id = texture->remote_id;
        iosurface.request.bytes_per_row = texture->surface_bytes_per_row;
        iosurface.request.alloc_size = (uint32_t)texture->surface_length;
        iosurface.request.segment_count = count;
        if (mw_send(&gWire, MW_TEXTURE_SET_IOSURFACE, &iosurface,
                    (uint32_t)(sizeof(iosurface.request) +
                               (size_t)count * sizeof(struct mw_segment))) != 0) {
            return -1;
        }
        texture->surface_generation = gGeneration;
        if (count != 0) {
            mw_log("texture %u: IOSurface %u %u byte(s) in %u physical segment(s), row %u; the "
                   "host can reach every byte",
                   texture->remote_id, texture->surface_id,
                   (unsigned)texture->surface_length, count, texture->surface_bytes_per_row);
        }
    }
    if (texture->replace_length != 0 &&
        texture->replace_serial != texture->replace_sent_serial) {
        struct mw_texture_replace_region replace;

        replace.texture_id = texture->remote_id;
        replace.mipmap_level = texture->replace_mipmap_level;
        replace.slice = texture->replace_slice;
        replace.origin_x = texture->replace_origin[0];
        replace.origin_y = texture->replace_origin[1];
        replace.origin_z = texture->replace_origin[2];
        replace.width = texture->replace_width;
        replace.height = texture->replace_height;
        replace.depth = texture->replace_depth;
        replace.bytes_per_row = texture->replace_bytes_per_row;
        replace.bytes_per_image = texture->replace_bytes_per_image;
        replace.length = texture->replace_length;
        if (mw_send_with_body(&gWire, MW_TEXTURE_REPLACE_REGION, &replace, sizeof(replace),
                              texture->replace_bytes, texture->replace_length) != 0) {
            return -1;
        }
        texture->replace_sent_serial = texture->replace_serial;
        mw_log("texture %u: %u byte(s) of replaceRegion content on the wire", texture->remote_id,
               texture->replace_length);
    }
    return 0;
}

/* The three render objects: one message each, cached per connection like buffers and textures. */
static int mw_wire_ensure_pipeline(struct mw_stub_state *pipeline)
{
    struct {
        struct mw_create_render_pipeline header;
        struct mw_vertex_attribute attributes[MW_MAX_RENDER_ATTRIBUTES];
        struct mw_vertex_layout layouts[MW_MAX_RENDER_LAYOUTS];
    } message;
    uint32_t attribute_bytes;
    uint32_t layout_bytes;

    if (pipeline == NULL || pipeline->kind != 1) {
        return -1;
    }
    if (pipeline->created_generation == gGeneration) {
        return 0;
    }
    memset(&message, 0, sizeof(message));
    memcpy(&message.header, &pipeline->pipeline, sizeof(message.header));
    message.header.id = pipeline->remote_id;
    attribute_bytes = pipeline->pipeline.vertex_attribute_count *
                      (uint32_t)sizeof(struct mw_vertex_attribute);
    layout_bytes =
        pipeline->pipeline.vertex_layout_count * (uint32_t)sizeof(struct mw_vertex_layout);
    if (attribute_bytes > sizeof(message.attributes) || layout_bytes > sizeof(message.layouts)) {
        return -1;
    }
    memcpy(message.attributes, pipeline->attributes, attribute_bytes);
    memcpy(message.layouts, pipeline->layouts, layout_bytes);
    if (mw_send(&gWire, MW_CREATE_RENDER_PIPELINE, &message,
                (uint32_t)(sizeof(message.header) + attribute_bytes + layout_bytes)) != 0) {
        return -1;
    }
    /* WP-4: where the two functions came from, and the exact library records they name. The ref
     * is diagnostic; the pipeline itself was already sent, so a host that predates v4 reads the
     * stream exactly as before (it would then see unknown message types, so this is sent only
     * when the library is actually known -- which on this branch means the v4 host). */
    if (pipeline->vertex_library != NULL || pipeline->fragment_library != NULL) {
        struct mw_pipeline_library_ref ref;

        if (mw_wire_ensure_library(pipeline->vertex_library) != 0 ||
            mw_wire_ensure_library(pipeline->fragment_library) != 0) {
            return -1;
        }
        memset(&ref, 0, sizeof(ref));
        ref.pipeline_id = pipeline->remote_id;
        ref.vertex_library_id =
            pipeline->vertex_library != NULL ? pipeline->vertex_library->remote_id : 0;
        ref.fragment_library_id =
            pipeline->fragment_library != NULL ? pipeline->fragment_library->remote_id : 0;
        if (mw_send(&gWire, MW_PIPELINE_LIBRARY_REF, &ref, sizeof(ref)) != 0) {
            return -1;
        }
    }
    pipeline->created_generation = gGeneration;
    return 0;
}

static int mw_wire_ensure_sampler(struct mw_stub_state *sampler)
{
    struct mw_create_sampler message;

    if (sampler == NULL || sampler->kind != 3) {
        return -1;
    }
    if (sampler->created_generation == gGeneration) {
        return 0;
    }
    memcpy(&message, &sampler->sampler, sizeof(message));
    message.id = sampler->remote_id;
    if (mw_send(&gWire, MW_CREATE_SAMPLER, &message, sizeof(message)) != 0) {
        return -1;
    }
    sampler->created_generation = gGeneration;
    return 0;
}

static int mw_wire_ensure_depth_stencil(struct mw_stub_state *depth)
{
    struct mw_create_depth_stencil message;

    if (depth == NULL || depth->kind != 2) {
        return -1;
    }
    if (depth->created_generation == gGeneration) {
        return 0;
    }
    memcpy(&message, &depth->depth_stencil, sizeof(message));
    message.id = depth->remote_id;
    if (mw_send(&gWire, MW_CREATE_DEPTH_STENCIL, &message, sizeof(message)) != 0) {
        return -1;
    }
    depth->created_generation = gGeneration;
    return 0;
}

/*
 * How many rows apart the frame's CRC samples: 1 = every row (the default, full verification),
 * N = every Nth row. The host must be given the same stride (--crc-stride N). It is an env
 * variable so metal_frame can set it before the device exists; sampling exists only to price the
 * checksum itself, and every measurement says which stride it used.
 */
static uint32_t mw_crc_stride(void)
{
    static uint32_t stride;

    if (stride == 0) {
        const char *text = getenv("METALWIRE_CRC_STRIDE");

        stride = 1;
        if (text != NULL) {
            long value = strtol(text, NULL, 10);

            if (value >= 1 && value <= 4096) {
                stride = (uint32_t)value;
            }
        }
        mw_log("crc: every %u row(s)", stride);
    }
    return stride;
}

static uint32_t mw_crc_of_copy(const struct mw_op *copy, struct mw_buffer_state *buffer)
{
    uint64_t skip;
    uint64_t length;

    if (buffer == NULL || buffer->memory == NULL) {
        return 0;
    }
    skip = copy->buffer_offset;
    if (skip > buffer->length) {
        return 0;
    }
    length = buffer->length - skip;
    return mw_crc32_rows_stride((const uint8_t *)buffer->memory + skip, length, copy->bytes_per_row,
                                copy->width * 4, copy->height, mw_crc_stride());
}

/*
 * The picture's upload, by reference when this kernel can name the physical memory.
 *
 * The naive wire sends 4.87 MB down the socket inside every frame; under TCG that is ~0.5 s a
 * frame, which is what the first measured boot did (docs/stage29-metal-translation.md, T1). The
 * GL replay path does not have that cost because it gives the host physical segments instead, and
 * this is the same trick at this seam: ask hakios_vtop for the runs of memory behind the buffer
 * and send the runs (16 bytes each) instead of their contents. A kernel without the call, a
 * buffer that is not resident, or an unexpectedly fragmented buffer falls back to the copy; the
 * fallback is measured, not assumed.
 */
static int mw_send_buffer_upload(struct mw_buffer_state *buffer)
{
    static struct mw_segment segments[MW_VTOP_SEGMENTS];
    uint64_t covered = 0;
    uint32_t count = 0;

    if (buffer->memory != NULL && buffer->length != 0 &&
        syscall(SYS_hakios_vtop, (uintptr_t)buffer->memory, (uintptr_t)buffer->length,
                (uintptr_t)segments, (uintptr_t)MW_VTOP_SEGMENTS, (uintptr_t)&count) == 0 &&
        count != 0 && count <= MW_VTOP_SEGMENTS) {
        for (uint32_t i = 0; i < count; i++) {
            covered += segments[i].len;
        }
        if (covered == buffer->length) {
            struct {
                struct mw_upload_buffer_phys request;
                struct mw_segment segments[MW_VTOP_SEGMENTS];
            } message;

            if (count > UINT32_MAX / (uint32_t)sizeof(struct mw_segment)) {
                return -1;
            }
            message.request.id = buffer->remote_id;
            message.request.offset = 0;
            message.request.length = (uint32_t)buffer->length;
            /* (an int, because the field is 32 bits and count is bounded above) */
            message.request.segment_count = count;
            memcpy(message.segments, segments, (size_t)count * sizeof(struct mw_segment));
            if (mw_send(&gWire, MW_UPLOAD_BUFFER_PHYS, &message,
                        (uint32_t)(sizeof(message.request) +
                                   (size_t)count * sizeof(struct mw_segment))) != 0) {
                return -1;
            }
            gStats.phys_bytes += buffer->length;
            gStats.phys_frames++;
            return 0;
        }
    }
    {
        struct mw_upload_buffer upload;

        upload.id = buffer->remote_id;
        upload.offset = 0;
        upload.length = (uint32_t)buffer->length;
        upload.reserved = 0;
        if (mw_send_with_body(&gWire, MW_UPLOAD_BUFFER, &upload, sizeof(upload), buffer->memory,
                              buffer->length) != 0) {
            return -1;
        }
        gStats.bytes += buffer->length;
    }
    return 0;
}

/* Writes the whole frame. gWireLock held. */
static int mw_send_frame(struct mw_command_buffer_state *state)
{
    struct mw_frame_begin begin;
    struct mw_frame_end end;
    struct mw_op *last_copy = NULL;
    struct mw_texture_state *last_present = NULL;

    begin.seq = state->seq;
    begin.guest_t_begin_ns = state->t_begin_ns;
    if (mw_send(&gWire, MW_FRAME_BEGIN, &begin, sizeof(begin)) != 0) {
        return -1;
    }
    for (int i = 0; i < state->op_count; i++) {
        struct mw_op *op = &state->ops[i];

        if (op->kind == MW_OP_COPY_BUFFER_TO_TEXTURE) {
            struct mw_buffer_state *buffer = mw_buffer_state_of(op->buffer);
            struct mw_texture_state *texture = mw_texture_state_of(op->texture);
            struct mw_copy_buffer_to_texture copy;

            if (buffer == NULL || texture == NULL) {
                return -1;
            }
            if (mw_wire_ensure_buffer(buffer) != 0 || mw_wire_ensure_texture(texture) != 0) {
                return -1;
            }
            if (mw_send_buffer_upload(buffer) != 0) {
                return -1;
            }
            copy.buffer_id = buffer->remote_id;
            copy.texture_id = texture->remote_id;
            copy.buffer_offset = op->buffer_offset;
            copy.bytes_per_row = op->bytes_per_row;
            copy.bytes_per_image = op->bytes_per_image;
            copy.width = op->width;
            copy.height = op->height;
            copy.origin_x = op->origin_x;
            copy.origin_y = op->origin_y;
            if (mw_send(&gWire, MW_COPY_BUFFER_TO_TEXTURE, &copy, sizeof(copy)) != 0) {
                return -1;
            }
            last_copy = op;
        } else if (op->kind == MW_OP_PRESENT) {
            struct mw_texture_state *texture = mw_texture_state_of(op->texture);
            struct mw_present present;

            if (texture == NULL) {
                return -1;
            }
            if (mw_wire_ensure_texture(texture) != 0) {
                return -1;
            }
            present.texture_id = texture->remote_id;
            present.flags = 0;
            if (mw_send(&gWire, MW_PRESENT, &present, sizeof(present)) != 0) {
                return -1;
            }
            last_present = texture;
        }
    }
    memset(&end, 0, sizeof(end));
    end.seq = state->seq;
    end.guest_t_encoded_ns = state->encoded_ns;
    if (last_copy != NULL) {
        struct mw_buffer_state *buffer = mw_buffer_state_of(last_copy->buffer);

        end.width = last_copy->width;
        end.height = last_copy->height;
        end.bytes_per_row = last_copy->width * 4;
        end.crc = mw_crc_of_copy(last_copy, buffer);
    } else if (last_present != NULL) {
        end.width = last_present->width;
        end.height = last_present->height;
        end.bytes_per_row = last_present->width * 4;
        end.crc = 0;
    }
    if (mw_send(&gWire, MW_FRAME_END, &end, sizeof(end)) != 0) {
        return -1;
    }
    return 0;
}

static void mw_note_timing(uint64_t encoded_ns, uint64_t wait_ns)
{
    gStats.encoded_sum += encoded_ns;
    if (encoded_ns < gStats.encoded_min) {
        gStats.encoded_min = encoded_ns;
    }
    if (encoded_ns > gStats.encoded_max) {
        gStats.encoded_max = encoded_ns;
    }
    gStats.wait_sum += wait_ns;
    if (wait_ns < gStats.wait_min) {
        gStats.wait_min = wait_ns;
    }
    if (wait_ns > gStats.wait_max) {
        gStats.wait_max = wait_ns;
    }
}

/* Reads the acknowledgement for cbobj — the frame whose ack is outstanding — and reports it on its
 * command buffer. */
static void mw_finish_frame(id cbobj)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(cbobj);
    struct mw_ack ack;
    uint64_t now;
    uint64_t wait_ns;
    int result;

    memset(&ack, 0, sizeof(ack));
    pthread_mutex_lock(&gWireLock);
    result = mw_recv_ack(&gWire, &ack);
    if (result != 0) {
        mw_wire_offline("no acknowledgement");
    }
    pthread_mutex_unlock(&gWireLock);
    /* The frame is on the wire and its picture is in the host's hands: the buffers it used may be
     * handed out again (CoreAnimation keeps its own references to them, so the pool cannot wait for
     * a release). */
    mw_frame_buffers_mark_available();

    if (gAwaitingCommandBuffer == cbobj) {
        [gAwaitingCommandBuffer release];
        gAwaitingCommandBuffer = nil;
    }
    if (state == NULL) {
        return;
    }
    state->awaiting_ack = 0;
    now = mw_now_ns();
    wait_ns = now > state->t_commit_ns ? now - state->t_commit_ns : 0;
    mw_note_timing(state->encoded_ns, wait_ns);
    gStats.frames++;
    if (result != 0) {
        gStats.failed++;
        mw_cb_set_error(state, MW_STATUS_BAD_COMMAND, "no acknowledgement from %s (%s)",
                        gWire.target, gWire.last_error);
        mw_log("frame %llu seq %llu: encoded %llu us, wait %llu us, NO ACK (%s)",
               (unsigned long long)state->seq, (unsigned long long)state->seq,
               (unsigned long long)(state->encoded_ns / 1000), (unsigned long long)(wait_ns / 1000),
               gWire.last_error);
        return;
    }
    if (ack.seq != state->seq) {
        gStats.failed++;
        mw_wire_offline("ack out of order");
        mw_cb_set_error(state, MW_STATUS_BAD_COMMAND,
                        "ack for seq %llu arrived while waiting for seq %llu",
                        (unsigned long long)ack.seq, (unsigned long long)state->seq);
        return;
    }
    if (ack.status != MW_STATUS_OK) {
        gStats.failed++;
        state->status = MTLCommandBufferStatusError;
        mw_cb_set_error(state, (int)ack.status, "host status %u for seq %llu", ack.status,
                        (unsigned long long)ack.seq);
    } else if (ack.crc_ok == 0) {
        /* the picture crossed but did not come back the same: a translation bug, not a hang */
        gStats.failed++;
        gStats.crc_mismatches++;
        state->status = MTLCommandBufferStatusCompleted;
        mw_cb_set_error(state, MW_STATUS_CRC_MISMATCH,
                        "CRC mismatch: guest 0x%08x, host read back 0x%08x (%ux%u)", ack.crc,
                        ack.crc, ack.width, ack.height);
        state->status = MTLCommandBufferStatusCompleted;
    } else {
        gStats.ok++;
        state->status = MTLCommandBufferStatusCompleted;
    }
    mw_log("frame %llu seq %llu: encoded %llu us, wait %llu us, host %ux%u crc 0x%08x %s",
           (unsigned long long)state->seq, (unsigned long long)state->seq,
           (unsigned long long)(state->encoded_ns / 1000), (unsigned long long)(wait_ns / 1000),
           ack.width, ack.height, ack.crc, ack.crc_ok != 0 ? "matches" : "MISMATCH");
}

/* -------------------------------------------------------------------------------------------------
 * Command buffer.
 * ---------------------------------------------------------------------------------------------- */

static void mw_cb_commit(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);
    int fd;
    int sent;

    (void)_cmd;
    if (state == NULL || state->committed) {
        return;
    }
    state->committed = 1;
    state->seq = ++gFrameSeq;
    if (state->seq % 600 == 0) {
        mw_lifecycle_dump("periodic");
    }
    state->t_commit_ns = mw_now_ns();
    state->encoded_ns =
        state->t_commit_ns > state->t_begin_ns ? state->t_commit_ns - state->t_begin_ns : 0;
    state->status = MTLCommandBufferStatusCommitted;
    if (state->local_error != NULL) {
        mw_cb_set_error(state, MW_STATUS_BAD_COMMAND, "%s", state->local_error);
        gStats.frames++;
        gStats.failed++;
        mw_note_timing(state->encoded_ns, 0);
        return;
    }
    /* CoreAnimation's render path: the encoders it ended are recorded in plain C and serialized
     * here, as protocol-2 render messages. A command buffer with no render encoder is the
     * synthetic client's blit path, unchanged. */
    if (state->render_encoder_stored != 0) {
        mw_log("CA frame: %d render encoders, %llu draws (serializing on the wire)",
               state->render_encoder_stored, (unsigned long long)state->render_draw_count);
    }
    if (state->render_encoder_stored == 0 && state->op_count == 0) {
        /* Metal allows an empty command buffer; there is nothing for a host to execute */
        state->status = MTLCommandBufferStatusCompleted;
        return;
    }
    if (!gWireReady) {
        state->status = MTLCommandBufferStatusCompleted;
        return;
    }
    /* A frame whose ack was never read would leave the stream one ack behind. Read it first. */
    if (gAwaitingCommandBuffer != nil) {
        mw_finish_frame(gAwaitingCommandBuffer);
    }
    pthread_mutex_lock(&gWireLock);
    fd = mw_wire_online();
    if (fd < 0) {
        pthread_mutex_unlock(&gWireLock);
        gStats.frames++;
        gStats.failed++;
        mw_note_timing(state->encoded_ns, 0);
        mw_cb_set_error(state, 1, "the host at %s is unreachable (%s); the frame was not sent",
                        gWire.target, gWire.last_error);
        mw_frame_buffers_mark_available();
        return;
    }
    if (gHelloGeneration != gGeneration) {
        if (mw_send_hello(&gWire, (uint32_t)getpid(), getprogname()) != 0) {
            mw_wire_offline("hello failed");
            pthread_mutex_unlock(&gWireLock);
            gStats.frames++;
            gStats.failed++;
            mw_note_timing(state->encoded_ns, 0);
            mw_cb_set_error(state, 1, "could not say hello to %s (%s)", gWire.target,
                            gWire.last_error);
            mw_frame_buffers_mark_available();
            return;
        }
        gHelloGeneration = gGeneration;
        mw_log("hello to %s: pid %u, program %s", gWire.target, (unsigned)getpid(), getprogname());
    }
    sent = state->render_encoder_stored != 0 ? mw_send_render_frame(state) : mw_send_frame(state);
    if (sent != 0) {
        mw_wire_offline(gWire.last_error);
    }
    pthread_mutex_unlock(&gWireLock);
    if (sent != 0) {
        gStats.frames++;
        gStats.failed++;
        mw_note_timing(state->encoded_ns, 0);
        mw_cb_set_error(state, 1, "sending the frame to %s failed (%s)", gWire.target,
                        gWire.last_error);
        mw_frame_buffers_mark_available();
        return;
    }
    state->sent = 1;
    state->awaiting_ack = 1;
    gAwaitingCommandBuffer = [self retain];
}

static void mw_cb_wait(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    if (state == NULL || !state->committed) {
        return;
    }
    if (state->awaiting_ack) {
        if (gAwaitingCommandBuffer == self) {
            mw_finish_frame(self);
        } else {
            state->awaiting_ack = 0;
            mw_log("frame seq %llu: not the frame waiting for an ack; nothing to wait for",
                   (unsigned long long)state->seq);
        }
    }
    if (state->handlers != nil) {
        for (id handler in (NSArray *)state->handlers) {
            ((void (^)(id))handler)(self);
        }
        [(NSMutableArray *)state->handlers removeAllObjects];
    }
}

static void mw_cb_noop(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
}

static void mw_cb_label_set(id self, SEL _cmd, id label)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
}

static id mw_cb_label(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static id mw_cb_queue(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? state->queue : nil;
}

static NSUInteger mw_cb_status(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? state->status : MTLCommandBufferStatusNotEnqueued;
}

static id mw_cb_error(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? state->error : nil;
}

static void mw_cb_add_handler(id self, SEL _cmd, id handler)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    if (state == NULL || handler == nil) {
        return;
    }
    if (state->handlers == nil) {
        state->handlers = [[NSMutableArray alloc] init];
    }
    /* Metal copies a handler; a block from the caller's stack must be moved to the heap. */
    id copied = ((id(*)(id, SEL))objc_msgSend)(handler, sel_registerName("copy"));

    [(NSMutableArray *)state->handlers addObject:copied];
    [copied release];
}

static void mw_cb_present(id self, SEL _cmd, id drawable)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);
    struct mw_op *op;
    id texture = nil;

    (void)_cmd;
    if (state == NULL || state->committed) {
        return;
    }
    if ([drawable respondsToSelector:@selector(texture)]) {
        texture = ((id(*)(id, SEL))objc_msgSend)(drawable, @selector(texture));
    }
    if (texture == nil || mw_texture_state_of(texture) == NULL) {
        /* CoreAnimation's renderer presents a drawable that is not this experiment's (its texture
         * is Apple's, not a HakiosMetalTexture). There is nothing to send on the wire for it, and
         * failing the frame would make the renderer churn on empty frames, so the present is a
         * no-op success: no local_error, no op, and an all-render command buffer still commits as
         * the empty buffer Metal allows. Logged once. */
        static int logged;

        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
            mw_log("presentDrawable: the drawable's texture is not a metalwire texture; treating "
                   "the present as a no-op (logged once)");
        }
        return;
    }
    if (state->op_count >= (int)(sizeof(state->ops) / sizeof(state->ops[0]))) {
        state->local_error =
            "presentDrawable: more commands in one v1 frame than the plug-in records";
        return;
    }
    op = &state->ops[state->op_count++];
    memset(op, 0, sizeof(*op));
    op->kind = MW_OP_PRESENT;
    op->texture = texture;
}

static double mw_cb_no_time(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return 0.0;
}

/* The rest of the command-buffer surface CoreAnimation touched or may touch next. Every one of
 * these is answered here so no _MTLCommandBuffer implementation runs on this object (Apple's
 * scheduled/completed paths use libdispatch state a plain -init never creates). */
static BOOL mw_cb_profiling_enabled(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    return state != NULL && state->profiling_enabled != 0;
}

static void mw_cb_set_profiling_enabled(id self, SEL _cmd, BOOL enabled)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);

    (void)_cmd;
    if (state != NULL) {
        state->profiling_enabled = enabled ? 1 : 0;
    }
}

static BOOL mw_cb_retained_references(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return YES; /* the plain -commandBuffer path Metal clients use */
}

/* -------------------------------------------------------------------------------------------------
 * Blit encoder.
 * ---------------------------------------------------------------------------------------------- */

static id mw_cb_blit_encoder(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);
    id encoder;
    void **slot;
    int reused = 0;

    (void)_cmd;
    if (state == NULL || state->committed || gBlitEncoderClass == Nil) {
        return nil;
    }
    encoder = mw_pool_take_free(&gEncoderPool, gBlitEncoderClass);
    if (encoder != nil) {
        void **cached = mw_slot(encoder, gBlitEncoderClass);

        if (cached != NULL && *cached != NULL) {
            mw_free_encoder(*cached);
            *cached = NULL;
        }
        mw_lifecycle_note(gBlitEncoderClass, MW_LIFE_REUSED);
        reused = 1;
    } else {
        encoder = mw_alloc_init(gBlitEncoderClass, "blit encoder");
        if (encoder == nil) {
            return nil;
        }
        mw_pool_remember(&gEncoderPool, encoder);
    }
    struct mw_encoder_state *encoder_state = calloc(1, sizeof(*encoder_state));

    slot = mw_slot(encoder, gBlitEncoderClass);
    if (encoder_state == NULL || slot == NULL) {
        free(encoder_state);
        if (!reused) {
            [encoder release];
        }
        return nil;
    }
    encoder_state->command_buffer = self; /* the command buffer keeps the encoder's memory safe */
    *slot = encoder_state;
    return reused ? [[encoder retain] autorelease] : [encoder autorelease];
}

static void mw_blit_copy_to_texture(id self, SEL _cmd, id source_buffer, NSUInteger source_offset,
                                    NSUInteger bytes_per_row, NSUInteger bytes_per_image,
                                    MTLSize source_size, id destination_texture, NSUInteger slice,
                                    NSUInteger level, MTLOrigin origin)
{
    struct mw_encoder_state *encoder = mw_encoder_state_of(self);
    struct mw_command_buffer_state *state =
        encoder != NULL ? mw_command_buffer_state_of(encoder->command_buffer) : NULL;
    struct mw_buffer_state *buffer = mw_buffer_state_of(source_buffer);
    struct mw_texture_state *texture = mw_texture_state_of(destination_texture);
    struct mw_op *op;

    (void)_cmd;
    if (encoder == NULL || state == NULL || encoder->ended || state->committed) {
        return;
    }
    if (buffer == NULL) {
        state->local_error = "copyFromBuffer: the source is not a metalwire buffer";
        mw_log("%s", state->local_error);
        return;
    }
    if (texture == NULL) {
        state->local_error = "copyFromBuffer: the destination is not a metalwire texture";
        mw_log("%s", state->local_error);
        return;
    }
    if (texture->texture_type != (uint32_t)MTLTextureType2D ||
        (texture->pixel_format != (uint32_t)MTLPixelFormatBGRA8Unorm &&
         texture->pixel_format != (uint32_t)MTLPixelFormatRGBA8Unorm)) {
        /* placeholder textures (sRGB, depth, float, arrays) exist for CoreAnimation's render path,
         * not for the wire: a blit into one stays the error it was when they could not be made */
        state->local_error = "copyFromBuffer: the destination is not a 2D BGRA8/RGBA8 texture";
        mw_log("%s (format %u type %u)", state->local_error, texture->pixel_format,
               texture->texture_type);
        return;
    }
    if (source_size.width == 0 || source_size.height == 0) {
        state->local_error = "copyFromBuffer: an empty copy";
        mw_log("%s", state->local_error);
        return;
    }
    if (bytes_per_row < source_size.width * 4) {
        state->local_error = "copyFromBuffer: sourceBytesPerRow is smaller than one row of pixels";
        mw_log("%s (%lu < %lu)", state->local_error, (unsigned long)bytes_per_row,
               (unsigned long)(source_size.width * 4));
        return;
    }
    if (origin.x + source_size.width > texture->width ||
        origin.y + source_size.height > texture->height) {
        state->local_error = "copyFromBuffer: the copy is outside the destination texture";
        mw_log("%s", state->local_error);
        return;
    }
    if (source_size.depth > 1 || slice != 0 || level != 0) {
        state->local_error = "copyFromBuffer: v1 copies one 2D level/slice";
        mw_log("%s", state->local_error);
        return;
    }
    if ((uint64_t)source_offset + (uint64_t)(source_size.height - 1) * bytes_per_row +
            source_size.width * 4 >
        buffer->length) {
        state->local_error = "copyFromBuffer: the source range is outside the buffer";
        mw_log("%s", state->local_error);
        return;
    }
    if (state->op_count >= (int)(sizeof(state->ops) / sizeof(state->ops[0]))) {
        state->local_error =
            "copyFromBuffer: more commands in one v1 frame than the plug-in records";
        return;
    }
    op = &state->ops[state->op_count++];
    memset(op, 0, sizeof(*op));
    op->kind = MW_OP_COPY_BUFFER_TO_TEXTURE;
    op->buffer = source_buffer;
    op->texture = destination_texture;
    op->buffer_offset = source_offset;
    op->bytes_per_row = (uint32_t)bytes_per_row;
    op->bytes_per_image = (uint32_t)bytes_per_image;
    op->width = (uint32_t)source_size.width;
    op->height = (uint32_t)source_size.height;
    op->depth = (uint32_t)source_size.depth;
    op->origin_x = (uint32_t)origin.x;
    op->origin_y = (uint32_t)origin.y;
    op->origin_z = (uint32_t)origin.z;
    op->slice = (uint32_t)slice;
    op->level = (uint32_t)level;
}

/* A second copy command would be a second MW_COPY_BUFFER_TO_TEXTURE in the same frame, which v1
 * allows once; it is recorded as a frame error rather than silently dropped. */
static void mw_blit_copy_to_texture_options(id self, SEL _cmd, id source_buffer,
                                            NSUInteger source_offset, NSUInteger bytes_per_row,
                                            NSUInteger bytes_per_image, MTLSize source_size,
                                            id destination_texture, NSUInteger slice,
                                            NSUInteger level, MTLOrigin origin, NSUInteger options)
{
    (void)options;
    mw_blit_copy_to_texture(self, _cmd, source_buffer, source_offset, bytes_per_row,
                            bytes_per_image, source_size, destination_texture, slice, level,
                            origin);
}

/* The two texture-level blits CoreAnimation already used: explicit no-ops so they are counted by
 * the encoder rather than logged once by the generic "asked and not implemented" path. */
static void mw_blit_copy_texture_to_texture(id self, SEL _cmd, id source_texture,
                                            NSUInteger source_slice, NSUInteger source_level,
                                            MTLOrigin source_origin, MTLSize source_size,
                                            id destination_texture, NSUInteger destination_slice,
                                            NSUInteger destination_level, MTLOrigin destination_origin)
{
    struct mw_encoder_state *encoder = mw_encoder_state_of(self);
    static int logged;

    (void)_cmd;
    (void)source_texture;
    (void)source_slice;
    (void)source_level;
    (void)source_origin;
    (void)source_size;
    (void)destination_texture;
    (void)destination_slice;
    (void)destination_level;
    (void)destination_origin;
    if (encoder != NULL && !encoder->ended) {
        encoder->texture_copy_count++;
    }
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        mw_log("blit encoder: copyFromTexture:...toTexture:... is a record-only no-op (a Stage C "
               "gap: the v1 wire has no texture-to-texture command)");
    }
}

static void mw_blit_generate_mipmaps(id self, SEL _cmd, id texture)
{
    struct mw_encoder_state *encoder = mw_encoder_state_of(self);
    static int logged;

    (void)_cmd;
    (void)texture;
    if (encoder != NULL && !encoder->ended) {
        encoder->mipmap_count++;
    }
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        mw_log("blit encoder: generateMipmapsForTexture: is a record-only no-op (a Stage C gap: "
               "no mipmap generation in the v1 wire)");
    }
}

static void mw_blit_not_in_v1(id self, SEL _cmd)
{
    struct mw_encoder_state *encoder = mw_encoder_state_of(self);
    struct mw_command_buffer_state *state =
        encoder != NULL ? mw_command_buffer_state_of(encoder->command_buffer) : NULL;

    if (state != NULL && !state->committed && state->local_error == NULL) {
        state->local_error =
            "a blit command outside the v1 subset (only copyFromBuffer:...toTexture:)";
        mw_log("%s: -%s", state->local_error, sel_getName(_cmd));
    }
}

static void mw_blit_end(id self, SEL _cmd)
{
    struct mw_encoder_state *encoder = mw_encoder_state_of(self);

    (void)_cmd;
    if (encoder != NULL) {
        encoder->ended = 1;
        if (encoder->texture_copy_count != 0 || encoder->mipmap_count != 0) {
            mw_log("CA blit encoder: %llu texture copies, %llu mipmap passes (record only)",
                   (unsigned long long)encoder->texture_copy_count,
                   (unsigned long long)encoder->mipmap_count);
        }
    }
}

static void mw_blit_synchronize(id self, SEL _cmd, id resource)
{
    (void)self;
    (void)_cmd;
    (void)resource;
    /* the buffer is plain host memory shared with the host by the wire: nothing to flush */
}

static id mw_blit_label(id self, SEL _cmd)
{
    struct mw_encoder_state *encoder = mw_encoder_state_of(self);

    (void)_cmd;
    return encoder != NULL ? encoder->label : nil;
}

static void mw_blit_set_label(id self, SEL _cmd, id label)
{
    struct mw_encoder_state *encoder = mw_encoder_state_of(self);

    (void)_cmd;
    if (encoder != NULL && encoder->label != label) {
        [encoder->label release];
        encoder->label = [label copy];
    }
}

/* -------------------------------------------------------------------------------------------------
 * Buffers.
 * ---------------------------------------------------------------------------------------------- */

static void mw_free_buffer(struct mw_buffer_state *state)
{
    if (state == NULL) {
        return;
    }
    free(state->memory);
    [state->label release];
    free(state);
}

static id mw_buffer_contents(id self, SEL _cmd)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? (id)state->memory : nil;
}

static NSUInteger mw_buffer_length(id self, SEL _cmd)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? (NSUInteger)state->length : 0;
}

static NSUInteger mw_buffer_options(id self, SEL _cmd)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? (NSUInteger)state->options : 0;
}

static NSUInteger mw_buffer_storage_mode(id self, SEL _cmd)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? (state->options >> MTLResourceStorageModeShift) & 0xf : 0;
}

static NSUInteger mw_buffer_cache_mode(id self, SEL _cmd)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? (state->options >> MTLResourceCPUCacheModeShift) & 0xf : 0;
}

static NSUInteger mw_buffer_hazard_mode(id self, SEL _cmd)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? (state->options >> MTLResourceHazardTrackingModeShift) & 0x3 : 0;
}

static NSUInteger mw_buffer_allocated_size(id self, SEL _cmd)
{
    NSUInteger length = mw_buffer_length(self, _cmd);
    NSUInteger page = (NSUInteger)getpagesize();

    return page != 0 ? (length + page - 1) / page * page : length;
}

static void mw_buffer_did_modify(id self, SEL _cmd, NSRange range)
{
    (void)self;
    (void)_cmd;
    (void)range;
}

static NSUInteger mw_buffer_purgeable(id self, SEL _cmd, NSUInteger purgeable_state)
{
    (void)self;
    (void)_cmd;
    (void)purgeable_state;
    return (NSUInteger)MTLPurgeableStateNonVolatile;
}

static uint64_t mw_buffer_gpu_address(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return 0; /* no GPU address in the guest: the pixels are the host's */
}

static id mw_buffer_device(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gDevice;
}

static id mw_buffer_label(id self, SEL _cmd)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static void mw_buffer_set_label(id self, SEL _cmd, id label)
{
    struct mw_buffer_state *state = mw_buffer_state_of(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
}

/* -------------------------------------------------------------------------------------------------
 * Textures. The pixels are not in the guest; width/height/format are what the host needs.
 * ---------------------------------------------------------------------------------------------- */

static void mw_free_texture(struct mw_texture_state *state)
{
    if (state == NULL) {
        return;
    }
    free(state->replace_bytes);
    [state->label release];
    free(state);
}

static NSUInteger mw_texture_width(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->width : 0;
}

static NSUInteger mw_texture_height(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->height : 0;
}

static NSUInteger mw_texture_depth(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->depth : 0;
}

static NSUInteger mw_texture_array_length(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->array_length : 0;
}

static NSUInteger mw_texture_mipmaps(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->mipmaps : 0;
}

static NSUInteger mw_texture_samples(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->samples : 0;
}

static NSUInteger mw_texture_pixel_format(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->pixel_format : 0;
}

static NSUInteger mw_texture_type(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->texture_type : 0;
}

static NSUInteger mw_texture_usage(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->usage : 0;
}

static NSUInteger mw_texture_storage_mode(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->storage_mode : 0;
}

static NSUInteger mw_texture_resource_options(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? (state->storage_mode << MTLResourceStorageModeShift) : 0;
}

static NSUInteger mw_texture_zero(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return 0;
}

static NSUInteger mw_texture_allocated_size(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? (NSUInteger)state->width * state->height * 4 : 0;
}

static BOOL mw_texture_no(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return NO;
}

static id mw_texture_device(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gDevice;
}

static id mw_texture_label(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static void mw_texture_set_label(id self, SEL _cmd, id label)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
}

/* WP-3: Metal's -replaceRegion:mipmapLevel:withBytes:bytesPerRow: (and the slice/bytesPerImage
 * sibling) is how CoreAnimation hands a texture its pixels. The bytes are copied out here and the
 * newest content is what the wire carries; CA uploads a layer image once and then samples it. */
static uint32_t mw_texture_bytes_per_pixel(uint32_t pixel_format)
{
    return pixel_format == MW_FORMAT_R8_UNORM ? 1u : 4u;
}

static void mw_texture_take_replace(id self, const char *what, MTLRegion region, NSUInteger level,
                                    NSUInteger slice, const void *bytes,
                                    NSUInteger bytes_per_row, NSUInteger bytes_per_image)
{
    struct mw_texture_state *state = mw_texture_state_of(self);
    uint32_t bpp;
    size_t row_bytes;
    size_t length;

    if (state == NULL) {
        mw_log("%s: not a metalwire texture", what);
        return;
    }
    if (bytes == NULL || region.size.width == 0 || region.size.height == 0) {
        mw_log("%s: texture %u: empty region/image (%p, %lux%lu)", what, state->remote_id, bytes,
               (unsigned long)region.size.width, (unsigned long)region.size.height);
        return;
    }
    if (level != 0) {
        mw_log("%s: texture %u: mipmap level %lu ignored (the wire carries level 0 only)", what,
               state->remote_id, (unsigned long)level);
        return;
    }
    if (slice != 0) {
        mw_log("%s: texture %u: array slice %lu treated as 0 (the wire carries one layer)", what,
               state->remote_id, (unsigned long)slice);
        slice = 0;
    }
    bpp = mw_texture_bytes_per_pixel(state->pixel_format);
    row_bytes = bytes_per_row != 0 ? (size_t)bytes_per_row : (size_t)region.size.width * bpp;
    length = bytes_per_image != 0
                 ? (size_t)bytes_per_image
                 : row_bytes * (size_t)region.size.height *
                       (size_t)(region.size.depth != 0 ? region.size.depth : 1);
    if (length == 0 || length > 64u * 1024u * 1024u) {
        mw_log("%s: texture %u: %lu bytes is outside the wire subset", what, state->remote_id,
               (unsigned long)length);
        return;
    }
    free(state->replace_bytes);
    state->replace_bytes = malloc(length);
    if (state->replace_bytes == NULL) {
        state->replace_length = 0;
        state->replace_serial++;
        return;
    }
    memcpy(state->replace_bytes, bytes, length);
    state->replace_length = (uint32_t)length;
    state->replace_bytes_per_row = (uint32_t)row_bytes;
    state->replace_bytes_per_image = (uint32_t)bytes_per_image;
    state->replace_mipmap_level = (uint32_t)level;
    state->replace_slice = (uint32_t)slice;
    state->replace_origin[0] = (uint32_t)region.origin.x;
    state->replace_origin[1] = (uint32_t)region.origin.y;
    state->replace_origin[2] = (uint32_t)region.origin.z;
    state->replace_width = (uint32_t)region.size.width;
    state->replace_height = (uint32_t)region.size.height;
    state->replace_depth = (uint32_t)(region.size.depth != 0 ? region.size.depth : 1);
    state->replace_serial++;
    mw_log("CA texture: %s texture %u: %lux%lu at %lu,%lu level %lu row %lu image %lu "
           "(%lu byte(s); content on the wire, serial %llu)",
           what, state->remote_id, (unsigned long)region.size.width,
           (unsigned long)region.size.height, (unsigned long)region.origin.x,
           (unsigned long)region.origin.y, (unsigned long)level, (unsigned long)row_bytes,
           (unsigned long)bytes_per_image, (unsigned long)length,
           (unsigned long long)state->replace_serial);
}

static void mw_texture_replace_region(id self, SEL _cmd, MTLRegion region, NSUInteger level,
                                      const void *bytes, NSUInteger bytes_per_row)
{
    (void)_cmd;
    mw_texture_take_replace(self, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", region, level,
                            0, bytes, bytes_per_row, 0);
}

static void mw_texture_replace_region_slice(id self, SEL _cmd, MTLRegion region, NSUInteger level,
                                            NSUInteger slice, const void *bytes,
                                            NSUInteger bytes_per_row, NSUInteger bytes_per_image)
{
    (void)_cmd;
    mw_texture_take_replace(self, "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:"
                                   "bytesPerImage:",
                            region, level, slice, bytes, bytes_per_row, bytes_per_image);
}

static void *mw_texture_iosurface(id self, SEL _cmd)
{
    struct mw_texture_state *state = mw_texture_state_of(self);

    (void)_cmd;
    return state != NULL ? state->surface : NULL;
}

/* -------------------------------------------------------------------------------------------------
 * Queue.
 * ---------------------------------------------------------------------------------------------- */

static id mw_queue_device(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gDevice;
}

static id mw_queue_label(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return @"metalwire queue";
}

static void mw_queue_noop(id self, SEL _cmd, id argument)
{
    (void)self;
    (void)_cmd;
    (void)argument;
}

static id mw_queue_command_buffer(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state;
    id command_buffer;
    void **slot;
    int reused = 0;

    (void)_cmd;
    if (gCommandBufferClass == Nil) {
        mw_log("commandBuffer: no command buffer class");
        return nil;
    }
    command_buffer = mw_pool_take_free(&gCommandBufferPool, gCommandBufferClass);
    if (command_buffer != nil) {
        /* A pool hit keeps the base object (and whatever Metal's -init allocated in it) alive; the
         * frame it carried last time is done, so its state goes and a fresh one is built below. */
        void **cached = mw_slot(command_buffer, gCommandBufferClass);

        if (cached != NULL && *cached != NULL) {
            mw_free_command_buffer(*cached);
            *cached = NULL;
        }
        mw_lifecycle_note(gCommandBufferClass, MW_LIFE_REUSED);
        reused = 1;
    } else {
        command_buffer = mw_alloc_init(gCommandBufferClass, "command buffer");
        if (command_buffer == nil) {
            return nil;
        }
        mw_pool_remember(&gCommandBufferPool, command_buffer);
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(command_buffer, gCommandBufferClass);
    if (state == NULL || slot == NULL) {
        free(state);
        if (!reused) {
            [command_buffer release];
        }
        return nil;
    }
    state->status = MTLCommandBufferStatusNotEnqueued;
    state->t_begin_ns = mw_now_ns();
    state->queue = [self retain];
    *slot = state;
    /* Both paths hand the caller a +0 reference while the pool keeps its own: a fresh object has
     * the pool's retain plus the autorelease token; a reused one has count 1 from the pool, so it
     * needs its own +1 for the token. The pool drains back to exactly one reference either way. */
    return reused ? [[command_buffer retain] autorelease] : [command_buffer autorelease];
}

/* -------------------------------------------------------------------------------------------------
 * Device.
 * ---------------------------------------------------------------------------------------------- */

static id mw_device_init(id self, SEL _cmd, unsigned int accelerator_port)
{
    struct objc_super super = {self, gDeviceBase};
    id (*send_super)(struct objc_super *, SEL) = (id(*)(struct objc_super *, SEL))objc_msgSendSuper;

    self = send_super(&super, @selector(init));
    if (self == nil) {
        mw_log("the device: _MTLDevice's -init gave nil");
        return nil;
    }
    gAcceleratorPort = accelerator_port;
    gDevice = self;
    mw_log("a device for the accelerator port %u is made; it translates Metal to %s",
           accelerator_port, gWire.target);
    return self;
}

static id mw_device_name(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return @"HakiOS metalwire device (Metal on the host's Vulkan)";
}

static uint64_t mw_device_registry_id(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gAcceleratorPort;
}

static uint64_t mw_device_max_buffer_length(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return 256ull << 20;
}

static BOOL mw_device_yes(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return YES;
}

static BOOL mw_device_no(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return NO;
}

static BOOL mw_device_no_feature_set(id self, SEL _cmd, NSUInteger feature_set)
{
    (void)self;
    (void)_cmd;
    (void)feature_set;
    return NO;
}

static BOOL mw_device_no_family(id self, SEL _cmd, NSInteger family)
{
    (void)self;
    (void)_cmd;
    (void)family;
    return NO;
}

static uint64_t mw_device_no_dedicated_memory(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return 0;
}

static uint64_t mw_device_shared_memory(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return (uint64_t)[[NSProcessInfo processInfo] physicalMemory];
}

static id mw_device_new_queue(id self, SEL _cmd)
{
    id queue;

    (void)_cmd;
    (void)self;
    queue = mw_alloc_init(gQueueClass, "command queue");
    if (queue == nil) {
        mw_log("newCommandQueue: nil");
        return nil;
    }
    mw_log("newCommandQueue: a queue that records frames for the host");
    return queue; /* +1: a "new" method, as Metal's are (the ARC clients release it) */
}

static id mw_device_new_queue_max(id self, SEL _cmd, NSUInteger max_command_buffers)
{
    (void)max_command_buffers;
    return mw_device_new_queue(self, _cmd);
}

static id mw_device_new_buffer(id self, SEL _cmd, NSUInteger length, NSUInteger options)
{
    struct mw_buffer_state *state;
    id buffer;
    void **slot;

    (void)self;
    (void)_cmd;
    if (length == 0 || (uint64_t)length > mw_device_max_buffer_length(self, _cmd)) {
        mw_log("newBufferWithLength:%lu: refused (maxBufferLength is %llu)", (unsigned long)length,
               (unsigned long long)mw_device_max_buffer_length(self, _cmd));
        return nil;
    }
    buffer = mw_pool_take_free_buffer(&gBufferPool, gBufferClass, (uint64_t)length);
    if (buffer != nil) {
        /* A pooled buffer of this exact length: its Metal buffer object already exists on the host
         * under this id, so the frame's upload refreshes the bytes instead of creating another
         * buffer there (boot wp4-v14 created 1,803 of them in a 25-minute soak). The instance is
         * handed to the caller as a "new" object (+1); the pool keeps its own reference. */
        state = mw_buffer_state_of(buffer);
        if (state != NULL && state->memory != NULL) {
            mw_lifecycle_note(gBufferClass, MW_LIFE_REUSED);
            [state->label release];
            state->label = nil;
            memset(state->memory, 0, (size_t)length);
            state->options = options;
            state->uploaded_seq = 0; /* the new frame's contents go to the host again */
            state->available = 0;
            if (state->created_generation != gGeneration) {
                /* A new connection may face a host that still holds the old id; a fresh id keeps
                 * the create unambiguous (the host rejects duplicate buffer ids). */
                state->remote_id = mw_next_object_id();
                state->created_generation = 0;
            }
            mw_frame_buffer_track(buffer);
            gBufferHits++;
            if (mw_buffer_log_enabled() && (gBufferHits % 120) == 1) {
                mw_log("buffer pool: %lu bytes hit (pool=%d frame=%d hits=%llu misses=%llu)",
                       (unsigned long)length, gBufferPool.count, gFrameBufferCount,
                       (unsigned long long)gBufferHits, (unsigned long long)gBufferMisses);
            }
            return [buffer retain];
        }
    }
    gBufferMisses++;
    if (mw_buffer_log_enabled() && (gBufferMisses % 30) == 1) {
        mw_log("buffer pool: %lu bytes MISS (pool=%d frame=%d hits=%llu misses=%llu) -- a new "
               "Metal buffer object", (unsigned long)length, gBufferPool.count, gFrameBufferCount,
               (unsigned long long)gBufferHits, (unsigned long long)gBufferMisses);
    }
    buffer = mw_alloc_init(gBufferClass, "buffer");
    if (buffer == nil) {
        return nil;
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(buffer, gBufferClass);
    if (state == NULL || slot == NULL) {
        free(state);
        [buffer release];
        return nil;
    }
    state->memory = calloc(1, (size_t)length);
    if (state->memory == NULL) {
        free(state);
        [buffer release];
        mw_log("newBufferWithLength:%lu: no memory", (unsigned long)length);
        return nil;
    }
    state->length = length;
    state->options = options;
    state->remote_id = mw_next_object_id();
    *slot = state;
    mw_pool_remember_buffer(&gBufferPool, buffer, (uint64_t)length);
    mw_frame_buffer_track(buffer);
    mw_log("newBufferWithLength:%lu options:0x%lx: buffer %u (%lu bytes, sent on first use)",
           (unsigned long)length, (unsigned long)options, state->remote_id, (unsigned long)length);
    return buffer; /* +1, as a Metal "new" method */
}

static id mw_device_new_buffer_bytes(id self, SEL _cmd, const void *bytes, NSUInteger length,
                                     NSUInteger options)
{
    id buffer = mw_device_new_buffer(self, _cmd, length, options);

    if (buffer != nil && bytes != NULL && length != 0) {
        struct mw_buffer_state *state = mw_buffer_state_of(buffer);

        if (state != NULL && state->memory != NULL) {
            memcpy(state->memory, bytes, (size_t)length);
        }
    }
    return buffer;
}

/* The v1 wire path can carry exactly one texture shape: 2D, BGRA8Unorm/RGBA8Unorm. Until
 * CoreAnimation arrived, everything else was refused; now a descriptor outside that shape still
 * gets a placeholder texture (metadata only, no pixels, never sent on the wire), and only the first
 * use of each (type, format) pair is logged. */
static NSMutableSet *gLoggedTextureFormats; /* NSString "type/format", under gStubLock */

static BOOL mw_note_texture_format(unsigned long texture_type, unsigned long pixel_format)
{
    NSString *key = [[NSString alloc] initWithFormat:@"%lu/%lu", texture_type, pixel_format];
    BOOL first = NO;

    pthread_mutex_lock(&gStubLock);
    if (gLoggedTextureFormats == nil) {
        gLoggedTextureFormats = [[NSMutableSet alloc] init];
    }
    if (![gLoggedTextureFormats containsObject:key]) {
        [gLoggedTextureFormats addObject:key];
        first = YES;
    }
    pthread_mutex_unlock(&gStubLock);
    [key release];
    return first;
}

/* IOSurfaceGetID, looked up rather than linked: the bundle links Foundation/Metal/libobjc only.
 * The same lookup covers the address/stride functions WP-3 needs; a system whose IOSurface
 * library is incomplete simply reports no surface and the host keeps its own read-back. */
static uint32_t mw_iosurface_id(void *surface)
{
    typedef uint32_t (*mw_iosurface_get_id_fn)(void *);
    static mw_iosurface_get_id_fn get_id;
    static int resolved;

    if (!resolved) {
        void *symbol = dlsym(RTLD_DEFAULT, "IOSurfaceGetID");

        memcpy(&get_id, &symbol, sizeof(get_id));
        resolved = 1;
    }
    return surface != NULL && get_id != NULL ? get_id(surface) : 0;
}

static void *mw_iosurface_base(void *surface)
{
    typedef void *(*mw_iosurface_base_fn)(void *);
    static mw_iosurface_base_fn get_base;
    static int resolved;

    if (!resolved) {
        void *symbol = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");

        memcpy(&get_base, &symbol, sizeof(get_base));
        resolved = 1;
    }
    return surface != NULL && get_base != NULL ? get_base(surface) : NULL;
}

static uint64_t mw_iosurface_alloc_size(void *surface)
{
    typedef size_t (*mw_iosurface_size_fn)(void *);
    static mw_iosurface_size_fn get_size;
    static int resolved;

    if (!resolved) {
        void *symbol = dlsym(RTLD_DEFAULT, "IOSurfaceGetAllocSize");

        memcpy(&get_size, &symbol, sizeof(get_size));
        resolved = 1;
    }
    return surface != NULL && get_size != NULL ? (uint64_t)get_size(surface) : 0;
}

static uint32_t mw_iosurface_bytes_per_row(void *surface)
{
    typedef size_t (*mw_iosurface_row_fn)(void *);
    static mw_iosurface_row_fn get_row;
    static int resolved;

    if (!resolved) {
        void *symbol = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");

        memcpy(&get_row, &symbol, sizeof(get_row));
        resolved = 1;
    }
    return surface != NULL && get_row != NULL ? (uint32_t)get_row(surface) : 0;
}

/* The first MW_IOSURFACE_LOG_LIMIT distinct surfaces are logged once each; the same report is
 * what tells us whether the port's IOSurface implementation handed us real memory. */
#define MW_IOSURFACE_LOG_LIMIT 64
static NSMutableSet *gLoggedIosurfaces; /* NSString "pointers", under gStubLock */

static BOOL mw_note_iosurface(void *surface)
{
    NSString *key = [[NSString alloc] initWithFormat:@"%p", surface];
    BOOL first = NO;

    pthread_mutex_lock(&gStubLock);
    if (gLoggedIosurfaces == nil) {
        gLoggedIosurfaces = [[NSMutableSet alloc] init];
    }
    if (![gLoggedIosurfaces containsObject:key] &&
        [gLoggedIosurfaces count] < MW_IOSURFACE_LOG_LIMIT) {
        [gLoggedIosurfaces addObject:key];
        first = YES;
    }
    pthread_mutex_unlock(&gStubLock);
    [key release];
    return first;
}

/* The texture object for a descriptor; selector_name only shapes the log line, so the wire-path
 * line keeps the exact "newTextureWithDescriptor: texture N (...)" wording the first boots have. */
static id mw_texture_object(id descriptor, const char *selector_name)
{
    MTLTextureDescriptor *texture_descriptor = (MTLTextureDescriptor *)descriptor;
    struct mw_texture_state *state;
    NSUInteger width, height, depth, mipmaps, samples, array_length;
    MTLPixelFormat pixel_format;
    MTLTextureType texture_type;
    MTLTextureUsage usage;
    MTLStorageMode storage_mode;
    int wire_format;
    id texture;
    void **slot;

    if (texture_descriptor == nil) {
        mw_log("%s: nil descriptor", selector_name);
        return nil;
    }
    width = [texture_descriptor width];
    height = [texture_descriptor height];
    depth = [texture_descriptor depth];
    mipmaps = [texture_descriptor mipmapLevelCount];
    samples = [texture_descriptor sampleCount];
    array_length = [texture_descriptor arrayLength];
    pixel_format = [texture_descriptor pixelFormat];
    texture_type = [texture_descriptor textureType];
    usage = [texture_descriptor usage];
    storage_mode = [texture_descriptor storageMode];
    wire_format = texture_type == MTLTextureType2D && width != 0 && height != 0 && depth <= 1 &&
                  (pixel_format == MTLPixelFormatBGRA8Unorm ||
                   pixel_format == MTLPixelFormatRGBA8Unorm ||
                   pixel_format == MTLPixelFormatR8Unorm);
    if (!wire_format &&
        mw_note_texture_format((unsigned long)texture_type, (unsigned long)pixel_format)) {
        mw_log("%s: placeholder texture for format %lu type %lu (%lux%lux%lu; not in the wire "
               "subset, which carries only BGRA8Unorm/RGBA8Unorm/R8Unorm 2D)",
               selector_name, (unsigned long)pixel_format, (unsigned long)texture_type,
               (unsigned long)width, (unsigned long)height, (unsigned long)depth);
    }
    texture = mw_alloc_init(gTextureClass, "texture");
    if (texture == nil) {
        return nil;
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(texture, gTextureClass);
    if (state == NULL || slot == NULL) {
        free(state);
        [texture release];
        return nil;
    }
    state->remote_id = mw_next_object_id();
    state->width = (uint32_t)width;
    state->height = (uint32_t)height;
    state->depth = (uint32_t)(depth != 0 ? depth : 1);
    state->mipmaps = (uint32_t)(mipmaps != 0 ? mipmaps : 1);
    state->samples = (uint32_t)(samples != 0 ? samples : 1);
    state->array_length = (uint32_t)(array_length != 0 ? array_length : 1);
    state->texture_type = (uint32_t)texture_type;
    state->pixel_format = (uint32_t)pixel_format;
    state->usage = (unsigned long)usage;
    state->storage_mode = (unsigned long)storage_mode;
    *slot = state;
    if (wire_format) {
        mw_log(
            "%s: texture %u (%lux%lu format %lu usage 0x%lx; pixels on the host)", selector_name,
            state->remote_id, (unsigned long)width, (unsigned long)height,
            (unsigned long)pixel_format, (unsigned long)usage);
    }
    return texture; /* +1, as a Metal "new" method */
}

static id mw_device_new_texture(id self, SEL _cmd, id descriptor)
{
    (void)self;
    (void)_cmd;
    return mw_texture_object(descriptor, "newTextureWithDescriptor:");
}

static id mw_device_new_texture_iosurface(id self, SEL _cmd, id descriptor, void *surface,
                                          NSUInteger plane)
{
    uint32_t surface_id = mw_iosurface_id(surface);
    struct mw_texture_state *state;
    id texture;

    (void)self;
    (void)_cmd;
    texture = mw_texture_object(descriptor, "newTextureWithDescriptor:iosurface:plane:");
    if (texture == nil) {
        return nil;
    }
    state = mw_texture_state_of(texture);
    if (state == NULL) {
        return texture;
    }
    state->surface = surface;
    state->surface_id = surface_id;
    state->surface_base = (uintptr_t)mw_iosurface_base(surface);
    state->surface_length = mw_iosurface_alloc_size(surface);
    state->surface_bytes_per_row = mw_iosurface_bytes_per_row(surface);
    if (state->surface_bytes_per_row == 0) {
        state->surface_bytes_per_row = state->width * 4;
    }
    state->has_surface = state->surface_base != 0 && state->surface_length != 0;
    if (mw_note_iosurface(surface)) {
        mw_log("newTextureWithDescriptor:iosurface:plane: IOSurface %u (%p) plane %lu -> texture %u "
               "(%lux%lu); surface base %p, row %u, %llu byte(s) %s",
               surface_id, surface, (unsigned long)plane, state->remote_id,
               (unsigned long)state->width, (unsigned long)state->height,
               (void *)state->surface_base, state->surface_bytes_per_row,
               (unsigned long long)state->surface_length,
               state->has_surface ? "(real: the host will read and write it by reference)"
                                  : "(no address from IOSurface; host read-back only)");
    }
    return texture;
}

static id mw_device_new_drawable(id self, SEL _cmd, id texture)
{
    (void)self;
    (void)_cmd;
    if (mw_texture_state_of(texture) == NULL) {
        mw_log("newDrawableWithTexture: not a metalwire texture");
        return nil;
    }
    return [[MWDrawable alloc] initWithTexture:texture]; /* +1, as a "new" method */
}

/* -------------------------------------------------------------------------------------------------
 * Libraries, functions and pipeline states: CoreAnimation's library wall.
 *
 * CoreAnimation's Metal renderer loads QuartzCore.framework/default.metallib through the device's
 * -newLibraryWithURL:error:. Apple's implementation of that method lives on _MTLDevice itself, so a
 * subclass that does not override it runs Apple's code on a file that is not a real metallib and
 * kills the process ("[Metal] Metal failed to load library: Invalid library file"). This experiment
 * measures the channel, not shader translation, so the premise here is that the library is a
 * placeholder: the URL is logged and its bytes are never read. A library accepts any function name,
 * a function is the vertex/fragment guess that name implies (logged), and the pipeline,
 * depth-stencil and sampler states are empty objects that let CoreAnimation walk past the library
 * phase. Nothing is compiled and nothing is sent on the wire.
 * ---------------------------------------------------------------------------------------------- */

static NSMutableSet *gLoggedFunctionNames; /* NSString, under gStubLock: each name logged once */

static struct mw_library_state *mw_library_state_of(id object)
{
    void **slot;

    if (object == nil || gLibraryClass == Nil || object_getClass(object) != gLibraryClass) {
        return NULL;
    }
    slot = mw_slot(object, gLibraryClass);
    return slot != NULL ? *slot : NULL;
}

static struct mw_function_state *mw_function_state_of(id object)
{
    void **slot;

    if (object == nil || gFunctionClass == Nil || object_getClass(object) != gFunctionClass) {
        return NULL;
    }
    slot = mw_slot(object, gFunctionClass);
    return slot != NULL ? *slot : NULL;
}

static struct mw_stub_state *mw_stub_state_of(id object, Class cls)
{
    void **slot;

    if (object == nil || cls == Nil || object_getClass(object) != cls) {
        return NULL;
    }
    slot = mw_slot(object, cls);
    return slot != NULL ? *slot : NULL;
}

static struct mw_stub_state *mw_stub_state_of_object(id object)
{
    Class cls = object_getClass(object);

    if (cls != gPipelineStateClass && cls != gDepthStencilStateClass && cls != gSamplerStateClass) {
        return NULL;
    }
    return mw_stub_state_of(object, cls);
}

static void mw_free_library(struct mw_library_state *state)
{
    if (state == NULL) {
        return;
    }
    [state->install_name release];
    [state->label release];
    [state->names release];
    free(state);
}

static void mw_free_function(struct mw_function_state *state)
{
    if (state == NULL) {
        return;
    }
    [state->name release];
    [state->label release];
    free(state);
}

static void mw_free_stub(struct mw_stub_state *state)
{
    if (state == NULL) {
        return;
    }
    [state->label release];
    free(state);
}

/* The object shells. Each is +1 (a Metal "new" method's convention), carries one _mw state, and
 * the class is the run-time class the constructor registered. */
static id mw_library_object(const char *selector_name, id what, id install_name, uint32_t kind)
{
    struct mw_library_state *state;
    id library;
    void **slot;

    if (gLibraryClass == Nil) {
        mw_log("%s: no library class is registered", selector_name);
        return nil;
    }
    library = mw_alloc_init(gLibraryClass, "library");
    if (library == nil) {
        return nil;
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(library, gLibraryClass);
    if (state == NULL || slot == NULL) {
        free(state);
        [library release];
        return nil;
    }
    state->stub_id = mw_next_stub_id();
    state->install_name = [install_name copy];
    state->names = [[NSMutableArray alloc] init];
    state->wire.kind = kind;
    state->wire.names = state->names;
    *slot = state;
    mw_log("%s: %s -> placeholder library %u (install name %s; the library bytes are not parsed)",
           selector_name, mw_text(what), state->stub_id, mw_text(install_name));
    return library;
}

static id mw_function_object(NSString *name, struct mw_wire_library *library)
{
    struct mw_function_state *state;
    id function;
    void **slot;

    if (gFunctionClass == Nil) {
        mw_log("newFunctionWithName: no function class is registered");
        return nil;
    }
    function = mw_alloc_init(gFunctionClass, "function");
    if (function == nil) {
        return nil;
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(function, gFunctionClass);
    if (state == NULL || slot == NULL) {
        free(state);
        [function release];
        return nil;
    }
    state->name = [name copy];
    state->library = library;
    state->function_type = ([name rangeOfString:@".vert."].location != NSNotFound ||
                            [name rangeOfString:@"Vertex"].location != NSNotFound)
                               ? MTLFunctionTypeVertex
                               : MTLFunctionTypeFragment;
    *slot = state;
    return function;
}

static id mw_stub_object(Class cls, const char *what, id label)
{
    struct mw_stub_state *state;
    id object;
    void **slot;

    if (cls == Nil) {
        mw_log("%s: no class is registered", what);
        return nil;
    }
    object = mw_alloc_init(cls, what);
    if (object == nil) {
        return nil;
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(object, cls);
    if (state == NULL || slot == NULL) {
        free(state);
        [object release];
        return nil;
    }
    state->label = [label copy];
    state->remote_id = mw_next_object_id();
    if (cls == gPipelineStateClass) {
        state->kind = 1;
        state->pipeline.id = state->remote_id;
        if (label != nil && [label isKindOfClass:[NSString class]]) {
            const char *utf8 = [(NSString *)label UTF8String];

            if (utf8 != NULL) {
                strncpy(state->pipeline.pipeline_name, utf8, MW_NAME_MAX - 1);
            }
        }
    } else if (cls == gDepthStencilStateClass) {
        state->kind = 2;
        state->depth_stencil.id = state->remote_id;
    } else if (cls == gSamplerStateClass) {
        state->kind = 3;
        state->sampler.id = state->remote_id;
    }
    *slot = state;
    return object;
}

/* -newFunctionWithName:... on a library: any name is accepted, the first ask for a name is logged,
 * and every name a library was asked about ends up in its -functionNames. */
static id mw_library_make_function(id self, const char *selector_name, id name)
{
    struct mw_library_state *library = mw_library_state_of(self);
    struct mw_function_state *function_state;
    BOOL first = NO;
    id function;

    if (name == nil || ![name isKindOfClass:[NSString class]]) {
        mw_log("%s: the function name is nil or not a string; nil", selector_name);
        return nil;
    }
    pthread_mutex_lock(&gStubLock);
    if (gLoggedFunctionNames == nil) {
        gLoggedFunctionNames = [[NSMutableSet alloc] init];
    }
    if (![gLoggedFunctionNames containsObject:name]) {
        [gLoggedFunctionNames addObject:name];
        first = YES;
    }
    if (library != NULL && library->names != nil && ![library->names containsObject:name]) {
        [library->names addObject:name];
    }
    pthread_mutex_unlock(&gStubLock);
    function = mw_function_object((NSString *)name,
                                    library != NULL ? &library->wire : NULL);
    function_state = mw_function_state_of(function);
    if (function != nil && first) {
        mw_log("%s: %s -> placeholder %s function (guessed from the name; no shader is compiled)",
               selector_name, mw_text(name),
               function_state != NULL && function_state->function_type == MTLFunctionTypeVertex
                   ? "vertex"
                   : "fragment");
    }
    return function;
}

/* Library. */
static id mw_library_function_names(id self, SEL _cmd)
{
    struct mw_library_state *state = mw_library_state_of(self);
    NSArray *names = @[];

    (void)_cmd;
    if (state != NULL) {
        pthread_mutex_lock(&gStubLock);
        names = state->names != nil ? [NSArray arrayWithArray:state->names] : @[];
        pthread_mutex_unlock(&gStubLock);
    }
    return names;
}

static id mw_library_new_function(id self, SEL _cmd, id name)
{
    (void)_cmd;
    return mw_library_make_function(self, "newFunctionWithName:", name);
}

static id mw_library_new_function_constants(id self, SEL _cmd, id name, id constant_values,
                                            NSError **error)
{
    (void)_cmd;
    (void)constant_values;
    mw_clear_error(error);
    return mw_library_make_function(self, "newFunctionWithName:constantValues:error:", name);
}

/* The completion-handler form of the same call (public MTLibrary API): answer synchronously with a
 * placeholder function, no compiler and no dispatch queue involved. */
static void mw_library_new_function_completion(id self, SEL _cmd, id name, id constant_values,
                                               id handler)
{
    id function;

    (void)_cmd;
    (void)constant_values;
    function = mw_library_make_function(self, "newFunctionWithName:constantValues:completionHandler:",
                                        name);
    if (handler != nil) {
        ((void (^)(id, NSError *))handler)(function, nil);
    }
    [function autorelease];
}

static id mw_library_label(id self, SEL _cmd)
{
    struct mw_library_state *state = mw_library_state_of(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static void mw_library_set_label(id self, SEL _cmd, id label)
{
    struct mw_library_state *state = mw_library_state_of(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
}

static id mw_library_device(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gDevice;
}

static NSInteger mw_library_type(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return (NSInteger)MTLLibraryTypeExecutable;
}

static id mw_library_install_name(id self, SEL _cmd)
{
    struct mw_library_state *state = mw_library_state_of(self);

    (void)_cmd;
    return state != NULL ? state->install_name : nil;
}

/* Function: the name and the vertex/fragment guess; no attributes, no constants. */
static id mw_function_name(id self, SEL _cmd)
{
    struct mw_function_state *state = mw_function_state_of(self);

    (void)_cmd;
    return state != NULL ? state->name : nil;
}

static NSUInteger mw_function_type(id self, SEL _cmd)
{
    struct mw_function_state *state = mw_function_state_of(self);

    (void)_cmd;
    return state != NULL ? (NSUInteger)state->function_type : MTLFunctionTypeFragment;
}

static id mw_function_empty_array(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return @[];
}

static id mw_function_empty_dictionary(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return @{};
}

static id mw_function_device(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gDevice;
}

static id mw_function_label(id self, SEL _cmd)
{
    struct mw_function_state *state = mw_function_state_of(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static void mw_function_set_label(id self, SEL _cmd, id label)
{
    struct mw_function_state *state = mw_function_state_of(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
}

/* A placeholder that keeps the base function's name and type: runtime specialisation never
 * compiles anything here, it only has to hand CoreAnimation an object of the right shape. */
static id mw_function_specialized_copy(id self)
{
    struct mw_function_state *base_state = mw_function_state_of(self);
    struct mw_function_state *specialized_state;
    id specialized;

    if (base_state == NULL || base_state->name == nil) {
        return nil;
    }
    specialized = mw_function_object((NSString *)base_state->name, NULL);
    specialized_state = mw_function_state_of(specialized);
    if (specialized_state != NULL) {
        specialized_state->function_type = base_state->function_type;
        [specialized_state->label release];
        specialized_state->label = [base_state->label copy];
    }
    return specialized; /* +1, as a Metal "new" method */
}

/* The wall of ca-metal3-dealloc1: CoreAnimation loads the fixed_* base functions from the
 * placeholder library and specialises them with constants. Apple's _MTLFunction implementation
 * dereferences Metal's real function object, so this answers a placeholder instead and logs what
 * was asked: the base function's name and the descriptor's function-constants dictionary. */
static id mw_function_new_specialized(id self, SEL _cmd, id descriptor, id destination_archive,
                                      id function_cache, NSError **error)
{
    struct mw_function_state *base_state = mw_function_state_of(self);
    id constants = nil;
    id constants_text = nil;
    id specialized;

    (void)_cmd;
    (void)destination_archive;
    (void)function_cache;
    mw_clear_error(error);
    @try {
        if (descriptor != nil) {
            if ([descriptor respondsToSelector:@selector(functionConstantsDictionary)]) {
                constants = ((id(*)(id, SEL))objc_msgSend)(
                    descriptor, @selector(functionConstantsDictionary));
            }
            if (constants != nil && [constants respondsToSelector:@selector(description)]) {
                constants_text = ((id(*)(id, SEL))objc_msgSend)(constants, @selector(description));
            }
        }
    } @catch (NSException *exception) {
        mw_log("newSpecializedFunction: reading the descriptor's constants raised %s; nil",
               mw_text([exception reason]));
        constants = nil;
        constants_text = nil;
    }
    mw_log("newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:error: %s + %s "
           "(placeholder specialized function; no shader is compiled)",
           mw_text(base_state != NULL ? base_state->name : nil), mw_text(constants_text));
    specialized = mw_function_specialized_copy(self);
    if (specialized == nil && error != NULL) {
        *error = mw_nserror("the base function has no name to specialize");
    }
    return specialized;
}

/* The shorter private spelling (some Metal builds carry it): same answer. */
static id mw_function_new_specialized_descriptor(id self, SEL _cmd, id descriptor, NSError **error)
{
    return mw_function_new_specialized(self, _cmd, descriptor, nil, nil, error);
}

static void mw_function_new_specialized_sync(id self, SEL _cmd, id descriptor,
                                             id destination_archive, id function_cache, BOOL sync,
                                             id handler)
{
    id specialized = mw_function_new_specialized(self, _cmd, descriptor, destination_archive,
                                                 function_cache, NULL);

    (void)sync;
    if (handler != nil) {
        ((void (^)(id, NSError *))handler)(specialized, nil);
    }
    [specialized autorelease];
}

static id mw_function_new_specialized_name(id self, SEL _cmd, id name, id constants)
{
    struct mw_function_state *state = mw_function_state_of(self);
    static int logged;
    id function_name = nil;
    id specialized;

    (void)_cmd;
    (void)constants;
    if (name != nil && [name isKindOfClass:[NSString class]]) {
        function_name = name;
    } else if (state != NULL) {
        function_name = state->name;
    }
    if (function_name == nil) {
        return nil;
    }
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        mw_log("newSpecializedFunctionWithName:constants: %s -> placeholder function "
               "(no shader is compiled)",
               mw_text(function_name));
    }
    specialized = mw_function_object((NSString *)function_name, NULL);
    if (specialized != nil && state != NULL) {
        struct mw_function_state *specialized_state = mw_function_state_of(specialized);

        if (specialized_state != NULL) {
            specialized_state->function_type = state->function_type;
        }
    }
    return specialized;
}

/* The three empty states (render pipeline, depth-stencil, sampler): a label and the device. */
static id mw_stub_label(id self, SEL _cmd)
{
    struct mw_stub_state *state = mw_stub_state_of_object(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static void mw_stub_set_label(id self, SEL _cmd, id label)
{
    struct mw_stub_state *state = mw_stub_state_of_object(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
}

static id mw_stub_device(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gDevice;
}

/* Device: the library phase. For URL/data/file the premise is that the library is a placeholder, so
 * the call always succeeds; only source cannot be honoured, because the guest has no Metal
 * compiler. */
static id mw_device_new_library_url(id self, SEL _cmd, NSURL *url, NSError **error)
{
    id text = nil;
    id path = nil;
    id library;

    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    if (url != nil) {
        text = [url absoluteString];
        path = [url path];
    }
    library = mw_library_object("newLibraryWithURL:error:", text, path, MW_LIBRARY_URL);
    if (library != nil) {
        struct mw_library_state *state = mw_library_state_of(library);
        const char *path_text = path != nil && [path isKindOfClass:[NSString class]]
                                    ? [(NSString *)path fileSystemRepresentation]
                                    : NULL;
        uint64_t hash = 0;
        uint64_t length = 0;

        if (state != NULL && mw_fnv1a64_file(path_text, &hash, &length)) {
            state->wire.hash_known = 1;
            state->wire.hash = hash;
            state->wire.byte_length = length;
            mw_log("newLibraryWithURL:error: %llu byte(s) hashed, %016llx",
                   (unsigned long long)length, (unsigned long long)hash);
        }
    }
    if (library == nil && error != NULL) {
        *error = mw_nserror("the placeholder library object could not be made");
    }
    return library;
}

static id mw_device_new_library_data(id self, SEL _cmd, dispatch_data_t data, NSError **error)
{
    id library;

    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    library = mw_library_object("newLibraryWithData:error:", @"in-memory data", nil, MW_LIBRARY_DATA);
    if (library != nil) {
        struct mw_library_state *state = mw_library_state_of(library);

        if (state != NULL && data != NULL) {
            uint64_t hash = 0;
            uint64_t length = 0;

            state->wire.hash_known = mw_fnv1a64_dispatch(data, &hash, &length);
            state->wire.hash = hash;
            state->wire.byte_length = length;
            mw_log("newLibraryWithData:error: %llu byte(s), hash %016llx%s",
                   (unsigned long long)length, (unsigned long long)hash,
                   state->wire.hash_known ? "" : " (over the hash cap; hash_known = 0)");
        }
    }
    if (library == nil && error != NULL) {
        *error = mw_nserror("the placeholder library object could not be made");
    }
    return library;
}

static id mw_device_new_library_file(id self, SEL _cmd, NSString *path, NSError **error)
{
    id library;

    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    library = mw_library_object("newLibraryWithFile:error:", path, path, MW_LIBRARY_FILE);
    if (library != nil) {
        struct mw_library_state *state = mw_library_state_of(library);
        const char *path_text = path != nil && [path isKindOfClass:[NSString class]]
                                    ? [(NSString *)path fileSystemRepresentation]
                                    : NULL;
        uint64_t hash = 0;
        uint64_t length = 0;

        if (state != NULL && mw_fnv1a64_file(path_text, &hash, &length)) {
            state->wire.hash_known = 1;
            state->wire.hash = hash;
            state->wire.byte_length = length;
            mw_log("newLibraryWithFile:error: %llu byte(s) hashed, %016llx",
                   (unsigned long long)length, (unsigned long long)hash);
        }
    }
    if (library == nil && error != NULL) {
        *error = mw_nserror("the placeholder library object could not be made");
    }
    return library;
}

static id mw_device_new_library_source(id self, SEL _cmd, NSString *source, id options,
                                       NSError **error)
{
    unsigned long length = 0;

    (void)self;
    (void)_cmd;
    (void)options;
    if (source != nil && [source isKindOfClass:[NSString class]]) {
        length = (unsigned long)[source length];
    }
    mw_log("newLibraryWithSource:options:error: %lu characters of source are not compiled (the "
           "guest has no Metal compiler); nil",
           length);
    if (error != NULL) {
        *error = mw_nserror("newLibraryWithSource:options:error: is not supported: there is no "
                            "Metal shader compiler in the guest; this experiment provides "
                            "placeholder libraries only");
    }
    return nil;
}

static id mw_device_new_default_library(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return mw_library_object("newDefaultLibrary", @"the framework's default library", nil, MW_LIBRARY_DEFAULT);
}

static id mw_device_new_default_library_bundle(id self, SEL _cmd, NSBundle *bundle,
                                               NSError **error)
{
    id text = nil;
    id library;

    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    if (bundle != nil && [bundle isKindOfClass:[NSBundle class]]) {
        text = [bundle bundlePath];
    }
    library = mw_library_object("newDefaultLibraryWithBundle:error:", text, nil, MW_LIBRARY_DEFAULT);
    if (library == nil && error != NULL) {
        *error = mw_nserror("the placeholder library object could not be made");
    }
    return library;
}

/* Device: the pipeline phase. The descriptor is read for its vertex and fragment function names
 * (defensively: it is CoreAnimation's object, and @try is the contract here), then a placeholder
 * state is answered. Nothing is compiled. */
static id mw_render_pipeline_object(const char *selector_name, id descriptor, NSUInteger options,
                                    id *reflection, NSError **error)
{
    id vertex = nil;
    id fragment = nil;
    id label = nil;
    id pipeline;

    mw_clear_error(error);
    if (reflection != NULL) {
        *reflection = nil;
    }
    @try {
        if (descriptor != nil) {
            if ([descriptor respondsToSelector:@selector(vertexFunction)]) {
                vertex = ((id(*)(id, SEL))objc_msgSend)(descriptor, @selector(vertexFunction));
            }
            if ([descriptor respondsToSelector:@selector(fragmentFunction)]) {
                fragment = ((id(*)(id, SEL))objc_msgSend)(descriptor, @selector(fragmentFunction));
            }
            label = mw_safe_label(descriptor);
        }
    } @catch (NSException *exception) {
        mw_log("%s: reading the descriptor's functions raised %s; both are logged as nil",
               selector_name, mw_text([exception reason]));
        vertex = nil;
        fragment = nil;
    }
    mw_log("%s: vertex %s, fragment %s, options 0x%lx (placeholder render pipeline; no shader is "
           "compiled)",
           selector_name, mw_text(mw_safe_function_name(vertex)),
           mw_text(mw_safe_function_name(fragment)), (unsigned long)options);
    pipeline = mw_stub_object(gPipelineStateClass, "render pipeline state", label);
    if (pipeline != nil) {
        struct mw_stub_state *pipeline_state = mw_stub_state_of_object(pipeline);

        mw_render_capture_pipeline(pipeline_state, descriptor);
        if (pipeline_state != NULL) {
            mw_log("%s: serialized pipeline %u name \"%.*s\" vertex \"%.*s\" fragment \"%.*s\"",
                   selector_name, pipeline_state->remote_id, MW_NAME_MAX,
                   pipeline_state->pipeline.pipeline_name, MW_NAME_MAX,
                   pipeline_state->pipeline.vertex_function, MW_NAME_MAX,
                   pipeline_state->pipeline.fragment_function);
        }
    }
    if (pipeline == nil && error != NULL) {
        *error = mw_nserror("the placeholder render pipeline state could not be made");
    }
    return pipeline;
}

static id mw_device_new_render_pipeline(id self, SEL _cmd, id descriptor, NSError **error)
{
    (void)self;
    (void)_cmd;
    return mw_render_pipeline_object("newRenderPipelineStateWithDescriptor:error:", descriptor, 0,
                                     NULL, error);
}

static id mw_device_new_render_pipeline_options(id self, SEL _cmd, id descriptor,
                                                NSUInteger options, NSError **error)
{
    (void)self;
    (void)_cmd;
    return mw_render_pipeline_object("newRenderPipelineStateWithDescriptor:options:error:",
                                     descriptor, options, NULL, error);
}

static id mw_device_new_render_pipeline_reflection(id self, SEL _cmd, id descriptor,
                                                   NSUInteger options, id *reflection,
                                                   NSError **error)
{
    (void)self;
    (void)_cmd;
    return mw_render_pipeline_object(
        "newRenderPipelineStateWithDescriptor:options:reflection:error:", descriptor, options,
        reflection, error);
}

/* The SPI path CoreAnimation actually uses (16.5): the selector carries a completion handler and
 * Apple's _MTLDevice implementation hands the descriptor to MTLCompilerService. That service
 * inspects the placeholder functions -- and kills the process. This override answers the handler
 * synchronously with a placeholder state and never creates a compiler request. The state is
 * handed over with its +1 reference; CoreAnimation's handler retains it, and if it does not, the
 * leak is one state object per pipeline. */
static void mw_device_new_render_pipeline_internal(id self, SEL _cmd, id descriptor,
                                                   NSUInteger options, id *reflection,
                                                   id destination_archive, NSError **error,
                                                   id handler)
{
    id pipeline;

    (void)self;
    (void)_cmd;
    (void)destination_archive;
    pipeline = mw_render_pipeline_object(
        "newRenderPipelineStateWithDescriptorInternal:options:reflection:destinationBinaryArchive:"
        "error:completionHandler:",
        descriptor, options, reflection, error);
    if (handler != nil) {
        ((void (^)(id, id, id))handler)(pipeline, nil, nil);
    } else {
        mw_log("newRenderPipelineStateWithDescriptorInternal: no completion handler: the "
               "placeholder state is dropped");
    }
}

static void mw_device_new_compute_pipeline_internal(id self, SEL _cmd, id descriptor,
                                                    NSUInteger options, id pipeline_cache,
                                                    id destination_archive, id *reflection,
                                                    NSError **error, id handler)
{
    NSError *failure = mw_nserror("compute pipeline creation is not part of this experiment");

    (void)self;
    (void)_cmd;
    (void)descriptor;
    (void)options;
    (void)pipeline_cache;
    (void)destination_archive;
    if (reflection != NULL) {
        *reflection = nil;
    }
    if (error != NULL) {
        *error = failure;
    }
    mw_log("newComputePipelineStateWithDescriptorInternal:options:pipelineCache:"
           "destinationBinaryArchive:reflection:error:completionHandler: (nil; not in this "
           "experiment)");
    if (handler != nil) {
        ((void (^)(id, id, id))handler)(nil, nil, failure);
    }
}

/* Compute pipelines are not part of what CoreAnimation's renderer asks for; this override exists so
 * Apple's inherited implementation cannot run, and it says so instead of compiling. */
static id mw_device_new_compute_pipeline(id self, SEL _cmd, id function, NSError **error)
{
    (void)self;
    (void)_cmd;
    mw_log("newComputePipelineStateWithFunction:error: %s is not made in this experiment (nil)",
           mw_text(mw_safe_function_name(function)));
    if (error != NULL) {
        *error = mw_nserror("compute pipelines are not part of the metalwire placeholder object "
                            "graph");
    }
    return nil;
}

/* Device: the depth-stencil and sampler states are empty placeholders (the label is kept so the
 * object can tell CoreAnimation which descriptor it came from). */
static id mw_device_new_depth_stencil_state(id self, SEL _cmd, id descriptor)
{
    id label = mw_safe_label(descriptor);
    id state;

    (void)self;
    (void)_cmd;
    state = mw_stub_object(gDepthStencilStateClass, "depth stencil state", label);
    if (state != nil) {
        struct mw_stub_state *stub = mw_stub_state_of_object(state);

        mw_render_capture_depth_stencil(stub, descriptor);
        if (stub != NULL) {
            mw_log("newDepthStencilStateWithDescriptor: label %s -> depth-stencil state %u "
                   "(compare %u, write %u)",
                   mw_text(label), stub->remote_id, stub->depth_stencil.depth_compare,
                   stub->depth_stencil.depth_write);
        }
    }
    return state;
}

static id mw_device_new_sampler_state(id self, SEL _cmd, id descriptor)
{
    id label = mw_safe_label(descriptor);
    id state;

    (void)self;
    (void)_cmd;
    state = mw_stub_object(gSamplerStateClass, "sampler state", label);
    if (state != nil) {
        struct mw_stub_state *stub = mw_stub_state_of_object(state);

        mw_render_capture_sampler(stub, descriptor);
        if (stub != NULL) {
            mw_log("newSamplerStateWithDescriptor: label %s -> sampler state %u (min %u mag %u "
                   "mip %u address %u/%u/%u)",
                   mw_text(label), stub->remote_id, stub->sampler.min_filter,
                   stub->sampler.mag_filter, stub->sampler.mip_filter, stub->sampler.address_s,
                   stub->sampler.address_t, stub->sampler.address_r);
        }
    }
    return state;
}

/* -------------------------------------------------------------------------------------------------
 * The pipeline library: CoreAnimation builds one from QuartzCore.framework/default.pipelinelib and
 * asks it for render pipeline states by name. The selector lives on Metal's private
 * _MTLPipelineLibrary, and the inherited _MTLDevice implementation deserializes the file and
 * SIGSEGVs on a path that is not a real pipelinelib; the device override below answers a
 * placeholder instead, so the path is logged and the file is never opened. The pipeline names
 * CoreAnimation asks for are a result of this phase: the first MW_PIPELINE_NAME_LOG_LIMIT distinct
 * names are printed, once each.
 *
 * MTLPipelineLibrary has no public SDK header on iOS 16; this declaration makes the compiled
 * protocol reference the runtime may need (the framework's own object is still preferred by name
 * when it is realized).
 * ---------------------------------------------------------------------------------------------- */

@protocol MTLPipelineLibrary <NSObject>
@property(nullable, copy, nonatomic) NSString *label;
@property(readonly) id<MTLDevice> device;
@end

#define MW_PIPELINE_NAME_LOG_LIMIT 128
static NSMutableSet *gLoggedPipelineNames; /* NSString, under gStubLock */
static unsigned gLoggedPipelineNameCount;
static int gPipelineNameCapLogged;

static struct mw_pipeline_library_state *mw_pipeline_library_state_of(id object)
{
    void **slot;

    if (object == nil || gPipelineLibraryClass == Nil ||
        object_getClass(object) != gPipelineLibraryClass) {
        return NULL;
    }
    slot = mw_slot(object, gPipelineLibraryClass);
    return slot != NULL ? *slot : NULL;
}

static void mw_free_pipeline_library(struct mw_pipeline_library_state *state)
{
    if (state == NULL) {
        return;
    }
    [state->file_path release];
    [state->label release];
    free(state);
}

static id mw_pipeline_library_object(const char *selector_name, id file_path)
{
    struct mw_pipeline_library_state *state;
    id library;
    void **slot;

    if (gPipelineLibraryClass == Nil) {
        mw_log("%s: no pipeline library class is registered", selector_name);
        return nil;
    }
    library = mw_alloc_init(gPipelineLibraryClass, "pipeline library");
    if (library == nil) {
        return nil;
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(library, gPipelineLibraryClass);
    if (state == NULL || slot == NULL) {
        free(state);
        [library release];
        return nil;
    }
    state->stub_id = mw_next_stub_id();
    state->file_path = [file_path copy];
    state->wire.kind = MW_LIBRARY_PIPELINE;
    {
        const char *path_text = file_path != nil && [file_path isKindOfClass:[NSString class]]
                                    ? [(NSString *)file_path fileSystemRepresentation]
                                    : NULL;
        uint64_t hash = 0;
        uint64_t length = 0;

        if (mw_fnv1a64_file(path_text, &hash, &length)) {
            state->wire.hash_known = 1;
            state->wire.hash = hash;
            state->wire.byte_length = length;
            mw_log("%s: %s -> placeholder pipeline library %u (%llu byte(s), hash %016llx; "
                   "the bytes are not parsed)",
                   selector_name, mw_text(file_path), state->stub_id,
                   (unsigned long long)length, (unsigned long long)hash);
        } else {
            mw_log("%s: %s -> placeholder pipeline library %u (the pipelinelib file is never read)",
                   selector_name, mw_text(file_path), state->stub_id);
        }
    }
    *slot = state;
    return library; /* +1, as a Metal "new" method */
}

/* The names CoreAnimation asks a pipeline library for: one line each up to the cap, then one line
 * saying the cap was reached. */
static void mw_note_pipeline_name(id name, const char *kind, NSUInteger options)
{
    BOOL log_first = NO;
    BOOL log_cap = NO;

    if (name == nil) {
        return;
    }
    pthread_mutex_lock(&gStubLock);
    if (gLoggedPipelineNames == nil) {
        gLoggedPipelineNames = [[NSMutableSet alloc] init];
    }
    if (![gLoggedPipelineNames containsObject:name]) {
        [gLoggedPipelineNames addObject:name];
        if (gLoggedPipelineNameCount < MW_PIPELINE_NAME_LOG_LIMIT) {
            gLoggedPipelineNameCount++;
            log_first = YES;
        } else if (!gPipelineNameCapLogged) {
            gPipelineNameCapLogged = 1;
            log_cap = YES;
        }
    }
    pthread_mutex_unlock(&gStubLock);
    if (log_first) {
        mw_log("CA pipeline name: %s (%s, options 0x%lx)", mw_text(name), kind,
               (unsigned long)options);
    } else if (log_cap) {
        mw_log("CA pipeline name: the cap of %d distinct names is reached; further names are not "
               "logged",
               MW_PIPELINE_NAME_LOG_LIMIT);
    }
}

static id mw_pipeline_library_label(id self, SEL _cmd)
{
    struct mw_pipeline_library_state *state = mw_pipeline_library_state_of(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static void mw_pipeline_library_set_label(id self, SEL _cmd, id label)
{
    struct mw_pipeline_library_state *state = mw_pipeline_library_state_of(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
    mw_log("pipeline library: setLabel:%s", mw_text(label));
}

/* The method the last boot faulted in: Apple's inherited implementation dereferences Metal's
 * internal pipeline-library object and dies at 0x10. Here it is one stored bit, logged once. */
static void mw_pipeline_library_set_disable_rtc(id self, SEL _cmd, BOOL disabled)
{
    struct mw_pipeline_library_state *state = mw_pipeline_library_state_of(self);
    static int logged;

    (void)_cmd;
    if (state != NULL) {
        state->disable_run_time_compilation = disabled ? 1 : 0;
    }
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        mw_log("pipeline library: setDisableRunTimeCompilation:%d", disabled ? 1 : 0);
    }
}

static BOOL mw_pipeline_library_disable_rtc(id self, SEL _cmd)
{
    struct mw_pipeline_library_state *state = mw_pipeline_library_state_of(self);

    (void)_cmd;
    return state != NULL && state->disable_run_time_compilation != 0;
}

/* -serialize (a private pipeline-library selector): nothing to serialize here; nil is a legal
 * answer for an object-returning method and 0 is also NO for a BOOL-returning one. */
static id mw_pipeline_library_serialize(id self, SEL _cmd)
{
    static int logged;

    (void)self;
    (void)_cmd;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        mw_log("pipeline library: serialize is a placeholder (nil; nothing is serialized)");
    }
    return nil;
}

/* Any -newSerializedRenderPipelineStateWithName... method the running base has: nil, logged. */
static id mw_pipeline_library_serialized_factory(id self, SEL _cmd)
{
    static int logged;

    (void)self;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        mw_log("pipeline library: -%s is not made in this experiment (nil)", sel_getName(_cmd));
    }
    return nil;
}


static id mw_pipeline_library_device(id self, SEL _cmd)
{
    (void)self;
    (void)_cmd;
    return gDevice;
}

static id mw_pipeline_library_render_pipeline(id self, SEL _cmd, id name, NSUInteger options,
                                              id *reflection, NSError **error)
{
    struct mw_pipeline_library_state *library_state = mw_pipeline_library_state_of(self);
    id pipeline;

    (void)_cmd;
    mw_clear_error(error);
    if (reflection != NULL) {
        *reflection = nil;
    }
    /* WP-4 experiment switch: boot-arg byname=nil makes the precompiled-by-name path fail, so
     * CoreAnimation takes its runtime-specialisation path for the same layer. A real device does
     * fail this call when the pipelinelib has no such pipeline; the interesting question is
     * whether CA then builds one from the base functions this host does have. */
    if (gBynameFail && name != nil) {
        static int said;
        if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED)) {
            mw_log("newRenderPipelineStateWithName: %s fails on purpose (byname=nil): testing "
                   "CoreAnimation's runtime-specialisation fallback", mw_text(name));
        }
        if (error != NULL) {
            *error = mw_nserror("the pipeline library has no such pipeline (byname=nil)");
        }
        return nil;
    }
    if (name == nil || ![name isKindOfClass:[NSString class]]) {
        mw_log("newRenderPipelineStateWithName:options:reflection:error: the name is nil or not a "
               "string; nil");
        if (error != NULL) {
            *error = mw_nserror("the pipeline name is nil or not a string");
        }
        return nil;
    }
    mw_note_pipeline_name(name, "render", options);
    /* the name becomes the state's label, so the render encoder's record says which CA pipeline
     * a draw used; the pipeline state's wire record keeps the same name */
    pipeline = mw_stub_object(gPipelineStateClass, "render pipeline state", name);
    if (pipeline != nil) {
        struct mw_stub_state *pipeline_state = mw_stub_state_of_object(pipeline);

        if (pipeline_state != NULL) {
            const char *utf8 = [(NSString *)name UTF8String];

            if (utf8 != NULL) {
                strncpy(pipeline_state->pipeline.pipeline_name, utf8, MW_NAME_MAX - 1);
            }
            if (library_state != NULL) {
                pipeline_state->vertex_library = &library_state->wire;
                pipeline_state->fragment_library = &library_state->wire;
            }
            mw_log("CA pipeline state %u: %s (serialized by name; no shader yet)",
                   pipeline_state->remote_id, mw_text(name));
        }
    }
    if (pipeline == nil && error != NULL) {
        *error = mw_nserror("the placeholder render pipeline state could not be made");
    }
    return pipeline; /* +1, as a Metal "new" method */
}

static id mw_pipeline_library_compute_pipeline(id self, SEL _cmd, id name, NSUInteger options,
                                               id *reflection, NSError **error)
{
    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    if (reflection != NULL) {
        *reflection = nil;
    }
    if (name != nil && [name isKindOfClass:[NSString class]]) {
        mw_note_pipeline_name(name, "compute", options);
    }
    mw_log("newComputePipelineStateWithName:options:reflection:error: %s is not made in this "
           "experiment (nil)",
           mw_text(name));
    if (error != NULL) {
        *error = mw_nserror("compute pipeline states are not part of the metalwire placeholder "
                            "object graph");
    }
    return nil;
}

/* Whether a private class of the running Metal.framework implements a selector. The by-name
 * descriptor factories are private (no SDK header): they are only overridden when the base class
 * really has them, exactly as the device's SPI methods are treated. */
static BOOL mw_private_class_implements(const char *class_name, const char *selector_name)
{
    Class cls = objc_getClass(class_name);

    if (cls == Nil) {
        return NO;
    }
    return class_getInstanceMethod(cls, sel_registerName(selector_name)) != NULL;
}

static const char *mw_presence(const char *class_name, const char *selector_name)
{
    return mw_private_class_implements(class_name, selector_name) ? "yes" : "no";
}

/* The by-name serialized render-pipeline-state factory has changed spelling between Metal
 * versions; the running base's own method list decides whether there is one to shadow, and the
 * shadow answers nil (logged once) instead of dereferencing the placeholder library. */
static void mw_pipeline_library_override_serialized_factory(Class cls)
{
    static const char prefix[] = "newSerializedRenderPipelineStateWithName";
    Class base = objc_getClass("_MTLPipelineLibrary");
    Method *methods;
    unsigned count = 0;
    int installed = 0;

    if (base == Nil) {
        return;
    }
    methods = class_copyMethodList(base, &count);
    for (unsigned i = 0; i < count; i++) {
        SEL selector = method_getName(methods[i]);
        const char *name = sel_getName(selector);
        const char *types = method_getTypeEncoding(methods[i]);

        if (name == NULL || types == NULL || strncmp(name, prefix, sizeof(prefix) - 1) != 0) {
            continue;
        }
        if (class_addMethod(cls, selector, (IMP)mw_pipeline_library_serialized_factory, types)) {
            mw_log("the _MTLPipelineLibrary base implements -%s; overridden to return nil", name);
            installed++;
        }
    }
    free(methods);
    if (installed == 0) {
        mw_log("the _MTLPipelineLibrary base has no newSerializedRenderPipelineStateWithName... "
               "selector; not overridden");
    }
}

static id mw_pipeline_library_render_descriptor(id self, SEL _cmd, id name, NSError **error)
{
    static int logged;
    MTLRenderPipelineDescriptor *descriptor;

    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    if (name != nil && [name isKindOfClass:[NSString class]]) {
        mw_note_pipeline_name(name, "render-descriptor", 0);
    }
    descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    if (descriptor == nil) {
        if (error != NULL) {
            *error = mw_nserror("the placeholder render pipeline descriptor could not be made");
        }
        return nil;
    }
    if (name != nil && [name isKindOfClass:[NSString class]]) {
        descriptor.label = (NSString *)name;
    }
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED)) {
        mw_log("newRenderPipelineDescriptorWithName:error: answers a real MTLRenderPipelineDescriptor "
               "with no functions (the pipeline library is a placeholder)");
    }
    return descriptor; /* +1, as a Metal "new" method */
}

static id mw_pipeline_library_compute_descriptor(id self, SEL _cmd, id name, NSError **error)
{
    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    if (name != nil && [name isKindOfClass:[NSString class]]) {
        mw_note_pipeline_name(name, "compute-descriptor", 0);
    }
    mw_log("newComputePipelineDescriptorWithName:error: %s is not made in this experiment (nil)",
           mw_text(name));
    if (error != NULL) {
        *error = mw_nserror("compute pipeline descriptors are not part of the metalwire placeholder "
                            "object graph");
    }
    return nil;
}

static id mw_device_new_pipeline_library_file(id self, SEL _cmd, id file_path, NSError **error)
{
    id text = file_path;
    id library;

    (void)self;
    (void)_cmd;
    mw_clear_error(error);
    if (file_path != nil && [file_path isKindOfClass:[NSURL class]]) {
        text = [(NSURL *)file_path path];
    }
    library = mw_pipeline_library_object("newPipelineLibraryWithFilePath:error:", text);
    if (library == nil && error != NULL) {
        *error = mw_nserror("the placeholder pipeline library object could not be made");
    }
    return library;
}

/* -------------------------------------------------------------------------------------------------
 * The render command encoder: CoreAnimation's render calls, recorded and serialized (stage C).
 *
 * -[HakiosMetalCommandBuffer renderCommandEncoderWithDescriptor:] answers this object instead of
 * letting Apple's _MTLCommandBuffer implementation run. Every render-pass field and every set/
 * draw call is copied out of CoreAnimation's objects into a plain C record (the same defensive
 * @try contract the rest of this file uses); at -commit the command buffer sends the encoders'
 * records as protocol-2 render messages, and the host replays them against its Vulkan device.
 * The synthetic metal_frame blit path still never enters this code.
 * ---------------------------------------------------------------------------------------------- */

#define MW_CA_SET_LOG_LIMIT 200
static int gCaSetLogCount;

/* Copies an NSString's UTF-8 into a wire field (zero-padded, MW_NAME_MAX bytes). */
static void mw_copy_name(NSString *text, char *field, size_t size)
{
    const char *utf8;
    size_t length;

    memset(field, 0, size);
    if (text == nil || ![text isKindOfClass:[NSString class]] || size == 0) {
        return;
    }
    utf8 = [text UTF8String];
    if (utf8 == NULL) {
        return;
    }
    length = strlen(utf8);
    if (length >= size) {
        length = size - 1;
    }
    memcpy(field, utf8, length);
}

static int mw_read_uint(id object, const char *selector_name, NSUInteger *value)
{
    SEL selector;

    if (object == nil) {
        return 0;
    }
    selector = sel_registerName(selector_name);
    if (![object respondsToSelector:selector]) {
        return 0;
    }
    *value = ((NSUInteger(*)(id, SEL))objc_msgSend)(object, selector);
    return 1;
}

static int mw_read_bool(id object, const char *selector_name, BOOL *value)
{
    SEL selector;

    if (object == nil) {
        return 0;
    }
    selector = sel_registerName(selector_name);
    if (![object respondsToSelector:selector]) {
        return 0;
    }
    *value = ((BOOL(*)(id, SEL))objc_msgSend)(object, selector);
    return 1;
}

static int mw_read_float(id object, const char *selector_name, float *value)
{
    SEL selector;

    if (object == nil) {
        return 0;
    }
    selector = sel_registerName(selector_name);
    if (![object respondsToSelector:selector]) {
        return 0;
    }
    *value = ((float(*)(id, SEL))objc_msgSend)(object, selector);
    return 1;
}

static id mw_read_object(id object, const char *selector_name)
{
    SEL selector;

    if (object == nil) {
        return nil;
    }
    selector = sel_registerName(selector_name);
    if (![object respondsToSelector:selector]) {
        return nil;
    }
    return ((id(*)(id, SEL))objc_msgSend)(object, selector);
}

static id mw_element(id container, NSUInteger index)
{
    if (container != nil && [container respondsToSelector:@selector(objectAtIndexedSubscript:)]) {
        return ((id(*)(id, SEL, NSUInteger))objc_msgSend)(
            container, @selector(objectAtIndexedSubscript:), index);
    }
    return nil;
}

static void mw_read_clear_color(id attachment, float out[4])
{
    SEL selector = sel_registerName("clearColor");
    MTLClearColor color;

    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (attachment == nil || ![attachment respondsToSelector:selector]) {
        return;
    }
    color = ((MTLClearColor(*)(id, SEL))objc_msgSend)(attachment, selector);
    out[0] = (float)color.red;
    out[1] = (float)color.green;
    out[2] = (float)color.blue;
    out[3] = (float)color.alpha;
}

/* Fills the encoder's render-pass record from CoreAnimation's MTLRenderPassDescriptor. */
static void mw_render_capture_pass(struct mw_render_encoder_state *state, id descriptor)
{
    struct mw_render_begin *begin = &state->begin;
    NSUInteger value;

    @try {
        id attachments = mw_read_object(descriptor, "colorAttachments");
        id attachment = mw_element(attachments, 0);
        id texture = mw_read_object(attachment, "texture");
        struct mw_texture_state *texture_state = mw_texture_state_of(texture);
        id depth = mw_read_object(descriptor, "depthAttachment");
        id depth_texture = mw_read_object(depth, "texture");
        struct mw_texture_state *depth_state = mw_texture_state_of(depth_texture);

        if (texture_state != NULL) {
            state->target = [texture retain];
            begin->target_texture = texture_state->remote_id;
            begin->width = texture_state->width;
            begin->height = texture_state->height;
            begin->pixel_format = texture_state->pixel_format;
        }
        if (mw_read_uint(attachment, "loadAction", &value)) {
            begin->load_action = (uint32_t)value;
        }
        if (mw_read_uint(attachment, "storeAction", &value)) {
            begin->store_action = (uint32_t)value;
        }
        mw_read_clear_color(attachment, begin->clear_color);
        if (depth_state != NULL) {
            state->depth_target = [depth_texture retain];
            begin->has_depth = 1;
            begin->depth_texture = depth_state->remote_id;
        }
        if (mw_read_uint(depth, "loadAction", &value)) {
            begin->depth_load_action = (uint32_t)value;
        }
        if (mw_read_uint(depth, "storeAction", &value)) {
            begin->depth_store_action = (uint32_t)value;
        }
        begin->clear_depth = 1.0f;
        {
            SEL selector = sel_registerName("clearDepth");

            if (depth != nil && [depth respondsToSelector:selector]) {
                begin->clear_depth = (float)((double(*)(id, SEL))objc_msgSend)(depth, selector);
            }
        }
    } @catch (NSException *exception) {
        mw_log("renderCommandEncoderWithDescriptor: reading the pass raised %s; recorded empty",
               mw_text([exception reason]));
    }
    mw_log("CA render pass: encoder %u target texture %u %ux%u format %u load %u store %u "
           "clear %.2f,%.2f,%.2f,%.2f%s",
           state->encoder_index, begin->target_texture, begin->width, begin->height,
           begin->pixel_format, begin->load_action, begin->store_action, begin->clear_color[0],
           begin->clear_color[1], begin->clear_color[2], begin->clear_color[3],
           begin->has_depth ? " (depth attachment present)" : "");
}

/* Fills a render pipeline state's wire record from its MTLRenderPipelineDescriptor. */
static void mw_render_capture_pipeline(struct mw_stub_state *state, id descriptor)
{
    NSUInteger value;
    BOOL flag;
    id color_attachment;
    id vertex_descriptor;
    id attributes;
    id layouts;

    if (state == NULL || descriptor == nil) {
        return;
    }
    state->captured = 1;
    state->pipeline.id = state->remote_id;
    @try {
        if (state->label != nil) {
            mw_copy_name((NSString *)state->label, state->pipeline.pipeline_name, MW_NAME_MAX);
        }
        if (mw_read_uint(descriptor, "sampleCount", &value)) {
            state->pipeline.sample_count = (uint32_t)value;
        }
        mw_copy_name(mw_safe_function_name(mw_read_object(descriptor, "vertexFunction")),
                     state->pipeline.vertex_function, MW_NAME_MAX);
        mw_copy_name(mw_safe_function_name(mw_read_object(descriptor, "fragmentFunction")),
                     state->pipeline.fragment_function, MW_NAME_MAX);
        {
            struct mw_function_state *vertex_state =
                mw_function_state_of(mw_read_object(descriptor, "vertexFunction"));
            struct mw_function_state *fragment_state =
                mw_function_state_of(mw_read_object(descriptor, "fragmentFunction"));

            state->vertex_library = vertex_state != NULL ? vertex_state->library : NULL;
            state->fragment_library = fragment_state != NULL ? fragment_state->library : NULL;
        }
        color_attachment = mw_element(mw_read_object(descriptor, "colorAttachments"), 0);
        if (mw_read_uint(color_attachment, "pixelFormat", &value)) {
            state->pipeline.color_format = (uint32_t)value;
        }
        if (mw_read_bool(color_attachment, "blendingEnabled", &flag)) {
            state->pipeline.blend_enabled = flag ? 1 : 0;
        }
        if (mw_read_uint(color_attachment, "rgbBlendOperation", &value)) {
            state->pipeline.rgb_blend_operation = (uint32_t)value;
        }
        if (mw_read_uint(color_attachment, "alphaBlendOperation", &value)) {
            state->pipeline.alpha_blend_operation = (uint32_t)value;
        }
        if (mw_read_uint(color_attachment, "sourceRGBBlendFactor", &value)) {
            state->pipeline.src_rgb_blend_factor = (uint32_t)value;
        }
        if (mw_read_uint(color_attachment, "destinationRGBBlendFactor", &value)) {
            state->pipeline.dst_rgb_blend_factor = (uint32_t)value;
        }
        if (mw_read_uint(color_attachment, "sourceAlphaBlendFactor", &value)) {
            state->pipeline.src_alpha_blend_factor = (uint32_t)value;
        }
        if (mw_read_uint(color_attachment, "destinationAlphaBlendFactor", &value)) {
            state->pipeline.dst_alpha_blend_factor = (uint32_t)value;
        }
        if (mw_read_uint(color_attachment, "writeMask", &value)) {
            state->pipeline.write_mask = (uint32_t)value;
        }
        vertex_descriptor = mw_read_object(descriptor, "vertexDescriptor");
        attributes = mw_read_object(vertex_descriptor, "attributes");
        layouts = mw_read_object(vertex_descriptor, "layouts");
        for (NSUInteger i = 0; i < MW_MAX_RENDER_ATTRIBUTES; i++) {
            id attribute = mw_element(attributes, i);
            struct mw_vertex_attribute *out;

            if (attribute == nil) {
                break;
            }
            out = &state->attributes[state->pipeline.vertex_attribute_count];
            if (mw_read_uint(attribute, "format", &value)) {
                out->format = (uint32_t)value;
            }
            if (mw_read_uint(attribute, "offset", &value)) {
                out->offset = (uint32_t)value;
            }
            if (mw_read_uint(attribute, "bufferIndex", &value)) {
                out->buffer_index = (uint32_t)value;
            }
            state->pipeline.vertex_attribute_count++;
        }
        for (NSUInteger i = 0; i < MW_MAX_RENDER_LAYOUTS; i++) {
            id layout = mw_element(layouts, i);
            struct mw_vertex_layout *out;

            if (layout == nil) {
                break;
            }
            out = &state->layouts[state->pipeline.vertex_layout_count];
            if (mw_read_uint(layout, "stride", &value)) {
                out->stride = (uint32_t)value;
            }
            if (mw_read_uint(layout, "stepFunction", &value)) {
                out->step_function = (uint32_t)value;
            }
            if (mw_read_uint(layout, "stepRate", &value)) {
                out->step_rate = (uint32_t)value;
            }
            state->pipeline.vertex_layout_count++;
        }
    } @catch (NSException *exception) {
        mw_log("render pipeline: reading the descriptor raised %s; the state is recorded empty",
               mw_text([exception reason]));
    }
}

static void mw_render_capture_sampler(struct mw_stub_state *state, id descriptor)
{
    NSUInteger value;
    float number;
    BOOL flag;

    if (state == NULL || descriptor == nil) {
        return;
    }
    state->captured = 1;
    @try {
        if (mw_read_uint(descriptor, "minFilter", &value)) {
            state->sampler.min_filter = (uint32_t)value;
        }
        if (mw_read_uint(descriptor, "magFilter", &value)) {
            state->sampler.mag_filter = (uint32_t)value;
        }
        if (mw_read_uint(descriptor, "mipFilter", &value)) {
            state->sampler.mip_filter = (uint32_t)value;
        }
        if (mw_read_uint(descriptor, "sAddressMode", &value)) {
            state->sampler.address_s = (uint32_t)value;
        }
        if (mw_read_uint(descriptor, "tAddressMode", &value)) {
            state->sampler.address_t = (uint32_t)value;
        }
        if (mw_read_uint(descriptor, "rAddressMode", &value)) {
            state->sampler.address_r = (uint32_t)value;
        }
        if (mw_read_bool(descriptor, "normalizedCoordinates", &flag)) {
            state->sampler.normalized_coords = flag ? 1 : 0;
        }
        if (mw_read_uint(descriptor, "compareFunction", &value)) {
            state->sampler.compare_function = (uint32_t)value;
        }
        if (mw_read_float(descriptor, "lodMinClamp", &number)) {
            state->sampler.lod_min = number;
        }
        if (mw_read_float(descriptor, "lodMaxClamp", &number)) {
            state->sampler.lod_max = number;
        }
        if (mw_read_uint(descriptor, "maxAnisotropy", &value)) {
            state->sampler.max_anisotropy = (uint32_t)value;
        }
    } @catch (NSException *exception) {
        mw_log("sampler state: reading the descriptor raised %s; the state is recorded empty",
               mw_text([exception reason]));
    }
}

static void mw_render_capture_depth_stencil(struct mw_stub_state *state, id descriptor)
{
    NSUInteger value;
    BOOL flag;

    if (state == NULL || descriptor == nil) {
        return;
    }
    state->captured = 1;
    @try {
        if (mw_read_uint(descriptor, "depthCompareFunction", &value)) {
            state->depth_stencil.depth_compare = (uint32_t)value;
        }
        if (mw_read_bool(descriptor, "depthWriteEnabled", &flag)) {
            state->depth_stencil.depth_write = flag ? 1 : 0;
        }
    } @catch (NSException *exception) {
        mw_log("depth-stencil state: reading the descriptor raised %s; recorded empty",
               mw_text([exception reason]));
    }
}

/* The encoder's op array: append one operation, or count a dropped one. */
static struct mw_render_op *mw_render_op_new(struct mw_render_encoder_state *state, uint16_t kind)
{
    struct mw_render_op *op;

    if (state == NULL || state->ended) {
        return NULL;
    }
    if (state->op_count >= MW_RENDER_MAX_OPS) {
        state->dropped_count++;
        return NULL;
    }
    op = &state->ops[state->op_count++];
    memset(op, 0, sizeof(*op));
    op->kind = kind;
    return op;
}

static void mw_render_op_take(struct mw_render_op *op, id object)
{
    if (op != NULL && object != nil) {
        op->object = [object retain];
    }
}

/* One line per recorded call (bounded), so the boot log names every set as it is serialized. */
static void mw_render_log_set(const char *what, id object, NSUInteger index)
{
    if (__atomic_fetch_add(&gCaSetLogCount, 1, __ATOMIC_RELAXED) >= MW_CA_SET_LOG_LIMIT) {
        return;
    }
    if (object != nil) {
        mw_log("CA set: %s %s (index %lu)", what, mw_text(mw_safe_label(object)),
               (unsigned long)index);
    } else {
        mw_log("CA set: %s (index %lu)", what, (unsigned long)index);
    }
}

static NSMutableSet *gLoggedDrawCombinations; /* NSString "pipeline/primitive", under gStubLock */

static struct mw_render_encoder_state *mw_render_encoder_state_of(id object)
{
    void **slot;

    if (object == nil || gRenderEncoderClass == Nil ||
        object_getClass(object) != gRenderEncoderClass) {
        return NULL;
    }
    slot = mw_slot(object, gRenderEncoderClass);
    return slot != NULL ? *slot : NULL;
}

static void mw_release_render_op(struct mw_render_op *op)
{
    [op->object release];
    [op->object2 release];
    free(op->bytes);
    memset(op, 0, sizeof(*op));
}

static void mw_free_render_encoder(struct mw_render_encoder_state *state)
{
    if (state == NULL) {
        return;
    }
    for (int i = 0; i < state->op_count; i++) {
        mw_release_render_op(&state->ops[i]);
    }
    [state->label release];
    [state->target release];
    [state->depth_target release];
    free(state);
}

static id mw_cb_render_encoder(id self, SEL _cmd, id descriptor)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);
    struct mw_render_encoder_state *encoder_state;
    id encoder;
    void **slot;
    int reused = 0;

    (void)_cmd;
    (void)descriptor;
    if (state == NULL || state->committed) {
        mw_log("renderCommandEncoderWithDescriptor: no command buffer or already committed; nil");
        return nil;
    }
    if (gRenderEncoderClass == Nil) {
        mw_log("renderCommandEncoderWithDescriptor: no render encoder class is registered; nil");
        return nil;
    }
    encoder = mw_pool_take_free(&gEncoderPool, gRenderEncoderClass);
    if (encoder != nil) {
        void **cached = mw_slot(encoder, gRenderEncoderClass);

        if (cached != NULL && *cached != NULL) {
            mw_free_render_encoder(*cached);
            *cached = NULL;
        }
        mw_lifecycle_note(gRenderEncoderClass, MW_LIFE_REUSED);
        reused = 1;
    } else {
        encoder = mw_alloc_init(gRenderEncoderClass, "render command encoder");
        if (encoder == nil) {
            return nil;
        }
        mw_pool_remember(&gEncoderPool, encoder);
    }
    encoder_state = calloc(1, sizeof(*encoder_state));
    slot = mw_slot(encoder, gRenderEncoderClass);
    if (encoder_state == NULL || slot == NULL) {
        free(encoder_state);
        if (!reused) {
            [encoder release];
        }
        return nil;
    }
    encoder_state->command_buffer = self;
    encoder_state->encoder_index = (uint32_t)state->render_encoder_count;
    mw_render_capture_pass(encoder_state, descriptor);
    *slot = encoder_state;
    state->render_encoder_count++;
    return reused ? [[encoder retain] autorelease] : [encoder autorelease];
}

static void mw_render_note_set(struct mw_render_encoder_state *state)
{
    if (state != NULL) {
        state->set_count++;
    }
}

static void mw_render_set_pipeline(id self, SEL _cmd, id pipeline)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    if (state == NULL) {
        return;
    }
    state->pipeline = pipeline; /* not retained: the client keeps it alive while encoding */
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_PIPELINE);
    mw_render_op_take(op, pipeline);
    mw_log("CA set: setRenderPipelineState: %s", mw_text(mw_safe_label(pipeline)));
}

static void mw_render_set_depth_stencil(id self, SEL _cmd, id depth_stencil)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_DEPTH_STENCIL);
    mw_render_op_take(op, depth_stencil);
    mw_log("CA set: setDepthStencilState: %s", mw_text(mw_safe_label(depth_stencil)));
}

static void mw_render_set_cull_mode(id self, SEL _cmd, NSUInteger cull_mode)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_CULL_MODE);
    if (op != NULL) {
        op->values[0] = (uint32_t)cull_mode;
    }
    mw_log("CA set: setCullMode: %lu", (unsigned long)cull_mode);
}

static void mw_render_set_winding(id self, SEL _cmd, NSUInteger winding)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_WINDING);
    if (op != NULL) {
        op->values[0] = (uint32_t)winding;
    }
    mw_log("CA set: setFrontFacingWinding: %lu", (unsigned long)winding);
}

static void mw_render_set_fill_mode(id self, SEL _cmd, NSUInteger fill_mode)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_FILL_MODE);
    if (op != NULL) {
        op->values[0] = (uint32_t)fill_mode;
    }
    mw_log("CA set: setTriangleFillMode: %lu", (unsigned long)fill_mode);
}

static void mw_render_set_viewport(id self, SEL _cmd, MTLViewport viewport)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_VIEWPORT);
    if (op != NULL) {
        op->fvalues[0] = (float)viewport.originX;
        op->fvalues[1] = (float)viewport.originY;
        op->fvalues[2] = (float)viewport.width;
        op->fvalues[3] = (float)viewport.height;
        op->fvalues[4] = (float)viewport.znear;
        op->fvalues[5] = (float)viewport.zfar;
    }
    mw_log("CA set: setViewport: %.1fx%.1f at %.1f,%.1f", viewport.width, viewport.height,
           viewport.originX, viewport.originY);
}

static void mw_render_set_scissor(id self, SEL _cmd, MTLScissorRect rect)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_SCISSOR);
    if (op != NULL) {
        op->values[0] = (uint32_t)rect.x;
        op->values[1] = (uint32_t)rect.y;
        op->values[2] = (uint32_t)rect.width;
        op->values[3] = (uint32_t)rect.height;
    }
    mw_log("CA set: setScissorRect: %lux%lu at %lu,%lu", (unsigned long)rect.width,
           (unsigned long)rect.height, (unsigned long)rect.x, (unsigned long)rect.y);
}

static void mw_render_set_blend_color(id self, SEL _cmd, float red, float green, float blue,
                                      float alpha)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_BLEND_COLOR);
    if (op != NULL) {
        op->fvalues[0] = red;
        op->fvalues[1] = green;
        op->fvalues[2] = blue;
        op->fvalues[3] = alpha;
    }
    mw_log("CA set: setBlendColorRed:%.3f green:%.3f blue:%.3f alpha:%.3f", red, green, blue,
           alpha);
}

static void mw_render_set_stencil_reference(id self, SEL _cmd, uint32_t reference)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_STENCIL_REFERENCE);
    if (op != NULL) {
        op->values[0] = reference;
    }
    mw_log("CA set: setStencilReferenceValue: %u", reference);
}

static void mw_render_set_vertex_buffer(id self, SEL _cmd, id buffer, NSUInteger offset,
                                        NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_VERTEX_BUFFER);
    if (op != NULL) {
        op->index = (uint32_t)index;
        op->value64 = offset;
        mw_render_op_take(op, buffer);
    }
    mw_render_log_set("setVertexBuffer:offset:atIndex:", buffer, index);
}

static void mw_render_set_vertex_buffer_offset(id self, SEL _cmd, NSUInteger offset,
                                               NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_VERTEX_BUFFER_OFFSET);
    if (op != NULL) {
        op->index = (uint32_t)index;
        op->value64 = offset;
    }
    mw_log("CA set: setVertexBufferOffset:%lu atIndex:%lu", (unsigned long)offset,
           (unsigned long)index);
}

static void mw_render_set_vertex_bytes(id self, SEL _cmd, const void *bytes, NSUInteger length,
                                       NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_VERTEX_BYTES);
    if (op != NULL) {
        op->stage = MW_STAGE_VERTEX;
        op->index = (uint32_t)index;
        op->length = (uint32_t)length;
        if (bytes != NULL && length != 0) {
            op->bytes = malloc((size_t)length);
            if (op->bytes != NULL) {
                memcpy(op->bytes, bytes, (size_t)length);
            }
        }
    }
    mw_render_log_set("setVertexBytes:length:atIndex:", nil, index);
}

static void mw_render_set_vertex_texture(id self, SEL _cmd, id texture, NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_VERTEX_TEXTURE);
    if (op != NULL) {
        op->index = (uint32_t)index;
        mw_render_op_take(op, texture);
    }
    mw_render_log_set("setVertexTexture:atIndex:", texture, index);
}

static void mw_render_set_vertex_sampler(id self, SEL _cmd, id sampler, NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_VERTEX_SAMPLER);
    if (op != NULL) {
        op->index = (uint32_t)index;
        mw_render_op_take(op, sampler);
    }
    mw_render_log_set("setVertexSamplerState:atIndex:", sampler, index);
}

static void mw_render_set_vertex_sampler_lod(id self, SEL _cmd, id sampler, float lod_min,
                                             float lod_max, NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_VERTEX_SAMPLER);
    if (op != NULL) {
        op->index = (uint32_t)index;
        op->fvalues[0] = lod_min;
        op->fvalues[1] = lod_max;
        mw_render_op_take(op, sampler);
    }
    mw_render_log_set("setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:", sampler, index);
}

static void mw_render_set_fragment_buffer(id self, SEL _cmd, id buffer, NSUInteger offset,
                                          NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_FRAGMENT_BUFFER);
    if (op != NULL) {
        op->index = (uint32_t)index;
        op->value64 = offset;
        mw_render_op_take(op, buffer);
    }
    mw_render_log_set("setFragmentBuffer:offset:atIndex:", buffer, index);
}

static void mw_render_set_fragment_buffer_offset(id self, SEL _cmd, NSUInteger offset,
                                                 NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_FRAGMENT_BUFFER_OFFSET);
    if (op != NULL) {
        op->index = (uint32_t)index;
        op->value64 = offset;
    }
    mw_log("CA set: setFragmentBufferOffset:%lu atIndex:%lu", (unsigned long)offset,
           (unsigned long)index);
}

static void mw_render_set_fragment_bytes(id self, SEL _cmd, const void *bytes, NSUInteger length,
                                         NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_FRAGMENT_BYTES);
    if (op != NULL) {
        op->stage = MW_STAGE_FRAGMENT;
        op->index = (uint32_t)index;
        op->length = (uint32_t)length;
        if (bytes != NULL && length != 0) {
            op->bytes = malloc((size_t)length);
            if (op->bytes != NULL) {
                memcpy(op->bytes, bytes, (size_t)length);
            }
        }
    }
    mw_render_log_set("setFragmentBytes:length:atIndex:", nil, index);
}

static void mw_render_set_fragment_texture(id self, SEL _cmd, id texture, NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_FRAGMENT_TEXTURE);
    if (op != NULL) {
        op->index = (uint32_t)index;
        mw_render_op_take(op, texture);
    }
    mw_render_log_set("setFragmentTexture:atIndex:", texture, index);
}

static void mw_render_set_fragment_sampler(id self, SEL _cmd, id sampler, NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_FRAGMENT_SAMPLER);
    if (op != NULL) {
        op->index = (uint32_t)index;
        mw_render_op_take(op, sampler);
    }
    mw_render_log_set("setFragmentSamplerState:atIndex:", sampler, index);
}

static void mw_render_set_fragment_sampler_lod(id self, SEL _cmd, id sampler, float lod_min,
                                               float lod_max, NSUInteger index)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_render_op *op;

    (void)_cmd;
    mw_render_note_set(state);
    op = mw_render_op_new(state, MWR_OP_SET_FRAGMENT_SAMPLER);
    if (op != NULL) {
        op->index = (uint32_t)index;
        op->fvalues[0] = lod_min;
        op->fvalues[1] = lod_max;
        mw_render_op_take(op, sampler);
    }
    mw_render_log_set("setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:", sampler, index);
}

static void mw_render_push_debug_group(id self, SEL _cmd, id name)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);

    (void)_cmd;
    (void)state;
    mw_log("CA set: pushDebugGroup: %s (not a draw state; not serialized)", mw_text(name));
}

static void mw_render_pop_debug_group(id self, SEL _cmd)
{
    (void)_cmd;
    mw_log("CA set: popDebugGroup (not a draw state; not serialized)");
}

/* One draw: the state snapshot is the encoder's ordered op stream, so a draw only needs its own
 * arguments plus the index buffer; the pipeline is the one most recently bound. */
static void mw_render_record_draw(id self, unsigned long primitive_type, unsigned long vertex_start,
                                  unsigned long vertex_count, unsigned long index_count,
                                  NSUInteger index_type, id index_buffer,
                                  NSUInteger index_buffer_offset, NSUInteger instance_count,
                                  NSInteger base_vertex, NSUInteger base_instance, int indexed)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_command_buffer_state *command_buffer;
    struct mw_render_op *op;
    struct mw_render_draw *draw;
    id pipeline_label;
    NSString *key;
    BOOL first = NO;

    if (state == NULL || state->ended) {
        return;
    }
    command_buffer = mw_command_buffer_state_of(state->command_buffer);
    pipeline_label = mw_safe_label(state->pipeline);
    state->draw_count++;
    if (command_buffer != NULL) {
        command_buffer->render_draw_count++;
    }
    op = mw_render_op_new(state, MWR_OP_DRAW);
    if (op == NULL) {
        return;
    }
    draw = &op->draw;
    draw->primitive_type = (uint32_t)primitive_type;
    draw->flags = indexed ? MW_DRAW_INDEXED : 0;
    if (instance_count > 1) {
        draw->flags |= MW_DRAW_INSTANCED;
    }
    if (indexed) {
        draw->index_type = (uint32_t)index_type;
        draw->index_count = index_count;
        draw->index_buffer_offset = index_buffer_offset;
        op->object2 = [index_buffer retain];
    } else {
        draw->vertex_start = vertex_start;
        draw->vertex_count = vertex_count;
    }
    if (base_vertex > 0) {
        draw->flags |= MW_DRAW_BASE_VERTEX;
        draw->base_vertex = base_vertex;
    }
    if (base_instance > 0) {
        draw->flags |= MW_DRAW_BASE_INSTANCE;
        draw->base_instance = (uint32_t)base_instance;
    }
    draw->instance_count = (uint32_t)instance_count;
    mw_render_op_take(op, state->pipeline);

    key = [[NSString alloc] initWithFormat:@"%s/%lu", mw_text(pipeline_label), primitive_type];
    pthread_mutex_lock(&gStubLock);
    if (gLoggedDrawCombinations == nil) {
        gLoggedDrawCombinations = [[NSMutableSet alloc] init];
    }
    if (![gLoggedDrawCombinations containsObject:key] &&
        [gLoggedDrawCombinations count] < 256) {
        [gLoggedDrawCombinations addObject:key];
        first = YES;
    }
    pthread_mutex_unlock(&gStubLock);
    [key release];
    if (first) {
        mw_log("CA draw: pipeline %s prim %lu %s (vertices %lu, indices %lu, instances %lu; "
               "serialized)",
               mw_text(pipeline_label), primitive_type, indexed ? "indexed" : "non-indexed",
               vertex_count, index_count, instance_count);
    }
}

static void mw_render_end_encoding(id self, SEL _cmd)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);
    struct mw_command_buffer_state *command_buffer;
    struct mw_render_op *end;
    id pipeline_label;

    (void)_cmd;
    if (state == NULL || state->ended) {
        return;
    }
    state->ended = 1;
    end = mw_render_op_new(state, MWR_OP_END);
    if (end != NULL) {
        end->values[0] = (uint32_t)state->draw_count;
        end->values[1] = (uint32_t)state->set_count;
    }
    pipeline_label = mw_safe_label(state->pipeline);
    if (state->dropped_count != 0) {
        mw_log("CA render encoder: %d recorded calls dropped (cap %d reached)", state->dropped_count,
               MW_RENDER_MAX_OPS);
    }
    command_buffer = mw_command_buffer_state_of(state->command_buffer);
    if (command_buffer == NULL) {
        return;
    }
    if (command_buffer->render_encoder_stored < MW_MAX_RENDER_ENCODERS) {
        command_buffer->render_encoders[command_buffer->render_encoder_stored++] = [self retain];
    } else {
        mw_log("CA render encoder: more than %d encoders in one command buffer; not recorded",
               MW_MAX_RENDER_ENCODERS);
        return;
    }
    if (state->draw_count != 0) {
        mw_log("CA render encoder: %llu draws, %llu sets, pipeline %s (serialized at commit)",
               (unsigned long long)state->draw_count, (unsigned long long)state->set_count,
               mw_text(pipeline_label));
    } else {
        mw_log("CA render encoder: 0 draws, %llu sets, pipeline %s (serialized at commit)",
               (unsigned long long)state->set_count, mw_text(pipeline_label));
    }
}

static id mw_render_label(id self, SEL _cmd)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);

    (void)_cmd;
    return state != NULL ? state->label : nil;
}

static void mw_render_set_label(id self, SEL _cmd, id label)
{
    struct mw_render_encoder_state *state = mw_render_encoder_state_of(self);

    (void)_cmd;
    if (state != NULL && state->label != label) {
        [state->label release];
        state->label = [label copy];
    }
}

static void mw_render_draw_primitives(id self, SEL _cmd, NSUInteger primitive_type,
                                      NSUInteger vertex_start, NSUInteger vertex_count)
{
    (void)_cmd;
    mw_render_record_draw(self, (unsigned long)primitive_type, (unsigned long)vertex_start,
                          (unsigned long)vertex_count, 0, 0, nil, 0, 1, 0, 0, 0);
}

static void mw_render_draw_primitives_instanced(id self, SEL _cmd, NSUInteger primitive_type,
                                                NSUInteger vertex_start, NSUInteger vertex_count,
                                                NSUInteger instance_count)
{
    (void)_cmd;
    mw_render_record_draw(self, (unsigned long)primitive_type, (unsigned long)vertex_start,
                          (unsigned long)vertex_count, 0, 0, nil, 0, (unsigned long)instance_count,
                          0, 0, 0);
}

static void mw_render_draw_indexed(id self, SEL _cmd, NSUInteger primitive_type,
                                   NSUInteger index_count, NSUInteger index_type, id index_buffer,
                                   NSUInteger index_buffer_offset)
{
    (void)_cmd;
    mw_render_record_draw(self, (unsigned long)primitive_type, 0, 0,
                          (unsigned long)index_count, index_type, index_buffer,
                          index_buffer_offset, 1, 0, 0, 1);
}

static void mw_render_draw_indexed_instanced(id self, SEL _cmd, NSUInteger primitive_type,
                                             NSUInteger index_count, NSUInteger index_type,
                                             id index_buffer, NSUInteger index_buffer_offset,
                                             NSUInteger instance_count)
{
    (void)_cmd;
    mw_render_record_draw(self, (unsigned long)primitive_type, 0, 0,
                          (unsigned long)index_count, index_type, index_buffer,
                          index_buffer_offset, (unsigned long)instance_count, 0, 0, 1);
}

static void mw_render_draw_indexed_base(id self, SEL _cmd, NSUInteger primitive_type,
                                        NSUInteger index_count, NSUInteger index_type,
                                        id index_buffer, NSUInteger index_buffer_offset,
                                        NSUInteger instance_count, NSInteger base_vertex,
                                        NSUInteger base_instance)
{
    (void)_cmd;
    mw_render_record_draw(self, (unsigned long)primitive_type, 0, 0,
                          (unsigned long)index_count, index_type, index_buffer,
                          index_buffer_offset, (unsigned long)instance_count, base_vertex,
                          base_instance, 1);
}

/* -------------------------------------------------------------------------------------------------
 * Serializing a recorded render frame (protocol 2). Called with gWireLock held.
 *
 * The order is the encoder's op order, with object creation hoisted: a buffer is created and
 * uploaded, a texture created, a pipeline/sampler/depth-stencil state sent, each on its first use
 * in this connection. The frame's picture is its last render pass's color attachment; the guest
 * cannot hash it (it did not draw it), so MW_FRAME_END carries crc 0 and the host answers without
 * a read-back until its execution half exists.
 * ---------------------------------------------------------------------------------------------- */

static int mw_send_render_buffer(struct mw_command_buffer_state *state, id object)
{
    struct mw_buffer_state *buffer = mw_buffer_state_of(object);

    if (buffer == NULL) {
        return -1;
    }
    if (buffer->uploaded_seq == state->seq) {
        return 0;
    }
    if (mw_wire_ensure_buffer(buffer) != 0) {
        return -1;
    }
    if (mw_send_buffer_upload(buffer) != 0) {
        return -1;
    }
    buffer->uploaded_seq = state->seq;
    return 0;
}

static int mw_send_render_frame(struct mw_command_buffer_state *state)
{
    struct mw_frame_begin begin;
    struct mw_frame_end end;
    struct mw_texture_state *last_target = NULL;

    begin.seq = state->seq;
    begin.guest_t_begin_ns = state->t_begin_ns;
    if (mw_send(&gWire, MW_FRAME_BEGIN, &begin, sizeof(begin)) != 0) {
        return -1;
    }
    for (int e = 0; e < state->render_encoder_stored; e++) {
        struct mw_render_encoder_state *encoder =
            mw_render_encoder_state_of(state->render_encoders[e]);
        struct mw_texture_state *target;

        if (encoder == NULL) {
            continue;
        }
        target = mw_texture_state_of(encoder->target);
        if (target == NULL) {
            mw_log("render frame: encoder %d has no serializable render target", e);
            return -1;
        }
        if (mw_wire_ensure_texture(target) != 0) {
            return -1;
        }
        if (encoder->begin.has_depth && encoder->depth_target != nil) {
            struct mw_texture_state *depth = mw_texture_state_of(encoder->depth_target);

            if (depth != NULL && mw_wire_ensure_texture(depth) != 0) {
                return -1;
            }
        }
        if (mw_send(&gWire, MW_RENDER_BEGIN, &encoder->begin, sizeof(encoder->begin)) != 0) {
            return -1;
        }
        for (int i = 0; i < encoder->op_count; i++) {
            struct mw_render_op *op = &encoder->ops[i];

            switch (op->kind) {
            case MWR_OP_SET_PIPELINE: {
                struct mw_stub_state *pipeline = mw_stub_state_of_object(op->object);
                struct mw_render_set_pipeline message;

                if (mw_wire_ensure_pipeline(pipeline) != 0) {
                    return -1;
                }
                message.pipeline_id = pipeline->remote_id;
                message.reserved = 0;
                if (mw_send(&gWire, MW_RENDER_SET_PIPELINE, &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_DEPTH_STENCIL: {
                struct mw_stub_state *depth = mw_stub_state_of_object(op->object);
                struct mw_render_set_depth_stencil message;

                if (depth != NULL && mw_wire_ensure_depth_stencil(depth) != 0) {
                    return -1;
                }
                message.state_id = depth != NULL ? depth->remote_id : 0;
                message.reserved = 0;
                if (mw_send(&gWire, MW_RENDER_SET_DEPTH_STENCIL, &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_VERTEX_BUFFER:
            case MWR_OP_SET_FRAGMENT_BUFFER: {
                struct mw_buffer_state *buffer = mw_buffer_state_of(op->object);
                struct mw_render_set_buffer message;

                if (mw_send_render_buffer(state, op->object) != 0 || buffer == NULL) {
                    return -1;
                }
                message.buffer_id = buffer->remote_id;
                message.index = op->index;
                message.offset = op->value64;
                if (mw_send(&gWire,
                            op->kind == MWR_OP_SET_VERTEX_BUFFER ? MW_RENDER_SET_VERTEX_BUFFER
                                                                 : MW_RENDER_SET_FRAGMENT_BUFFER,
                            &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_VERTEX_BUFFER_OFFSET:
            case MWR_OP_SET_FRAGMENT_BUFFER_OFFSET: {
                struct mw_render_set_buffer_offset message;

                message.index = op->index;
                message.reserved = 0;
                message.offset = op->value64;
                if (mw_send(&gWire,
                            op->kind == MWR_OP_SET_VERTEX_BUFFER_OFFSET
                                ? MW_RENDER_SET_VERTEX_BUFFER_OFFSET
                                : MW_RENDER_SET_FRAGMENT_BUFFER_OFFSET,
                            &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_VERTEX_BYTES:
            case MWR_OP_SET_FRAGMENT_BYTES: {
                struct mw_render_set_bytes message;

                message.index = op->index;
                message.length = op->length;
                message.stage = op->stage;
                message.reserved = 0;
                if (mw_send_with_body(&gWire,
                                      op->kind == MWR_OP_SET_VERTEX_BYTES
                                          ? MW_RENDER_SET_VERTEX_BYTES
                                          : MW_RENDER_SET_FRAGMENT_BYTES,
                                      &message, sizeof(message), op->bytes,
                                      op->length) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_VERTEX_TEXTURE:
            case MWR_OP_SET_FRAGMENT_TEXTURE: {
                struct mw_texture_state *texture = mw_texture_state_of(op->object);
                struct mw_render_set_texture message;

                if (texture == NULL) {
                    return -1;
                }
                if (mw_wire_ensure_texture(texture) != 0) {
                    return -1;
                }
                message.texture_id = texture->remote_id;
                message.index = op->index;
                if (mw_send(&gWire,
                            op->kind == MWR_OP_SET_VERTEX_TEXTURE ? MW_RENDER_SET_VERTEX_TEXTURE
                                                                  : MW_RENDER_SET_FRAGMENT_TEXTURE,
                            &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_VERTEX_SAMPLER:
            case MWR_OP_SET_FRAGMENT_SAMPLER: {
                struct mw_stub_state *sampler = mw_stub_state_of_object(op->object);
                struct mw_render_set_sampler message;

                if (sampler != NULL && mw_wire_ensure_sampler(sampler) != 0) {
                    return -1;
                }
                message.sampler_id = sampler != NULL ? sampler->remote_id : 0;
                message.index = op->index;
                if (mw_send(&gWire,
                            op->kind == MWR_OP_SET_VERTEX_SAMPLER ? MW_RENDER_SET_VERTEX_SAMPLER
                                                                  : MW_RENDER_SET_FRAGMENT_SAMPLER,
                            &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_VIEWPORT: {
                struct mw_render_set_viewport message;

                message.x = op->fvalues[0];
                message.y = op->fvalues[1];
                message.width = op->fvalues[2];
                message.height = op->fvalues[3];
                message.znear = op->fvalues[4];
                message.zfar = op->fvalues[5];
                if (mw_send(&gWire, MW_RENDER_SET_VIEWPORT, &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_SCISSOR: {
                struct mw_render_set_scissor message;

                message.x = op->values[0];
                message.y = op->values[1];
                message.width = op->values[2];
                message.height = op->values[3];
                if (mw_send(&gWire, MW_RENDER_SET_SCISSOR, &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_CULL_MODE:
            case MWR_OP_SET_WINDING:
            case MWR_OP_SET_FILL_MODE:
            case MWR_OP_SET_STENCIL_REFERENCE: {
                struct mw_render_set_state message;
                uint16_t type = MW_RENDER_SET_CULL_MODE;

                message.value = op->values[0];
                message.reserved = 0;
                if (op->kind == MWR_OP_SET_WINDING) {
                    type = MW_RENDER_SET_WINDING;
                } else if (op->kind == MWR_OP_SET_FILL_MODE) {
                    type = MW_RENDER_SET_FILL_MODE;
                }
                if (mw_send(&gWire, type, &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_SET_BLEND_COLOR: {
                struct mw_render_set_blend_color message;

                message.red = op->fvalues[0];
                message.green = op->fvalues[1];
                message.blue = op->fvalues[2];
                message.alpha = op->fvalues[3];
                if (mw_send(&gWire, MW_RENDER_SET_BLEND_COLOR, &message, sizeof(message)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_DRAW: {
                struct mw_render_draw draw = op->draw;

                if ((draw.flags & MW_DRAW_INDEXED) != 0) {
                    struct mw_buffer_state *index;

                    if (mw_send_render_buffer(state, op->object2) != 0) {
                        return -1;
                    }
                    index = mw_buffer_state_of(op->object2);
                    if (index == NULL) {
                        return -1;
                    }
                    draw.index_buffer = index->remote_id;
                }
                if (mw_send(&gWire, MW_RENDER_DRAW, &draw, sizeof(draw)) != 0) {
                    return -1;
                }
                break;
            }
            case MWR_OP_END:
            default:
                break;
            }
        }
        {
            struct mw_render_end message;

            message.encoder_index = encoder->encoder_index;
            message.draw_count = (uint32_t)encoder->draw_count;
            message.set_count = (uint32_t)encoder->set_count;
            message.reserved = 0;
            if (mw_send(&gWire, MW_RENDER_END, &message, sizeof(message)) != 0) {
                return -1;
            }
        }
        last_target = target;
    }
    if (last_target == NULL) {
        return -1;
    }
    {
        struct mw_present present;

        present.texture_id = last_target->remote_id;
        present.flags = 0;
        if (mw_send(&gWire, MW_PRESENT, &present, sizeof(present)) != 0) {
            return -1;
        }
    }
    memset(&end, 0, sizeof(end));
    end.seq = state->seq;
    end.guest_t_encoded_ns = state->encoded_ns;
    end.crc = 0; /* no guest checksum: the host did not draw these pixels in the guest */
    end.width = last_target->width;
    end.height = last_target->height;
    end.bytes_per_row = last_target->width * 4;
    if (mw_send(&gWire, MW_FRAME_END, &end, sizeof(end)) != 0) {
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------------------------------
 * The classes, and the process gate.
 * ---------------------------------------------------------------------------------------------- */

static void mw_register_buffer_class(void)
{
    gBufferClass = mw_make_class("HakiosMetalBuffer", "_MTLResource", 1);
    if (gBufferClass == Nil) {
        return;
    }
    mw_add_public_protocol(gBufferClass, @protocol(MTLBuffer), "MTLBuffer");
    mw_add_private_protocol(gBufferClass, "MTLBufferSPI");
    mw_add_public_protocol(gBufferClass, @protocol(MTLResource), "MTLResource");
    mw_add_private_protocol(gBufferClass, "MTLResourceSPI");
    class_addMethod(gBufferClass, sel_registerName("contents"), (IMP)mw_buffer_contents, "^v@:");
    class_addMethod(gBufferClass, sel_registerName("length"), (IMP)mw_buffer_length, "Q@:");
    class_addMethod(gBufferClass, sel_registerName("resourceOptions"), (IMP)mw_buffer_options,
                    "Q@:");
    class_addMethod(gBufferClass, sel_registerName("storageMode"), (IMP)mw_buffer_storage_mode,
                    "Q@:");
    class_addMethod(gBufferClass, sel_registerName("cpuCacheMode"), (IMP)mw_buffer_cache_mode,
                    "Q@:");
    class_addMethod(gBufferClass, sel_registerName("hazardTrackingMode"),
                    (IMP)mw_buffer_hazard_mode, "Q@:");
    class_addMethod(gBufferClass, sel_registerName("allocatedSize"), (IMP)mw_buffer_allocated_size,
                    "Q@:");
    class_addMethod(gBufferClass, sel_registerName("didModifyRange:"), (IMP)mw_buffer_did_modify,
                    "v@:{_NSRange=QQ}");
    class_addMethod(gBufferClass, sel_registerName("setPurgeableState:"), (IMP)mw_buffer_purgeable,
                    "Q@:Q");
    class_addMethod(gBufferClass, sel_registerName("gpuAddress"), (IMP)mw_buffer_gpu_address,
                    "Q@:");
    class_addMethod(gBufferClass, sel_registerName("device"), (IMP)mw_buffer_device, "@@:");
    class_addMethod(gBufferClass, sel_registerName("label"), (IMP)mw_buffer_label, "@@:");
    class_addMethod(gBufferClass, sel_registerName("setLabel:"), (IMP)mw_buffer_set_label, "v@:@");
    objc_registerClassPair(gBufferClass);
    mw_log("the class HakiosMetalBuffer is registered (a subclass of %s)",
           objc_getClass("_MTLResource") != Nil ? "_MTLResource"
                                                : "NSObject: this system has no _MTLResource");
}

static void mw_register_texture_class(void)
{
    gTextureClass = mw_make_class("HakiosMetalTexture", "_MTLTexture", 1);
    if (gTextureClass == Nil) {
        return;
    }
    mw_add_public_protocol(gTextureClass, @protocol(MTLTexture), "MTLTexture");
    mw_add_private_protocol(gTextureClass, "MTLTextureSPI");
    mw_add_public_protocol(gTextureClass, @protocol(MTLResource), "MTLResource");
    mw_add_private_protocol(gTextureClass, "MTLResourceSPI");
    class_addMethod(gTextureClass, sel_registerName("width"), (IMP)mw_texture_width, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("height"), (IMP)mw_texture_height, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("depth"), (IMP)mw_texture_depth, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("arrayLength"), (IMP)mw_texture_array_length,
                    "Q@:");
    class_addMethod(gTextureClass, sel_registerName("mipmapLevelCount"), (IMP)mw_texture_mipmaps,
                    "Q@:");
    class_addMethod(gTextureClass, sel_registerName("sampleCount"), (IMP)mw_texture_samples, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("pixelFormat"), (IMP)mw_texture_pixel_format,
                    "Q@:");
    class_addMethod(gTextureClass, sel_registerName("textureType"), (IMP)mw_texture_type, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("usage"), (IMP)mw_texture_usage, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("storageMode"), (IMP)mw_texture_storage_mode,
                    "Q@:");
    class_addMethod(gTextureClass, sel_registerName("cpuCacheMode"), (IMP)mw_texture_zero, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("hazardTrackingMode"), (IMP)mw_texture_zero,
                    "Q@:");
    class_addMethod(gTextureClass, sel_registerName("resourceOptions"),
                    (IMP)mw_texture_resource_options, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("allocatedSize"),
                    (IMP)mw_texture_allocated_size, "Q@:");
    class_addMethod(gTextureClass, sel_registerName("isFramebufferOnly"), (IMP)mw_texture_no,
                    "B@:");
    class_addMethod(gTextureClass, sel_registerName("isShareable"), (IMP)mw_texture_no, "B@:");
    class_addMethod(gTextureClass, sel_registerName("device"), (IMP)mw_texture_device, "@@:");
    class_addMethod(gTextureClass, sel_registerName("label"), (IMP)mw_texture_label, "@@:");
    class_addMethod(gTextureClass, sel_registerName("setLabel:"), (IMP)mw_texture_set_label,
                    "v@:@");
    class_addMethod(gTextureClass, sel_registerName("replaceRegion:mipmapLevel:withBytes:"
                                                    "bytesPerRow:"),
                    (IMP)mw_texture_replace_region, "v@:{MTLRegion=QQQQQQ}Q^vQ");
    class_addMethod(gTextureClass,
                    sel_registerName("replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:"
                                     "bytesPerImage:"),
                    (IMP)mw_texture_replace_region_slice, "v@:{MTLRegion=QQQQQQ}QQ^vQQ");
    class_addMethod(gTextureClass, sel_registerName("iosurface"), (IMP)mw_texture_iosurface,
                    "^v@:");
    objc_registerClassPair(gTextureClass);
    mw_log("the class HakiosMetalTexture is registered (a subclass of %s)",
           objc_getClass("_MTLTexture") != Nil ? "_MTLTexture"
                                               : "NSObject: this system has no _MTLTexture");
}

/* The descriptor-based function API of Metal 16.5. CoreAnimation builds an MTLFunctionDescriptor
 * (name + constant values) and sends -newFunctionWithDescriptor:error: to the library. Apple's
 * _MTLLibrary implementation looks the base function up and stores the answer in its own
 * _cacheEntry ivar; that ivar is nil on this stub, so the real implementation faults at 0x10
 * (ca-metal3-int1: pc 0x181aec43c, Metal _MTLLibrary, x8 = nil). Answer the descriptor's name with
 * a placeholder function instead. */
static id mw_descriptor_function_name(id descriptor)
{
    id name = nil;

    @try {
        if (descriptor != nil && [descriptor respondsToSelector:@selector(name)]) {
            name = ((id(*)(id, SEL))objc_msgSend)(descriptor, @selector(name));
        }
    } @catch (NSException *exception) {
        mw_log("function descriptor: reading -name raised %s; nil", mw_text([exception reason]));
        name = nil;
    }
    return [name isKindOfClass:[NSString class]] ? name : nil;
}

static id mw_library_function_for_descriptor(id self, const char *selector_name, id descriptor,
                                             NSError **error)
{
    id name = mw_descriptor_function_name(descriptor);
    struct mw_library_state *library = mw_library_state_of(self);
    struct mw_wire_library *wire_library = library != NULL ? &library->wire : NULL;
    id function;

    mw_clear_error(error);
    if (name == nil) {
        function = mw_function_object(@"com.apple.coreanimation.frag.unnamed", wire_library);
    } else {
        function = mw_function_object(name, wire_library);
    }
    mw_log("%s: %s -> placeholder function (the descriptor's function is not compiled)", selector_name,
           mw_text(name != nil ? name : @"(no name)"));
    return function;
}

static id mw_library_new_function_descriptor(id self, SEL _cmd, id descriptor, NSError **error)
{
    (void)_cmd;
    return mw_library_function_for_descriptor(self, "newFunctionWithDescriptor:error:", descriptor,
                                              error);
}

static id mw_library_new_specialized_internal(id self, SEL _cmd, id descriptor, id archive,
                                              id function_cache, NSError **error)
{
    (void)_cmd;
    (void)archive;
    (void)function_cache;
    return mw_library_function_for_descriptor(
        self, "newSpecializedFunctionWithDescriptorInternal:destinationArchive:functionCache:error:",
        descriptor, error);
}

static id mw_library_new_specialized_descriptor(id self, SEL _cmd, id descriptor, id archive,
                                                id function_cache, NSError **error)
{
    (void)_cmd;
    (void)archive;
    (void)function_cache;
    return mw_library_function_for_descriptor(
        self, "newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:error:", descriptor,
        error);
}

static void mw_register_library_class(void)
{
    gLibraryClass = mw_make_class("HakiosMetalLibrary", "_MTLLibrary", 1);
    if (gLibraryClass == Nil) {
        return;
    }
    mw_add_public_protocol(gLibraryClass, @protocol(MTLLibrary), "MTLLibrary");
    mw_add_protocol_family(gLibraryClass, "MTLLibrary");
    class_addMethod(gLibraryClass, sel_registerName("functionNames"),
                    (IMP)mw_library_function_names, "@@:");
    class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:"),
                    (IMP)mw_library_new_function, "@@:@");
    class_addMethod(gLibraryClass, sel_registerName("newFunctionWithName:constantValues:error:"),
                    (IMP)mw_library_new_function_constants, "@@:@@^@");
    class_addMethod(gLibraryClass,
                    sel_registerName("newFunctionWithName:constantValues:completionHandler:"),
                    (IMP)mw_library_new_function_completion, "v@:@@@?");
    class_addMethod(gLibraryClass, sel_registerName("newFunctionWithDescriptor:error:"),
                    (IMP)mw_library_new_function_descriptor, "@@:@^@");
    class_addMethod(gLibraryClass,
                    sel_registerName("newSpecializedFunctionWithDescriptorInternal:destinationArchive:"
                                     "functionCache:error:"),
                    (IMP)mw_library_new_specialized_internal, "@@:@@@^@");
    class_addMethod(gLibraryClass,
                    sel_registerName("newSpecializedFunctionWithDescriptor:destinationArchive:"
                                     "functionCache:error:"),
                    (IMP)mw_library_new_specialized_descriptor, "@@:@@@^@");
    class_addMethod(gLibraryClass, sel_registerName("label"), (IMP)mw_library_label, "@@:");
    class_addMethod(gLibraryClass, sel_registerName("setLabel:"), (IMP)mw_library_set_label,
                    "v@:@");
    class_addMethod(gLibraryClass, sel_registerName("device"), (IMP)mw_library_device, "@@:");
    class_addMethod(gLibraryClass, sel_registerName("type"), (IMP)mw_library_type, "q@:");
    class_addMethod(gLibraryClass, sel_registerName("installName"), (IMP)mw_library_install_name,
                    "@@:");
    objc_registerClassPair(gLibraryClass);
    mw_log("the class HakiosMetalLibrary is registered (a subclass of %s)",
           objc_getClass("_MTLLibrary") != Nil ? "_MTLLibrary"
                                               : "NSObject: this system has no _MTLLibrary");
}

static void mw_register_function_class(void)
{
    gFunctionClass = mw_make_class("HakiosMetalFunction", "_MTLFunction", 1);
    if (gFunctionClass == Nil) {
        return;
    }
    mw_add_public_protocol(gFunctionClass, @protocol(MTLFunction), "MTLFunction");
    mw_add_protocol_family(gFunctionClass, "MTLFunction");
    class_addMethod(gFunctionClass, sel_registerName("name"), (IMP)mw_function_name, "@@:");
    class_addMethod(gFunctionClass, sel_registerName("functionType"), (IMP)mw_function_type,
                    "Q@:");
    class_addMethod(gFunctionClass, sel_registerName("vertexAttributes"),
                    (IMP)mw_function_empty_array, "@@:");
    class_addMethod(gFunctionClass, sel_registerName("stageInputAttributes"),
                    (IMP)mw_function_empty_array, "@@:");
    class_addMethod(gFunctionClass, sel_registerName("functionConstantsDictionary"),
                    (IMP)mw_function_empty_dictionary, "@@:");
    class_addMethod(gFunctionClass, sel_registerName("device"), (IMP)mw_function_device, "@@:");
    class_addMethod(gFunctionClass, sel_registerName("label"), (IMP)mw_function_label, "@@:");
    class_addMethod(gFunctionClass, sel_registerName("setLabel:"), (IMP)mw_function_set_label,
                    "v@:@");
    /* Runtime specialisation, the ca-metal3-dealloc1 wall: never let _MTLFunction's real
     * implementation run on a placeholder. The long selector is the one CoreAnimation sends; the
     * shorter spellings are added only where the running base has them. */
    class_addMethod(gFunctionClass,
                    sel_registerName("newSpecializedFunctionWithDescriptor:destinationArchive:"
                                     "functionCache:error:"),
                    (IMP)mw_function_new_specialized, "@@:@@@^@");
    /* the exact spelling CoreAnimation reaches through the library wrapper */
    class_addMethod(gFunctionClass,
                    sel_registerName("newSpecializedFunctionWithDescriptorInternal:destinationArchive:"
                                     "functionCache:error:"),
                    (IMP)mw_function_new_specialized, "@@:@@@^@");
    if (mw_private_class_implements("_MTLFunction", "newSpecializedFunctionWithDescriptor:error:")) {
        class_addMethod(gFunctionClass,
                        sel_registerName("newSpecializedFunctionWithDescriptor:error:"),
                        (IMP)mw_function_new_specialized_descriptor, "@@:@^@");
    } else {
        mw_log("the _MTLFunction base does not implement "
               "-newSpecializedFunctionWithDescriptor:error:; not overridden");
    }
    if (mw_private_class_implements("_MTLFunction",
                                    "newSpecializedFunctionWithDescriptor:destinationArchive:"
                                    "functionCache:sync:completionHandler:")) {
        class_addMethod(gFunctionClass,
                        sel_registerName("newSpecializedFunctionWithDescriptor:destinationArchive:"
                                         "functionCache:sync:completionHandler:"),
                        (IMP)mw_function_new_specialized_sync, "v@:@@@B@?");
    } else {
        mw_log("the _MTLFunction base does not implement the sync/completionHandler specialised "
               "factory; not overridden");
    }
    if (mw_private_class_implements("_MTLFunction", "newSpecializedFunctionWithName:constants:")) {
        class_addMethod(gFunctionClass,
                        sel_registerName("newSpecializedFunctionWithName:constants:"),
                        (IMP)mw_function_new_specialized_name, "@@:@@");
    } else {
        mw_log("the _MTLFunction base does not implement -newSpecializedFunctionWithName:constants:; "
               "not overridden");
    }
    mw_log("the _MTLFunction base specialised factories: long:%s short:%s sync:%s by-name:%s "
           "(the long one is overridden unconditionally; the others only where present)",
           mw_presence("_MTLFunction", "newSpecializedFunctionWithDescriptor:destinationArchive:"
                                       "functionCache:error:"),
           mw_presence("_MTLFunction", "newSpecializedFunctionWithDescriptor:error:"),
           mw_presence("_MTLFunction", "newSpecializedFunctionWithDescriptor:destinationArchive:"
                                       "functionCache:sync:completionHandler:"),
           mw_presence("_MTLFunction", "newSpecializedFunctionWithName:constants:"));
    objc_registerClassPair(gFunctionClass);
    mw_log("the class HakiosMetalFunction is registered (a subclass of %s)",
           objc_getClass("_MTLFunction") != Nil ? "_MTLFunction"
                                                : "NSObject: this system has no _MTLFunction");
}

static void mw_register_render_pipeline_state_class(void)
{
    gPipelineStateClass =
        mw_make_class("HakiosMetalRenderPipelineState", "_MTLRenderPipelineState", 1);
    if (gPipelineStateClass == Nil) {
        return;
    }
    mw_add_public_protocol(gPipelineStateClass, @protocol(MTLRenderPipelineState),
                           "MTLRenderPipelineState");
    mw_add_protocol_family(gPipelineStateClass, "MTLRenderPipelineState");
    class_addMethod(gPipelineStateClass, sel_registerName("label"), (IMP)mw_stub_label, "@@:");
    class_addMethod(gPipelineStateClass, sel_registerName("setLabel:"), (IMP)mw_stub_set_label,
                    "v@:@");
    class_addMethod(gPipelineStateClass, sel_registerName("device"), (IMP)mw_stub_device, "@@:");
    objc_registerClassPair(gPipelineStateClass);
    mw_log("the class HakiosMetalRenderPipelineState is registered (a subclass of %s)",
           objc_getClass("_MTLRenderPipelineState") != Nil
               ? "_MTLRenderPipelineState"
               : "NSObject: this system has no _MTLRenderPipelineState");
}

static void mw_register_depth_stencil_state_class(void)
{
    gDepthStencilStateClass =
        mw_make_class("HakiosMetalDepthStencilState", "_MTLDepthStencilState", 1);
    if (gDepthStencilStateClass == Nil) {
        return;
    }
    mw_add_public_protocol(gDepthStencilStateClass, @protocol(MTLDepthStencilState),
                           "MTLDepthStencilState");
    mw_add_protocol_family(gDepthStencilStateClass, "MTLDepthStencilState");
    class_addMethod(gDepthStencilStateClass, sel_registerName("label"), (IMP)mw_stub_label, "@@:");
    class_addMethod(gDepthStencilStateClass, sel_registerName("setLabel:"),
                    (IMP)mw_stub_set_label, "v@:@");
    class_addMethod(gDepthStencilStateClass, sel_registerName("device"), (IMP)mw_stub_device,
                    "@@:");
    objc_registerClassPair(gDepthStencilStateClass);
    mw_log("the class HakiosMetalDepthStencilState is registered (a subclass of %s)",
           objc_getClass("_MTLDepthStencilState") != Nil
               ? "_MTLDepthStencilState"
               : "NSObject: this system has no _MTLDepthStencilState");
}

static void mw_register_sampler_state_class(void)
{
    gSamplerStateClass = mw_make_class("HakiosMetalSamplerState", "_MTLSamplerState", 1);
    if (gSamplerStateClass == Nil) {
        return;
    }
    mw_add_public_protocol(gSamplerStateClass, @protocol(MTLSamplerState), "MTLSamplerState");
    mw_add_protocol_family(gSamplerStateClass, "MTLSamplerState");
    class_addMethod(gSamplerStateClass, sel_registerName("label"), (IMP)mw_stub_label, "@@:");
    class_addMethod(gSamplerStateClass, sel_registerName("setLabel:"), (IMP)mw_stub_set_label,
                    "v@:@");
    class_addMethod(gSamplerStateClass, sel_registerName("device"), (IMP)mw_stub_device, "@@:");
    objc_registerClassPair(gSamplerStateClass);
    mw_log("the class HakiosMetalSamplerState is registered (a subclass of %s)",
           objc_getClass("_MTLSamplerState") != Nil
               ? "_MTLSamplerState"
               : "NSObject: this system has no _MTLSamplerState");
}

static void mw_register_pipeline_library_class(void)
{
    gPipelineLibraryClass = mw_make_class("HakiosMetalPipelineLibrary", "_MTLPipelineLibrary", 1);
    if (gPipelineLibraryClass == Nil) {
        return;
    }
    /* MTLPipelineLibrary is private (no SDK header): @protocol() below makes the compiled
     * reference, and objc_getProtocol() prefers the framework's own object when it has one. */
    mw_add_public_protocol(gPipelineLibraryClass, @protocol(MTLPipelineLibrary),
                           "MTLPipelineLibrary");
    mw_add_private_protocol(gPipelineLibraryClass, "MTLPipelineLibrarySPI");
    mw_add_protocol_family(gPipelineLibraryClass, "MTLPipelineLibrary");
    class_addMethod(gPipelineLibraryClass, sel_registerName("label"), (IMP)mw_pipeline_library_label,
                    "@@:");
    class_addMethod(gPipelineLibraryClass, sel_registerName("setLabel:"),
                    (IMP)mw_pipeline_library_set_label, "v@:@");
    class_addMethod(gPipelineLibraryClass, sel_registerName("device"),
                    (IMP)mw_pipeline_library_device, "@@:");
    /* The fault of the last boot: Apple's -setDisableRunTimeCompilation: on _MTLPipelineLibrary
     * dereferences Metal's internal object; this override stores one bit instead. */
    class_addMethod(gPipelineLibraryClass, sel_registerName("setDisableRunTimeCompilation:"),
                    (IMP)mw_pipeline_library_set_disable_rtc, "v@:B");
    class_addMethod(gPipelineLibraryClass, sel_registerName("disableRunTimeCompilation"),
                    (IMP)mw_pipeline_library_disable_rtc, "B@:");
    class_addMethod(gPipelineLibraryClass, sel_registerName("isRunTimeCompilationDisabled"),
                    (IMP)mw_pipeline_library_disable_rtc, "B@:");
    class_addMethod(gPipelineLibraryClass,
                    sel_registerName("newRenderPipelineStateWithName:options:reflection:error:"),
                    (IMP)mw_pipeline_library_render_pipeline, "@@:@Q^@^@");
    class_addMethod(gPipelineLibraryClass,
                    sel_registerName("newComputePipelineStateWithName:options:reflection:error:"),
                    (IMP)mw_pipeline_library_compute_pipeline, "@@:@Q^@^@");
    /* The two private descriptor factories are added only when the running _MTLPipelineLibrary
     * really implements them (the selectors are not in the SDK header). */
    if (mw_private_class_implements("_MTLPipelineLibrary",
                                    "newRenderPipelineDescriptorWithName:error:")) {
        class_addMethod(gPipelineLibraryClass,
                        sel_registerName("newRenderPipelineDescriptorWithName:error:"),
                        (IMP)mw_pipeline_library_render_descriptor, "@@:@^@");
    } else {
        mw_log("the _MTLPipelineLibrary base does not implement "
               "-newRenderPipelineDescriptorWithName:error:; not overridden");
    }
    if (mw_private_class_implements("_MTLPipelineLibrary",
                                    "newComputePipelineDescriptorWithName:error:")) {
        class_addMethod(gPipelineLibraryClass,
                        sel_registerName("newComputePipelineDescriptorWithName:error:"),
                        (IMP)mw_pipeline_library_compute_descriptor, "@@:@^@");
    } else {
        mw_log("the _MTLPipelineLibrary base does not implement "
               "-newComputePipelineDescriptorWithName:error:; not overridden");
    }
    /* -serialize and the serialized by-name factory, same rule: shadow only what the base has. */
    if (mw_private_class_implements("_MTLPipelineLibrary", "serialize")) {
        class_addMethod(gPipelineLibraryClass, sel_registerName("serialize"),
                        (IMP)mw_pipeline_library_serialize, "@@:");
    } else {
        mw_log("the _MTLPipelineLibrary base does not implement -serialize; not overridden");
    }
    mw_pipeline_library_override_serialized_factory(gPipelineLibraryClass);
    objc_registerClassPair(gPipelineLibraryClass);
    mw_log("the class HakiosMetalPipelineLibrary is registered (a subclass of %s)",
           objc_getClass("_MTLPipelineLibrary") != Nil
               ? "_MTLPipelineLibrary"
               : "NSObject: this system has no _MTLPipelineLibrary");
}

static void mw_register_render_encoder_class(void)
{
    gRenderEncoderClass =
        mw_make_class("HakiosMetalRenderCommandEncoder", "_MTLRenderCommandEncoder", 1);
    if (gRenderEncoderClass == Nil) {
        return;
    }
    mw_add_public_protocol(gRenderEncoderClass, @protocol(MTLRenderCommandEncoder),
                           "MTLRenderCommandEncoder");
    mw_add_protocol_family(gRenderEncoderClass, "MTLRenderCommandEncoder");
    mw_add_public_protocol(gRenderEncoderClass, @protocol(MTLCommandEncoder), "MTLCommandEncoder");
    class_addMethod(gRenderEncoderClass, sel_registerName("endEncoding"),
                    (IMP)mw_render_end_encoding, "v@:");
    class_addMethod(gRenderEncoderClass, sel_registerName("label"), (IMP)mw_render_label, "@@:");
    class_addMethod(gRenderEncoderClass, sel_registerName("setLabel:"), (IMP)mw_render_set_label,
                    "v@:@");
    class_addMethod(gRenderEncoderClass, sel_registerName("setRenderPipelineState:"),
                    (IMP)mw_render_set_pipeline, "v@:@");
    class_addMethod(gRenderEncoderClass, sel_registerName("setDepthStencilState:"),
                    (IMP)mw_render_set_depth_stencil, "v@:@");
    class_addMethod(gRenderEncoderClass, sel_registerName("setCullMode:"), (IMP)mw_render_set_cull_mode,
                    "v@:Q");
    class_addMethod(gRenderEncoderClass, sel_registerName("setFrontFacingWinding:"),
                    (IMP)mw_render_set_winding, "v@:Q");
    class_addMethod(gRenderEncoderClass, sel_registerName("setTriangleFillMode:"),
                    (IMP)mw_render_set_fill_mode, "v@:Q");
    class_addMethod(gRenderEncoderClass, sel_registerName("setViewport:"),
                    (IMP)mw_render_set_viewport, "v@:{MTLViewport=dddddd}");
    class_addMethod(gRenderEncoderClass, sel_registerName("setScissorRect:"),
                    (IMP)mw_render_set_scissor, "v@:{MTLScissorRect=QQQQ}");
    class_addMethod(gRenderEncoderClass, sel_registerName("setBlendColorRed:green:blue:alpha:"),
                    (IMP)mw_render_set_blend_color, "v@:ffff");
    class_addMethod(gRenderEncoderClass, sel_registerName("setStencilReferenceValue:"),
                    (IMP)mw_render_set_stencil_reference, "v@:I");
    class_addMethod(gRenderEncoderClass, sel_registerName("setVertexBuffer:offset:atIndex:"),
                    (IMP)mw_render_set_vertex_buffer, "v@:@QQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("setVertexBufferOffset:atIndex:"),
                    (IMP)mw_render_set_vertex_buffer_offset, "v@:QQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("setVertexBytes:length:atIndex:"),
                    (IMP)mw_render_set_vertex_bytes, "v@:r^vQQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("setVertexTexture:atIndex:"),
                    (IMP)mw_render_set_vertex_texture, "v@:@Q");
    class_addMethod(gRenderEncoderClass, sel_registerName("setVertexSamplerState:atIndex:"),
                    (IMP)mw_render_set_vertex_sampler, "v@:@Q");
    class_addMethod(
        gRenderEncoderClass,
        sel_registerName("setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:"),
        (IMP)mw_render_set_vertex_sampler_lod, "v@:@ffQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentBuffer:offset:atIndex:"),
                    (IMP)mw_render_set_fragment_buffer, "v@:@QQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentBufferOffset:atIndex:"),
                    (IMP)mw_render_set_fragment_buffer_offset, "v@:QQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentBytes:length:atIndex:"),
                    (IMP)mw_render_set_fragment_bytes, "v@:r^vQQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentTexture:atIndex:"),
                    (IMP)mw_render_set_fragment_texture, "v@:@Q");
    class_addMethod(gRenderEncoderClass, sel_registerName("setFragmentSamplerState:atIndex:"),
                    (IMP)mw_render_set_fragment_sampler, "v@:@Q");
    class_addMethod(
        gRenderEncoderClass,
        sel_registerName("setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:"),
        (IMP)mw_render_set_fragment_sampler_lod, "v@:@ffQ");
    class_addMethod(gRenderEncoderClass, sel_registerName("pushDebugGroup:"),
                    (IMP)mw_render_push_debug_group, "v@:@");
    class_addMethod(gRenderEncoderClass, sel_registerName("popDebugGroup"),
                    (IMP)mw_render_pop_debug_group, "v@:");
    class_addMethod(gRenderEncoderClass, sel_registerName("drawPrimitives:vertexStart:vertexCount:"),
                    (IMP)mw_render_draw_primitives, "v@:QQQ");
    class_addMethod(gRenderEncoderClass,
                    sel_registerName("drawPrimitives:vertexStart:vertexCount:instanceCount:"),
                    (IMP)mw_render_draw_primitives_instanced, "v@:QQQQ");
    class_addMethod(
        gRenderEncoderClass,
        sel_registerName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"),
        (IMP)mw_render_draw_indexed, "v@:QQQ@Q");
    class_addMethod(gRenderEncoderClass,
                    sel_registerName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
                                     "indexBufferOffset:instanceCount:"),
                    (IMP)mw_render_draw_indexed_instanced, "v@:QQQ@QQ");
    class_addMethod(gRenderEncoderClass,
                    sel_registerName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:"
                                     "indexBufferOffset:instanceCount:baseVertex:baseInstance:"),
                    (IMP)mw_render_draw_indexed_base, "v@:QQQ@QQqQ");
    objc_registerClassPair(gRenderEncoderClass);
    mw_log("the class HakiosMetalRenderCommandEncoder is registered (a subclass of %s)",
           objc_getClass("_MTLRenderCommandEncoder") != Nil
               ? "_MTLRenderCommandEncoder"
               : "NSObject: this system has no _MTLRenderCommandEncoder");
}

static void mw_free_encoder(struct mw_encoder_state *state)
{
    if (state == NULL) {
        return;
    }
    [state->label release];
    free(state);
}

static void mw_free_command_buffer(struct mw_command_buffer_state *state)
{
    if (state == NULL) {
        return;
    }
    for (int i = 0; i < state->render_encoder_stored; i++) {
        [state->render_encoders[i] release];
    }
    [state->error release];
    [state->queue release];
    [state->label release];
    [(NSMutableArray *)state->handlers release];
    free(state);
}

static void mw_register_command_buffer_class(void)
{
    gCommandBufferClass = mw_make_class("HakiosMetalCommandBuffer", "_MTLCommandBuffer", 1);
    if (gCommandBufferClass == Nil) {
        return;
    }
    mw_add_public_protocol(gCommandBufferClass, @protocol(MTLCommandBuffer), "MTLCommandBuffer");
    mw_add_protocol_family(gCommandBufferClass, "MTLCommandBuffer");
    class_addMethod(gCommandBufferClass, sel_registerName("commit"), (IMP)mw_cb_commit, "v@:");
    class_addMethod(gCommandBufferClass, sel_registerName("enqueue"), (IMP)mw_cb_noop, "v@:");
    class_addMethod(gCommandBufferClass, sel_registerName("waitUntilCompleted"), (IMP)mw_cb_wait,
                    "v@:");
    class_addMethod(gCommandBufferClass, sel_registerName("waitUntilScheduled"), (IMP)mw_cb_noop,
                    "v@:");
    class_addMethod(gCommandBufferClass, sel_registerName("status"), (IMP)mw_cb_status, "Q@:");
    class_addMethod(gCommandBufferClass, sel_registerName("error"), (IMP)mw_cb_error, "@@:");
    class_addMethod(gCommandBufferClass, sel_registerName("commandQueue"), (IMP)mw_cb_queue, "@@:");
    class_addMethod(gCommandBufferClass, sel_registerName("blitCommandEncoder"),
                    (IMP)mw_cb_blit_encoder, "@@:");
    /* CoreAnimation's render path: answered by this plug-in (record-only), never by Apple's
     * _MTLCommandBuffer implementation. */
    class_addMethod(gCommandBufferClass, sel_registerName("renderCommandEncoderWithDescriptor:"),
                    (IMP)mw_cb_render_encoder, "@@:@");
    class_addMethod(gCommandBufferClass, sel_registerName("presentDrawable:"), (IMP)mw_cb_present,
                    "v@:@");
    class_addMethod(gCommandBufferClass, sel_registerName("presentDrawable:atTime:"),
                    (IMP)mw_cb_present, "v@:@d");
    class_addMethod(gCommandBufferClass, sel_registerName("presentDrawable:afterMinimumDuration:"),
                    (IMP)mw_cb_present, "v@:@d");
    class_addMethod(gCommandBufferClass, sel_registerName("addCompletedHandler:"),
                    (IMP)mw_cb_add_handler, "v@:@?");
    class_addMethod(gCommandBufferClass, sel_registerName("addScheduledHandler:"),
                    (IMP)mw_cb_add_handler, "v@:@?");
    class_addMethod(gCommandBufferClass, sel_registerName("GPUStartTime"), (IMP)mw_cb_no_time,
                    "d@:");
    class_addMethod(gCommandBufferClass, sel_registerName("GPUEndTime"), (IMP)mw_cb_no_time, "d@:");
    class_addMethod(gCommandBufferClass, sel_registerName("isProfilingEnabled"),
                    (IMP)mw_cb_profiling_enabled, "B@:");
    class_addMethod(gCommandBufferClass, sel_registerName("setProfilingEnabled:"),
                    (IMP)mw_cb_set_profiling_enabled, "v@:B");
    class_addMethod(gCommandBufferClass, sel_registerName("retainedReferences"),
                    (IMP)mw_cb_retained_references, "B@:");
    class_addMethod(gCommandBufferClass, sel_registerName("label"), (IMP)mw_cb_label, "@@:");
    class_addMethod(gCommandBufferClass, sel_registerName("setLabel:"), (IMP)mw_cb_label_set,
                    "v@:@");
    objc_registerClassPair(gCommandBufferClass);
    mw_log("the class HakiosMetalCommandBuffer is registered (a subclass of %s)",
           objc_getClass("_MTLCommandBuffer") != Nil
               ? "_MTLCommandBuffer"
               : "NSObject: this system has no _MTLCommandBuffer");
    if (objc_getClass("_MTLCommandBuffer") != Nil) {
        mw_log("the _MTLCommandBuffer base implements waitUntilScheduled:%s enqueue:%s "
               "addScheduledHandler:%s isProfilingEnabled:%s setProfilingEnabled:%s "
               "retainedReferences:%s (all overridden by the plug-in)",
               mw_presence("_MTLCommandBuffer", "waitUntilScheduled"),
               mw_presence("_MTLCommandBuffer", "enqueue"),
               mw_presence("_MTLCommandBuffer", "addScheduledHandler:"),
               mw_presence("_MTLCommandBuffer", "isProfilingEnabled"),
               mw_presence("_MTLCommandBuffer", "setProfilingEnabled:"),
               mw_presence("_MTLCommandBuffer", "retainedReferences"));
    } else {
        mw_log("this system has no _MTLCommandBuffer base; the command buffer has no inherited "
               "Apple methods to report");
    }
}

static void mw_register_blit_encoder_class(void)
{
    gBlitEncoderClass = mw_make_class("HakiosMetalBlitEncoder", "_MTLCommandEncoder", 1);
    if (gBlitEncoderClass == Nil) {
        return;
    }
    mw_add_public_protocol(gBlitEncoderClass, @protocol(MTLBlitCommandEncoder),
                           "MTLBlitCommandEncoder");
    mw_add_public_protocol(gBlitEncoderClass, @protocol(MTLCommandEncoder), "MTLCommandEncoder");
    mw_add_protocol_family(gBlitEncoderClass, "MTLCommandEncoder");
    class_addMethod(
        gBlitEncoderClass,
        sel_registerName("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:"
                         "sourceSize:toTexture:destinationSlice:destinationLevel:"
                         "destinationOrigin:"),
        (IMP)mw_blit_copy_to_texture, "v@:@QQQ{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}");
    class_addMethod(
        gBlitEncoderClass,
        sel_registerName("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:"
                         "sourceSize:toTexture:destinationSlice:destinationLevel:"
                         "destinationOrigin:options:"),
        (IMP)mw_blit_copy_to_texture_options, "v@:@QQQ{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}Q");
    class_addMethod(
        gBlitEncoderClass,
        sel_registerName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"),
        (IMP)mw_blit_not_in_v1, "v@:@Q@QQ");
    class_addMethod(gBlitEncoderClass, sel_registerName("fillBuffer:range:value:"),
                    (IMP)mw_blit_not_in_v1, "v@:@{_NSRange=QQ}C");
    /* CoreAnimation's two texture-level blits: explicit record-only no-ops, counted. */
    class_addMethod(
        gBlitEncoderClass,
        sel_registerName("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:"
                         "toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
        (IMP)mw_blit_copy_texture_to_texture,
        "v@:@QQ{MTLOrigin=QQQ}{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}");
    class_addMethod(gBlitEncoderClass, sel_registerName("generateMipmapsForTexture:"),
                    (IMP)mw_blit_generate_mipmaps, "v@:@");
    class_addMethod(gBlitEncoderClass, sel_registerName("endEncoding"), (IMP)mw_blit_end, "v@:");
    class_addMethod(gBlitEncoderClass, sel_registerName("synchronizeResource:"),
                    (IMP)mw_blit_synchronize, "v@:@");
    class_addMethod(gBlitEncoderClass, sel_registerName("label"), (IMP)mw_blit_label, "@@:");
    class_addMethod(gBlitEncoderClass, sel_registerName("setLabel:"), (IMP)mw_blit_set_label,
                    "v@:@");
    objc_registerClassPair(gBlitEncoderClass);
    mw_log("the class HakiosMetalBlitEncoder is registered (a subclass of %s)",
           objc_getClass("_MTLCommandEncoder") != Nil
               ? "_MTLCommandEncoder"
               : "NSObject: this system has no _MTLCommandEncoder");
}

static void mw_register_queue_class(void)
{
    gQueueClass = mw_make_class("HakiosMetalQueue", "_MTLCommandQueue", 0);
    if (gQueueClass == Nil) {
        return;
    }
    mw_add_public_protocol(gQueueClass, @protocol(MTLCommandQueue), "MTLCommandQueue");
    mw_add_protocol_family(gQueueClass, "MTLCommandQueue");
    class_addMethod(gQueueClass, sel_registerName("device"), (IMP)mw_queue_device, "@@:");
    class_addMethod(gQueueClass, sel_registerName("label"), (IMP)mw_queue_label, "@@:");
    class_addMethod(gQueueClass, sel_registerName("setLabel:"), (IMP)mw_queue_noop, "v@:@");
    class_addMethod(gQueueClass, sel_registerName("setCompletionQueue:"), (IMP)mw_queue_noop,
                    "v@:@");
    class_addMethod(gQueueClass, sel_registerName("commandBuffer"), (IMP)mw_queue_command_buffer,
                    "@@:");
    class_addMethod(gQueueClass, sel_registerName("commandBufferWithUnretainedReferences"),
                    (IMP)mw_queue_command_buffer, "@@:");
    objc_registerClassPair(gQueueClass);
    mw_log("the class HakiosMetalQueue is registered (a subclass of %s)",
           objc_getClass("_MTLCommandQueue") != Nil
               ? "_MTLCommandQueue"
               : "NSObject: this system has no _MTLCommandQueue");
}

static void mw_register_device_class(void)
{
    Class device = mw_make_class("HakiosMetalDevice", "_MTLDevice", 0);
    if (device == Nil) {
        return;
    }
    /* MTLAddDevice asserts conformance to MTLDevice and then to the private MTLDeviceSPI; neither
     * is declared by _MTLDevice itself (the HakiOS runs found both, in that order). */
    mw_add_public_protocol(device, @protocol(MTLDevice), "MTLDevice");
    mw_add_private_protocol(device, "MTLDeviceSPI");
    class_addMethod(device, @selector(initWithAcceleratorPort:), (IMP)mw_device_init, "@@:I");
    class_addMethod(device, @selector(name), (IMP)mw_device_name, "@@:");
    class_addMethod(device, sel_registerName("registryID"), (IMP)mw_device_registry_id, "Q@:");
    class_addMethod(device, sel_registerName("maxBufferLength"), (IMP)mw_device_max_buffer_length,
                    "Q@:");
    class_addMethod(device, sel_registerName("hasUnifiedMemory"), (IMP)mw_device_yes, "B@:");
    class_addMethod(device, sel_registerName("supportsFeatureSet:"), (IMP)mw_device_no_feature_set,
                    "B@:Q");
    class_addMethod(device, sel_registerName("supportsFamily:"), (IMP)mw_device_no_family, "B@:q");
    class_addMethod(device, sel_registerName("isLowPower"), (IMP)mw_device_no, "B@:");
    class_addMethod(device, sel_registerName("isRemovable"), (IMP)mw_device_no, "B@:");
    class_addMethod(device, sel_registerName("isHeadless"), (IMP)mw_device_no, "B@:");
    class_addMethod(device, sel_registerName("dedicatedMemorySize"),
                    (IMP)mw_device_no_dedicated_memory, "Q@:");
    class_addMethod(device, sel_registerName("sharedMemorySize"), (IMP)mw_device_shared_memory,
                    "Q@:");
    class_addMethod(device, @selector(newCommandQueue), (IMP)mw_device_new_queue, "@@:");
    class_addMethod(device, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"),
                    (IMP)mw_device_new_queue_max, "@@:Q");
    class_addMethod(device, @selector(newBufferWithLength:options:), (IMP)mw_device_new_buffer,
                    "@@:QQ");
    class_addMethod(device, @selector(newBufferWithBytes:length:options:),
                    (IMP)mw_device_new_buffer_bytes, "@@:r^vQQ");
    class_addMethod(device, @selector(newTextureWithDescriptor:), (IMP)mw_device_new_texture,
                    "@@:@");
    class_addMethod(device, sel_registerName("newTextureWithDescriptor:iosurface:plane:"),
                    (IMP)mw_device_new_texture_iosurface, "@@:@^vQ");
    class_addMethod(device, sel_registerName("newDrawableWithTexture:"),
                    (IMP)mw_device_new_drawable, "@@:@");
    /* The library phase: these overrides are what keeps Apple's _MTLDevice implementation (which
     * aborts on a file that is not a real metallib) from ever running for this device. */
    class_addMethod(device, @selector(newLibraryWithURL:error:), (IMP)mw_device_new_library_url,
                    "@@:@^@");
    class_addMethod(device, sel_registerName("newLibraryWithData:error:"),
                    (IMP)mw_device_new_library_data, "@@:@^@");
    class_addMethod(device, sel_registerName("newLibraryWithFile:error:"),
                    (IMP)mw_device_new_library_file, "@@:@^@");
    class_addMethod(device, sel_registerName("newLibraryWithSource:options:error:"),
                    (IMP)mw_device_new_library_source, "@@:@@^@");
    class_addMethod(device, sel_registerName("newDefaultLibrary"),
                    (IMP)mw_device_new_default_library, "@@:");
    class_addMethod(device, sel_registerName("newDefaultLibraryWithBundle:error:"),
                    (IMP)mw_device_new_default_library_bundle, "@@:@^@");
    /* The pipeline phase: placeholder states, read from the descriptor where CoreAnimation's
     * descriptor can be read. */
    class_addMethod(device, @selector(newDepthStencilStateWithDescriptor:),
                    (IMP)mw_device_new_depth_stencil_state, "@@:@");
    class_addMethod(device, @selector(newSamplerStateWithDescriptor:),
                    (IMP)mw_device_new_sampler_state, "@@:@");
    class_addMethod(device, @selector(newRenderPipelineStateWithDescriptor:error:),
                    (IMP)mw_device_new_render_pipeline, "@@:@^@");
    class_addMethod(device, sel_registerName("newRenderPipelineStateWithDescriptor:options:error:"),
                    (IMP)mw_device_new_render_pipeline_options, "@@:@Q^@");
    class_addMethod(
        device,
        sel_registerName("newRenderPipelineStateWithDescriptor:options:reflection:error:"),
        (IMP)mw_device_new_render_pipeline_reflection, "@@:@Q^@^@");
    class_addMethod(device, @selector(newComputePipelineStateWithFunction:error:),
                    (IMP)mw_device_new_compute_pipeline, "@@:@^@");
    {
        /* The SPI selectors Metal 306.6.4 uses for the completion-handler path. Added with the
         * base class's own type encoding so the argument order cannot drift from the system. */
        static const struct {
            const char *selector;
            IMP implementation;
        } internal[] = {
            { "newRenderPipelineStateWithDescriptorInternal:options:reflection:"
              "destinationBinaryArchive:error:completionHandler:",
              (IMP)mw_device_new_render_pipeline_internal },
            { "newComputePipelineStateWithDescriptorInternal:options:pipelineCache:"
              "destinationBinaryArchive:reflection:error:completionHandler:",
              (IMP)mw_device_new_compute_pipeline_internal },
        };

        for (unsigned i = 0; i < sizeof(internal) / sizeof(internal[0]); i++) {
            Method base = class_getInstanceMethod(gDeviceBase, sel_registerName(internal[i].selector));

            if (base == NULL) {
                mw_log("the _MTLDevice base does not implement -%s; not overridden",
                       internal[i].selector);
                continue;
            }
            class_addMethod(device, sel_registerName(internal[i].selector), internal[i].implementation,
                            method_getTypeEncoding(base));
            mw_log("the device overrides -%s (type %s)", internal[i].selector,
                   method_getTypeEncoding(base));
        }
    }
    /* The pipeline library CoreAnimation builds from QuartzCore's default.pipelinelib: the
     * inherited _MTLDevice implementation deserializes the file and crashes, this one logs the
     * path and answers a placeholder. */
    class_addMethod(device, sel_registerName("newPipelineLibraryWithFilePath:error:"),
                    (IMP)mw_device_new_pipeline_library_file, "@@:@^@");
    objc_registerClassPair(device);
    mw_log("the class HakiosMetalDevice is registered (a subclass of _MTLDevice; declares "
           "MTLDevice and MTLDeviceSPI)");
}

/* Whether this process may be shown the device: the boot-arg level (as the HakiOS plug-in reads
 * it) and a clients file beside the bundle, plus this experiment's own two programs, which must
 * work whether or not the image carries the clients file. */
static BOOL mw_listed_in_file(const char *path, const char *program)
{
    char line[256];
    FILE *file = fopen(path, "r");
    BOOL listed = NO;

    if (file == NULL) {
        return NO;
    }
    while (!listed && fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, " \t\r\n")] = '\0';
        listed = strcmp(line, "*") == 0 || (line[0] != '\0' && strcmp(line, program) == 0);
    }
    fclose(file);
    mw_log("%s %s listed in %s", program, listed ? "is" : "is not", path);
    return listed;
}

/* The clients file is looked for beside the bundle executable, both as the bundle was opened
 * (/System/Library/Extensions/..., where a symlink usually lives) and at its real path
 * (/redwood/lib/..., where this port builds it). The HakiOS plug-in only tries the first form; the
 * experiment's image is described by the second, so both are tried. */
static BOOL mw_listed_beside_the_bundle(const char *program)
{
    Dl_info info;
    char candidates[2][1024];
    char resolved[PATH_MAX];
    int count = 0;

    if (dladdr((const void *)&mw_listed_beside_the_bundle, &info) == 0 || info.dli_fname == NULL) {
        return NO;
    }
    const char *paths[2] = {info.dli_fname, NULL};

    if (realpath(info.dli_fname, resolved) != NULL) {
        paths[1] = resolved;
    }
    for (int i = 0; i < 2; i++) {
        char path[1024];
        char *slash;

        if (paths[i] == NULL) {
            continue;
        }
        strlcpy(path, paths[i], sizeof(path)); /* <somewhere>/HakiosMetal.bundle/HakiosMetal */
        if ((slash = strrchr(path, '/')) == NULL) {
            continue;
        }
        *slash = '\0'; /* <somewhere>/HakiosMetal.bundle */
        if ((slash = strrchr(path, '/')) == NULL) {
            continue;
        }
        snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "HakiosMetal.clients");
        int duplicate = 0;

        for (int j = 0; j < count; j++) {
            duplicate |= strcmp(candidates[j], path) == 0;
        }
        if (!duplicate) {
            strlcpy(candidates[count++], path, sizeof(candidates[0]));
        }
    }
    for (int i = 0; i < count; i++) {
        if (mw_listed_in_file(candidates[i], program)) {
            return YES;
        }
    }
    if (count > 1) {
        mw_log("no clients file naming %s at %s or %s", program, candidates[0], candidates[1]);
    } else if (count == 1) {
        mw_log("no clients file naming %s at %s", program, candidates[0]);
    }
    return NO;
}

static BOOL mw_process_allowed(void)
{
    char boot_args[1024] = "";
    size_t size = sizeof(boot_args) - 1;
    const char *program = getprogname();
    int level = 1;

    if (program == NULL) {
        return NO;
    }
    if (sysctlbyname("kern.bootargs", boot_args, &size, NULL, 0) == 0) {
        const char *at = strstr(boot_args, "metal=");

        if (at != NULL) {
            level = atoi(at + 6);
        }
        if (strstr(boot_args, "byname=nil") != NULL) {
            gBynameFail = 1;
        }
    }
    if (strcmp(program, "metal_frame") == 0 || strcmp(program, "metal_probe") == 0) {
        mw_log("%s is this experiment's client: the device is shown to it", program);
        return YES;
    }
    if (level >= 3 || (level == 2 && strcmp(program, "backboardd") != 0) ||
        (level == 1 && strcmp(program, "metal_test") == 0)) {
        return YES;
    }
    if (mw_listed_beside_the_bundle(program)) {
        mw_log("%s is shown the device: HakiosMetal.clients beside the bundle names it", program);
        return YES;
    }
    mw_log("%s is not shown the device (metal=%d: 1 metal_test only, 2 all but backboardd, 3 all)",
           program, level);
    return NO;
}

static void mw_atexit_summary(void)
{
    uint64_t frames = gStats.frames;
    uint64_t encoded_mean = frames != 0 ? gStats.encoded_sum / frames : 0;
    uint64_t wait_mean = frames != 0 ? gStats.wait_sum / frames : 0;

    mw_log("summary: %llu frames, %llu ok, %llu failed (%llu crc mismatches), %llu bytes sent, "
           "%llu bytes by reference in %llu frames; "
           "encoded mean/min/max %llu/%llu/%llu us; wait mean/min/max %llu/%llu/%llu us; host %s",
           (unsigned long long)frames, (unsigned long long)gStats.ok,
           (unsigned long long)gStats.failed, (unsigned long long)gStats.crc_mismatches,
           (unsigned long long)gStats.bytes, (unsigned long long)gStats.phys_bytes,
           (unsigned long long)gStats.phys_frames, (unsigned long long)(encoded_mean / 1000),
           (unsigned long long)(gStats.encoded_min == UINT64_MAX ? 0 : gStats.encoded_min / 1000),
           (unsigned long long)(gStats.encoded_max / 1000), (unsigned long long)(wait_mean / 1000),
           (unsigned long long)(gStats.wait_min == UINT64_MAX ? 0 : gStats.wait_min / 1000),
           (unsigned long long)(gStats.wait_max / 1000), gWire.target);
}

/* -------------------------------------------------------------------------------------------------
 * Census mode (/redwood/etc/metal-census).
 *
 * The experiment's walls are Apple private-base methods a subclass inherits: when one of them
 * crashes inside Metal.framework the serial log has a backtrace but no selector. With the census
 * file present, every method the private base implements and our class does not is replaced on our
 * class by _objc_msgForward; the existing methodSignatureForSelector:/forwardInvocation: machinery
 * then logs `census: -[<class> <selector>] (inherited from <base>; answered zero)` once and
 * answers zero, so the next wall names itself instead of taking the process down. metal_frame and
 * metal_probe are excluded: their path stays byte-identical even when the file exists.
 * ---------------------------------------------------------------------------------------------- */

static BOOL mw_census_prefix_excluded(const char *name)
{
    static const char *prefixes[] = {
        "init",      "dealloc",   "class",           "superclass",  "self",
        "isKind",    "isMember",  "respondsTo",      "conformsTo",  "methodSignature",
        "forwardInvocation",      "description",     "debugDescription",
        "retain",    "release",   "autorelease",     "retainCount", "zone",
        "hash",      "isEqual",   "copy",            "mutableCopy", "alloc",
    };

    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t length = strlen(prefixes[i]);

        if (strncmp(name, prefixes[i], length) == 0) {
            return YES;
        }
    }
    return NO;
}

static void mw_census_trap_base_methods(Class cls, IMP forward_imp)
{
    Class base = class_getSuperclass(cls);
    Method *methods;
    unsigned count = 0;
    unsigned trapped = 0;
    unsigned implemented = 0;
    unsigned excluded = 0;
    unsigned untrapped = 0;

    methods = class_copyMethodList(base, &count);
    if (methods == NULL) {
        mw_log("census: %s: %s has no method list", class_getName(cls), class_getName(base));
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        SEL selector = method_getName(methods[i]);
        const char *name = sel_getName(selector);
        const char *types = method_getTypeEncoding(methods[i]);

        if (name == NULL || types == NULL) {
            untrapped++;
            continue;
        }
        if (mw_census_prefix_excluded(name)) {
            excluded++;
            continue;
        }
        /* the class's own implementation (not the inherited one) must win */
        if (class_getInstanceMethod(cls, selector) != class_getInstanceMethod(base, selector)) {
            implemented++;
            continue;
        }
        if (class_addMethod(cls, selector, forward_imp, types)) {
            trapped++;
        } else {
            untrapped++;
        }
    }
    free(methods);
    mw_log("census: %s (from %s): %u trapped, %u already implemented, %u excluded, %u not trapped",
           class_getName(cls), class_getName(base), trapped, implemented, excluded, untrapped);
}

static void mw_census_install_if_requested(void)
{
    const char *program = getprogname();
    void *symbol;
    IMP forward_imp = NULL;
    Class *classes;
    int total;
    unsigned walked = 0;

    if (program != NULL &&
        (strcmp(program, "metal_frame") == 0 || strcmp(program, "metal_probe") == 0)) {
        return; /* the experiment's own clients must keep their byte-identical path */
    }
    if (access("/redwood/etc/metal-census", F_OK) != 0) {
        return;
    }
    symbol = dlsym(RTLD_DEFAULT, "_objc_msgForward");
    if (symbol == NULL) {
        mw_log("census: /redwood/etc/metal-census is present but _objc_msgForward is not in the "
               "process; census is off");
        return;
    }
    memcpy(&forward_imp, &symbol, sizeof(forward_imp));
    gCensusEnabled = 1;
    mw_log("census: /redwood/etc/metal-census is present: methods inherited from Metal's private "
           "base classes are trapped; each first call is logged once and answered zero. "
           "HakiosMetalDevice and HakiosMetalQueue are excluded: trapping Metal's own device "
           "methods broke its registration (the census boot crashed in CAMetalContextCreate)");
    total = objc_getClassList(NULL, 0);
    if (total <= 0) {
        return;
    }
    classes = malloc(sizeof(Class) * (size_t)total);
    if (classes == NULL) {
        return;
    }
    total = objc_getClassList(classes, total);
    for (int i = 0; i < total; i++) {
        Class cls = classes[i];
        Class base;
        const char *name = class_getName(cls);

        if (name == NULL || strncmp(name, "HakiosMetal", 11) != 0) {
            continue;
        }
        if (strcmp(name, "HakiosMetalDevice") == 0 || strcmp(name, "HakiosMetalQueue") == 0) {
            /* Metal's own device registration (MTLAddDevice/CAMetalContextCreate) and the window
             * server's queue probing depend on the base implementations here; trapping them made
             * the census boot crash before any CoreAnimation line. */
            mw_log("census: %s is excluded from trapping", name);
            continue;
        }
        base = class_getSuperclass(cls);
        if (base == Nil || base == objc_getClass("NSObject")) {
            continue; /* the NSObject fallback classes have no Apple private base to trap */
        }
        if (strncmp(class_getName(base), "_MTL", 4) != 0) {
            continue;
        }
        mw_census_trap_base_methods(cls, forward_imp);
        walked++;
    }
    free(classes);
    mw_log("census: %u classes on Metal private bases walked (device and queue excluded)", walked);
}

/*
 * Boot w (wp4-d1-w) showed SBRendererService reaching the device and then dying with SIGABRT
 * without any text: the process ending in -doesNotRecognizeSelector: would print an uncaught
 * exception, but launchd only records the signal. The selector name is what the next iteration
 * needs, so the diagnostic hook below prints it on fd2 (which reaches the serial log) and then
 * lets the crash happen exactly as before.
 */
/*
 * The selector diagnostic above never fired for SBRendererService (wp4-d1-z): the renderer aborts
 * without going through -doesNotRecognizeSelector:. A SIGABRT handler cannot call mw_log (it
 * allocates), so it writes a fixed-size line straight to fd2: the faulting pc, link register and
 * a short frame-pointer walk, which is enough to name the image and function that aborted.
 */
static void mw_sigabrt_report(int sig, siginfo_t *info, void *context)
{
    char line[256];
    int length;

#if defined(__aarch64__)
    {
        ucontext_t *uc = (ucontext_t *)context;
        struct __darwin_mcontext64 *mc = uc->uc_mcontext;
        uint64_t pc = mc->__ss.__pc;
        uint64_t lr = mc->__ss.__lr;
        uint64_t fp = mc->__ss.__fp;
        int i;

        length = snprintf(line, sizeof(line), "metalwire: sigabrt: sig=%d code=%d pc=0x%llx lr=0x%llx\n",
                          sig, info != NULL ? info->si_code : -1, (unsigned long long)pc, (unsigned long long)lr);
        if (length > 0) {
            (void)write(2, line, (size_t)length);
        }
        for (i = 0; i < 8 && fp != 0; i++) {
            uint64_t next;
            uint64_t ret;

            memcpy(&next, (void *)fp, sizeof(next));
            memcpy(&ret, (void *)(fp + 8), sizeof(ret));
            length = snprintf(line, sizeof(line), "metalwire: sigabrt: frame %d ret=0x%llx\n", i,
                              (unsigned long long)ret);
            if (length > 0) {
                (void)write(2, line, (size_t)length);
            }
            if (next <= fp || (next & 7) != 0) {
                break;
            }
            fp = next;
        }
    }
#else
    (void)context;
    length = snprintf(line, sizeof(line), "metalwire: sigabrt: sig=%d\n", sig);
    (void)write(2, line, (size_t)length);
#endif
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

static void mw_install_crash_diagnostics(void)
{
    struct sigaction action;

    /* A crash line names runtime addresses; the dyld shared cache is slid per boot, so the slide
     * has to be in the same log before the addresses can be resolved against the cache file. */
    {
        uint32_t count = _dyld_image_count();
        uint32_t i;

        for (i = 0; i < count; i++) {
            const char *name = _dyld_get_image_name(i);

            if (name != NULL && strstr(name, "libobjc.A.dylib") != NULL) {
                mw_log("diagnostics: libobjc slide 0x%llx base 0x%llx", (unsigned long long)_dyld_get_image_vmaddr_slide(i),
                       (unsigned long long)(uintptr_t)_dyld_get_image_header(i));
                break;
            }
        }
        if (i == count) {
            mw_log("diagnostics: no libobjc image; image0 slide 0x%llx", (unsigned long long)_dyld_get_image_vmaddr_slide(0));
        }
    }
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = mw_sigabrt_report;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGABRT, &action, NULL) == 0) {
        mw_log("diagnostics: SIGABRT handler writes pc/lr/frames to fd2");
    }
}

/* CoreAnimation's OpenGL-over-Metal path (QuartzCore.framework, ogl-metal.mm) asks its own bundle
 * for a precompiled shader library and asserts metallib_url != nil when it is not on disk. The
 * image's select-system.py leaves every framework's .metallib out to save RAM, so the renderer
 * aborts; this returns the image that the system root does have, from /redwood, instead. Narrow
 * on purpose: only NSBundle lookups that fail, only the QuartzCore bundle, only .metallib. */
static IMP g_orig_url_for_resource = NULL;

static id mw_url_for_resource(id self, SEL _cmd, id name, id extension)
{
    id url = ((id (*)(id, SEL, id, id))g_orig_url_for_resource)(self, _cmd, name, extension);
    const char *path;
    const char *ext;
    struct stat st;

    if (url != nil) {
        return url;
    }
    path = self != nil ? [[(NSBundle *)self bundlePath] UTF8String] : NULL;
    ext = extension != nil ? [extension UTF8String] : "";
    if (path == NULL || strstr(path, "QuartzCore.framework") == NULL || strcmp(ext, "metallib") != 0) {
        return nil;
    }
    if (stat("/redwood/lib/QuartzCore-default.metallib", &st) != 0) {
        return nil;
    }
    url = [NSURL fileURLWithPath:@"/redwood/lib/QuartzCore-default.metallib"];
    mw_log("resources: %s/%s.%s -> /redwood/lib/QuartzCore-default.metallib (%lld byte(s))", path,
           name != nil ? [name UTF8String] : "", ext, (long long)st.st_size);
    return url;
}

static void mw_install_resource_overrides(void)
{
    Class bundle_class = objc_getClass("NSBundle");
    Method method = bundle_class != Nil
                        ? class_getInstanceMethod(bundle_class, sel_registerName("URLForResource:withExtension:"))
                        : NULL;

    if (method == NULL) {
        mw_log("resources: no NSBundle URLForResource:withExtension:; QuartzCore shader library stays missing");
        return;
    }
    g_orig_url_for_resource = method_getImplementation(method);
    method_setImplementation(method, (IMP)mw_url_for_resource);
    mw_log("resources: NSBundle URLForResource:withExtension: falls back to /redwood for QuartzCore.metallib");
}

static void mw_does_not_recognize_selector(id self, SEL _cmd, SEL selector)
{
    mw_log("doesNotRecognizeSelector: [%s %s] -> aborting as before",
           object_getClassName(self), sel_getName(selector));
    abort();
}

static void mw_install_selector_diagnostics(void)
{
    Class object_class = objc_getClass("NSObject");
    Method method = object_class != Nil
                        ? class_getInstanceMethod(object_class, sel_registerName("doesNotRecognizeSelector:"))
                        : NULL;

    if (method == NULL) {
        return;
    }
    method_setImplementation(method, (IMP)mw_does_not_recognize_selector);
    mw_log("diagnostics: doesNotRecognizeSelector: reports the selector before aborting");
}

__attribute__((constructor)) static void mw_plugin_load(void)
{
    if (!mw_process_allowed()) {
        return;
    }
    mw_install_selector_diagnostics();
    mw_install_resource_overrides();
    mw_install_crash_diagnostics();
    gWireReady = 0;
    mw_conn_init(&gWire);
    gWireReady = 1;
    gDeviceBase = objc_getClass("_MTLDevice");
    if (gDeviceBase == Nil) {
        mw_log("this system has no _MTLDevice: no device class can be registered");
        return;
    }
    if (objc_getClass("HakiosMetalDevice") != Nil) {
        mw_log("HakiosMetalDevice is already registered; this load does nothing");
        return;
    }
    mw_lifecycle_install();
    mw_register_device_class();
    mw_register_queue_class();
    mw_register_command_buffer_class();
    mw_register_blit_encoder_class();
    mw_register_texture_class();
    mw_register_buffer_class();
    mw_register_library_class();
    mw_register_function_class();
    mw_register_render_pipeline_state_class();
    mw_register_depth_stencil_state_class();
    mw_register_sampler_state_class();
    mw_register_pipeline_library_class();
    mw_register_render_encoder_class();
    mw_census_install_if_requested();
    atexit(mw_atexit_summary);
    mw_log("the plug-in is ready: object graph device -> queue -> command buffer -> blit encoder, "
           "buffers and textures; every other Metal object answers nil and is logged once");
}
