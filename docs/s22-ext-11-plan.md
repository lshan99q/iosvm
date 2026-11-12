# s22 ext11 — probe2 逐步打印（模板，未 boot）

**目的**：用 code 的新探针 `hostinfo_probe2`（逐步打印 + fflush + host_info 在前）
判定 ext10 的「第一行后崩掉」到底是阻塞还是崩溃；看 `probe2:` 行停在哪一步。

## 准备（已做）

- `rc.vtop.hostinfo2`：`rc.vtop` 的 `exec /sbin/launchd.darwin` **之前**插入
  `/redwood/bin/hostinfo_probe2 >>/dev/console 2>&1`；
- 探针二进制等 code 放 `/host/iosvm/logs/s22-ext/probe-bin/hostinfo_probe2`
  （**当前未到**）。

## 启动（探针到后）

```sh
cd /host/iosvm-work/hakios-xnu-kernel/userland
rm -f root.apfs
EXTRA="/host/iosvm/logs/s22-ext/probe-bin/hostinfo_probe2" \
RC=rc.vtop.hostinfo2 PREBUILT=1 GL=1 GLREMOTE=1 CONFIGD=1 ./build.sh
cp -a root.apfs /tmp/root-ext11.apfs
# boot 1000s
IOSVM=… GLWIRE=… QEMU_EFI=… QEMU_VARS=… QEMU_SMP=1 ZERO_RAM=1 \
  MINFRAMES=999999 MINSHOT=300000 UI_GRACE=400 NOUI_BY=900 MAXSECS=1000 \
  XCMD="glwire_local=1 launchd_disable_all_timeouts=1" HOST_ENV="GLWIRE_NO_PANEL=1" \
  ./harness/boot-measure.sh ext11 /tmp/root-ext11.apfs 1000 --no-panel
```

## 判据

| 观察 | 结论 |
|---|---|
| `probe2: before host_info` 之后无 `after` | **真阻塞**（host_info 卡住） |
| 打印 `host_info kr=0` | 链路正常 |
| 停在更早的 `probe2:` 行 | 那一步崩/阻塞 |

## 记录（待填）

- root md5、内核 md5、`probe2:` 全部行、`checked in as alive`、`exit(78)`。
