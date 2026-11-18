# stage 26 计划：目标 20 fps（≤50 ms/帧）

起点（2026-11-18，交付配置，单核 `SMP=1` 绑核 + 补丁常开）：**75–76 ms/帧 ≈ 13.9–14.5 fps**，画面逐字节同基线。
目标 20 fps = 50 ms/帧 → 要砍掉 **约 26 ms**。

## 一、当前账（实测）
| 段 | ms/帧 | 依据 |
|---|---:|---|
| wire 发送 + 等 ack 往返（census `copying out`） | ~10.5 | census 15 行中位 10 537 µs |
| 像素相关（750×1624 surface 的合成/布局/表面搬运） | ~31–37 | `fbscale=1`（像素 ÷4）实测 CPU/帧 75 → 44 ms |
| CA/SpringBoard 其余 + TCG 开销（未插桩） | ~25–30 | 余量 |
| display 整屏放大 blit | ~0.03 | 驱动自报 26–38 µs（`fb_noblit` 已废） |
| 宿主回放（不计入客机帧） | 1.4 | records 4–19 KB/帧 |

## 二、三条路线
- **路线 A（低风险，先做）：wire 与 flush 路径，预计 −10~14 ms → 62–66 ms ≈ 15–16 fps**
  1. ack 流水线 `glwire_ack_window=2`（实现已在树里，本轮正在验，−5~9 ms）；
  2. glFlush 13.3 ms 拆解（`clear_now` / 真 `glFlush` / socket 写），只砍能延后的（−3~5 ms）。
  做完 A 仍差 ~13–16 ms，到不了 20 fps。
- **路线 B（高收益、高风险）：像素路径，预计 −25~30 ms → 46–51 ms ≈ 20–21 fps**
  - 关键事实：`fbscale` 只改显示面尺寸（面板 1125×2436 → 显示 750×1624 对应 fbscale=2），而 iOS 的 UIKit 缩放来自**设备身份**（iPhone10,3 = 3×）。
    所以单独把 `fbscale` 调小（1×）会让 UI 画 750 宽的层进 375 宽的 surface → 裁切（18:14 实测：Dock 只剩 2 图标、右侧列缺、文字糊）。
  - 真正的做法二选一：
    a. **换设备身份/缩放**：让 iOS 以 1× 渲染（375×812 点），像素 ÷4。需要在 loader 的 identity（`xnuloader/mkidentity.sh`，从 IPSW 来）里改屏幕与 scale 字段；
    b. **改 UIScreen 的 scale 属性来源**（更 invasive，风险高）。
  - 先做可行性侦察（不改码）：找 identity 里 scale/屏幕的定义点，确认 iOS 16.5 在 1× 下能否正常布局（用一个 1× 设备身份试一发）。
- **路线 C（中风险）：脏矩形合成**。guest 侧只对变化区域做 CA 合成/表面搬运（对应上面 31–37 ms 里的“搬运”部分），需要 guest 改动 + 正确 root 构建（现在已能构建，配置配方本轮刚补：`GL=1 GLREMOTE=1 REST=1 CONFIGD=1`）。

## 三、判据与顺序
1. ack window 验证：CPU/帧 87–75 → ≤70 ms，且起机正常、画面 sha256 = 基线；无效则记负结果。
2. glFlush 拆解：先出三段耗时，再决定砍哪段。
3. 线程级采样（SpringBoard / backboardd / eagl 每帧 CPU）：把 25–30 ms 盲区分掉，确认它是否也是“像素相关”。
4. 若 1–3 后仍 > 60 ms → 上路线 B 的 identity 侦察（一发测试）。20 fps 大概率只能从这里拿。
5. 纪律：一发一台；墙钟 + load 一起报；画面必须同基准 sha256（`42e14ed2…`）或人工确认。

## 四、构建配方（本轮补齐，别再踩坑）
```
# 容器 iosvm--iosvm-code 内；SDK 必须放在容器可见路径（/host 只挂 iosvm 与 iosvm-work）
cp -r /home/lshan/theos/sdks-git/iPhoneOS16.5.sdk /home/lshan/DSH-desktop/iosvm-work/tmp-theos/sdks/
GL=1 GLREMOTE=1 REST=1 CONFIGD=1 THEOS=/host/iosvm-work/tmp-theos DARWIN=$PWD/sysimage ./userland/build.sh
```
- `GL=1`：编 `libhakios.gl.dylib`（不加就跳过 GL 库）；`GLREMOTE=1`：生成 `/redwood/bin/glremote_on`（不开就没有 remote-only，实测 `on=0`）；
  `REST=1`/`CONFIGD=1`：补齐 launchd 任务与配置。
- 构建会重写 `system.apfs`（2.4 GB）——跑前备份；输入不变时内容等价（实测回到了原始 `49ae3cf4`）。

## 五、本轮结果（2026-11-18 13:26 / 13:50）
### A1 ack 流水线（`glwire_ack_window=2`，新 root 已验证 `on=1`）
- 起机正常、画面 **sha256 = 基线 `42e14ed2…`**、0 panic；`s26-ackwin` 711 帧。
- 数字：墙钟中位 **14.3 fps**（18.5/16.3/14.3/12.8/4.0，load 6.7–8.8），CPU/帧中位 **72 ms**（对照同负载 75–76 ms）；
  census `copying out` **9 120 µs**（对照 10 537 µs，只降 1.4 ms）。
- 判读：**提升在噪声内（~4%），ack 往返的 10.5 ms 大头其实不是“等 ack”**，而是发送与记录路径本身。
  → `glwire_ack_window` 保持默认 1，不作为交付项；该实现留档。
### B 像素路径：卡在 loader 工具链
- 真凶确认：`xnuloader/loader.c:3247` **硬编码 `display-scale=3`**（并写进 `/chosen`，MobileGestalt 从这里取主屏 scale）。
  驱动侧虽然会随 `fbscale` 改 DT 属性，但 MobileGestalt 早已读走缓存 → 这就是 18:14 那次 `fbscale=1` 画面被裁的原因。
- 已做的准备：给 loader.c 加了 `#ifndef LOADER_DISPLAY_SCALE`（默认 3，行为不变），重建时 `-DLOADER_DISPLAY_SCALE=1` 即可 1× 运行。
- **阻塞**：容器里没有 `lld-link`，`xnuloader/build.sh` 跑不了（`command not found`），loader.efi 无法重建。
  两条路：① 装 lld（新软件，需登记 `/home/lshan/new/n.md` 并征得同意）；② 直接对 loader.efi 打**二进制补丁**把 `mov #3` 改成 `#1`（可逆、但需要反汇编定位）。本轮均未做。
- 结论：**20 fps 的路线 B 需要先解决 loader 构建/改值**；路线 A 已证明最多 ~1.4 ms，达不到 20 fps。

## 六、下一轮建议顺序
1. 解决 loader：装 lld 或用 llvm-objdump 定位并二进制改 `display-scale`（3→1），再 `fbscale=1` 起一发，看布局是否正常（预期：画面正确 + CPU/帧 75 → ~45–50 ms）。
2. 若 1× 布局可接受 → 巩固 20 fps；若布局崩 → 回到 C（脏矩形）路线，20 fps 降级为“需更大改动”。
3. 线程级采样仍值得做（补 25–30 ms 盲区），可与上面并行。
