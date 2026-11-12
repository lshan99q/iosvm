# S22 ext4b — 正确布局内核跑完：host_info trace 0 次

**结论：内核布局已修好，boot 正常到 UI，但 `hakios: host_info` **0 次** ——
串口 cmdline 已确认含 `hakios_host_info_trace=1`（269/607 字符），所以不是
boot-arg 没进，而是 **extension 从未调用 `host_info`**。**

## 内核

- `hakios-kernel` **15,789,552 B / md5 `4d9f32f5325a30d82e09cd8d2ac78e28`**；
- `__TEXT=0xfffffff007004000`、`__PRELINK_TEXT=0xfffffff004004000`、
  `__LINKEDIT=0xfffffff00802c000`；trace 串 2 行；
- 旧 vmaddr=0 的根因：Ubuntu LLD 18 不实现 `-image_base`/`-segaddr`；必须 cctools
  `aarch64-apple-darwin11-ld`，且重链前删 `mach.development.bcm2837{,.unstripped}`。

## Numbers

| 项 | 值 |
|---|---|
| `hakios: host_info` | **0** |
| `xpcproxy spawned` | 35 |
| Widget INIT/running | 15（`FindMyWidgetPeople[48]` 05:28:35 等） |
| `checked in as alive` | 0 |
| end | `qemu-exited`（ui=840s），guestframes 37 / hostframes 536 |
| cmdline | 含 `hakios_host_info_trace=1`（269/607） |
| 截图 | 320K |

## 判断

- trace 开关有效但 0 次 → **extension 没调 host_info**；H2 的卡点比
  `pthread_init` 的 host_info 更早（logs/s22-ext/ext3 的 90 次采样是旧内核，
  可能因布局/初始化路径不同而停在 host_info；新内核下走得更早/不同路径）。
- 需要 code 看：是否 trace 只在 `host_info` 调用时才打印（无调用=无输出），
  以及 extension 在新内核下的实际卡点（建议再用 ustacks 采一次 live 栈）。
- 本发不计入 H2 闭环。

Artifacts: `logs/s22-ext/ext4/`（report.md 已更新；旧 report 保留为
`report-old-linker-fail.md`）。
