# xlt QEMU 8.2.2 patch series

Apply the patches in order to an unmodified QEMU 8.2.2 tree:

```sh
cd /path/to/qemu-8.2.2
for p in /path/to/xlt/qemu/patches/*.patch; do
    git apply "$p"
done
```

- `0001-accel-add-xlt-accelerator.patch`: the `xlt-accel` accelerator class,
  its mandatory `xlt-accel-ops` object, `CONFIG_XLT` wiring and the PL011
  dependency.
- `0002-hw-arm-add-xlt-machine.patch`: the `xlt-machine` board, PL011, ELF
  loader and host callbacks.

`xlt/qemu/build-xlt.sh` performs this sequence on a disposable copy in
`$QEMU_WORK_DIR/qemu-8.2.2-xlt`, so the pristine tree stays untouched. On a
host where `git apply` is run from a parent repository that also contains the
QEMU source, use `git apply --directory=qemu-8.2.2 <patch>` from that parent,
or `patch -p1 -d qemu-8.2.2 < <patch>`.
