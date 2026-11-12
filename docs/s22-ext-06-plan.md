# s22 ext6 — host_info guest 探针（模板，未 boot）

**目标**：一个直接调 `host_info` 的 guest 可执行探针，判定 host port MIG 是
普遍坏了还是 extension 特有。**不需要 UI，快跑快收。**

## 最小 boot 配置（准备）

- **RC=rc.darwin**（`rc.darwin` 的头部示例：`/redwood/bin/hello_dyn one two`
  后面接 `/bin/df` 等；探针加一行 `/redwood/bin/hostinfo_probe` 即可）；
- `loader_return_after` 由 `boot-measure.sh` 的 `SECS` 决定（`run-darwin.sh:100`
  的 `loader_return_after=$SECS`），**用 120s**（够 rc 跑完探针，不等 UI）；
- 命令：`PREBUILT=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.darwin ./build.sh` →
  `/tmp/root-ext6.apfs`；然后
  `IOSVM=… GLWIRE=… QEMU_EFI=… QEMU_VARS=… QEMU_SMP=1 ZERO_RAM=1 \
   ./harness/boot-measure.sh ext6 /tmp/root-ext6.apfs 120 --no-panel`
  （`UI_GRACE=0`、`MINFRAMES=999999`，不看 UI）；
- serial 落盘复用 `boot-measure.sh` 的 `/tmp/ext6.log`，归档到
  `logs/s22-ext/ext6/serial.log`。

## 判据（code 的 3 种输出）

| 输出 | 判定 | 含义 |
|---|---|---|
| `hostinfo: OK ncpu=N mem=M` | host port 正常 | 问题在 extension 的调用上下文 |
| `hostinfo: BLOCKED after 5s` | host port MIG 普遍阻塞 | 大发现，直接指向内核 IPC |
| `hostinfo: KR=...` | 明确错误码 | 按码查（KERN_INVALID_ARGUMENT 等） |

## 待办

- [ ] code 给 `userland/src/hostinfo_probe` 源码 + 构建/注入命令；
- [ ] 把探针放进 `root/bin/`，`rc.darwin` 加 `/redwood/bin/hostinfo_probe`；
- [ ] 构建 `root-ext6.apfs`（md5 记录）；
- [ ] boot（120s），归档 drive/serial/host/shot；
- [ ] 按上表判定，写结论；推 iosvm-exp 合 main。

## 记录

- system：待填（预期 `8bf5f838`）；
- root：待填；
- host/kernel：`hakios-kernel` 4d9f32（正确布局，含 host_info trace）；
- 探针 md5：待填。
