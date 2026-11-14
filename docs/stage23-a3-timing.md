# A3 push 计时（2026-11-14）

内核 **af866234843ddbc281668359f4d0a0a4**，在 `gpu_ioctl()` 里记录：

- `copyin`：1624 行 × 3000 B 的逐行 `copyin`；
- `transfer`：`TRANSFER_TO_HOST_2D`；
- `flush`：`RESOURCE_FLUSH`。

每 60 次 push 打一行：

```text
hakios-gpu: push 61 copyin=... us transfer=... us flush=... us (total ... us)
```

root **21eea74b5c74eda57f7706887723fa27** 是 `GL=1 GLREMOTE=1`（含 `/bin/glremote_on`），
用于 host glwire 路径 + A3 对照；`/host/iosvm-work/root-a3-glremote.apfs`。

启动：

```bash
QEMU_RAM_GB=6 QEMU_ESP=1 QEMU_VIRTIO_GPU=2 QEMU_DISPLAY=none \
QEMU_EFI=/tmp/uefi/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd \
QEMU_VARS=/tmp/vars-ff.fd \
  qemu/run-darwin.sh 700 -L "$PWD/qemu-roms"
```

`GLREMOTE=1` 时 `glwire_remote` 需要 host；exp 的 host_glwire 路径负责。若只看 A3 开销，
clone 的 local root 9246d1c5 也能打同样的计时行。
