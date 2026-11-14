# s23-a3 — virtio_gpu=2 验证（内核侧通过；userland 推送被编译环境卡住）

## 一、内核侧（已验证）

启动：`QEMU_VIRTIO_GPU=2`（run-darwin 自动加 `-device virtio-gpu-device` 并把 guest
cmdline 设成 `virtio_gpu=2`）+ `QEMU_ESP=1` + `-L qemu-roms`，root 用 `/tmp/root-w15.apfs`。

串口证据：

```text
virtio-gpu: slot 27 id=16 transport=2 queue=8 max=1024 features=30000002:00000101 ...
virtio-gpu: display 0: 1280x800 enabled=1
virtio-gpu: command 0x101 -> response 0x1100 (used id=0 len=24)   RESOURCE_CREATE_2D
virtio-gpu: command 0x106 -> response 0x1100 (used id=0 len=24)   ATTACH_BACKING
virtio-gpu: command 0x103 -> response 0x1100 (used id=0 len=24)   SET_SCANOUT
virtio-gpu: command 0x105 -> response 0x1100 (used id=0 len=24)   TRANSFER_TO_HOST_2D
virtio-gpu: command 0x104 -> response 0x1100 (used id=0 len=24)   RESOURCE_FLUSH
virtio-gpu: A2: resource 1 750x1624, 298 entries, scanout 0, panel push enabled
virtio-gpu: /dev/hakios-gpu ready (panel push)
virtio-gpu: device in slot 27
```

- a) `/dev/hakios-gpu ready (panel push)` **出现**；
- b) `hakios-gpu: panel push failed` **0 次**；
- A2 五条命令全部 `0x1100`（之前的 response 0 问题已消失）；
- `QMP screendump` 尺寸从 1280x800（固件 scanout）变成 **750x1624**（guest SET_SCANOUT 生效），
  画面全黑（backing 未被推过）—— 与「eagl 还没 push」一致。

## 二、userland 推送（未验证，被环境卡住）

`userland/build.sh` 在本容器无法编译：**`/usr/bin/clang` 不存在**
（`xnu-clang: line 19: /usr/bin/clang: No such file or directory`，容器只剩 gcc）。
且现有 `userland/root.apfs`（17:44）是 **无 GL** 构建（`/lib` 里没有 `libhakios.gl.dylib`），
`/tmp/root-w15.apfs`（11-09）虽有 GL 但早于 eagl.c 的 A3 改动（`strings` 里 `hakios-gpu` = 0 次）。

→ 需要：带 clang 的构建环境，或 code 侧产出的 **GL=1 + A3 eagl.c** 的 root.apfs。

## 三、fps

`end=timeout ... hostframes=2`（300 s 内只出 2 帧，处于已知的 ~500 s 首帧慢窗口），
且本发 root 没有 eagl 推送，**fps 不可比**。要测 A3 的帧率收益，需要新的 root + 更长的
MAXSECS（≥600），并同时记录 `frame_us`/`copying it out`。

## 四、产物

- `tools/glwire/harness/gpushot.py`：QMP `screendump` → PNG（`-display none` 下实测可用）；
- 本目录：serial/host/drive、`scanout-black-750x1624.png`、`userland-build-blocked.log`。

---

# 补充（18:25）：用 code 的 GL root 跑，A3 push 仍未触发 —— 根因在 eagl 的调用位置

用 `/host/iosvm-work/root-a3-gl.apfs`（md5 537998c4，GL=1 已确认，`/lib/libhakios.gl.dylib`
里确有 `/dev/hakios-gpu` 和 `panel push failed` 字符串）跑 900 s：

- `virtio-gpu: /dev/hakios-gpu ready (panel push)` ✅；
- **每帧 `command 0x105/0x104` 没有出现**（各只有 A2 初始化 1 次）；
- `hakios-gpu: panel push failed` = 0（ioctl 根本没被调用，不是失败）；
- 串口大量 `hakios-remote: end of frame, on=0 images=28 first=750x1624 panel_up=1 pid=14`；
- `hakios-gl: frame 420: ... 1 picture copied out`（GL 层正常出帧）；
- `host_glwire` 0 帧；screendump 750x1624 全黑；`end=noui hostframes=0 guestframes=37 panic=0`。

## 根因

`userland/src/eagl.c`：

```c
1411: if (eagl_remote_is_on() && e->image_count > 0 && e->images[0].width > 0) {
        ...
1494:     eagl_gpu_push(e);        /* 只在 remote 开启时调用 */
```

`eagl_remote_is_on()` = `remote_enabled`，它只在 `/redwood/bin/glremote_on` 存在时置 1
（`build.sh` 的 `GLREMOTE=1`）。code 的 `root-a3-gl.apfs` 里 **没有** `/bin/glremote_on`
（ext37 的 `/tmp/root-w15.apfs` 有），所以 `on=0`，`eagl.c:1411` 整块跳过，
`eagl_gpu_push()` 从不执行。

## 要的修改

1. **把 `eagl_gpu_push(e);` 移出 `if (eagl_remote_is_on() ...)` 块**（或在 `panel_up`
   分支后无条件调用）—— 否则 `glwire_local=1`（产品路径，on=0）永远不会 push；
2. 想先用 remote 路径验证：`build.sh` 加 `GLREMOTE=1` 产出带 `/bin/glremote_on` 的 root
   （但那是 R1 路径）。

Artifacts: `/host/iosvm/logs/s23-a3/glroot-no-glremote/`.

---

# 补充（18:43）：push 改动生效，但首次 push 触发内核 panic —— gpu_dev 是悬垂栈指针

用修复版 GL root（md5 `edadfc9b2c46371372ce44c6b2ab0b0d`）跑 900 s：

- boot 正常：A2 五条命令 `0x1100`、`/dev/hakios-gpu ready (panel push)`；
- 约 8 分钟后 **panic**：

```text
panic(cpu 0 caller 0xfffffff007af4e34): Kernel data abort.
  at pc 0xfffffff0074f1fa4, lr 0xfffffff0074f1f74
  x20: 0xfffffffb0d9a8000  x21: 0xfffffffa94ae3d18  far: 0xfffffffa94af66c0  esr: 0x96000047
```

- 符号化：`pc = _gpu_cmd+0xa8`（指令 `strh wzr, [x8, #4]`，即 `d->avail->ring[slot] = 0`），
  `lr = _gpu_cmd+0x78`；
- `end=crash … panic=2`。

## 根因

`drivers/gpu/hakios.virtio_gpu.c`：

```c
666: struct gpu_device d;              /* 栈上局部 */
684: if (gpu_attach(&d, slot)) {
371:     gpu_dev = d;                  /* 把栈地址存进全局 */
695:     return;                       /* d 出作用域 → gpu_dev 悬垂 */
```

init 时 A2 的 `gpu_cmd(&d, …)` 仍有效，所以五条命令都成功；之后第一次
`EAGL_GPU_PUSH` → `gpu_present()` → `gpu_cmd(gpu_dev, …)` 用的是已失效栈帧
（x21=0xfffffffa94ae3d18 就是内核栈地址），`d->avail` 变成垃圾指针 → data abort。

## 修法

`struct gpu_device d;` → `static struct gpu_device d;`（或文件作用域 / kmem_alloc）。

证据：`/host/iosvm/logs/s23-a3/glroot-a3-crash/`（panic.txt / serial.log / drive.log）。

## 2026-11-14 19:20：code 容器首跑（glwire_local=1，700s）

内核 **8d63362f6f0345d0c9abbef73b1a9df2**（`gpu_device` static 修复），
root **9246d1c57f965d6d7a5adcc80268fa08**（GL=1，panel 1125x2436 最近邻缩到 750x1624）。
启动：`QEMU_RAM_GB=6 QEMU_ESP=1 QEMU_VIRTIO_GPU=2 QEMU_DISPLAY=none
CMDLINE="glwire_local=1 virtio_gpu=2" -L qemu-roms`，UEFI AAVMF 64MB + vars 64MB 0xFF。

结果：

- `virtio-gpu: /dev/hakios-gpu ready`：**出现**；
- `hakios-gpu: panel push failed`：**0 次**；
- `command 0x105/0x104`：各 **55** 次（A3 push 真正触发）；
- A1/A2 五条仍全 0x1100，`display 0: 1280x800 enabled=1`；
- eagl census（4 个有 surface 的帧）：

| frame | frame_us | copying out |
|---:|---:|---:|
| 1 | 4,570,889 | 72,337 |
| 2 | 3,911,209 | 28,908 |
| 3 | 3,773,712 | 57,116 |
| 4 | 3,983,590 | 48,956 |

- 700s 内 `hakios-gl: frame` 到 50+，但大量是 "1 draw, none into an IOSurface"；
  有效帧约 **0.08–0.25 fps**，**低于** ext39 默认路径的 245 ms/frame（~4 fps）。
- 没有做 virtio_gpu=1 同参数对照；ext39 的 245ms 是 glwire host 路径，不是本轮的
  glwire_local。

**结论**：A3 内核/用户态 push 链路已通（尺寸、命令、0 失败），但 **glwire_local=1 的
guest Mesa 路径本身慢**，A3 只增加一次 1125→750 缩放，未带来 fps 收益。下一步应：
1. 用默认 glwire host 路径 + A3（需要 GLREMOTE=1 的 root）对照；
2. 查 `glwire_local` 下 3.2s 未归类耗时（handing 594ms + copying 49ms + before 134ms
   只占约 0.8s，剩余像 `glFinish/sync_frames`）。

---

# 结论（19:37）：A3 push 端到端通过，但比 glwire 慢 6–9 倍

内核 `8d63362f6f0345d0c9abbef73b1a9df2`（gpu_device static 修复）+ root
`9246d1c57f965d6d7a5adcc80268fa08`（GL=1，1125x2436→750x1624 最近邻），
`QEMU_ESP=1 QEMU_VIRTIO_GPU=2 QEMU_DISPLAY=none`，900 s。

| 判据 | 结果 |
|---|---|
| `/dev/hakios-gpu ready (panel push)` | ✅ |
| `hakios-gpu: panel push failed` | ✅ 0 次 |
| `0x105 / 0x104 -> 0x1100` | ✅ **各 1001 次**（每帧 push） |
| `command timed out` / panic | ✅ 0 / 0（`end=qemu-exited panic=0`） |
| 宿主侧画面 | ✅ 有真实内容：Apple 启动 logo → Weather widget 槽（黑底白卡），不再全黑 |
| fps | ❌ `hakios-gl: frame` **1.6–2.3 s/帧**（约 0.4–0.6 fps），98 帧 |

同 glwire_local 对照（ext39，无 push）：`frame_us` ≈ 245 ms/帧，稳态 2–3.3 fps。
→ **A3 这条全帧 push 路径（guest Mesa + 4.8 MB copyin + TRANSFER/FLUSH）比 glwire 慢 6–9 倍**，
链路通但不带来 fps 收益，与 code 的判断一致。

截图：`logs/s23-a3/glroot-a3-ok/scanout-apple-logo.png`、`scanout-weather-widget.png`。

---

# 补充（20:01）：gpu_ioctl 计时 —— A3 push 只花 ~7 ms，不是瓶颈

内核 `af866234`（每 60 次 push 打计时）+ root `21eea74b`（GL=1 GLREMOTE=1），900 s：

```text
copyin=4516..6195 us  transfer=1181..2306 us  flush=555..964 us  total=6624..8467 us
```

- 2410 次 push，0 失败；
- **每帧 push = 6.6–8.5 ms**（copyin 占大头 4.5–6.2 ms，transfer 1.2–2.3 ms，flush 0.6–0.9 ms）；
- → **A3 不是慢的原因**；1.6–2.3 s/帧在 guest CA/Mesa/glFinish 一侧。

## 同时发现：GLREMOTE（remote-only）本发没送出帧

- `hakios-remote: end of frame, on=1 … pid=14`，但 **`flush -> 0` ×2407**（一条都没发）；
- `host_glwire` 只有 listening，4 行，hostframes=0；
- 结束帧只有 pid=14（backboardd），**SpringBoard(pid 8) 没有 frame_end**；
- screendump 全黑（3622 B），`end=noui hostframes=0 panic=0`。

对照 ext37（local）：`flush -> 2 … surface 1 row 3072 pid=16`，host 3823 帧。
→ remote-only 下 wire 为空、SpringBoard 不进画帧，是独立问题（不是 A3），需要 code 另查。

证据：`/host/iosvm/logs/s23-a3/glremote-timing/`。
