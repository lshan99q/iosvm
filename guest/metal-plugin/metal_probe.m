/* SPDX-License-Identifier: MIT */
/*
 * metal_probe: what Metal answers in this process, when the metalwire device bundle is installed.
 *
 * The first boot's question is not "is the picture right" but "did the discovery chain work at
 * all": the kernel named the plug-in (boot-arg metal=N), Metal loaded the bundle, the class passed
 * MTLAddDevice's _MTLDevice/MTLDevice conformance checks, and this process is one the plug-in shows
 * the device to. When metal_frame fails, this program says which of those steps answered nil.
 *
 * It creates the device and a queue and prints what they answer; it does not make a command buffer
 * and sends nothing on the wire, so it works with the host translator not running.
 *
 *   usage: metal_probe
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/message.h>
#include <stdio.h>

#include "mw_metal_extras.h"

int main(void)
{
    @autoreleasepool {
        printf("metal_probe: MTLCreateSystemDefaultDevice()...\n");

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();

        if (device == nil) {
            printf(
                "metal_probe: nil. The chain is: kernel publishes IOAcceleratorES with\n"
                "  MetalPluginName=HakiosMetal (boot-arg metal=N) -> Metal loads\n"
                "  /System/Library/Extensions/HakiosMetal.bundle -> the bundle registers\n"
                "  HakiosMetalDevice -> MTLAddDevice accepts it. Check the boot-arg, the bundle\n"
                "  path, and the metalwire: lines this process printed before this one.\n");
            return 1;
        }
        printf("metal_probe: device \"%s\"\n", [[device name] UTF8String]);
        printf("metal_probe: registryID %llu\n", (unsigned long long)[device registryID]);
        printf("metal_probe: maxBufferLength %llu\n", (unsigned long long)[device maxBufferLength]);
        printf("metal_probe: hasUnifiedMemory %d\n", (int)[device hasUnifiedMemory]);
        /* macOS-only in the SDK, but Darwin asks a device for them (CoreImage's limits): call them
         * by name so the probe reports what this device actually answers. */
        SEL low_power = sel_registerName("isLowPower");
        SEL removable = sel_registerName("isRemovable");
        SEL headless = sel_registerName("isHeadless");
        BOOL (*bool_send)(id, SEL) = (BOOL(*)(id, SEL))objc_msgSend;

        printf("metal_probe: isLowPower %d isRemovable %d isHeadless %d\n",
               [device respondsToSelector:low_power] ? (int)bool_send(device, low_power) : -1,
               [device respondsToSelector:removable] ? (int)bool_send(device, removable) : -1,
               [device respondsToSelector:headless] ? (int)bool_send(device, headless) : -1);
        if ([device respondsToSelector:@selector(supportsFamily:)]) {
            printf("metal_probe: supportsFamily(Apple1=%d) %d\n", (int)MTLGPUFamilyApple1,
                   (int)[device supportsFamily:MTLGPUFamilyApple1]);
        }
        if ([device respondsToSelector:@selector(supportsFeatureSet:)]) {
            printf("metal_probe: supportsFeatureSet(iOS_GPUFamily1_v1) %d\n",
                   (int)[device supportsFeatureSet:MTLFeatureSet_iOS_GPUFamily1_v1]);
        }
        printf("metal_probe: newDrawableWithTexture: %s\n",
               [device respondsToSelector:@selector(newDrawableWithTexture:)] ? "yes" : "no");

        id<MTLCommandQueue> queue = [device newCommandQueue];

        if (queue == nil) {
            printf("metal_probe: newCommandQueue returned nil\n");
            return 1;
        }
        printf("metal_probe: queue \"%s\"\n", [[queue label] UTF8String]);
        printf("metal_probe: ok\n");
        return 0;
    }
}
