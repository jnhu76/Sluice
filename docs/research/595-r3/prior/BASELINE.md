# R3 产品基线冻结记录

记录时间：2026-11-05
角色：E5 基线冻结员
结论：**基线已冻结**。`baseSha=0a22a79195f1b913145fcd443496e7f70b08cdcf`，与设计预期一致；相对 R2 锚点无产品代码变化。

## 1. 基线 SHA

| 项 | 值 |
|---|---|
| 期望基线 | `0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| 实际 `git rev-parse HEAD` | `0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| 是否一致 | **是**（未用期望值覆盖事实，事实本身即等于期望） |
| 主工作树分支 | `research/595-causal-r2` |
| 主工作树 `status --porcelain` | 0 行（冻结前 clean） |

```bash
git -C /home/jnhu/Source/Sluice rev-parse HEAD
git -C /home/jnhu/Source/Sluice status --porcelain
```

证据：`../raw/git-baseline.txt`。

## 2. 隔离 worktree

```bash
git -C /home/jnhu/Source/Sluice worktree add \
  /home/jnhu/sluice-595-e5/r3/worktree -b research-595-r3 \
  0a22a79195f1b913145fcd443496e7f70b08cdcf
```

创建前核查：`/home/jnhu/sluice-595-e5/r3/worktree` **不存在**（`r3/` 下仅有空的 `evidence/`），故无删除动作；`refs/heads/research-595-r3` 不存在。未触碰 `archive-wt`、`causal-r2/worktree`、`causal-r2/worktree-timing`、`worktree` 这四个既有 worktree。

创建后验证：

| 检查 | 结果 |
|---|---|
| `rev-parse --show-toplevel` | `/home/jnhu/sluice-595-e5/r3/worktree` |
| `rev-parse --abbrev-ref HEAD` | `research-595-r3` |
| `rev-parse HEAD` | `0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| `status --porcelain` | 0 行 |

## 3. 相对 R2 锚点（`271b81bf`）的差分

```bash
git -C /home/jnhu/Source/Sluice diff --stat \
  271b81bfc9d64eecc0266fe7066b9040525dd710..HEAD
git -C /home/jnhu/Source/Sluice diff --name-status \
  271b81bfc9d64eecc0266fe7066b9040525dd710..HEAD
```

**结论：产品代码零变化；差分全部是研究证据文档。**

- 区间内提交只有 1 个：`0a22a791 research(595): archive causal-r2 adaptive cross-layer attribution evidence (#595)`
- 改动文件 2577 个，`+65557` 行，**全部**位于 `docs/research/595-causal-r2/**`
- 顶层路径分组：`2577 docs`；`docs/` 之外改动文件数 = **0**

更强的核验（子树对象哈希逐一比对，比路径过滤更可靠）：

| 子树 | `271b81bf` | `0a22a791` | 判定 |
|---|---|---|---|
| `include` | `21607460fa1d7cb6bd8046a6c5ed4c9ed8f55398` | 同左 | IDENTICAL |
| `src` | `289c7c088fcbbdf885ae2b36066fa9dd4e7b4fd6` | 同左 | IDENTICAL |
| `apps` | `0a8e7738e2169c6c29c23fa85b94c96021a957b4` | 同左 | IDENTICAL |
| `xmake` | `f9ab3e31ac4a4fd3375a128765738164d75f17fa` | 同左 | IDENTICAL |
| `tests` | `5649b77ec9d3b620d8328e749cfb89dc6f682f0c` | 同左 | IDENTICAL |
| `xmake.lua` | `e9df0c17e7c12d15c2406d4122998d3618c8349e` | 同左 | IDENTICAL |
| `CMakeLists.txt` | 不存在 | 不存在 | — |

根树差异仅 `docs` 一项（`cac309b0…` → `9d35a1fe…`）。

**含义**：R3 基线与 R2 锚点在构建输入上逐字节相同。因此 R2 在 `271b81bf` 上测得的 A_/B_ 形态，其源码条件在 R3 完全保留；但仍须以本轮新构建的二进制 SHA 为准（见 §5），不得复用 R2 计时数值。

## 4. 构建

### 4.1 实际执行的命令

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake f -c -y --liburing=y -m release
xmake build bench_w02_pipeline bench_w03_external_loop sluice-copy uring_submit_boundary_test
```

配置关键行：

```
checking for platform ... linux (x86_64)
checking for liburing capability ... ok (/usr/include/liburing.h + .../lib64/liburing.so)
checking for sluice_liburing_capability ... ok
warning: Ascend SDK not found
```

构建结束：`build ok`，`exit=0`。完整日志：`../logs/build-baseline.log`。

### 4.2 与 ask 原文命令的偏差（必须记录，不得掩盖）

ask 给出的形式是 `xmake -P <worktree> f -c -y --liburing=y -m release`。**该形式在本机 xmake v3.1.2 下不可用**，实际经历三步：

1. `xmake -P <dir> f …` → 报错 `'f' is not a valid target name for this project`。xmake 把 `f` 当作 target。
2. `xmake f -P <dir> …` → 配置成功，但随后查阅 `xmake show -t` 发现 `targetfile: /tmp/build/linux/x86_64/release/bench_w02_pipeline`：**xmake 的 builddir 相对进程 CWD 解析，而非相对 `-P` 指定的项目目录**。首次构建产物因此落在 `/tmp/build`（已删除），既不在 worktree 也不在主树。
3. `xmake f … -o <dir>` 试图固定 builddir：`xmake f` 接受 `-o`，但 `xmake build -o …` 报 `Invalid option`，且 `-o` 未改变 `targetfile` 解析。最终采用 `cd <worktree> && xmake f … && xmake build …`，`targetfile` 变为 `build/linux/x86_64/release/<target>`（worktree 内），产物落在隔离 worktree。

副作用核查：主工作树 `build/` 内无任何 24 小时内修改的文件；`build/` 亦被 `.gitignore:14` 忽略，主树 `status` 始终为 0 行。首次误落的 `/tmp/build` 已 `rm -rf`。

### 4.3 Target 名称核验

ask 给出的四个名称**全部存在**，无需替换：

| 请求 target | 定义位置 |
|---|---|
| `bench_w02_pipeline` | `xmake/bench.lua` |
| `bench_w03_external_loop` | `xmake/bench.lua` |
| `sluice-copy` | `xmake/apps.lua` |
| `uring_submit_boundary_test` | `xmake/tests.lua` |

## 5. 基线二进制

路径根：`/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/`

| Target | sha256 | size |
|---|---|---|
| `bench_w02_pipeline` | `78d0db9dc2f9653b71d3613e17b38fad319ef066902aecf308e9ce0369b1ccec` | 225032 |
| `bench_w03_external_loop` | `929bc12fc093a137778657592e00f029636cabca70daddc286d019d6f1a99c09` | 200456 |
| `sluice-copy` | `2ff18cc0fc70901af8b711fda63017662577351671c08f6df8be891e4b12fd4b` | 110104 |
| `uring_submit_boundary_test` | `e2fad3e32863c08185396fd9a08512688363940b491a500e8cd44c22a5d23ba4` | 11144 |
| `libsluice_core.a` | `a536bc6ac37c5c680f2cbe5d1714df7e09dbaf1ef1945cb509969f9bf110055f` | 19000 |
| `libsluice_async.a` | `76e59bd6290dc0a99c656d984187816aef65a3a89589ae3ecdb596d81038e2a4` | 270666 |

证据：`../raw/binary-sha256.txt`。

### 5.1 可复现性核验（已执行）

在 worktree 内共执行 3 次构建，其中 2 次为 `xmake clean` 后重建，**三次记录到的四个二进制 sha256 完全一致**：

- 第 1 次：worktree 内构建，记录 `78d0db9d… / 929bc12f… / 2ff18cc0… / e2fad3e3…`
- 第 2 次：`xmake clean` 后重建，四个 SHA 与第 1 次逐一相同
- 第 3 次：再次 `xmake clean` 后重建，`build ok, spent 0.488s`，四个 SHA 仍逐一相同（日志 `../logs/build-baseline.log`）

判定：**DETERMINISTIC**，摘要可复现。

不属于上述核验的构建：更早一次构建因 builddir 解析到 `/tmp/build`（见 §4.2 第 2 步），其产物已 `rm -rf`，**未记录摘要**，不计入复现性证据。

限定说明：xmake 使用 ccache（`/usr/lib64/ccache/g++`）与自身 build cache，因此严格意义上的「冷缓存、从零编译」字节级复现本轮**未**单独验证；已验证的是同机同工具链下 clean+rebuild 的稳定复现。

## 6. 测量锁与并发核查

```bash
touch /home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock
```

结果：存在，0 字节，`2026-11-05 11:49:21 +08:00`。

并发进程核查（用 `pgrep -x` 精确名匹配，避免把 shell 自身命令行算进来）：

| 进程名 | 结果 |
|---|---|
| `bench_w02_pipeline` | none |
| `bench_w03_external_loop` | none |
| `bench_w01_direct` | none |
| `bpftrace` | none |
| `perf` | none |

`ps -eo pid,comm,args` 中 comm 匹配 `bench|bpftrace|perf` 的进程：none。核查时 load average `2.08, 1.29, 0.57`（1 分钟值偏高，属构建刚结束的余波；15 分钟值 0.57 显示机器整体空闲）。

## 7. 未做事项

- **未运行任何 benchmark 计时**（按 ask 要求）。
- 未运行 `uring_submit_boundary_test` 等测试（本轮只构建）。
- 未修改 governor / turbo / SMT / 任何系统旋钮。
- 未 commit、未 push、未改公开 API、未动远端与 Issue。
- 未验证 `/tmp` 上 warm-cache 协议的实际工作集容量需求（仅记录容量与占用）。
