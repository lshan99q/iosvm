# 设计基线 · Git 规范

> 状态：仍然有效，已并入 CONTRIBUTING.md 与 ADR-006。见 docs/zh/README.md。

# IOSVM Git 规范

| 项 | 值 |
|---|---|
| 版本 | v0.1 |
| 适用 | IOSVM 仓库及其子模块（`qemu/`、设备仓库） |
| 配套 | `IOSVM-工程文档.md`、`IOSVM-工作规范.md` |

> **第一原则：仓库里永远不出现秘密。** token、私钥、证书、IPSW、固件，一律不入库。CI 有强制检查，人工 review 只作兜底。

---

## 1. 分支模型

采用 **trunk-based**（主干开发），不用 git-flow。

| 分支 | 用途 | 生命周期 |
|---|---|---|
| `main` | 唯一长期分支，始终可构建可测 | 永久 |
| `feat/*` `fix/*` `docs/*` … | 短期特性分支 | ≤ 2 周，超期拆分 |
| `spike/*` | 时间盒研究（见工作规范 §3.2） | ≤ 5 天，结束即删或合并为文档 |
| `release/*` | 仅当需要稳定补丁时临时开 | 用完删 |

**不设 `develop`。** 个人/小团队项目，多一条长期分支只会增加 merge 成本（工程文档 R7）。

### 分支命名

```
<type>/<短横线描述>[-<issue号>]
```

```sh
feat/metal-blit-encoder-42
fix/qemu-build-x86-host
docs/d1-display-path
hw/virtio-iosvm-gpu-ring
proto/iosvm-gpu-v0.1
```

| type | 含义 |
|---|---|
| `feat` | 新功能 |
| `fix` | bug 修复 |
| `docs` | 只改文档（含 ADR） |
| `hw` | QEMU 虚拟设备 |
| `proto` | 传输协议定义 |
| `host` | `host/vkbridge` |
| `guest` | `guest/` 注入组件 |
| `refactor` | 不改行为 |
| `test` | 只加/改测试 |
| `ci` | CI 配置 |
| `chore` | 依赖、格式化、杂务 |

---

## 2. 提交规范

采用 **Conventional Commits**，`type` 与分支 type 对齐，另加项目 scope。

```
<type>(<scope>): <subject>

<body>

<footer>
```

### 规则

| 项 | 规定 |
|---|---|
| subject | **英文**，祈使句，≤ 72 字符，结尾不加句号 |
| body | 英文优先，中文可接受（项目有多语言 README）；解释 **why**，不解释 what |
| 粒度 | 一个 commit 一件事，**每个 commit 独立可编译** |
| 禁止 | `update`、`fix bug`、`wip`、`--amend` 到别人的分支、force push `main` |
| 署名 | 允许 `Signed-off-by`（DCO），不要求 CLA |

### scope 表

| scope | 范围 |
|---|---|
| `qemu` | QEMU fork 本身（尽量少） |
| `hw` | `hw/` 本项目虚拟设备 |
| `proto` | `proto/` 协议 |
| `vk` | `host/vkbridge` |
| `plugin` | `guest/metal-plugin` |
| `hooks` | `guest/hooks` |
| `tools` | `tools/` 脚本 |
| `ci` | `ci/` |
| `docs` | 文档与 ADR |
| `deps` | submodule、依赖版本 |

### 示例

```
feat(proto): add IOSVM_OP_PRESENT and swapchain negotiation

CoreAnimation needs to hand a finished surface to the host without
reading it back. PRESENT carries an image handle plus a damage rect;
vkbridge blits it to the swapchain and signals the fence.

Refs: #57
```

```
fix(hw): drop ring commands when doorbell fires during reset

The iothread could observe cmd_head behind cmd_tail after a guest
reset, which made vkbridge replay stale packets. Guard on a reset
generation counter.

Fixes: #63
```

```
docs(adr): record D1 display path decision as C2

Spike A (userspace present hook) reached WindowServer; Spike B
(AppleDisplay MMIO) never bound. Decision and evidence attached.
```

### 禁止的提交内容

- 任何二进制（除明确允许的小图标、`.gitkeep`）
- `firmware/`、`userland/` 下任何文件
- 任何 token / key / 证书 / `.env`
- 注释掉的死代码块 > 20 行
- 未经 `clang-format` 的 C/C++/ObjC 文件

---

## 3. Pull Request 规范

### 必须

- 标题同 commit 规范（squash 后即 commit message）
- 描述填模板：**改了什么 / 为什么 / 怎么验证的**
- 勾选合规清单（见 §7）
- 关联 issue（`Closes #N` / `Refs: #N`）
- CI 全绿
- 自审一遍 diff（个人项目没有人替你看）

### 合并策略

| 场景 | 策略 |
|---|---|
| `feat`/`fix`/`hw`/`proto`/`vk`/`plugin` | **Squash merge**，保留单条清晰提交 |
| `docs` 小改动 | Squash |
| 大型重构（一次改多个文件） | 允许 **Rebase merge** 保留分步提交，需在 PR 说明理由 |
| 禁止 | Merge commit（保持线性历史） |

合并后**立即删除远端分支**。

### Review 要求

- 作者本人：合并前自审 diff，至少在 PR 里写一行「我检查过什么」
- 外部贡献：至少 1 名维护者 approve
- 涉及 `LICENSE*`、`PROVENANCE.md`、`ci/`：必须维护者 approve（合规敏感）

---

## 4. 版本与标签

### 版本号

SemVer，0.x 阶段：

```
v0.1.0-alpha.1   M2 完成（probe 全绿）
v0.1.0           M3 完成（第一帧）
v1.0.0           M4 完成（主屏可用）
```

预发布标识固定用 `-alpha.N` / `-beta.N` / `-rc.N`。

### Tag 要求

```sh
git tag -a v0.1.0-alpha.1 -m "iosvm-probe green on lavapipe and radv"
```

tag message 必须包含：

1. 对应里程碑（M0/M1/M2/M3/M4）
2. 已验证的宿主环境（发行版、Vulkan 驱动）
3. **使用者自备固件的版本范围**（例如 `iOS 26.x, iPhone17,3 t8140`）
4. **`firmware/` 内容的 SHA256 清单**（证明可复现，但不含文件本身）

```sh
# 生成清单（写进 release notes，不入库）
sha256sum firmware/* userland/*.apfs > release-v0.1.0-alpha.1.sha256
```

### Release notes 模板

```markdown
## v0.1.0-alpha.1 — M2: GPU channel works

### 可用
- iosvm-probe 全绿：buffer 创建/写/读、blit copy/fill、fence 顺序
- lavapipe 与 radv 双后端验证

### 已知不可用
- 无桌面渲染（M3）
- 无输入（M4）

### 复现条件
- 宿主：Linux x86_64，Vulkan 1.1+，QEMU 自编
- 需自备：iOS 26.x IPSW（提取用）；`firmware/` SHA256 见下

### 不含
不含任何 Apple 二进制。
```

---

## 5. 凭证与密钥管理（强制）

### 5.1 禁止事项

| ❌ 绝对不做 | 原因 |
|---|---|
| 把 token 写进任何文件 | 一旦 commit，历史里永久存在 |
| `git remote add origin https://user:TOKEN@github.com/...` | token 会进 `.git/config` |
| 在 issue / PR / 聊天里贴 token | 同上，且会被爬虫抓 |
| 用 classic PAT 且全 scope | 权限过大，泄露即全账号沦陷 |
| 把 token 写进 `Makefile` / `ci/*.yml` 字面量 | CI 日志会打印 |

### 5.2 正确做法

**方案 A（推荐）：`gh` CLI 走凭据助手**

```sh
# 一次性登录，token 由 gh 存入系统 keyring / 凭据管理器
gh auth login --hostname github.com --git-protocol https --web

# 检查
gh auth status
```

**方案 B：环境变量 + 凭据助手**

```sh
# 只在当前 shell 会话有效，不落盘
export GITHUB_TOKEN='...'       # 从密码管理器取值，粘贴到此为止

# 交给 git 的凭据助手（store 到 keyring 而非明文）
git config --global credential.helper libsecret    # GNOME
# 或 osxkeychain / wincred
```

**方案 C：SSH key（推代码最干净）**

```sh
ssh-keygen -t ed25519 -C "iosvm@$(hostname)" -f ~/.ssh/id_ed25519_iosvm
# 公钥加到 GitHub，私钥权限 600
git remote set-url origin git@github.com:<owner>/iosvm.git
```

**推荐组合**：SSH key 推代码 + `gh` 管 issue/PR。**完全不用 PAT。**

### 5.3 Token 的最小权限（若必须用 PAT）

| 项 | 规定 |
|---|---|
| 类型 | **Fine-grained PAT**（不用 classic） |
| 仓库范围 | **仅 `iosvm` 一个仓库** |
| 权限 | `Contents: Read and write`、`Pull requests: Read and write`、`Issues: Read and write`；其余全 `No access` |
| 有效期 | ≤ 90 天 |
| 命名 | `iosvm-ci-YYYYMM`，便于审计 |

### 5.4 泄露后的处置流程（SOP）

发现 token 出现在任何地方，**按顺序**执行：

```
1. 立即到 GitHub → Settings → Developer settings → Tokens 撤销（Revoke）
   —— 不要先做别的事，撤销优先级最高
2. 检查受影响仓库：Settings → Sessions / Audit log
3. 若已进入 git 历史：
     a. 撤销 token（已做）
     b. 用 git filter-repo 重写历史，强推所有分支
     c. 通知所有 fork 持有者（历史重写会失效他们的引用）
     d. 假设 token 已完全泄露，不做侥幸判断
4. 轮换该账号其它凭证（既然一个泄露了，可能不止一个）
5. 在仓库开启 Secret scanning + Push protection（见 §6）
```

**如果 token 已经进入了公开仓库的历史，就当作永久泄露处理**——GitHub 的公开仓库会被大量爬虫扫描，撤销前的时间窗足够被利用。

### 5.5 提交前自动检查

`.pre-commit-config.yaml`：

```yaml
repos:
  - repo: https://github.com/gitleaks/gitleaks
    rev: v8.18.4
    hooks:
      - id: gitleaks            # 拒绝 token/key/证书
  - repo: local
    hooks:
      - id: provenance
        name: provenance check
        entry: python3 tools/provenance-check.py
        language: system
        pass_filenames: false
      - id: clang-format
        name: clang-format
        entry: clang-format --dry-run --Werror
        language: system
        types_or: [c, c++, objective-c]
```

```sh
pip install pre-commit && pre-commit install
```

---

## 6. 仓库设置（必须开启）

| 设置 | 值 |
|---|---|
| Default branch | `main` |
| Branch protection on `main` | ✅ Require PR before merging；✅ Require status checks（`linux-ci`）；✅ 禁止 force push；✅ 禁止删除 |
| Merge 方式 | 只开 **Squash** 与 **Rebase**；关闭 Merge commit |
| 合并后 | ✅ 自动删除 head 分支 |
| **Secret scanning** | ✅ 开 |
| **Push protection** | ✅ 开（在 push 阶段就拦 token，而不只是扫描） |
| Dependabot | ✅ 开（submodule 与 actions 版本） |
| Actions 权限 | 默认 `GITHUB_TOKEN` 只读，按 job 提权 |
| Issue/PR 模板 | 见工作规范 §3 |

---

## 7. `.gitignore` 草案

```gitignore
# 使用者自备的 Apple 内容 —— 永不入库
firmware/
userland/
*.ipsw
*.dmg
bootkc
dtree
*.apfs
*.tc

# 设备固件
fw-*/

# 构建产物
build/
out/
*.o
*.a
*.dylib
*.so
qemu/*
!qemu/.git

# 日志与截图（demo 截图例外，手动挑）
logs/
*.log
!docs/demo/*.png

# 本机配置
.env
.env.*
*.local.conf
.vscode/
.idea/
.DS_Store

# 临时
*.swp
*~
```

---

## 8. 子模块规范

```sh
git submodule add https://github.com/jprx/qemu-sptm.git qemu
git submodule update --init --recursive
```

| 规则 |
|---|
| 子模块升级必须**单独一个 PR**，标题 `chore(deps): bump qemu-sptm to <sha>` |
| PR 描述里必须写：升级原因、上游变更摘要、本仓库是否需要适配 |
| 不允许在子模块目录里直接改代码后提交（改动必须在 fork 里做，再升级指针） |
| 升级后必须跑通 `make test` + `make run` 到 root shell，才能合并 |

---

## 9. 常见操作速查

```sh
# 同步主干
git switch main && git pull --rebase

# 开分支
git switch -c feat/metal-blit-encoder-42

# 提交（自动 clang-format）
git add -p && git commit -m "feat(plugin): add MTLBlitCommandEncoder fill"

# 推之前自查
pre-commit run --all-files
git log --oneline main..HEAD          # 确认没有多余提交
git diff main...HEAD --stat           # 确认改动范围

# 推
git push -u origin HEAD

# 事后清理
git branch -d feat/xxx
```

**撤销类操作**（只对未推送的提交）

```sh
git reset --soft HEAD~1     # 撤销提交保留改动
git commit --amend          # 改最后一条（仅限未推送）
```

⚠️ 已推送的提交**不做** rebase/amend/force push（`main` 已受保护，其他分支也避免），用 `git revert`。

---

## 10. 提交自检清单

提交前逐条过：

- [ ] commit message 符合规范，subject 英文 ≤ 72 字符
- [ ] 一个 commit 只做一件事
- [ ] `make test` 通过
- [ ] `git add` 的列表里没有 `firmware/`、`userland/`、token、`.env`
- [ ] 新增文件有 SPDX 头（见工作规范 §5）
- [ ] 借用第三方代码已登记到 `NOTICE`
- [ ] 涉及合规敏感路径（`LICENSE*`、`PROVENANCE.md`、`ci/`）已在 PR 里明说
