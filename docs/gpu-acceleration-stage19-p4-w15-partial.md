# P4 w15 partial-screen: one-frame capture race, not missing dock/in-place (analysis only)

**结论**：cell3/cell4 的「右下缺 dock/图标 + cell3 红条」不是 `glremote` compose 没画 dock/壁纸，也不是
in-place 行写的顺序/裁剪错误；证据指向 **截图撞上 host replay 的第一张完整 home-screen 帧（或该帧的
writeback）**，截图里是**半帧**。cell3 无 in-place、cell4 有 192 行 in-place，两者同一块右下区域缺失，
说明不是 in-place 独有。

## 证据（只读 `/host/iosvm/logs/s19o-p4-cells/`）

- 像素：cell3/cell4 顶部行（y≈300–1200）与 cell1 完全同位置，最右到 x≈1056；从 y≈1400 起最右只到
  x≈521/562。右下 `x>562,y>1300` 非黑像素 **0**；dock 右半 `x>562,y>2050` 也是 **0**，dock 左半有内容。
  → dock 不是没画：Phone/Safari + dock 背景在，缺的是右半；更像整帧只写/只传到一半。
- cell3 host：截图时窗口从 `frame 128` 开始；frame 128 = 82,080 records、**0 image refs**、picture
  `a610cb56…`；紧接着 frame 129 = **236,026 records、48 image refs（1,239,821 B）**、picture
  `deee9b17…`。下一帧才像完整 home screen，截图正好在它前面。
- cell4 host：有 **192 行 in-place**（cell3 0 行），但同一右下区域仍 0；frame 174 也出现
  ～236,272 records/48 refs 的完整上传；窗口 `frame 188` 才开始。in-place 只改变回写方式，不解释同形状缺失。
- cell1/cell2 是 guest 本地 scanout（host 帧 0/120 也能全屏），所以没有 host replay 的半帧窗口。
- cell3 顶部红条 `rdar:45025538` 只在 cell3 出现，cell4 同图同路径没有；它是 SpringBoard 的临时
  debug overlay，不是 w15 的 dock 根因。

## 一次 boot 验证假设

同一 cell3/cell4 镜像，**UI 后等 10 s 再截一张**（或按 `UI_GRACE`），同时 `GLWIRE_DUMP` 抓同一 late
frame 的 picture/panel。观测点：

1. host log 找到 `236k records / 48 image refs` 的完整帧；记录截图对应 frame 是否仍 ≤ 该帧；
2. 对 late shot 裁 `x>562,y>1300` 与 `x>562,y>2050`：期望非黑 >0，dock 右半出现；
3. 比较 `GLWIRE_DUMP` 的 picture 与 panel：picture 全、scanout 半 → writeback/截图竞态；
   picture 本身半 → replay 源/裁剪问题；
4. cell3（NO_INPLACE=1）与 cell4（in-place）都 late 全 → 纯时序；仅 cell4 半 → 查 in-place rows；
   两者都半 → 回到 replay FBO，不再归因 in-place。

**判定**：若 late shot 恢复全屏，P4 表应改用 late screenshot，w15 视觉缺陷定性为“截图时序/半帧”，
不是 compose 缺 dock；若 late 仍半，按第 3/4 条继续分 picture vs scanout。
