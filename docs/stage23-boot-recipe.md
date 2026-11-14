# stage 23 boot 配方（2026-11-15 00:55 实测可用；00:59 exp 验证通过）

## 结论：boot 掉 UEFI Shell 的真因是 **NVRAM（vars.fd）失效**

`qemu/vars.fd` 被之前的实验写入过（`-vga none` / `-nodefaults` 改变了 PCI 编号，`Pci(0x1,0x0)` → `Pci(0x2,0x0)`），
保存下来的启动项指向已不存在的设备，固件于是回落到内建 Shell，并且**会把这个错误状态继续保存下去**。

## 修法：把 vars.fd 重置成空白变量区

```bash
cd /host/iosvm-work/hakios-xnu-kernel
cp qemu/vars.fd /tmp/vars.fd.bak                    # 备份
python3 -c 'open("qemu/vars.fd","wb").write(b"\xff"*(64*1024*1024))'
```

固件启动时会重新格式化变量区并自己建立启动项。重置后 md5 = `8317cb794bd8dda60669eabb3840eff7`
（64 MiB 全 0xFF）。

## 可用启动命令（**payload 必须放进 ESP**）

`qemu/esp` 由项目工具生成，里面同时有 loader 和 payload：

```bash
python3 tools/mkesp.py qemu/esp-fresh qemu/payload.bin
rm -rf qemu/esp && mv qemu/esp-fresh qemu/esp     # 或先把旧目录改名备份
```

然后（**不要** `-nodefaults`，**不要** `-vga none`；`QEMU_ESP=1` 让 run.sh 用 ESP 里的 payload、去掉 loader 设备）：

```bash
cd /host/iosvm-work/hakios-xnu-kernel
QEMU_ESP=1 QEMU_RAM_GB=6 QEMU_INSTANCE=1 QEMU_VIRTIO_GPU=1 \
QEMU_DISPLAY=none QEMU_RAM_SHARE=off CMDLINE="glwire_local=1 virtio_gpu=1" \
QEMU_EFI=$PWD/qemu/efi64.fd QEMU_VARS=$PWD/qemu/vars.fd \
  bash qemu/run-darwin.sh 200 -L "$PWD/qemu-roms"
```

## boot-measure 一条命令（本次已加分支）

`boot-measure.sh` 原来无条件 `QEMU_ESP_IMAGE=$IOSVM/qemu/esp.img`，而 `esp.img`（11-02）里没有
payload → 固件找不到可引导项就掉内建 Shell。**这就是 ext53 之后所有 boot 全挂的原因。**

修法（只加分支，其他默认值不动）：

```bash
if [ "${QEMU_ESP:-0}" = 1 ]; then
	ESP_ENV="QEMU_ESP=1"
else
	ESP_ENV="QEMU_ESP_IMAGE=$IOSVM/qemu/esp.img"
fi
env QEMU_SMP=... QEMU_TB_MB=... QEMU_RAM_GB=12 QEMU_NET=1 $ESP_ENV \
	IMG=... ROOT=... CMDLINE=... \
	timeout 7500 bash qemu/run-darwin.sh "$MAXSECS" ${QEMU_EXTRA:-} >> "$LOG" 2>&1 &
```

于是：

```bash
cd /host/iosvm/tools/glwire
K=/host/iosvm-work/hakios-xnu-kernel
IOSVM=$K GLWIRE=/host/iosvm/tools/glwire QEMU_EFI=$K/qemu/efi64.fd QEMU_VARS=$K/qemu/vars.fd \
QEMU_ESP=1 QEMU_SMP=1 QEMU_VIRTIO_GPU=1 QEMU_DISPLAY=none QEMU_RAM_SHARE=off \
XCMD="glwire_local=1 virtio_gpu=1 launchd_disable_all_timeouts=1" \
QEMU_EXTRA="-L $K/qemu-roms" HOST_ENV="" \
  ./harness/boot-measure.sh <label> /tmp/root-w15.apfs 300
```

## 验证结果（2026-11-15 00:59，kernel md5 `23e67b27`）

boot 成功、kernel 加载、A1 驱动扫到设备（串口原文）：

```text
virtio-gpu: slot 27: after reset: status=0xb sel=0 num=0 ready=0
virtio-gpu: slot 27: wrote num=8, read back 0, ready=0
virtio-gpu: slot 27: after queue setup: status=0xf sel=0 num=0 ready=1
virtio-gpu: slot 27 id=16 transport=2 queue=8 max=1024 features=30000002:00000101 virgl=0 edid=1 uuid=0 blob=0 ctx_init=0 version1=1
virtio-gpu: slot 27: phys=0x19179c000 desc0 0x19179c400/24/0x1/1 desc1 0x19179c500/408/0x2/0 sizeof(resp)=408
virtio-gpu: slot 27: after poll: request deadbeef ..., page[0x500..0x510]=00000000 ...
virtio-gpu: slot 27: response area nonzero words=0, first at 0xffffffff
virtio-gpu: slot 27: used flags=0 idx=1 id=0 len=24; response 00000000 ...
```

- 判据一 `virtio-gpu: slot 27` **通过**；
- 判据二 `response area nonzero words=...` **出现**，但值为 0：设备写了 used ring（`idx=1 len=24`），
  **整个 4 KiB 响应区一个字节都没写** → A1 的 response 问题仍在（desc1 的写地址/NEXT 或 QEMU 走了 nodata 路径）；
- 队列：`desc0 0x19179c400/24/0x1/1`（READ|NEXT）、`desc1 0x19179c500/408/0x2/0`（WRITE）。

## 宿主侧看图（A2/A3 判据用）

`-display none` 下 **QMP `screendump` 可用**（实测），不需要 `egl-headless`/VNC：

```bash
# QEMU 本来就带 -qmp tcp:127.0.0.1:$((4445 + 10*INSTANCE)),server,nowait
python3 tools/glwire/harness/gpushot.py /tmp/gpuscanout.png --instance 1
```

- 工具：`tools/glwire/harness/gpushot.py`（QMP `screendump`；QEMU 10 直接写 PNG，老版本回退 PPM 转换）；
- 固件 scanout 是 1280x800；guest `SET_SCANOUT` 之后 screendump 变成 **750x1624**（A2 生效的证据）；
- 画面全黑是正常的（A3 前 backing 没被推过）；A3 之后应看到 dock/桌面。

## 注意
- **不要** `-nodefaults`（改 UEFI 默认设备/启动顺序，不跑 startup.nsh）；
- **不要** `-vga none`（PCI 编号变 `Pci(0x2,0x0)`，固件再保存一次坏 NVRAM）；
- `-L "$PWD/qemu-roms"` 提供 `efi-virtio.rom` 与 `vgabios-ramfb.bin`（默认设备集才需要）；
- boot 前确认无残留：`pgrep -af "qemu-syste[m]|host_glwir[e]"`；
- **每次起机前重置 vars.fd**：`python3 -c 'open("qemu/vars.fd","wb").write(b"\xff"*(64*1024*1024))'`；
- 若又掉 Shell：先查 `vars.fd` 是否被写脏（应全 0xFF）、`qemu/esp/EFI/hakios/payload.bin` 是否存在。
