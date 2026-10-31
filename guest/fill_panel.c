/* SPDX-License-Identifier: MIT */
/*
 * fill_panel: put a picture of our own on the panel of the guest, to prove the display path end to
 * end. It draws nothing through Metal or CoreAnimation -- it writes pixels itself and hands them to
 * the kernel's GPU service (the same call the upstream gl_test uses), so what it proves is the
 * path from a guest process, through the kernel, to the framebuffer the host reads.
 *
 * The panel's pixels are blue first (BGRA) and its rows go bottom up; both are the upstream
 * convention, and getting either wrong is a picture that is upside down and colour-swapped rather
 * than absent -- so this is written to be obviously right.
 *
 *   fill_panel [SECONDS [WIDTH HEIGHT]]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* launchd keeps a job's stdout to itself; the console of this guest is the serial line, which is
 * what the host reads after the run. So every marker goes there, twice if stdout is a terminal. */
static int console = -1;

static void tell(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (console >= 0) {
            ssize_t w = write(console, buf, (size_t)n);
            (void)w;
        }
        fputs(buf, stdout);
        fflush(stdout);
    }
}

/* drivers/gpu/hakios.gpu.cpp: kHakiosShow (0x7f), "pixels of the caller's onto the panel" */
struct show {
    uint64_t pixels;
    uint32_t width, height, row_bytes, pad;
};

static void paint_bgra(uint8_t *to, unsigned width, unsigned height)
{
    /* A picture that is unmistakably not a black screen and not a mistake of orientation:
     * a vertical gradient, then four 1/8-height bars at the bottom in R, G, B and white order,
     * then a white square in the middle. Every one of those is wrong in a different way if the
     * row order or the channel order is wrong. */
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            uint8_t *p = to + ((size_t)y * width + x) * 4;
            uint8_t v = (uint8_t)(255 - (y * 255 / (height ? height : 1)));

            p[0] = v; /* blue */
            p[1] = v; /* green */
            p[2] = v; /* red */
            p[3] = 255;
        }
    }
    unsigned band = height / 8;
    for (int i = 0; i < 4; i++) {
        for (unsigned y = height - band; y < height; y++) {
            for (unsigned x = (unsigned)i * width / 4; x < (unsigned)(i + 1) * width / 4; x++) {
                uint8_t *p = to + ((size_t)y * width + x) * 4;
                p[0] = (i == 0 || i == 3) ? 0 : 255;
                p[1] = (i == 2) ? 255 : 0;
                p[2] = (i == 0) ? 255 : 0;
                p[3] = 255;
            }
        }
    }
    for (unsigned y = height / 2 - 60; y < height / 2 + 60; y++) {
        for (unsigned x = width / 2 - 60; x < width / 2 + 60; x++) {
            uint8_t *p = to + ((size_t)y * width + x) * 4;
            p[0] = p[1] = p[2] = p[3] = 255;
        }
    }
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    unsigned width = argc > 3 ? (unsigned)atoi(argv[2]) : 1125;
    unsigned height = argc > 3 ? (unsigned)atoi(argv[3]) : 2436;
    const size_t bytes = (size_t)width * height * 4;
    uint8_t *drawn = malloc(bytes), *shown = malloc(bytes);
    io_service_t service;
    io_connect_t connection;

    if (drawn == NULL || shown == NULL) {
        tell("fill_panel: no memory for %zu bytes\n", bytes);
        return 1;
    }
    paint_bgra(drawn, width, height);
    /* the panel's rows go down, the picture's go up */
    for (unsigned y = 0; y < height; y++) {
        memcpy(shown + (size_t)y * width * 4, drawn + (size_t)(height - 1 - y) * width * 4,
               (size_t)width * 4);
    }

    console = open("/dev/console", O_WRONLY);
    tell("fill_panel: console %d, %ux%u\n", console, width, height);
    io_service_t wanted = 0;

    /* What is actually in the registry, before asking for a name that may not be there. A service
     * made by hand rather than by a kext can be published under a name the matcher does not expect,
     * and "no such service" and "a service with another name" look identical from here. */
    {
        io_iterator_t it = 0;

        if (IOServiceGetMatchingServices(0, IOServiceMatching("IOService"), &it) == 0 && it != 0) {
            io_object_t o;
            int shown = 0, total = 0;

            while ((o = IOIteratorNext(it)) != 0) {
                char name[128];
                io_name_t cls;

                total++;
                if (IORegistryEntryGetName(o, name) == 0 && IOObjectGetClass(o, cls) == 0) {
                    if ((strstr(name, "Hakios") != NULL || strstr(cls, "Hakios") != NULL) &&
                        shown < 8) {
                        tell("fill_panel: registry: name=%s class=%s\n", name, cls);
                        shown++;
                    }
                    /* Take the service while it is in hand: matching it again by name is what
                     * failed, so the object the enumeration produced is the one to open. */
                    if (wanted == 0 && strstr(cls, "HakiosGPU") != NULL) {
                        wanted = o;
                        IOObjectRetain(wanted);
                    }
                }
                IOObjectRelease(o);
            }
            tell("fill_panel: registry: %d services, %d named Hakios\n", total, shown);
            IOObjectRelease(it);
        } else {
            tell("fill_panel: registry: cannot enumerate\n");
        }
    }
    service = wanted != 0 ? wanted : IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
    {
        kern_return_t opened =
            (service == 0) ? -1 : IOServiceOpen(service, mach_task_self(), 0, &connection);

        tell("fill_panel: service %#x, open %#x\n", service, opened);
        if (service == 0 || opened != 0) {
            tell("fill_panel: no GPU service to put pixels on the panel with\n");
            return 1;
        }
    }
    tell("fill_panel: %ux%u, %d seconds; corner %u %u %u %u, middle %u %u %u %u\n", width, height,
         seconds, shown[0], shown[1], shown[2], shown[3], shown[(size_t)(height / 2) * width * 4],
         shown[(size_t)(height / 2) * width * 4 + 1], shown[(size_t)(height / 2) * width * 4 + 2],
         shown[(size_t)(height / 2) * width * 4 + 3]);
    for (int i = 0; i < seconds * 5 && i < 10; i++) {
        struct show s = {(uint64_t)(uintptr_t)shown, width, height, width * 4, 0};
        kern_return_t result =
            IOConnectCallStructMethod(connection, 0x7f, &s, sizeof(s), NULL, NULL);

        if (i == 0) {
            tell("fill_panel: on the panel (%#x)\n", result);
        }
        usleep(200000);
    }
    /* Leave the frame and go: the stage that started this program waits for it, so a long stay
     * delays the whole system. Nothing else writes the panel on a machine without a GPU, so the
     * picture is still there for whoever looks next. */
    tell("fill_panel: the picture is on the panel\n");
    /* A look at the user space from inside it, with nobody's permission needed: a child that waits
     * for the system to settle and then prints the process table on the same console. The launchd
     * job that was meant to do this never got past xpcproxy, and the question it was asked -- what
     * is actually running when the screen stays on the boot logo -- is still the one worth asking.
     */
    if (fork() == 0) {
        char *ps_argv[] = {"/bin/ps", "-A", NULL};
        char *ps_envp[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL};

        for (int i = 0; i < 900; i++) { /* 180 s, in the same 200 ms steps */
            usleep(200000);
        }
        execve(ps_argv[0], ps_argv, ps_envp);
        tell("fill_panel: cannot run %s\n", ps_argv[0]);
        _exit(1);
    }
    return 0;
}
