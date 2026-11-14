# Stage 23 userland build（GL=1 + A3）

在 code 容器里，只有 `/host` 和 `/usr/bin/clang` 可用。固定环境：

```bash
cd /host/iosvm-work/hakios-xnu-kernel/userland
export PATH=/host/iosvm-work/hakios-xnu-kernel/xnu-deps/host/bin:$PATH
export LD_LIBRARY_PATH=/host/iosvm-work/toolchain/lib
export THEOS=/host/iosvm-work/toolchain/theos
GL=1 ./build.sh
```

## 三个坑

1. **`GL=1` 是环境变量，不是参数**。写成 `./build.sh GL=1` 会静默构建无 GL 的
   `root.apfs`；必须 `GL=1 ./build.sh`（或 `env GL=1 ./build.sh`）。
2. **`ld64.lld` 找不到 `_getpid/_strdup`**：把 `xnu-deps/host/bin` 放到 `PATH` 前面，
   用 cctools 的 `ld`。
3. **`libBlocksRuntime.so` / `-lSystem`**：
   - `LD_LIBRARY_PATH=/host/iosvm-work/toolchain/lib`；
   - `THEOS=/host/iosvm-work/toolchain/theos`，让 `xnu-clang -isysroot` 指向
     `theos/sdks/iPhoneOS16.5.sdk`（`$HOME/theos` 不存在时就会报 `-lSystem`）。

产物：

```text
userland/root.apfs   5.7 MB
```

校验 lib 里确实有 A3：

```bash
strings userland/root/lib/libhakios.gl.dylib | grep /dev/hakios-gpu
```

`strings root.apfs` 看不到是正常的：APFS 镜像里的文件内容被压缩/分段，不要用它判断。

## 启动

```bash
cd /host/iosvm-work/hakios-xnu-kernel
python3 -c "open('/tmp/vars-ff.fd','wb').write(b'\\xff'*(64<<20))"
QEMU_RAM_GB=6 QEMU_ESP=1 QEMU_VIRTIO_GPU=2 QEMU_RAM_SHARE=off \
QEMU_VARS=/tmp/vars-ff.fd QEMU_DISPLAY=none \
  qemu/run-darwin.sh 700 -L "$PWD/qemu-roms"
```

`QEMU_ESP=1` 让 payload 走 ESP 文件；`QEMU_VARS` 是 64MB 0xFF 的模板，避免掉 UEFI Shell。
