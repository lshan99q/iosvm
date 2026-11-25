/* SPDX-License-Identifier: MIT */
/*
 * metal_frame: the Metal test client of the metalwire experiment.
 *
 * It is an ordinary Metal program: it asks for the system device, makes a queue, one buffer, one
 * texture and a drawable, and then for each frame:
 *
 *   fill the buffer with the test picture below, record a blit of the whole buffer into the
 *   texture, present the drawable, commit, wait.
 *
 * It never touches the socket. The device bundle installed in place of HakiOS's
 * (guest/metal-plugin/ HakiosMetal.m) records the commit onto the wire to the host translator and
 * answers the wait from the host's acknowledgement; this program only measures what Metal and the
 * device did.
 *
 * The drawable is the experiment's one deviation from stock Metal: there is no CAMetalLayer in this
 * port's user space to ask for one, so the device has its own -newDrawableWithTexture: (declared in
 * mw_metal_extras.h). Everything else here is public Metal API.
 *
 * The test picture (750x1624 pixel BGRA8Unorm, 4,872,000 bytes), deterministic in the frame number:
 *
 *   - a base row that changes per frame: for pixel x,
 *       R = x * 255 / 749                  (a left-to-right red ramp: tells x from x)
 *       G = (x * 3 + frame * 5) & 0xff     (moves left every frame)
 *       B = (x + frame * 3) & 0xff         (moves left, three times faster)
 *       A = 255
 *     and that row is copied to all 1624 rows. The result is horizontal bands that scroll — a
 *     gradient whose motion is unambiguous between two consecutive frames.
 *   - a dark ruler every 128 rows (y = 0, 128, 256, ...): tells y from y and catches a vertically
 *     flipped or transposed translation.
 *   - 16 blocks of 24x24 at y = 32, x = 32 + i * 28: block i is white when bit i of the frame
 * number is set and near-black when it is clear — the frame counter, readable in a screenshot.
 *   - a 4-pixel white border: catches cropping and off-by-one row/column amounts.
 *
 * The host reads the texture back and publishes it (tools/glwire/webview.py --shm ...), so the
 * picture is seen, not just checksummed. The CRC the host compares is computed by the device
 * bundle over exactly the bytes this program put in the buffer.
 *
 *   usage: metal_frame [-n FRAMES] [-s SECONDS] [-c STRIDE]
 *     -n FRAMES    stop after FRAMES frames (default 30)
 *     -s SECONDS   stop after SECONDS seconds, whichever comes first (default: no time limit)
 *     -c STRIDE    checksum every STRIDEth row (default 1 = every row; must match the host's
 *                  --crc-stride; it prices the checksum, not the translation)
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mw_metal_extras.h"

#define MW_WIDTH 750
#define MW_HEIGHT 1624
#define MW_BYTES_PER_PIXEL 4
#define MW_PICTURE_BYTES ((NSUInteger)MW_WIDTH * MW_HEIGHT * MW_BYTES_PER_PIXEL)

static mach_timebase_info_data_t gTimebase;
static pthread_once_t gTimebaseOnce = PTHREAD_ONCE_INIT;

static void timebase_init(void)
{
    if (mach_timebase_info(&gTimebase) != 0 || gTimebase.denom == 0) {
        gTimebase.numer = 1;
        gTimebase.denom = 1;
    }
}

/* microseconds since boot, from the same clock the device bundle's timing uses */
static uint64_t now_us(void)
{
    pthread_once(&gTimebaseOnce, timebase_init);
    return (uint64_t)((__uint128_t)mach_absolute_time() * gTimebase.numer / gTimebase.denom / 1000);
}

/* The picture, exactly as the comment at the top of this file describes it. */
static void fill_picture(uint32_t *pixels, unsigned frame)
{
    uint32_t row[MW_WIDTH];
    unsigned phase_g = frame * 5u;
    unsigned phase_b = frame * 3u;

    for (unsigned x = 0; x < MW_WIDTH; x++) {
        unsigned r = x * 255u / (MW_WIDTH - 1);
        unsigned g = (x * 3u + phase_g) & 0xffu;
        unsigned b = (x + phase_b) & 0xffu;

        /* memory order of BGRA8Unorm: byte 0 = B, 1 = G, 2 = R, 3 = A */
        row[x] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
    for (unsigned y = 0; y < MW_HEIGHT; y++) {
        memcpy(pixels + (size_t)y * MW_WIDTH, row, sizeof(row));
    }
    for (unsigned y = 0; y < MW_HEIGHT; y += 128) {
        for (unsigned x = 0; x < MW_WIDTH; x++) {
            pixels[(size_t)y * MW_WIDTH + x] = 0xff303030u;
        }
    }
    for (unsigned bit = 0; bit < 16; bit++) {
        uint32_t value = ((frame >> bit) & 1u) != 0 ? 0xffffffffu : 0xff101010u;

        for (unsigned y = 0; y < 24; y++) {
            for (unsigned x = 0; x < 24; x++) {
                pixels[(size_t)(32 + y) * MW_WIDTH + 32 + bit * 28 + x] = value;
            }
        }
    }
    for (unsigned y = 0; y < 4; y++) {
        memset(pixels + (size_t)y * MW_WIDTH, 0xff, MW_WIDTH * MW_BYTES_PER_PIXEL);
        memset(pixels + (size_t)(MW_HEIGHT - 1 - y) * MW_WIDTH, 0xff,
               MW_WIDTH * MW_BYTES_PER_PIXEL);
    }
    for (unsigned y = 4; y < MW_HEIGHT - 4; y++) {
        uint32_t *line = pixels + (size_t)y * MW_WIDTH;

        for (unsigned x = 0; x < 4; x++) {
            line[x] = 0xffffffffu;
            line[MW_WIDTH - 1 - x] = 0xffffffffu;
        }
    }
}

static void usage(const char *program)
{
    printf("usage: %s [-n FRAMES] [-s SECONDS]\n", program);
    printf("  -n FRAMES   stop after FRAMES frames (default 30)\n");
    printf("  -s SECONDS  stop after SECONDS seconds (default: no time limit)\n");
    printf(
        "  -c STRIDE   checksum every STRIDEth row (default 1; match the host's --crc-stride)\n");
}

int main(int argc, char **argv)
{
    unsigned requested_frames = 30;
    double requested_seconds = 0.0;
    unsigned done = 0;
    unsigned ok = 0;
    unsigned failed = 0;
    uint64_t encode_sum = 0, encode_min = UINT64_MAX, encode_max = 0;
    uint64_t trip_sum = 0, trip_min = UINT64_MAX, trip_max = 0;
    uint64_t started_us;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            requested_frames = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            requested_seconds = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            /* The device reads this when it computes the frame checksum; set before it exists. */
            setenv("METALWIRE_CRC_STRIDE", argv[++i], 1);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (requested_frames == 0) {
        requested_frames = 1;
    }

    @autoreleasepool {
        printf("metal_frame: %ux%u BGRA8Unorm, %lu bytes per frame, %u frames requested%s\n",
               MW_WIDTH, MW_HEIGHT, (unsigned long)MW_PICTURE_BYTES, requested_frames,
               requested_seconds > 0.0 ? ", with a time limit" : "");

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();

        if (device == nil) {
            printf("metal_frame: MTLCreateSystemDefaultDevice() returned nil\n");
            printf(
                "metal_frame: the metalwire bundle is not registered in this process. Check: the\n"
                "  boot-arg metal=N is set (the kernel must publish MetalPluginName); this "
                "program\n"
                "  is in /redwood/lib/HakiosMetal.clients beside the bundle (or is metal_frame); "
                "and\n"
                "  the bundle is at /System/Library/Extensions/HakiosMetal.bundle.\n");
            return 1;
        }
        printf("metal_frame: device \"%s\"\n", [[device name] UTF8String]);

        id<MTLCommandQueue> queue = [device newCommandQueue];

        if (queue == nil) {
            printf("metal_frame: newCommandQueue returned nil\n");
            return 1;
        }
        printf("metal_frame: queue \"%s\"\n", [[queue label] UTF8String]);

        id<MTLBuffer> buffer = [device newBufferWithLength:MW_PICTURE_BYTES
                                                   options:MTLResourceStorageModeShared];
        if (buffer == nil) {
            printf("metal_frame: newBufferWithLength returned nil\n");
            return 1;
        }
        if ([buffer contents] == NULL) {
            printf("metal_frame: the buffer has no CPU-visible contents\n");
            return 1;
        }
        printf("metal_frame: buffer %lu bytes, storage mode %lu\n", (unsigned long)[buffer length],
               (unsigned long)[buffer storageMode]);

        MTLTextureDescriptor *descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:MW_WIDTH
                                                              height:MW_HEIGHT
                                                           mipmapped:NO];
        if (descriptor == nil) {
            /* the convenience constructor was not there: the properties are all a descriptor is */
            descriptor = [[MTLTextureDescriptor alloc] init];
            descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
            descriptor.width = MW_WIDTH;
            descriptor.height = MW_HEIGHT;
            descriptor.mipmapLevelCount = 1;
            descriptor.sampleCount = 1;
            descriptor.textureType = MTLTextureType2D;
        }
        if (descriptor == nil) {
            printf("metal_frame: this Metal has no usable MTLTextureDescriptor\n");
            return 1;
        }
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];

        if (texture == nil) {
            printf("metal_frame: newTextureWithDescriptor returned nil\n");
            return 1;
        }
        printf("metal_frame: texture %lux%lu format %lu\n", (unsigned long)[texture width],
               (unsigned long)[texture height], (unsigned long)[texture pixelFormat]);

        id<MWRemoteDrawable> drawable = [(id<MWRemoteDevice>)device newDrawableWithTexture:texture];

        if (drawable == nil) {
            printf("metal_frame: newDrawableWithTexture returned nil (not our device?)\n");
            return 1;
        }
        printf(
            "metal_frame: drawable %lu; frame picture is documented at the top of metal_frame.m\n",
            (unsigned long)[drawable drawableID]);

        started_us = now_us();
        for (;;) {
            uint64_t elapsed_us = now_us() - started_us;

            if (done >= requested_frames) {
                break;
            }
            if (requested_seconds > 0.0 && (double)elapsed_us >= requested_seconds * 1e6) {
                break;
            }

            uint64_t fill_begin = now_us();

            fill_picture((uint32_t *)[buffer contents], done);
            uint64_t fill_us = now_us() - fill_begin;
            uint64_t encode_begin = now_us();
            id<MTLCommandBuffer> command_buffer = [queue commandBuffer];

            if (command_buffer == nil) {
                printf("metal_frame: frame %u: [queue commandBuffer] returned nil\n", done);
                return 1;
            }
            id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];

            if (encoder == nil) {
                printf("metal_frame: frame %u: blitCommandEncoder returned nil\n", done);
                return 1;
            }
            [encoder copyFromBuffer:buffer
                       sourceOffset:0
                  sourceBytesPerRow:MW_WIDTH * MW_BYTES_PER_PIXEL
                sourceBytesPerImage:MW_PICTURE_BYTES
                         sourceSize:MTLSizeMake(MW_WIDTH, MW_HEIGHT, 1)
                          toTexture:texture
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            [encoder endEncoding];
            [command_buffer presentDrawable:drawable];

            uint64_t encode_us = now_us() - encode_begin;
            uint64_t commit_begin = now_us();

            [command_buffer commit];
            uint64_t commit_us = now_us() - commit_begin;
            [command_buffer waitUntilCompleted];
            uint64_t wait_us = now_us() - commit_begin;
            uint64_t total_us = now_us() - encode_begin;
            MTLCommandBufferStatus status = [command_buffer status];
            NSError *error = [command_buffer error];
            const char *error_text = error != nil ? [[error localizedDescription] UTF8String] : "-";

            printf("metal_frame: frame=%u fill_us=%llu encode_us=%llu commit_us=%llu "
                   "wait_us=%llu total_us=%llu status=%lu error=%s\n",
                   done, (unsigned long long)fill_us, (unsigned long long)encode_us,
                   (unsigned long long)commit_us, (unsigned long long)wait_us,
                   (unsigned long long)total_us, (unsigned long)status, error_text);
            done++;
            if (status == MTLCommandBufferStatusCompleted && error == nil) {
                ok++;
                encode_sum += encode_us;
                if (encode_us < encode_min) {
                    encode_min = encode_us;
                }
                if (encode_us > encode_max) {
                    encode_max = encode_us;
                }
                trip_sum += wait_us;
                if (wait_us < trip_min) {
                    trip_min = wait_us;
                }
                if (wait_us > trip_max) {
                    trip_max = wait_us;
                }
            } else {
                failed++;
            }
        }

        uint64_t elapsed_us = now_us() - started_us;
        double fps = elapsed_us != 0 ? (double)done * 1e6 / (double)elapsed_us : 0.0;

        printf("metal_frame: summary: frames=%u ok=%u failed=%u elapsed_us=%llu fps=%.2f "
               "encoded_us_mean=%llu min=%llu max=%llu round_trip_us_mean=%llu min=%llu max=%llu\n",
               done, ok, failed, (unsigned long long)elapsed_us, fps,
               (unsigned long long)(ok != 0 ? encode_sum / ok : 0),
               (unsigned long long)(encode_min == UINT64_MAX ? 0 : encode_min),
               (unsigned long long)encode_max, (unsigned long long)(ok != 0 ? trip_sum / ok : 0),
               (unsigned long long)(trip_min == UINT64_MAX ? 0 : trip_min),
               (unsigned long long)trip_max);
        return failed == 0 && done != 0 ? 0 : 1;
    }
}
