# host-glquad — how long a frame takes when the host draws it

The interface in the guest costs about 700 ms a frame, because CoreAnimation's software renderer
runs as AArch64 instructions under QEMU's TCG. The question this answers is the one that decides
whether moving the drawing to the host is worth the work: **how long does the same frame take on
the host's GPU?**

```
$ make            # or: gcc -O2 -o glquad glquad.c -lEGL -lGL
$ ./glquad 1125 2436 10
glquad: renderer Tesla V100-SXM2-16GB/PCIe/SSE2 | NVIDIA Corporation | 4.6.0 NVIDIA 580.178.04
glquad: 1125x2436, 10 frames, 1.91 ms a frame (draw + readback)
```

It makes an EGL context with no window (picking an EGL device, so it can land on the real GPU
instead of llvmpipe), uploads a 1125x2436 BGRA layer the size CoreAnimation asks for, draws it as a
textured quad into a renderbuffer that size, and reads it back — every frame, timed.

**1.9 ms a frame, with a 41.7 ms budget for 24 a second.** So the host is not the problem and never
was; what is missing is the pipe that carries the guest's drawing to it.
