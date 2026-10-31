# 设计基线 · 可行性调研

> 状态：历史文档，部分结论已被 ADR-008 / ADR-009 取代，见 docs/zh/README.md。

# 在 Linux 上运行 iOS 虚拟机

## 结论

**没有合法且可日常使用的「iOS 虚拟机」**。Apple 的许可协议只允许 iOS 运行在 Apple 硬件上，官方唯一可用的虚拟化产品是 macOS 上的 **iOS Simulator**——它跑的是编译成 x86_64/arm64 的 App 二进制 + 一套模拟框架，**不是 iOS 系统本身**，也不能装 App Store 应用。

想在 Linux 上跑，只有 4 条路，按实用性排序：

| 方案 | 能拿到什么 | 能不能上 Linux | 代价 / 限制 |
|---|---|---|---|
| **云端真机**（BrowserStack / Sauce Labs / AWS Device Farm / 阿里云移动测试 / WeTest） | 真 iPhone，可点、可截图、可跑 Appium | ✅ 纯浏览器/API | 按分钟计费；有网络延迟；数据出内网 |
| **Appetize.io** | 浏览器里的 iOS Simulator（可交互） | ✅ 浏览器 | 运行的是模拟器二进制，非真系统；免费额度小 |
| **Corellium** | 真正的 iOS 虚拟设备（ARM 全系统虚拟化，可视化界面，可越狱调试） | ✅ 有 Linux Server 版，可本地部署 | 企业级价格；需自备强 ARM 或高主频 x86 + KVM；合规需自担 |
| **研究性 QEMU**（darwin-vm / qemu-t8030 / Docker-eyeOS） | 只到 root shell，无 SpringBoard、无图形、无 WiFi/蓝牙 | ✅ | 不是给人用的；darwin-vm 提取固件必须有一台 Mac；x86 上 ARM 全模拟极慢 |

## 各方案要点

### 1. 云端真机（推荐）
Linux 上装 Appium + 对应的 WebDriver 客户端，连远端真机：
```bash
npm i -g appium
appium driver install xcuitest
# 云端用 BrowserStack/Sauce 的 hub 地址替换本地 URL
```
适合：自动化测试、看真实渲染效果、验证真机 bug。

### 2. Appetize.io
纯浏览器交互，可上传自己的 `.app`/`.ipa`（模拟器构建）。适合演示、截图、验收 UI。不适合测性能/硬件能力。

### 3. Corellium
唯一能"在 Linux 上看到 iOS 桌面并操作"的方案：完整 ARM 虚拟化，提供 VNC/Web 界面、SSH、内核调试、越狱。Cellebrite 有专门的 Linux Server 部署文章。注意：Apple 曾起诉 Corellium，虽然后来部分胜诉，但商用前建议自己过一遍法务。

### 4. QEMU 研究方案
- `jprx/darwin-vm`：支持模拟到 A19 / iOS 27，**但作者明确说不是 iPhone 模拟器**——没有屏幕、WiFi、蓝牙、图形、GUI App、SpringBoard，只是启动到 root shell 让你跑命令行程序和调内核。且 `get_files.sh` 与固件签名 **必须在一台 Mac 上执行**。
- `TrungNguyen1909/qemu-t8030`：iPhone 11 内核模拟，同样是研究用途。
- `Docker-eyeOS`：容器里跑 iOS，带 KVM/GDB 内核调试，性质相同。

x86_64 主机上做 ARM 全系统模拟，性能会非常糟（分钟级启动，指令级翻译）。

### ❌ 不要走的路
- **Linux 上装 QEMU 跑 macOS（Docker-OSX）再开 iOS Simulator**：违反 macOS 许可（仅限 Apple 硬件），且拿到的只是 Simulator，不是 iOS。
- **Android 模拟器换 iOS 主题**：纯外壳，不是 iOS。
- **PlayCover**：只能在 Apple Silicon 的 macOS 上跑解密后的 ipa，与 Linux 无关。

## 实际建议

- 只是要**看/测 iOS App** → 云端真机（BrowserStack 等）或 Appetize.io。
- 要**交互式完整 iOS 环境**且预算允许 → Corellium。
- 要**玩 iOS 内核 / 底层研究** → darwin-vm（先借一台 Mac 生成固件）。
- 要在本机**长期开发 iOS App** → 老老实实买台 Mac mini，或云 Mac（MacStadium / AWS EC2 mac 实例）。

---

# 附：在安卓手机硬件上跑 iOS（HakiOS）

## 关键结论

**它不是虚拟机，是裸金属原生移植（native port / bring-up）。** 没有 hypervisor、没有指令翻译，XNU 内核直接跑在骁龙的 CPU 核心上，性能接近真机。和上面那套 QEMU 模拟是两条完全不同的路。

项目：
- [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) — 主干，XNU 移植 + 引导器 + 驱动 + 工具
- [superboyyy/hakios-arm64e-dada](https://github.com/superboyyy/hakios-arm64e-dada) — 分支，小米 15（SM8750 / 骁龙 8 Elite）跑 **iOS 26 arm64e**

## 为什么能跑（三个前提）

1. **XNU 是开源的**（APSL 2.0）。iOS 内核本体就是 XNU，Apple 以开源形式发布源码 → 可以自己编译。但 kernelcache 里 Apple 的 **kext 驱动不开源**，所以 HakiOS 不用 Apple 的驱动，而是**自己重写整套驱动**（显示、触摸、UFS 存储、GPU、Wi-Fi、音频 DSP、调制解调器、振动、摄像头）。参考对象是 Linux 的 `msm` / `ath12k` / `qcom` 驱动——高通平台的寄存器布局在 Linux 侧是公开的。

2. **指令集同源**：Apple A 系列和骁龙都是 ARM64。新一代骁龙的 Oryon 核心支持 **arm64e + 硬件 PAC（指针认证）**，所以 iOS 26 的 arm64e 用户空间二进制可以**原生执行**，不需要二进制翻译。这也解释了为什么老 SoC 不行——没有 arm64e/PAC 就跑不了现代 iOS 用户空间。

3. **引导链自己搭**：解锁 bootloader → `fastboot boot` 加载一个自编译 UEFI（[Mu-Silicium / Project Silicium](https://github.com/Project-Silicium/Mu-Silicium)，高通平台的 edk2 移植）→ UEFI 启动自己的 EFI loader（`xnuloader`）→ loader 按手机**自己的设备树**（不是硬编码地址）加载 XNU kernelcache → 内核启动 → 挂载 APFS 系统镜像（用户自己从 IPSW 提取的 iOS arm64e 用户空间）→ launchd 拉起 SpringBoard。设备树、中断、电源轨、固件全部从手机现有的 Android 读取，所以换机型是新建 `devices/<name>/` 目录而不是重写内核。

## 用户空间从哪来（法律边界）

项目**不分发任何 Apple 文件**。系统镜像要你自己拿一台真 iPhone 的 IPSW，用 `sysimage/unpack.sh` 解包。项目只开源：XNU 补丁（APSL 2.0，条款要求修改开源——已满足）、自己的驱动、loader、工具、Userland 里自己写的程序和库。

没有 Secure Enclave → **没有激活、Apple ID、iCloud、App Store**。这也是它天生不可能"变成一台 iPhone"的原因。

## 图形怎么画出来的

不完整的 Metal → [metal2vulkan](https://github.com/steelbrain/metal2vulkan) 翻译成 Vulkan → Mesa Turnip → 自写内核 GPU 驱动 → Adreno。小米 15 分支说 Liquid Glass 的核心 shader 已逐像素对过 Apple 的 GPU，能出图但还有 bug。

## 现状（官方 README）

| 能力 | POCO X5 Pro 5G（骁龙 778G） | 小米 15（骁龙 8 Elite） |
|---|---|---|
| 锁屏/主屏/控制中心/自带 App | ✅ | ✅ |
| 触摸、按键 | ✅ | ✅ |
| 8 核全开 + 调频 | ✅ 大核 2.4 GHz | ✅ 到大核 3.52 GHz |
| GPU 渲染 | ✅ Adreno + Mesa OpenGL ES | ◐ 部分 Metal/Liquid Glass |
| Wi-Fi | ✅ | ◐ 驱动是临时替代品，多数运行会崩 |
| 蓝牙 | ❌ | ✅ |
| 音频/电池/振动/摄像头 | ✅（摄像头部分） | ◐ 扬声器通路通，系统音与麦克风在调试 |
| 蜂窝 | ◐ modem 起来了、SIM 绑了、看到网络，但注册没完成 | ❌ 未开始 |
| 充电 | ❌ | — |
| 跨重启保留 `/private/var` | ✅（`var_store=1 ufs_write=1`） | ✅ |
| Sileo / Procursus / SSH | ✅ | — |

默认行为：**loader 走 `fastboot boot` 进内存跑一个会话，重启就回到 Android**。只有显式开 `ufs_write=1` 才写你自己建的 `darwin` 分区。官方定位原话：**"这是研究用的试验台，不是 ROM"**。

## 自己搞需要什么 / 风险

- 一台**已解锁 bootloader** 的骁龙机，最好是已有人移植的机型（POCO X5 Pro 5G = `redwood`、小米 15 = `dada`）；换新机型要自己写全套驱动。
- 一台 Linux 电脑（QEMU 里可以先试：`make qemu-open` 连 Apple 的东西都不用）。
- 一台真 iPhone 的 IPSW（arm64e 用户空间来源）。
- 风险：**变砖 / 丢数据 / 保修失效**。README 原文警告 "can cost you your data or even brick the phone"。

## 和 QEMU 路线的关系

| | QEMU 模拟（darwin-vm 等） | HakiOS |
|---|---|---|
| 本质 | 模拟 ARM 硬件 | 真机裸金属跑 |
| iOS 用户空间 | 不跑，只有 root shell | 真跑 SpringBoard |
| 性能 | 极慢 | 接近真机 |
| 需要 Mac | 是（提取固件） | 否，纯 Linux 可做 |
| 适用 | 内核研究 | 唯一"开源实现 iOS 界面"的路线 |

---

# 附二：自己写一个 iOS 虚拟机，可行吗

## 一句话结论

**要分成两档看，混在一起谈就会得出错误结论：**

- **"能引导 XNU、进 root shell、能调内核"的 VM** → 完全可行，个人 1–2 个月量级，已有开源可抄（`jprx/darwin-vm`，MIT，2026-08 创建，1252 star）。
- **"能跑 SpringBoard、能装 App"的完整 iOS 虚拟机** → 个人不可行，团队数年，且撞一堵**硬件硬墙**。不是"难"，是"缺一样物理上无法虚拟的东西"。

## 五层障碍，逐层量化

| # | 障碍 | 难度 | 能不能过 |
|---|---|---|---|
| 1 | **引导 + 内核** | 高但已解 | ✅ 已有解法：`qemu-sptm` fork 实现了 SPTM/TXM 语义 + Apple SoC 机器模型（`xnuboot_sptm.c`）。照抄即可 |
| 2 | **虚机设备树让 launchd 满意** | 高 | ◐ 要造一堆假 IOKit 节点响应私有 daemon 的握手（AppleSEPManager、AppleMobileFileIntegrity…），缺一个就卡住 |
| 3 | **系统盘 / 数据盘解密 + 激活** | **硬墙** | ❌ 盘由 SEP 的 UID key 加密；激活要 SEP attestation。**没有 Secure Enclave 就没有钥匙**，无法虚拟化熔断密钥 |
| 4 | **Metal GPU** | 巨量工程 | ◐✗ Apple GPU 是专有 TBDR，指令集不公开，社区没有 Turnip 那样的逆向产物。Corellium 靠团队多年 |
| 5 | **法律** | — | ❌ EULA 禁止非 Apple 硬件运行；分发固件/镜像是侵权；Corellium 被 Apple 起诉过 |

**注意第 3 层**：它直接决定了"完整 iOS VM"的边界——即使你把图形做出来了，**App Store 应用、钥匙串、激活相关的一切永远起不来**。这不是工程量问题。

## 成本估算（个人/小团队自研）

| 目标 | 人月 | 可行性 |
|---|---|---|
| QEMU 里 XNU 到 root shell | 1–2 | ✅ 有开源可抄，`make` 就能起 |
| 加虚拟 framebuffer + 打通日志到 launchd | +3–6 | ◐ 有产出价值 |
| 跑到 SpringBoard + 触摸可用 | +6–12 | ◐ 卡在第 2/3 层 |
| Metal 渲染 + 常用 App 能开 | 10–50 + 逆向 | ✗ 个人 |
| Corellium 级完整虚拟设备 | 团队多年 | ✗ 且要养法务 |

参照系：`darwin-vm` 作者自述**一个人约 2 个月**，成果是"boots to a root shell"，README 明确写 **"这不是完整 iPhone/Mac 模拟器，别指望屏幕、WiFi、蓝牙、图形、GUI App、SpringBoard"**。Corellium 从同一批研究出发做到完整设备，花了团队数年 + 一场官司。连**不做虚拟机**的 HakiOS（裸金属移植）都已经难到要按机型拆仓库，可见跨越点在哪。

## 如果真要做，务实路线

```
① fork jprx/darwin-vm（或 qemu-sptm）→ 复现 root shell          1–2 周
② 最小可用增量：虚拟显示 + 把 launchd 推进到 SpringBoard 前一步   1–2 月
   ↑ 这一步才是有真实贡献/论文价值的位置
③ 到了 GUI 阶段，就别继续做虚拟机了 —— 换 HakiOS / UniXNU 的真机路线
```

**关键判断**：想看到 iOS 界面，虚拟机是**最差的实现路径**。真机裸金属移植（HakiOS）免掉了整套硬件虚拟化，性能还更高。虚拟机唯一占优的场景是"没有 iOS 设备就要调试 iOS 内核"。

## 推荐

- 想**学习/研究 XNU** → fork darwin-vm，从补全第 2 层入手，是合理的一人项目。
- 想**看到 iOS 桌面** → 别自己写，用 Corellium 或买台真机。
- 想**给团队/App 用** → 云端真机，自研没有任何性价比。
- 想**做产品卖** → 法律风险 > 技术风险，先找律师。

## 其它可参考的同类项目

- [UniXNU](https://github.com/UniXNU/UniXNU)：面向多机型的通用 UEFI 层（小米 Pad 6S Pro / SM8550）
- [ios-sweet](https://github.com/dipa-studio/ios-sweet)：Redmi Note 10 Pro 上的 Darwin 16 userland bring-up，小到一下午能读完
- [Bergamota](https://github.com/ry-diffusion/bergamota)：Poco F1、Darwin 18，自带 arm64e/PAC 适配工具
- 对照表：HakiOS 的 [docs/comparison.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/comparison.md)

---

# 附三：App 能在 iOS 虚拟机里跑吗

## 结论：能，但只有一类能——「你自己编译 + 自签名」的程序；GUI App 基本不行

| App 类型 | 能否运行 | 卡在哪 |
|---|---|---|
| 自己用 Xcode 编译的 arm64 CLI 程序 + ad-hoc 签名 | ✅ **可以**（darwin-vm 官方文档给了完整流程） | 仅需 trustcache |
| 越狱生态包（Procursus / Sileo 的 .deb） | ◐ 真机移植可行（HakiOS 已装 Sileo） | VM 里要先打通 dpkg + 网络 |
| 自己编译的 GUI App | ◐✗ | 没有完整图形栈，画不出界面 |
| App Store 下载的 .ipa | ❌ **硬墙** | FairPlay 加密，密钥由设备 DRM 身份派生 |
| Simulator 构建（`-sdk iphonesimulator` 的 .app） | ❌ | 依赖 macOS + Simulator runtime，不是你的 VM |
| Apple 自带 App（Safari、照片…） | ◐ | 需 SpringBoard + GPU；真机路线可以，VM 卡在这里 |

## 两条硬约束

### 1. FairPlay：App Store 的 ipa 是加密的
App Store 的二进制带 `LC_ENCRYPTION_INFO`（cryptid=1），解密密钥在安装时由 Apple 按**设备 DRM 身份**下发，只有对应硬件能解。
→ 你的 VM/安卓机**没有这个身份**，所以「装个 App Store 的 App 跑起来」在 VM 里**不可能**。
→ 绕过签名（AltStore/TrollStore 那类）只解决**签名校验**，**不解决 FairPlay 加密**——仍需要别人提供的解密版 ipa。

反直觉的一点：**在你自己的 iOS VM 里，自己编译的 App 反而比 App Store 的容易跑**，因为它不加密。

### 2. 图形：GUI 是 VM 的死穴
App 要画界面就过 Metal/OpenGL ES。VM 里 GPU 是模拟的——`darwin-vm` 的 README 直接写了「别指望屏幕、图形、GUI App、SpringBoard 能工作」。
HakiOS 能出主屏，是因为它在**真 Adreno 硬件**上写了真驱动，不是模拟。VM 里没有这个。

## darwin-vm 里跑自己程序的真实流程（官方文档）

```sh
# 1. 写程序，用 iPhoneOS SDK 编译（必须在 Mac + Xcode 上做）
xcrun -sdk iphoneos clang hello.c -o hello

# 2. ad-hoc 签名
codesign -s - hello

# 3. 拷进 ramdisk
hdiutil attach -owners on -mountpoint mnt firmware/ramdisk.dmg
sudo cp hello mnt/bin/hello
hdiutil detach mnt

# 4. 取 CDHash，加进信任列表
codesign -d -vvv hello 2>&1 | grep -i cdhash= | cut -d= -f2- >> firmware/all_hashes
./build_tc.py firmware/all_hashes firmware/ramdisk.tc

# 5. 重启 VM，直接运行
bash-3.2# hello
hello, xnu!
```

注意第 1 步：**必须有一台 Mac 和 Xcode**。这是 VM 路线里又一个绕不过的依赖（跟 darwin-vm 提取固件要 Mac 是同一个原因）。

## 如果目标就是「跑某个 App」

那整条 VM / 模拟器路线性价比极低。优先级：

1. 云端真机 / Appetize.io（当天能用）
2. 买台二手 iPhone（几百到一千，一次投入，全部能力）
3. Corellium（完整虚拟设备，企业级价格）
4. 自研 VM（只能跑自己写的 CLI 程序，GUI 无解）

---

# 附四：需求「iOS 26 虚拟机 + Linux + 看到桌面」的结论

## 结论：现在没有可行解，而且卡点不是激活

把需求拆开只有一句：**让 iOS 26 的 SpringBoard 在一个没有 Apple 硬件的环境里画出界面。** 这件事在 2026 年 11 月这个时点，开源界没有任何项目做到，商业上只有 Corellium 做到。

现有项目的天花板（都读过 README）：

| 项目 | iOS 26/27 能做到 | 桌面？ |
|---|---|---|
| `jprx/darwin-vm`（QEMU） | 引导 XNU 到 **root shell**，可跑自编 CLI 程序 | ❌ 作者明说「没有屏幕、图形、GUI App、SpringBoard」 |
| `hakios-xnu-kernel`（真机 POCO X5 Pro） | Darwin 16.5 用户空间，主屏/触摸/WiFi/相机 | ✅ 但**需要真机**，不是 VM |
| `hakios-arm64e-dada`（真机 小米 15） | iOS 26 arm64e，主屏 + 触摸 + 部分 Metal | ✅ 但**需要真机 + 解锁 BL** |
| Corellium | 完整虚拟 iOS 设备 | ✅ 商业产品 |

**关键：能出桌面的两个项目都是真机裸金属，不是虚拟机。** 这不是巧合——桌面需要的正是「真 GPU」。

## 为什么 iOS 26 在 VM 里特别不可能

1. **Liquid Glass 是 GPU 世代**。iOS 26 的整套 UI 靠实时 Metal shader 做折射/模糊，几乎没有「不渲染的界面」。模拟 GPU 跑不动，而且它不是慢，是**画不出来**。
2. **Apple GPU 指令集不公开**，社区没有 Turnip 级替代品（Adreno 有是因为高通开源 + 长期逆向）。Corellium 自研整套 GPU 虚拟化，是它最核心的资产。
3. **darwin-vm 连 A19 的 SPTM/TXM 都要 fork QEMU 才过**，而它的成果仍是 root shell。桌面比这远得多。
4. 小米 15 那条分支能出界面，靠的是 `metal2vulkan → Turnip → 自写内核 GPU 驱动 → 真 Adreno 830`。**整条链路里没有「模拟」二字**，任何一环换成模拟就断了。

## 关于「绕过激活」

- 咱不提供绕过激活的方法（规避技术措施，违法；HakiOS 项目自己也在 LEGAL.md 里拒绝这类改动）。
- 更关键的是：**它解决不了你要的东西**。绕过激活只解决「进不进得去」，而你缺的是「进去之后有没有东西能画」。就算这一步清了，桌面还是不会出现。
- 另外，走真机移植路线时激活**根本不在路径上**——从 IPSW 平铺文件系统直接启动，系统认为自己是「已装好的系统」，从未进恢复模式。这不需要「绕过」，这叫「不经过」，合法且干净。

## 能真正达成「看到 iOS 26 桌面」的三条路

| 路径 | 代价 | 备注 |
|---|---|---|
| **A. 小米 15 + hakios-arm64e-dada** | 一台小米 15（解锁 BL，会丢保修/可能变砖）+ 自己 iPhone 的 IPSW + 一台 Linux 编译 | 最接近你的需求，且完全开源。但**不是 VM**，是在手机上跑 |
| **B. Corellium** | 企业级年费（需询价），合规需自审 | 唯一「Linux 里通过浏览器看到 iOS 26 桌面」的正规途径 |
| **C. 放弃桌面，做内核级 VM** | 1–2 人月 + 一台 Mac 提取固件 | `darwin-vm`，到 root shell，能跑自编 CLI 程序 |

**没有第四条。** 「Linux 里跑 iOS 26 虚拟机看到桌面」= Corellium 的产品定义，自研等于重做一家公司。

## 建议

如果目标是**看/演示 iOS 26 界面** → A（真机最便宜）或 B（最省事）。
如果目标是**研究 XNU** → C。
如果目标是**给人提供 iOS 26 云端设备** → 商业上只有 B，自研不成立。

---

> 决定自研后的正式文档：`IOSVM-工程文档.md`（架构与技术决策）、`IOSVM-阶段文档.md`（里程碑与验收）。

