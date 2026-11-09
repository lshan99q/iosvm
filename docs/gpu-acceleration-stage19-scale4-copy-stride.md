# scale=4 “host 卡死”：host 没卡在 replay，是 guest 局部拷贝 stride 错

## 日志事实

`logs/s20-payload4/host.log`：
- frame 10/11/12 都有 `upload scale 4 sends 303688 of 4872000 picture B`，frame 12
  interval 0.03 s；
- frame 12 之后 host 又 **replay 22063 records**，然后 **`client 16 closed`**；
- 再之后 `poll idle 663.40 s`，没有新 client。

所以 host 不是卡在 readback/FBO/poll/replay；最后是 **guest 主动关掉连接**，host 回到 poll 等新 client。guest 第 12 帧后全是 `flush -> 0` + `sent 0 rows` + `f 1 wait 4–11 ms`，不是 host 在回放。

## 直接 bug（guest）

`eagl.c:1458` 的局部拷贝用了：

```c
copy_used = used / scale;          /* 3000 / 4 = 750 */
```

但 host 的 packed 行宽是：

```c
w = width / scale;                 /* 750 / 4 = 187 */
row = w * 4;                       /* 748, not 750 */
```

guest 每行多读 **2 B**，从第 2 行起 source 指针逐行漂移，最后一共读 406*750=304,500 B，
而 host 只发了 303,688 B。结果：画面内容错位/越界读，guest 侧后续 present/CA 行为异常，
连接被关；host 只是看到 `client closed`。

## 最小修复

`copy_width = width / scale; copy_used = copy_width * 4;`（`s19o-p1-uploadscale-fix.patch`，
19 行）。scale=1 时 `copy_width=width`、`copy_used=used`，原全帧路径不变。

- 新 lib **401,776 B / md5 811e826d73f15076c07717909f44169e**；
- host 不变（`fd6667ad...`），e2e default PASS；
- 重跑 scale4：先看 host 是否继续出 frame、guest 是否不再 `sent 0 rows`；若仍关，再在
  guest `eagl_remote_flush` 入口加 `w.at`/send 日志，不要先改 dirty。
