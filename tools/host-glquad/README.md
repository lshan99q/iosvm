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

## gpu_host — the same drawing, on a socket

`gpu_host` is the prototype the guest will talk to: it listens on a port, accepts a layer
(`"F <w> <h> <bytes>\n"` then BGRA), draws it into a panel-sized frame on the GPU, and answers with
the composited frame (`"O <w> <h> <bytes>\n"` then BGRA).  A `Q` closes the connection.

Measured on this machine, with a real client sending a full 1125x2436 layer:

```
gpu_host: Tesla V100-SXM2-16GB/PCIe/SSE2 | 4.6.0 NVIDIA 580.178.04
gpu_host: 1125x2436 drawn in 30.82 ms
gpu_host: 1125x2436 drawn in 25.29 ms
```

25-31 ms a frame **including every copy**: 10.96 MB of layer in over TCP, upload, draw, 10.96 MB
frame back out. That already fits the 41.7 ms a frame that 24 a second allows -- and the copies are
exactly what disappears when the host maps the guest's RAM instead (then it is the 1.9 ms `glquad`
measures). The guest's half of this is the work that remains.

## The loop, closed: the guest asks the host to draw

`gpu_client` is the guest's half. Built for arm64-iOS, run inside the VM, it builds a layer,
sends it to the host over the QEMU user-mode network, and gets the composited frame back:

```
gpu_client: connected to 10.0.2.2:9100
gpu_client: 10962000 bytes of layer sent, waiting for the frame
gpu_client: frame back after 6800.2 ms
gpu_client: centre 127,127,60,255  corner 0,0,60,255
gpu_host:   1125x2436 drawn in 32.35 ms
```

The frame is pixel-correct -- the centre is exactly the gradient the client generated at that pixel
(127,127,60) -- so the drawing really did happen on the host's GPU and come back through the pipe.

The two numbers say where the work is now. **Drawing: 32 ms on the V100. Transfer: 6.8 s**, because
21.9 MB (a layer out, a frame back) is being copied twice through an emulated network stack that runs
as ARM instructions under TCG. That copy is the thing shared memory removes: when the host maps the
guest's RAM, the layer never moves, and the frame is written straight into the display surface.
