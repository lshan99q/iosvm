# IOSVM 装第三方软件 —— 工程交接文档

> 交接时间：2026-11-02
> 目标：让第三方 `.app`（最终目标是微信）**出现在主屏并能启动**
> 当前状态：**未达成**。一次图标都没出现过，但问题已经从「不知道卡在哪」收窄到「两个具体机制」

---

## 一、一句话现状

**装不进去，是因为「注册」这一环没人能做。** 内核不验签（能跑），但把应用登记进
LaunchServices 数据库需要特定进程权限，而那个进程（`installd`）当前**不会被系统拉起**。

---

## 二、iOS 应用是怎么"出现"在主屏上的

理解这条链，才能知道断在哪：

```
.app 目录在磁盘上
   │
   ▼ ① 有人把它登记进 LaunchServices 数据库
lsd（/private/var/db/lsd，每次开机是空的）
   │
   ▼ ② SpringBoard 读数据库 + 自己的布局 plist
主屏图标
```

**两个环节都要通**：
- **① 登记**：由 `installd` 这类被系统授权的进程完成（它带 `com.apple.private.coreservices.can-register-install-results`
  和 `can-perform-rebuild-registration`）
- **② 布局**：SpringBoard 用 `/System/Library/CoreServices/SpringBoard.app/DefaultIconState~iphone.plist`
  决定图标摆在哪；运行时会写 `/var/mobile/Library/SpringBoard/IconState.plist`

**现在的断点在第 ① 步。**

---

## 三、实测证据（全部来自真实启动的日志/截图）

| # | 尝试 | 结果 | 含义 |
|---|---|---|---|
| 1 | 把 `.app` 放进镜像的 `/Applications`，重建 2.4 GB 系统镜像并启动 | **无图标**，`lsd` 全程不提它 | 单纯拷文件不会触发登记 |
| 2 | `LSRegisterURL` | `dlsym → 0x0` | **iOS 16.5 没导出这个符号** |
| 3 | 直接 `objc_getClass("LSApplicationWorkspace")` | 返回 **NULL** | 必须先 `dlopen(.../MobileCoreServices.framework/MobileCoreServices)` |
| 4 | dlopen 后调用 `defaultWorkspace` / `applicationProxyForBundleURL:` | workspace 建成、proxy 非空，但 `allApplications` = **0** | 无权限的调用者，数据库对它「是空的」 |
| 5 | 调 `registerApplication:` | **永不返回**（等 5 分钟无结果） | 不是失败，是**没被回答** |
| 6 | 注入 SpringBoard 里调（它有权限） | `allApplications` = **173** ✅，但 `registerApplication:` 仍不返回 | 数据库不空；但登记这条路照样堵 |
| 7 | 同上，把调用放 SpringBoard **主队列** | **界面冻死**（画面全黑） | 该调用是同步阻塞的，绝不能放主线程 |
| 8 | 以 uid 501（launchd 拉起，mobile 域）调 | 连上 lsd 后**永久阻塞** | 用户域上下文能连但得不到回答 |
| 9 | 以 uid 0（init 子进程，无 bootstrap 端口）调 | **瞬间返回**，0 个应用 | 没有 bootstrap 端口就完全够不着 lsd |
| 10 | `INSTALLD=1` 加入 job 表 | **`installd` 根本没启动**（进程表无、launchd 日志无 spawn） | 它是 **on-demand** 任务，没人请求就不起 |
| 11 | `MobileInstallationInstall` | `dlsym → 0x0` | 16.5 未导出，MobileInstallation 老路线不通 |
| 12 | 写 `/var/mobile/Library/SpringBoard/IconState.plist` 加第三页 | 写入成功（`icons: wrote ... 3 pages`），但截图仍只有两页 | **SpringBoard 覆盖了它**，或读得比写入更早 |
| 13 | `lsd` 状态 | `/private/var/db/lsd` 在 t=150 仍是 **0 条目**；`lsd` 整轮刷 702+ 条 `denied attempt to escape CPU monitor` | lsd 在狂转，库没建起来 |
| 14 | `csops(CS_OPS_ENTITLEMENTS_BLOB)` | 已 ldid 签名的程序返回 **0 字节** | 移植的 `mini_OSEntitlements_get_xml` 返回 false，**从客户机里无法验证 entitlements** |

---

## 四、已否定的路线（别再试）

### 4.1 只把 .app 拷进镜像 —— 无效
见证据 1。目录在 `/Applications` 也没用，登记不会自动发生。

### 4.2 TrollStore / CoreTrust 绕过 —— **无对象**
移植的内核从不判决签名（AMFI 替身接受任何可解析签名，adhoc 直接给
`CS_PLATFORM_BINARY`）。TrollStore 解决的是「Apple 不批准的签名」，这台机器**从不做这个判决**。

### 4.3 `MobileInstallationInstall` —— 16.5 未导出
见证据 11。

### 4.4 手写 resolv.conf 式的"绕过" —— 对装 App 同样不存在
不存在「不经过 lsd 就能出图标」的路径：SpringBoard 的图标来自 LS 数据库。

---

## 五、仍然可行的路线（按优先级）

### 路线 A：强制拉起 `installd`（**最优先**）

`installd` 在镜像里（`/usr/libexec/installd`，plist 在
`sysimage/rootfs/System/Library/LaunchDaemons/com.apple.mobile.installd.plist`），
但没有进入 job 表；`build.sh` 已有开关：

```bash
INSTALLD=1 ... ./build.sh root.apfs
```

**但它仍然没启动**，因为它是 on-demand 的（`MachService` 触发）。所以要么：

- **A1**：给它一个 `RunAtLoad` 的变体 plist（把 `LaunchOnlyOnce`/`KeepAlive` 语义改成开机自启）
- **A2**：用 `launchctl kickstart system/com.apple.mobile.installd` 主动触发
  （需要能从客户机执行 launchctl；`/bin/launchctl` 是否在镜像里要先确认）

**预期**：`installd` 启动后会重建注册库（`can-perform-rebuild-registration`），
`/Applications` 里的 `.app` 有机会被登记。

**验证方法**：启动后检查 `/private/var/db/lsd` 条目数是否非 0，再截图看图标。

### 路线 B：让 lsd 自己重建（次优先）

`LSApplicationWorkspace` 有 `_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:`
方法。从**有权限的进程**（SpringBoard 注入库，它以 `canmaplsdatabase` 身份运行）调用它，
理论上能让 lsd 重新扫描 `/Applications`。

**注意**：必须放在**后台队列**（证据 7：主队列会冻死界面），并且给它几分钟。

**已知风险**：证据 6 显示即使是 SpringBoard，`registerApplication:` 也不返回 —— 但
`_LSPrivateRebuild...` 是另一个方法，值得单独一试。

### 路线 C：越狱前缀 + uicache（工程量最大，但最"正统"）

仓库里有 `tools/mk-jb.sh`（239 行），会拉 Procursus rootless bootstrap + Sileo + ElleKit，
生成 `/var/jb` 前缀。`build.sh` 在 `FLASHBOOT` 路线里会把前缀的 `.app` 复制进 `/Applications`，
并让 `uicache -p` 注册。

**但是**：**当前分支没有跑过 `mk-jb.sh`**，`jb/` 目录不存在，镜像里没有 `/var/jb`。
`docs/ru/notes.md` 说前缀活在 `camera` 分支。

**这条路的价值**：`uicache` 是专门做「让 app 出现在主屏」的工具，它知道正确的调用方式。
如果能跑通前缀，就绕过了我们自己摸索 LS API 的困境。

**风险**：需要外网（拉 bootstrap）—— 外网现在已经通了（见第六节）。

### 路线 D：检查「系统那 173 个应用是怎么登记的」

SpringBoard 里 `allApplications` = 173，说明**系统应用是有登记的**。找到这个登记动作发生在
开机哪一步（lsd 首次扫描？某个 plist 清单？），把我们的 app 按同样方式加进去。

**建议**：开机日志里搜 `lsd` 的所有输出，看它启动时读了什么。

---

## 六、环境与工具（当前可用）

### 6.1 外网已通
DNS/解析已验证：`resolve example.com, 0.05 s: 172.66.147.243`，TCP 到宿主 200。
所以「下载 IPA / 拉 bootstrap」在网络层面不再是障碍。

### 6.2 签名工具
```bash
/tmp/ldid/ldid -S<entitlements.plist> <binary>
```
可以从 dyld shared cache 里提取的 entitlements 包括：
- `com.apple.private.coreservices.can-register-install-results`
- `can-perform-rebuild-registration`
- `canmaplsdatabase`
- `can-send-install-notifications`
- `canmapbundleidtouuid`
- `com.apple.private.mobileinstall.allowedSPI`

### 6.3 已有工具
| 文件 | 作用 |
|---|---|
| `userland/src/reg_app.c` | 调 LS 注册的工具（含 `.entitlements`、预编译副本 `reg_app.prebuilt`） |
| `userland/src/lsprobe`（临时） | 反射列出 LS 类与方法 |
| `/Applications/TestApp.app` | 测试用 bundle（DemoApp 副本，`com.example.testapp`，已删 `SBAppTags`） |

### 6.4 构建与启动
```bash
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=<repo>/xnu-deps/host/bin:/tmp/cctools/bin:$PATH

# 构建（小镜像，1-2 分钟）
cd userland && INSTALLD=1 RC=rc.launchd EXTRA="/tmp/guestbin/fill_panel" ./build.sh root.apfs

# 启动（6-9 分钟，约 40% 概率卡死，重试）
cd .. && setsid nohup env TRIES=4 MAX_WAIT=3600 NO_FRAMES_BY=300 KEEP=1 \
  CMDLINE_EXTRA="fbscale=2" bash qemu/wait-for-ui.sh /tmp/app.png >/tmp/appboot.log 2>&1 &
```

### 6.5 杀进程（**重要**）
```bash
# 只按进程名，绝对不要 pkill -f 带脚本名（会杀掉自己的 shell）
ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'
```

### 6.6 看结果
```bash
log=$(ls -t /tmp/tmp.* | head -1)
grep -a "lsd\|installd\|register" $log
bash qemu/screendump.sh /tmp/shot.png     # 截图看图标
```

---

## 七、验收标准

**最小可行**（一次启动内可判定）：

1. `/private/var/db/lsd` 的条目数 **非 0**
2. 主屏出现 `TestApp` 图标（截图确认）
3. 点击图标能启动（`ps -A` 出现对应进程）

**最终目标**：同样的方法装上微信（需要先拿到 arm64 的 IPA，且登录仍受"无 Apple ID/无手机号"限制）。

---

## 八、诚实的状态与风险

### 已确认的
- 单纯拷文件无效（证据 1）
- `registerApplication:` 在无权限/用户域上下文下**不返回**（证据 4/5/8）
- `installd` 是 on-demand，加进 job 表也不会自启（证据 10）
- SpringBoard 会覆盖手写的 `IconState.plist`（证据 12）
- 内核不验签，但用户态按签名里的 entitlement 判权（证据 14 + `miniamfi_entitlements.cpp`）

### 仍是推断的
- `installd` 启动后是否真会重建库并登记 `/Applications`（**从未观测到**）
- `_LSPrivateRebuild...` 是否比 `registerApplication:` 更容易成功（未试）
- 系统那 173 个应用的确切登记时机（未查清）

### 已知的坑
- **别把阻塞调用放 SpringBoard 主队列** —— 会冻死界面（证据 7）
- **别用 fork** 去调 LS（fork 会破坏 CF/XPC 状态，子进程 1 秒内被信号打死）
- `INSTALLD=1` 只是把 plist 放进镜像，**不等于会启动**
- 改 `system.apfs` 需要 2.4 GB 重建（约 4 分钟），且会重写 `ram.img`

---

## 九、相关文件索引

| 路径 | 内容 |
|---|---|
| `userland/src/reg_app.c` + `.entitlements` | LS 注册测试工具 |
| `userland/src/metal_test.m` | 示例：如何写一个自诊断工具（可借用其结构） |
| `userland/rc.jobs.jb` / `rc.jobs.sileo` / `rc.jobs.tweakinstall` | 越狱前缀路线的预期用法 |
| `tools/mk-jb.sh` | 构建 rootless 前缀（未跑过） |
| `sysimage/var.skel` | 开机创建的 `/private/var` 骨架（含 `db/lsd`） |
| `sysimage/rootfs/System/Library/LaunchDaemons/com.apple.mobile.installd.plist` | installd 的 job 定义 |
| `docs/research/linux-kernel-build.md` 附录 34/34a/34b | 装 App 的完整调查记录 |
| `/home/lshan/dsh.MD/IOSVM.md` | 项目总体结论 |

---

## 十、给接手人的第一件事

**先做路线 A（强制拉起 installd）**，因为它最便宜且从未被真正验证过：

```bash
# 1. 确认 launchctl 是否在镜像里
ls sysimage/rootfs/bin/launchctl sysimage/rootfs/usr/bin/launchctl 2>/dev/null

# 2. 若在：开机后 kickstart 它（可写进 rc 或一个 launchd 作业）
launchctl kickstart -k system/com.apple.mobile.installd

# 3. 等 60 秒，检查库是否建起来
ls /private/var/db/lsd | wc -l

# 4. 截图看主屏
bash qemu/screendump.sh /tmp/after-kickstart.png
```

如果 `launchctl` 不在镜像里，改用「复制一份 plist 并加 `RunAtLoad`」的办法 —— 这仍然是
**一次构建 + 一次启动**就能判定的事。

**这一步的成败，决定后面走 B/C/D 哪条路。**
