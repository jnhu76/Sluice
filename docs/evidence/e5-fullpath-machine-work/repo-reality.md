# 仓库现实基线勘察（e5-fullpath-machine-work）

- 勘察时间：2026-11-18（本地时间 00:12–00:35）
- 勘察人：仓库勘察员（只读勘察 + 一次 git fetch；除本文档目录外未做任何写操作）
- 基线提交：master `5d94f233967c3aef993eb19ba9e3252c532b0951`
  （"Merge pull request #635 from jnhu76/product/621-progress-drain-single-read"，2026-11-13）
- 远端：`origin git@github.com:jnhu76/Sluice.git`
- xmake 版本：`xmake v3.1.2+HEAD.5d21e9d`

本文档冻结本次性能研究开始时的仓库现实。所有结论均来自本会话实际执行的命令与读到的文件；
未运行的检查（构建/测试/基准，按任务约束一律不跑）明确标注"未执行"。

## 1. Fetch 与 master SHA

命令：`git fetch origin --prune` → exit 0，无输出（无新引用变化）。

```
=== local master ===
5d94f233967c3aef993eb19ba9e3252c532b0951
=== origin/master ===
5d94f233967c3aef993eb19ba9e3252c532b0951
=== HEAD ===
5d94f233967c3aef993eb19ba9e3252c532b0951
5d94f233967c3aef993eb19ba9e3252c532b0951 Merge pull request #635 from jnhu76/product/621-progress-drain-single-read
```

结论：本地 master 与 origin/master 完全一致，无分叉；未做任何 pull/merge/checkout/rebase/stash。

## 2. 仓库状态盘点

### 2.1 git status --porcelain

```
?? docs/evidence/621-round2-R1/
```

唯一未跟踪项：`docs/evidence/621-round2-R1/`（他人 621 第二轮在制品，含
`REPORT.md` 138,781 字节，mtime 2026-11-12 19:38）。**列入 protectedDirs，本次不改、不读入结论。**
除此之外无未跟踪、无已修改跟踪文件。

### 2.2 git worktree list

```
/home/jnhu/Source/Sluice                  5d94f233 [master]
/home/jnhu/sluice-621-r2/wt-A-profile     3f22d2d0 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-A-sym         3f22d2d0 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-A-timing      3f22d2d0 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-B             a3a14a31 [research/621-r2-R1-drain-eagain-read]
/home/jnhu/sluice-621-r2/wt-B-sym         a3a14a31 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-B-timing      a3a14a31 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-base-check    3f22d2d0 [verify/621-r2-base-check]
/home/jnhu/sluice-621-r2/wt-base-semantic 3f22d2d0 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-base-verify   3f22d2d0 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-build-check   3f22d2d0 (detached HEAD)
/home/jnhu/sluice-621-r2/wt-evidence      0e50612c [evidence/621-progress-drain-single-read-20261112]
/home/jnhu/sluice-621-r2/wt-product       f707ff39 [product/621-progress-drain-single-read]
```

12 个并行 worktree 全部位于 `/home/jnhu/sluice-621-r2/`，是 621 第一/第二轮的实验在制品
（多个基于 `3f22d2d0`/`a3a14a31`，非当前 master）。**整体列入 protectedDirs。**

### 2.3 git branch -a

```
+ evidence/621-progress-drain-single-read-20261112
* master
+ product/621-progress-drain-single-read
+ research/621-r2-R1-drain-eagain-read
+ verify/621-r2-base-check
  remotes/origin/HEAD -> origin/master
  remotes/origin/archive/research-knowledge
  remotes/origin/evidence/621-progress-drain-single-read-20261112
  remotes/origin/master
  remotes/origin/product/609-lean-elisions-e5
  remotes/origin/product/621-progress-drain-single-read
  remotes/origin/product/621-threadpool-owed-count-gate
  remotes/origin/research-595-flow-audit
  remotes/origin/research-595-r5
  remotes/origin/research-595-r5-diag
  remotes/origin/research-595-r5rm-diag
  remotes/origin/research-595-r5rm-diag-cleanup
  remotes/origin/research/595-causal-r2
  remotes/origin/research/595-information-audit
  remotes/origin/research/595-pause-20261107
  remotes/origin/research/595-r3-archive
  remotes/origin/research/595-r3-continuation
  remotes/origin/research/595-r4-state-aware
  remotes/origin/research/595-r5-phase-a
  remotes/origin/research/609-e5-base-instrument
  remotes/origin/research/609-integration-all
  remotes/origin/security/ignore-agent-local-state-20261110
```

4 个本地非 master 分支均带 `+` 前缀 = 已检出到上述并行 worktree，不构成待合并变更。

### 2.4 git stash list

空（无任何 stash）。

### 2.5 /tmp 残留

`ls /tmp | grep -i sluice|621|595|609|e5|bench|profile` 与 `/tmp/*sluice*` glob 均无匹配；
仅有一个与 Sluice 无关的 socket `znr-1d5de5f2-...sock`。**/tmp 无 Sluice 实验残留。**
（注意：`bench/native-linux-m2/run.sh` 运行时会使用 `/tmp/sluice-m2-fixture`，当前不存在，即近期未跑过。）

## 3. 构建与目标盘点（xmake.lua + xmake/）

入口 `xmake.lua`：C++20，`-Wall -Werror`；modes `debug/release/valgrind` + asan/tsan/ubsan/asanubsan
（经 set_policy）；options：`hardened`（默认 false，须配 -m release）、`liburing`（默认 false）、
`sanitizers`（默认 ""）。子文件：`xmake/helpers.lua`（单文件目标助手，源文件存在才注册）、
`uring.lua`（liburing 能力探测，`--liburing=y` 要求真实 liburing，绝不静默降级）、
`libraries.lua`（生产库）、`apps.lua`（参考应用）、`tests.lua`（测试）、`bench.lua`（基准）。

### 3.1 生产库（libraries.lua）

- `sluice_core`（static，默认目标）：src/*.cpp，公开头 allowlist 安装。
- `sluice_async`（static，非默认，group "async"）：src/async/*.cpp + detail 两个 TU；
  `SLUICE_HAS_LIBURING` 与 `-luring` 由配置传播（消费者不得手工复制）。

### 3.2 应用目标（apps.lua，group "apps"，全部非默认，`xmake -g apps` 构建）

`sluice-copy`、`sluice-hash`、`sluice-grep`、`sluice-tail`（仅用公开头，无测试 seam）。

### 3.3 测试目标（tests.lua，group "test"）

静态清点（读 tests.lua 全文 1264 行得出；未跑 `xmake show`/`xmake test`）：

- **注册为测试（`add_tests`）的目标：81 个**（当前保存配置 liburing=y 时的数量；liburing=n 时为 60，
  差 21 个 uring-only 目标）。要点：
  - 直通 File 语义 28 个（file_*、blocking_file_*、semantic_*、direct_*、explicit_file_ref、
    async_sync_admission、uring_submit_boundary、file_access_precedence）；
  - 请求底座 4（request_core_{protocol,publication,consumer_probe,init_closure}_test）+ 消费者探针；
  - 进度/竞态 oracle（threadpool_*、uring_*、progress_source_*，多为 120s run_timeout）；
  - 公共 Request/RequestScope/shutdown/backend_contract/request_error_domain/file_move_borrow/
    submit_concurrency 各 threadpool+uring 双变体；
  - 应用消费测试（app_hash/grep/tail/copy 及 tail 生命周期/CLI oracle，后者 180s timeout）；
  - `uring_backend_smoke_test`（真环冒烟）、`semantic_backend_conformance_test`。
- **组内但不注册 `add_tests`（`xmake test` 不会跑）**：`tail_pread_shim`（共享库 interposer）、
  `progress_source_drain_mechanism_test`（计数机制目标，无纯基头 seam 不能注册）、
  **94 个突变构建**（`*_mut_*`，注释明言"executed and recorded manually, never run as regular
  tests"）。三类合计 96 + tail_pread_shim + drain_mechanism = 98。
- 测试组目标总数（liburing=y）：81 注册 + 98 非注册 = **179**（静态推算，未经 xmake 验证）。

### 3.4 基准目标（bench.lua，group "bench"，全部非默认）

`bench_w01_direct`（仅链 sluice_core，W-01 直通路径）、`bench_w02_pipeline`（+sluice_async）、
`bench_w03_external_loop`（+sluice_async）。bench.lua 文件头约定：

```
-- Build:  xmake build -g bench -y
-- Run:    SLUICE_BENCH_JSONL=<file> xmake run bench_w01_direct
```

### 3.5 bench/ 下的非 xmake 测量装置（脚本体，非 xmake 目标）

- `bench/build_profiles.sh`：G0 双 profile 构建。**每个 profile 独立 build 目录**
  （`xmake f -m release -o build-bench-{noring,ring} --liburing={n,y} -y` + `xmake build -y <target>`），
  注释警告：跨 `--liburing` 切换复用一个 build 目录会把 stub 链进 ring 消费者（#520）；
  并用 readelf 断言 profile 身份、断言 bench_w01_direct 跨 profile 字节一致。
- `bench/run_matrix.sh`：预注册 G0 矩阵执行器，`Usage: run_matrix.sh <outdir> [warmup] [measured] [stage] [suffix]`
  （默认 warmup=2 measured=7，stage=all|measured|diagnostics），每个 run 由
  `bench/check_run.py` 对照冻结参考（默认 `docs/evidence/g0-403-performance-baseline-20261015/raw`）
  校验，任何偏差即非零退出；拒绝向 docs/evidence/ 内写规范命名证据文件。
- `bench/attribution.sh <outdir> [warmup] [measured]`：G0 归因证据（syscall 计数、PMU、
  DSO/symbol、CPU placement、lock probe），profiling 构建 -O3 -g 不与基线吞吐可比。
- `bench/mechanisms/run_mechanisms.sh <output-dir> [reps] [ops-scale]`：M0/M1 机制实验仪
  （H1 router/H2 owed/H3 scope slots），g++ -O2 自构建、不链 Sluice 库、默认绑 CPU 2。
- `bench/native-linux-m2/run.sh <worktree-root> <output-dir> [reps] [cpu]`：M2 配对 A/B
  （base/h1/h2a/h2b/h2ab worktree，须各自 `xmake f -m release --liburing=y`），默认 reps=15 cpu=2，
  fixture `/tmp/sluice-m2-fixture`。
- `bench/research/`：593/580/595 研究脚本归档；其中 `uring-submit-policy/round-e5-20261104/`
  是**上一轮 E5（2026-11-04，uring submit policy）的 bench 侧证据归档**（patch.diff、
  PREREGISTERED.md、raw-minimal 等；叙事记录在 docs/research/uring-submit-policy/round-e5-20261104/）。
  本次 "e5-fullpath-machine-work" 命名与其同属 E5 线，但其内容是既有基线材料，全部已跟踪，不构成在制品。

### 3.6 scripts/（验证入口，非基准）

`verify_tla.sh`（ShutdownCore TLA+ 镜像，shutdown_mut_m6/m7/m8 由它杀伤）、
`verify_progress_source_model.sh`、`verify_v2_package.py`、`check_cpp_comment_authority.py`
（各自带自测试）。属形式/纪律验证入口，本次不跑。

### 3.7 已保存配置与既有构建产物

`.xmake/linux/x86_64/xmake.conf`（读文件得出）：

```
mode = "release"; liburing = true; hardened = false; sanitizers = ""; ccache = true; arch = x86_64
```

`build/` 目录存在（linux/x86_64）。即主仓已配置好 release + liburing=y 且有过构建；
`xmake build -y` 无需先跑 `xmake f`。**若需重建配置**，幂等命令为
`xmake f -m release --liburing=y -y`（README.md:146 与 bench/native-linux-m2/run.sh:46 均用此形式）。

## 4. 构建入口与参数决定

- **configureArgs = []**：`.xmake` 已有匹配基线的保存配置（release + liburing=y），无需先配置。
- **buildArgs = ["-y"]**：`xmake build -y`。注意默认目标只有 `sluice_core`（其余目标 set_default(false)）；
  测试目标由 `xmake test -y <name>` 自行构建，基准走 `xmake build -g bench -y`。
- **testArgs = ["test","-y", 12 个默认超时档测试名]**：挑选原则 = 只取 tests.lua 中**未带
  run_timeout 覆盖（120s/180s 档）**的注册测试，覆盖直通 File 语义、请求底座、公共 Request
  与真环冒烟，避免全矩阵：
  `file_read_test file_write_test file_sync_data_test file_sync_all_test
   blocking_file_read_test blocking_file_state_test blocking_file_sequential_test
   direct_composition_test file_access_precedence_test request_core_protocol_test
   public_request_test uring_backend_smoke_test`
  （实际共 12 个；完整命令 `xmake test -y file_read_test ... uring_backend_smoke_test`。）
  语法依据 xmake v3.1.2 标准 `xmake test [names...]`；**本会话未执行**（任务禁止跑测试），
  由工作流脚本首次运行验证。
- 本次性能研究的基准构建**必须**走 `bench/build_profiles.sh` 的独立
  `build-bench-noring/`、`build-bench-ring/` 目录，勿复用主 `build/`（#520 profile 混链风险，
  见 build_profiles.sh 注释与 docs/performance/methodology.md 的 Freeze/Baseline 规程）。

## 5. 保护清单与冲突风险

1. `docs/evidence/621-round2-R1/`：他人 621 第二轮在制品（唯一未跟踪路径），绝不触碰。
2. `/home/jnhu/sluice-621-r2/` 下 12 个并行 worktree：他人在制品，绝不 checkout/写入/删除。
3. 主仓 `build/`、`.xmake/`：既有 release+liburing=y 构建与配置。可以增量构建，但不要重置配置；
   确需重配置用幂等命令 `xmake f -m release --liburing=y -y`。
4. 其余 docs/evidence/* 均为已跟踪证据目录（本次只读保护，由"不修改被跟踪文件"约束覆盖）。
5. /tmp 无 Sluice 残留，无需清理。
6. 运行基准时注意 run_matrix.sh 的写入纪律：结果目录应在仓库外或带 suffix，避免覆盖
   docs/evidence/ 下既有规范证据（脚本自身会拒绝）。
