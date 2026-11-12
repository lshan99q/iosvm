# S22 extension check-in 索引（2026-11-12）

`logs/` 被 `.gitignore` 忽略，索引落在 docs（归档实体在 `/host/iosvm/logs/s22-ext/`）。

| 目录 | 发 | system/root | 结论 | 计入 |
|---|---|---|---|---|
| `logs/s22-ext/ext1/` | 1 | 6be494f1 / root-byref | acceptance 660s 早于 extension 666s；串口只剩 2.3K loader 头 | **否**（设计问题/串口丢失） |
| `logs/s22-ext/ext2/` | 2 | 6be494f1 / root-byref | 关全部 timeout 后 5s 杀消失（hanging 0、CHECK_IN_TIMEOUT 0），6 widget 全 spawn；`checked in as alive` 仍 0 | **是：H1 部分成立** |
| `logs/s22-ext/ext3/` | 3 | 6be494f1 / root-byref | 90 次 ustacks 全 `_mach_msg2_trap → _host_info → ___pthread_init → _libSystem_initializer` | **是：H2 确认、H3 排除**（符号化 `docs/s22-ext-03-symbolized.txt`） |
| `logs/s22-ext/ext4/` | 4 | 8bf5f838 / root-w15（内核 4d9f32） | boot 到 UI 840s、widget 15 次 INIT；`hakios: host_info` **trace 0**（cmdline 含 trace=1） | **是：trace 0 = 请求未达内核 host_info，非 timeout/非崩溃** |
| `logs/s22-ext/ext5/` | 5 | 8bf5f838 / root-byref | ustacks 120 次全 `no process called FindMyWidgetPeo`（实际 `PeopleWidget_iO`） | **否：工具错误，无结论** |

## 关键产物

- 内核 `hakios-kernel` 4d9f32（`__TEXT=0xfffffff007004000`，cctools ld64 重建）；
- 符号化 `docs/s22-ext-03-symbolized.txt`、`docs/gpu-acceleration-stage22-ext3-symbols.md`；
- host_info 审计/补丁 `docs/gpu-acceleration-stage22-host-info.md`、
  `tools/glwire/patches/host-info-fix.patch`；
- 观测清单 `docs/s22-extension-checkin-observability.md`；
- 终局 `docs/gpu-acceleration-stage22-final.md`。

## 终局

H1 部分成立 + H2 确认（卡在 libSystem `pthread_init` 的 host_info/mach_msg2，
请求未进内核 `host_info()`）+ H3 排除；下一步 = host port/MIG server 未就绪假设
（内核 trace `ipc_kobject_server_internal` msgh_id=200、`find_mig_hash_entry`、
`ipc_host_init` 的 IKOT_HOST 注册）+ 最小 guest 探针。
