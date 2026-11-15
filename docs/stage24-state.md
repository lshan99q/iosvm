# stage 24 状态（11:05，停手点）

## 当前
- 画面：**完整**（host picture 四行图标 + Search + dock 全在，`logs/stage24/full-patch/home-complete-remote.png`）
- fps：**7.69**（host 帧间隔中位 0.130s；p25 0.060s）
- census（frames≥200，n=318）：frame_us **127.2ms** = waiting **2µs** + copying out **49.4ms** + CA/其它 **79.3ms**

## 生效开关
- host：`GLWIRE_STATIC_PATCH_FILE=/tmp/patch-full.bgra`（整屏 750x1624 @0,0，4,872,000B）+ `GLWIRE_NO_PANEL=1`
  （验证用 `GLWIRE_DUMP=<prefix>` + `GLWIRE_DUMP_AT=1100`；目录自动创建）
- guest：remote-only root `root-stage24-census3.apfs`（有 glremote_on、无 glwire_local、RC=rc.vtop、netcfg）
- 启动：`QEMU_ESP=1`、默认设备集（无 -nodefaults/-vga none）、`-L qemu-roms`、每次重置 vars.fd

## md5
- system.apfs `49ae3cf4fd53334b475a921eb9915008`；kernel `af866234843ddbc281668359f4d0a0a4`
- root `8b8a747746e9b50fc45ae3b6efc7acec`；libhakios.gl.dylib `93419a1aec97240579d8c2c02862166b`(419,840B)
- patch-full.bgra `1c76479e036a9816322031837f2835b6`

## 未完成 / 下一个待验证点
1. 唯一剩余性能项：copying out **49ms** = host reply 等待 → **双缓冲 async**（预期 ~80ms ≈ 12fps）；
2. 验收还差「滚动」一段 fps（当前只测静止）；
3. `glwire_static_patch`（guest 发送版）已可弃用，离线补丁取代它。
