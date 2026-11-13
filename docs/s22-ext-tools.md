# S22 extension 诊断工具（2026-11-13 更新）

## 采样 guest 线程栈：按 **pid**，不要按名字

**旧说法（已废弃）**：`ustacks=<name>` 按名字采。两个坑：

1. `ustacks` 的 name 匹配是 `strncmp(proc_name_address(proc), name, 32)`，但
   `proc_name_address` 是 **16 字节 p_comm** → **名字 >15 字符必不匹配**
   （`BooksWidgetExtension`(21) 实际 `BooksWidgetExte`(15)）；
2. `qemu/touch.py stacks <name>` 走 guest virtio 键盘（QEMU
   `input-send-event`），**shift 不被 guest 驱动处理** → 带大写基本打不进去。

**新用法（可靠）**：按 pid 采。

```sh
# pid 来自 serial：
#   hakios: exec pid=N path=/Applications/.../XxxWidget.appex/XxxWidget
#   或 service spawned with pid N
cd /host/iosvm-work/hakios-xnu-kernel/qemu
python3 touch.py stacks <pid>
```

内核（`ustacks_fire()`，md5 `06bf09c2473e2792f90db068583bbab1` 起）在名字
**全数字**时按 `proc_pid(proc)==N` 匹配；`touch.py stacks <pid>` 即走这条路径。

## ext18 实测（2026-11-13）

- 内核 `193cbe74b000d3e53846de44a9f663ed`（pid 采样 + exit 追踪 + exec 追踪）；
- 按 pid 采到 5 个 widget 的栈，**每个只有 1 个线程** → 卡在单线程启动早期；
- exit 追踪也生效：`hakios: exit pid=… comm=… status=…`。

## 其他有用 boot-arg

| 参数 | 作用 |
|---|---|
| `hakios_host_info_trace=1` | `hakios: host_info enter/ret` |
| `hakios_hostport_trace=1` | `hakios: hostport msg id=<n>`（MIG 分发入口） |
| `hakios_*exec*/exit*` | 内核自带 exec/exit 追踪（ext14 起） |
| `ustacks=<pid>` | 数字名 = 按 pid 采样 |
| `ustacks_at=<sec> ustacks_count=N ustacks_ms=1000 ustacks_threads=8 ustacks_tname=*` | 定时窗口采样 |

## 归档位置

- `logs/s22-ext/{ext1..ext18}/`（未入库，`.gitignore` 忽略 logs/）；
- 摘要入库：`docs/s22-ext-index.md` 与各 `docs/s22-ext-NN.md`。
