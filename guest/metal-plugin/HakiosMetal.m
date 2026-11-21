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
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
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

static Class gDeviceBase;             /* Metal's private _MTLDevice */
static Class gQueueClass;             /* HakiosMetalQueue, a _MTLCommandQueue */
static Class gCommandBufferClass;     /* HakiosMetalCommandBuffer, a _MTLCommandBuffer */
static Class gBlitEncoderClass;       /* HakiosMetalBlitEncoder, a _MTLCommandEncoder */
static Class gBufferClass;            /* HakiosMetalBuffer, a _MTLResource */
static Class gTextureClass;           /* HakiosMetalTexture, a _MTLResource or NSObject */
static id gDevice;                    /* the one device, once made */
static unsigned int gAcceleratorPort; /* the IOAcceleratorES service it was made for */

#define MW_MAX_PROTOCOLS 96
static Protocol *gProtocols[MW_MAX_PROTOCOLS];
static unsigned gProtocolCount;

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

struct mw_buffer_state {
    uint32_t remote_id;
    uint32_t created_generation;
    uint64_t length;
    unsigned long options;
    void *memory;
    id label;
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
};

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
};

struct mw_encoder_state {
    id command_buffer; /* not retained: the encoder dies before its command buffer */
    int ended;
    id label;
};

static uint32_t mw_next_object_id(void)
{
    return __atomic_add_fetch(&gNextObjectId, 1, __ATOMIC_RELAXED);
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

static void mw_object_dealloc(id self, SEL _cmd)
{
    Class cls = object_getClass(self);
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
        }
        *slot = NULL;
    }
    struct objc_super super = {self, class_getSuperclass(cls)};

    ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&super, _cmd);
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

    (void)self;
    (void)_cmd;
    pthread_mutex_lock(&lock);
    if (seen == nil) {
        seen = [[NSMutableSet alloc] init];
    }
    if (![seen containsObject:name]) {
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
    if (texture->created_generation == gGeneration) {
        return 0;
    }
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
#define MW_VTOP_SEGMENTS 512

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
    if (state->op_count == 0) {
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
            return;
        }
        gHelloGeneration = gGeneration;
        mw_log("hello to %s: pid %u, program %s", gWire.target, (unsigned)getpid(), getprogname());
    }
    sent = mw_send_frame(state);
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
    if (mw_texture_state_of(texture) == NULL) {
        state->local_error = "presentDrawable: the drawable does not hold a metalwire texture";
        mw_log("%s", state->local_error);
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

/* -------------------------------------------------------------------------------------------------
 * Blit encoder.
 * ---------------------------------------------------------------------------------------------- */

static id mw_cb_blit_encoder(id self, SEL _cmd)
{
    struct mw_command_buffer_state *state = mw_command_buffer_state_of(self);
    id encoder;
    void **slot;

    (void)_cmd;
    if (state == NULL || state->committed || gBlitEncoderClass == Nil) {
        return nil;
    }
    encoder = mw_alloc_init(gBlitEncoderClass, "blit encoder");
    if (encoder == nil) {
        return nil;
    }
    struct mw_encoder_state *encoder_state = calloc(1, sizeof(*encoder_state));

    slot = mw_slot(encoder, gBlitEncoderClass);
    if (encoder_state == NULL || slot == NULL) {
        free(encoder_state);
        [encoder release];
        return nil;
    }
    encoder_state->command_buffer = self; /* the command buffer keeps the encoder's memory safe */
    *slot = encoder_state;
    return [encoder autorelease];
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

    (void)_cmd;
    if (gCommandBufferClass == Nil) {
        mw_log("commandBuffer: no command buffer class");
        return nil;
    }
    command_buffer = mw_alloc_init(gCommandBufferClass, "command buffer");
    if (command_buffer == nil) {
        return nil;
    }
    state = calloc(1, sizeof(*state));
    slot = mw_slot(command_buffer, gCommandBufferClass);
    if (state == NULL || slot == NULL) {
        free(state);
        [command_buffer release];
        return nil;
    }
    state->status = MTLCommandBufferStatusNotEnqueued;
    state->t_begin_ns = mw_now_ns();
    state->queue = [self retain];
    *slot = state;
    return [command_buffer autorelease];
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

static id mw_device_new_texture(id self, SEL _cmd, id descriptor)
{
    MTLTextureDescriptor *texture_descriptor = (MTLTextureDescriptor *)descriptor;
    struct mw_texture_state *state;
    NSUInteger width, height, depth, mipmaps, samples, array_length;
    MTLPixelFormat pixel_format;
    MTLTextureType texture_type;
    MTLTextureUsage usage;
    MTLStorageMode storage_mode;
    id texture;
    void **slot;

    (void)self;
    (void)_cmd;
    if (texture_descriptor == nil) {
        mw_log("newTextureWithDescriptor: nil descriptor");
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
    if (texture_type != MTLTextureType2D || width == 0 || height == 0 || depth > 1) {
        mw_log("newTextureWithDescriptor: only a 2D width x height texture is in v1 (type %lu "
               "%lux%lux%lu)",
               (unsigned long)texture_type, (unsigned long)width, (unsigned long)height,
               (unsigned long)depth);
        return nil;
    }
    if (pixel_format != MTLPixelFormatBGRA8Unorm && pixel_format != MTLPixelFormatRGBA8Unorm) {
        mw_log("newTextureWithDescriptor: pixel format %lu is not in v1 (80 BGRA8Unorm or 70 "
               "RGBA8Unorm)",
               (unsigned long)pixel_format);
        return nil;
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
    mw_log(
        "newTextureWithDescriptor: texture %u (%lux%lu format %lu usage 0x%lx; pixels on the host)",
        state->remote_id, (unsigned long)width, (unsigned long)height, (unsigned long)pixel_format,
        (unsigned long)usage);
    return texture; /* +1, as a Metal "new" method */
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
    objc_registerClassPair(gTextureClass);
    mw_log("the class HakiosMetalTexture is registered (a subclass of %s)",
           objc_getClass("_MTLTexture") != Nil ? "_MTLTexture"
                                               : "NSObject: this system has no _MTLTexture");
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
    class_addMethod(gCommandBufferClass, sel_registerName("label"), (IMP)mw_cb_label, "@@:");
    class_addMethod(gCommandBufferClass, sel_registerName("setLabel:"), (IMP)mw_cb_label_set,
                    "v@:@");
    objc_registerClassPair(gCommandBufferClass);
    mw_log("the class HakiosMetalCommandBuffer is registered (a subclass of %s)",
           objc_getClass("_MTLCommandBuffer") != Nil
               ? "_MTLCommandBuffer"
               : "NSObject: this system has no _MTLCommandBuffer");
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
    class_addMethod(device, sel_registerName("newDrawableWithTexture:"),
                    (IMP)mw_device_new_drawable, "@@:@");
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

__attribute__((constructor)) static void mw_plugin_load(void)
{
    if (!mw_process_allowed()) {
        return;
    }
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
    mw_register_device_class();
    mw_register_queue_class();
    mw_register_command_buffer_class();
    mw_register_blit_encoder_class();
    mw_register_texture_class();
    mw_register_buffer_class();
    atexit(mw_atexit_summary);
    mw_log("the plug-in is ready: object graph device -> queue -> command buffer -> blit encoder, "
           "buffers and textures; every other Metal object answers nil and is logged once");
}
