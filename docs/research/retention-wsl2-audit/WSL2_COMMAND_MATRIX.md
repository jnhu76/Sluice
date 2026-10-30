# WSL2 可执行命令矩阵 — 命令勘察（retention-wsl2-audit）

- 日期：2026-10-29。性质：只读勘察，未在主工作区执行任何构建；未修改产品代码。
- 勘察对象：本地 master `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`（= origin/master，
  工作区除本审计目录外 clean；#602/#612 已合并，见 receipts/06-merge-shas.txt 与
  state-freeze-report.md §1 的 backup/pr-612-original-head-70d48f5e 引用）。
- 主要依据（本轮全部实读）：
  - `xmake.lua`、`xmake/{helpers,uring,libraries,apps,tests,bench}.lua`
  - `research/direct-beyond-parity/WSL2_VALIDATION_HANDOFF.md`（ask 所指
    `docs/research/direct-beyond-parity/` 实际位于顶层 `research/`，见 gaps #3）
  - `research/direct-beyond-parity/closure/{r3-c1-review.md,r4-p2c1.md,
    r9-independent-correctness-review.md,perf-p2c1/{SUMMARY.md,ENV-P2C1.txt,
    paired.sh,paired2.sh,build-both.sh,ladder.cpp,analyze2-p5.py,mkfixture.cpp}}`
  - `research/direct-beyond-parity/p2-scripts/{P2C1-matrix.sh,P2C1-mutants.sh}`
  - `research/direct-beyond-parity/P1_MEASUREMENT_PLAN.md`（尺寸扫描 ops 表）
  - `bench/{bench_w01_direct.cpp,bench_common.hpp}`、`scripts/verify_v2_package.py`
- 本轮回执：`receipts/cmd-01-test-set-enumeration.txt`（静态测试集枚举 58/79）、
  `receipts/cmd-02-mutation-seams-killers.txt`（seam 与 killer 定义）、
  `receipts/cmd-03-wsl2-env.txt`（WSL2 工具链/liburing/io_uring/文件系统）。
- 纪律声明：本轮仅执行了只读命令（grep/ls/git 读命令/test -f、/tmp 下的
  `xmake test -h`）；未运行任何 `xmake f/build/test` 于仓库内；58/79 为静态
  枚举结果而非实跑，实跑确认须在 /tmp 验证 worktree 构建后进行（gaps #5）。

## 0. WSL2 环境事实（本轮实测，receipts/cmd-03）

| 项 | 值 | 影响 |
|---|---|---|
| xmake | v3.1.2+HEAD.5d21e9d（/home/hoo/.local/bin/xmake） | 与 E5 相同版本（handoff §7.7 满足） |
| g++ | Ubuntu 15.2.0 | 非 E5 的 GCC 16.2.1：机器码/布局类证据必须重导出，不得抄数（handoff §7.2/§7.3） |
| kernel | 6.18.54.1-microsoft-standard-WSL2 | |
| liburing | /usr/include/liburing.h + liburing.so/.so.2 均在 | `--liburing=y` 构建面可行（xmake/uring.lua 探针所需 header+library 齐） |
| io_uring | /proc/sys/kernel/io_uring_disabled = 0 | 内核允许 io_uring；`--liburing=y` 运行面可行，但结果属"新证据"，非 79/79 复现（handoff §7.6） |
| /tmp | tmpfs | 计时 fixture 可放 /tmp；另 /home/hoo 为 ext4（native），两者皆可用；**绝不使用 /mnt/***（handoff §7.4） |
| strace/python3 | 均在 | strace oracle 与 verify_v2_package.py 可执行 |

## 1. 构建开关与模式的权威定义（当前 master）

- 模式（`xmake.lua:3-19`）：`release` / `debug` / `asan` / `tsan` / `ubsan` /
  `asanubsan`（另有 valgrind 规则，未在本矩阵使用）。sanitizer 通过
  `set_policy("build.sanitizer.*")` 生效。
- 附加选项 `--sanitizers=`（`xmake.lua:70-89`，逗号分隔 asan/ubsan/tsan）与
  `--hardened`（`xmake.lua:27-51`，仅限 release）存在；E5 冻结矩阵未使用，
  本矩阵沿用 `-m` 单一路径。
- **liburing 开关：`--liburing=y` / `--liburing=n`**（选项定义
  `xmake/uring.lua:36-39`，默认 n）。`y` 要求真实 liburing 且绝不静默回退；
  配置期运行 capability 探针（`xmake/uring.lua:117-301`）。profile 身份宏
  `SLUICE_LIBURING_PROFILE=on|off` 无条件定义；`SLUICE_HAS_LIBURING` 仅在
  `y` 分支随 sluice_async 公共使用要求传播（`xmake/libraries.lua:66-81`）。
  消费者不得手抄该宏（tests.lua 注释 + libraries.lua:60-65）。
- 测试目标注册：常规套件经 `add_tests()`（`xmake/helpers.lua:16`）注册，
  `xmake test` 构建并运行；所有 mutant target 为 `set_default(false)` 且
  **无 `add_tests`**（`xmake/tests.lua:81-120`），永不进入常规 `xmake test`。
- 静态测试集（receipts/cmd-01，os.isfile 条件逐项镜像）：
  `--liburing=n` → **58** 个；`--liburing=y` → 58 + **21** = **79** 个。
  与 E5 冻结记录 58/58、79/79（r3 §1、r4 §6）数值一致；此为静态枚举一致，
  非本轮实跑。

## 2. 各配置确切命令序列

工作目录一律为 /tmp 验证 worktree（见 §6），绝不在主工作区构建。
`<WT>` = worktree 路径。二进制产出路径 `build/linux/x86_64/<mode>/`。

### 2.1 release（及 debug / asan / ubsan / asanubsan 同型）

E5 冻结口径（r3 §1、r4 §6、P2C1-matrix.sh:17-42）：

```sh
cd <WT>
xmake f -c -m release --liburing=n -y && xmake -y && xmake test -y
# 成功判据：0 failed；E5 参照 58/58 passed, 0 failed（候选 70d48f5e）
```

其余 leg 仅替换 `-m`：`debug`、`asan`、`ubsan`、`asanubsan`。

UBSan leg 追加输出检查（r3 §1 口径）：

```sh
xmake f -c -m ubsan --liburing=n -y && xmake -y && xmake test -v 2>&1 | tee /tmp/sluice-val-ubsan/ubsan.log
grep -c "runtime error" /tmp/sluice-val-ubsan/ubsan.log   # 期望 0
```

可选加固变体（非冻结矩阵成员）：`xmake f -c -m release --hardened -y`（规则
`xmake.lua:32-38` 强制 hardened 必须配 release）。

### 2.2 liburing on / off

- **off**：即 §2.1 全部命令（`--liburing=n` 为显式关）。含诚实 stub，无
  `SLUICE_HAS_LIBURING`。
- **on**（handoff §8.1：可选 leg，属新证据而非复现；本轮环境构建面/运行面
  均具备，见 §0）：

```sh
cd <WT>
xmake f -c -m release --liburing=y -y && xmake -y && xmake test -y
# 成功判据：0 failed；E5 参照 79/79（多出的 21 个 uring-only 套件，
# 清单见 receipts/cmd-01 下半段）
```

`y` 首次配置触发 capability 探针：解析 `<liburing.h>` 与 `-luring` 的真实
工具链解析、pin 一致性、声明/链接面检查（`xmake/uring.lua:117-301`）。

### 2.3 Direct Primitive 测试（read_at/write_at/read/write 及准入面）

handoff §2 的定义：四个公共原语 = `read_at`/`write_at`/`read`/`write`；
C1 三个 killer 全部落在原语/准入套件（§5 表）。`xmake test` 支持按名称
过滤（`xmake test [tests...]`，支持模式匹配；本轮于 /tmp 实测
`xmake test -h`，receipts/cmd-03 末段）。聚焦运行：

```sh
cd <WT>
xmake f -c -m release --liburing=n -y && xmake -y
xmake test -y blocking_file_read_test blocking_file_write_test \
    blocking_file_state_test blocking_file_sequential_test \
    direct_cursor_position_test file_access_precedence_test \
    file_close_semantics_test semantic_range_test \
    semantic_validation_precedence_test
```

（套件分组为本审计按上述定义归纳，仓库未注册同名目标组，见 gaps #7。
`file_close_semantics_test` 为 seam 构建，`SLUICE_FILE_INTERNAL_TESTING`
独立编入，`xmake/tests.lua:60-74`。）

### 2.4 Composition 测试（read_exact_at/write_all_at/read_exact/write_all）

```sh
cd <WT>
xmake f -c -m release --liburing=n -y && xmake -y
xmake test -y direct_composition_test direct_composition_fault_test \
    semantic_short_io_reference_test semantic_effect_outcome_test
```

- `direct_composition_fault_test`：组合全停机理由 oracle + 写侧
  impossible-count 脚本（r4 §6、§2 delta 表）。
- `semantic_short_io_reference_test`：SEM-05 规则表 oracle（r4 §6）。
- `semantic_effect_outcome_test`：Effect Certainty 记账（r4 §6 列举的义务面）。

### 2.5 Installed SDK Package Verifier

权威命令（r3 §3.1 / r4 §8.1 / r9 同型）：

```sh
cd <WT>
python3 scripts/verify_v2_package.py --mode release \
    --manifest-out /tmp/sluice-val-sdk/v2-manifest.json --keep-prefix
```

- 成功判据：exit 0，全部 gates PASS（E5 记录 24/24，含
  `V2_B_PACKAGE_PREFIX_READY`＝恰 11 契约头、`CLEAN_ROOM_*`、`P1_*`、
  `NEGATIVE_V2_REMOVED_HEADERS_UNAVAILABLE`、`NEGATIVE_V2_SEAM_UNAVAILABLE`、
  `NEGATIVE_V2_MUTANT_DEFINES_NOT_INSTALLED`、
  `NEGATIVE_ASYNC_SYMBOLS_NOT_IN_CORE`）。
- manifest 应含 `VERIFICATION_HEAD=<HEAD>`、`VERIFICATION_HEAD_DIRTY=False`。
- `--keep-prefix` 保留安装前缀（`scripts/verify_v2_package.py:443,766`），
  供 §2.6 独立消费者使用；前缀路径打印于 verifier 输出（E5 形如
  `/tmp/sluice-v2-prefix-*`）。
- 注意：verifier 自身会构建库并安装（构建型步骤，必须在 worktree 内执行）。
- liburing 变体（可选）：`--liburing --uring-prefix <prefix>`（usage 见
  `scripts/verify_v2_package.py:36-45`）。

### 2.6 Independent Consumer（独立消费者探针）

handoff §8.4 的 16-case 行为集，编链只对安装前缀（r4 §8.2 形状）：

```sh
# 前提：§2.5 已用 --keep-prefix 跑完，取其输出中的 prefix 路径
PFX=/tmp/sluice-v2-prefix-XXXXXXXX            # 以 verifier 实际输出为准
cat > /tmp/sluice-val-sdk/consumer_probe.cpp <<'EOF'
// 仅尖括号包含 <sluice/blocking/file.hpp>；按 16-case 行为集自写。
// WSL2 执行者需自行编写探针源码（R3 21-case / R4 16-case 源码未归档，见 gaps #1）。
EOF
g++ -std=c++20 -I"$PFX"/include /tmp/sluice-val-sdk/consumer_probe.cpp \
    -L"$PFX"/lib -Wl,-rpath,"$PFX"/lib -lsluice_core \
    -o /tmp/sluice-val-sdk/consumer_probe
/tmp/sluice-val-sdk/consumer_probe    # 期望 16/16 PASS
```

16-case 行为集（handoff §8.4 + r4 §8.2）：满读/写往返；past-EOF 31B/23B →
`eof_before_full` + confirmed=23 + remaining=accounted + 无 error；尾部跨界
（offset 20，3B 前缀）；零长度 → complete/0；`UINT64_MAX` offset →
`invalid_argument`；closed 文件组合 → `invalid_state`（显式 close 后，含
closed-before-empty 零长度臂）；write-only 组合读 → `invalid_argument`
（access-before-empty，含零长度臂）；move 后 close 语义；
`Result<CompositionOutcome>` 字段记账（confirmed/remaining/certainty）。
仓库内对应面：verifier 的 CLEAN_ROOM/P0/P1 消费者
（`tests/package/consumers/`，`scripts/verify_v2_package.py:31-32`）已在
§2.5 门内；本项是"只对安装面"的独立复核。

### 2.7 strace syscall oracle（形状检查，非计时；schema 外，记录于此）

r4 §8.3 形状（driver 源码未归档，需自写，gaps #2）：

```sh
strace -f -e trace=pread64,pwrite64 /tmp/sluice-val-sdk/strace_driver 2>&1 | grep -E "pread64|pwrite64"
# 期望：满 read_exact_at = 1×pread64；31B 请求/23B 文件 = 2（数据步+EOF 步各 1）；
# 零长度 = 0；closed = 0；write_all_at = 1×pwrite64
```

### 2.8 Mutation gate（C1 三项 + P2C1 七项）

冻结口径（r3 §2、r4 §5、P2C1-mutants.sh）：release + `--liburing=n`。
每个 mutant：`GREEN(基线套件) → RED(具名单行 FAIL, exit=1) → RESTORED GREEN`；
**crash/signal/build 失败/超时不算 kill**（P2C1-mutants.sh:60-72 显式拒收）。

```sh
cd <WT>
xmake f -c -m release --liburing=n -y
# C1 三项一次构建（r3 §2 原命令）：
xmake build -y blocking_file_mut_admit_invalid_range \
              blocking_file_mut_empty_before_closed \
              blocking_file_mut_admit_unrepresentable_size
# P2C1 七项：
xmake build -y blocking_file_mut_advance_by_requested \
              blocking_file_mut_retry_zero_progress \
              blocking_file_mut_short_io_retry_zero_progress \
              blocking_file_mut_failure_accounted \
              blocking_file_mut_skip_impossible_count \
              blocking_file_mut_step_reports_full_count \
              blocking_file_mut_step_skips_eintr_retry
# 逐个运行（timeout 120；期望 exit=1 且 stderr/stdout 恰一行 FAIL: <killer>）：
timeout 120 ./build/linux/x86_64/release/blocking_file_mut_admit_invalid_range; echo "exit=$?"
# …（10 个 target 逐一，kill 映射见 §3/§4 表）
# 恢复确认（r4 §5 RESTORED_GREEN）：
git status --porcelain -- src/ include/ tests/ xmake/ docs/   # 必须为空
xmake -y && xmake test -y                                     # 期望 58/58
```

## 3. C1 三项 mutation 精确定义

seam 权威：`include/sluice/detail/file_semantics.hpp`（未安装 detail 头）。
killer 行号已在本轮 master 实测核对（receipts/cmd-02）。

| id | target（xmake/tests.lua） | original | mutated | killCriterion（唯一 FAIL 行） |
|---|---|---|---|---|
| C1-1 | `blocking_file_mut_admit_invalid_range`（tests.lua:91-93） | `precheck_data_op` 在 length/kMaxNativeTransfer 前执行 `range_is_valid(offset,length)`，非法 → `reject_range`（file_semantics.hpp:171-174） | `#if defined(SLUICE_SEM_MUTANT_ADMIT_INVALID_RANGE)` 在 range 检查前直接 `return DataOpVerdict::execute`（file_semantics.hpp:168-170），偏移不再校验 | exit=1，恰一行 `FAIL: read_with_illegal_offset_reports_invalid_argument`（tests/blocking_file_read_test.cpp:122；`read_at(file, UINT64_MAX, 1B)` 期望 `invalid_argument`；kill 链：回绕偏移 → `pread` EINVAL → EINVAL 不在 `kNativeErrorMappings`（error.hpp:66-88）→ `backend_error` ≠ `invalid_argument`，r3 §2） |
| C1-2 | `blocking_file_mut_empty_before_closed`（tests.lua:94-96） | `precheck_data_op` 顺序：closed → access → length==0 → complete_empty（file_semantics.hpp:162-167） | `#if defined(SLUICE_SEM_MUTANT_EMPTY_BEFORE_CLOSED)` 把 `length==0 → complete_empty` 提到 closed/access 之前（file_semantics.hpp:158-161） | exit=1，恰一行 `FAIL: sequential_read_on_write_only_file_rejected_upfront`（tests/blocking_file_sequential_test.cpp:392；write-only 文件零长度 read 期望 `invalid_argument` 而非提前成功；closed+empty 第二探针 :418-437，r3 §2） |
| C1-3 | `blocking_file_mut_admit_unrepresentable_size`（tests.lua:97-99） | `precheck_resize` 在 execute 前检查 `size_is_representable(new_size)` → `reject_range`（file_semantics.hpp:192-195） | `#if !defined(SLUICE_SEM_MUTANT_ADMIT_UNREPRESENTABLE_SIZE)` 删除该检查（192-196），超 off_t 尺寸放行 | exit=1，恰一行 `FAIL: resize_overflow_reports_invalid_argument`（tests/blocking_file_state_test.cpp:320；`resize(UINT64_MAX)` 期望 `invalid_argument`；mutant 下 `ftruncate` 收回绕负值 → EINVAL → `backend_error`，r3 §2） |

## 4. P2C1 七项 mutation 精确定义

同一 define 同时武装两腿：机制腿（`src/blocking_file.cpp` compose 循环）与
规则表腿（`compose_progress`，非权威 oracle）。两腿各有一个 mutant target
（RETURY_ZERO_PROGRESS 场景），killer 不同（handoff §5、P2C1-mutants.sh:21-29）。
行号为本轮 master 实测（receipts/cmd-02）。

| id | define / target | original | mutated | killCriterion（唯一 FAIL 行） |
|---|---|---|---|---|
| P2C1-1 | `SLUICE_COMPOSE_MUTANT_ADVANCE_BY_REQUESTED` / `blocking_file_mut_advance_by_requested`（tests.lua:100-102） | compose 循环取 `transferred = step.value()`（blocking_file.cpp:308-313） | 改为 `transferred = total - confirmed_bytes`（blocking_file.cpp:308-310；规则表同定义 file_semantics.hpp:248-250） | exit=1，`FAIL: short_reads_report_eof_before_full_with_the_prefix`（tests/direct_composition_fault_test.cpp:63） |
| P2C1-2 | `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS`（fault 腿）/ `blocking_file_mut_retry_zero_progress`（tests.lua:103-105） | compose 循环 `transferred==0` → 停机 `eof_before_full`/`write_no_progress`（blocking_file.cpp:323-330） | `#if !defined(...)` 删除该停机，零进展无限重试（323-330） | exit=1，`FAIL: short_reads_report_eof_before_full_with_the_prefix`（direct_composition_fault_test.cpp:63） |
| P2C1-3 | `SLUICE_COMPOSE_MUTANT_RETRY_ZERO_PROGRESS`（rules 腿）/ `blocking_file_mut_short_io_retry_zero_progress`（tests.lua:106-108） | 规则表 `compose_progress`：`transferred==0` → `eof_before_full`(read)/`write_no_progress`(write)（file_semantics.hpp:264-270） | 同名 define 删除规则表停机分支 | exit=1，`FAIL: read_exact_accumulates_a_short_prefix_then_reports_eof`（tests/semantic_short_io_reference_test.cpp:59）。handoff §5 rules-stay-green control：规则表是非权威 oracle，规则表突变不得使实现套件变红（实现保持绿），规则表永不成为第二权威 |
| P2C1-4 | `SLUICE_COMPOSE_MUTANT_FAILURE_ACCOUNTED` / `blocking_file_mut_failure_accounted`（tests.lua:109-111） | `failure_publish_certainty()` 返回 `EffectCertainty::unknown`（blocking_file.cpp:244-250） | 改为返回 `EffectCertainty::accounted` | exit=1，`FAIL: error_after_confirmed_prefix_keeps_the_prefix`（tests/direct_composition_fault_test.cpp:197；钉住错误出口 remaining=unknown，r4 §4） |
| P2C1-5 | `SLUICE_COMPOSE_MUTANT_SKIP_IMPOSSIBLE_COUNT` / `blocking_file_mut_skip_impossible_count`（tests.lua:112-114） | compose 循环 `transferred > total - confirmed` → `invalid_state` 停机（blocking_file.cpp:314-319；规则表 251-261） | `#if !defined(...)` 删除 impossible-count 守卫 | exit=1，`FAIL: impossible_count_stops_immediately`（tests/direct_composition_fault_test.cpp:340；写侧脚本 `write_all_impossible_count_stops_immediately` :365 为 +27 行新增 killer） |
| P2C1-6 | `SLUICE_DIRECT_MUTANT_STEP_REPORTS_FULL_COUNT` / `blocking_file_mut_step_reports_full_count`（tests.lua:115-117） | `transferred = step.value()`（blocking_file.cpp:308-313；与 P2C1-1 同臂、独立 target/define） | 改为 `transferred = total - confirmed_bytes` | exit=1，`FAIL: short_reads_report_eof_before_full_with_the_prefix`（direct_composition_fault_test.cpp:63） |
| P2C1-7 | `SLUICE_DIRECT_MUTANT_STEP_SKIPS_EINTR_RETRY` / `blocking_file_mut_step_skips_eintr_retry`（tests.lua:118-120） | `step_retry_on_eintr` 经 `detail::retry_on_eintr`（仅 `result<0 && errno==EINTR` 重试）（blocking_file.cpp:78-84） | `#if defined(...)` 改为直接 `return fn()`，单次系统调用不重试 | exit=1，`FAIL: primitive_retries_eintr_without_a_completed_count`（tests/direct_composition_fault_test.cpp:224） |

## 5. benchCmds — 性能观测（WSL2 = 回归判定，永不对绝对值，handoff §8.6）

### 5.1 归档 ladder kit（配对 A/A 与 ABBA 回归；handoff §8.6 的正式口径）

工具已归档在 `research/direct-beyond-parity/closure/perf-p2c1/`
（paired2.sh / mkfixture.cpp / ladder.cpp / analyze2-p5.py / build-both.sh）。
拷入 /tmp/sluice-perf-ladder/ 后按 E5 SUMMARY §6 的命令形状执行：

```sh
cp research/direct-beyond-parity/closure/perf-p2c1/{ladder.cpp,mkfixture.cpp,paired2.sh,analyze2-p5.py} /tmp/sluice-perf-ladder/
# ladder 编译（build-both.sh 的配方，路径需改指本机 worktree，见 gaps #6）：
CCACHE_DISABLE=1 g++ -std=c++20 -O3 -DNDEBUG -fvisibility=hidden -Wall -Werror \
    -I<WT>/include ladder.cpp <WT>/build/linux/x86_64/release/libsluice_core.a -o ./ladder-cand
CCACHE_DISABLE=1 g++ -std=c++20 -O2 -DNDEBUG mkfixture.cpp -o ./mkfixture
./mkfixture /tmp/sluice600/data64m.bin 67108864 && sha256sum /tmp/sluice600/data64m.bin
# E5 参照 sha256 bc020fd1…2993a（mkfixture 确定性，WSL2 应逐字节同）——fixture 身份可比，墙钟数不可比

# (1) A/A 噪声底先行（≥16 pairs；同 binary 自配对，二进制身份序 ABBA）：
./paired2.sh ./ladder-cand ./ladder-cand ax 4096 1024 1024 5 16 /tmp/sluice600/data64m.bin          # comp-read-4k A/A
./paired2.sh ./ladder-cand ./ladder-cand ax 4096 1024 1024 5 16 /tmp/sluice600/data64m.bin tail     # eofmix A/A
./paired2.sh ./ladder-cand ./ladder-cand ac 4096 1024 1024 5 16 /tmp/sluice600/data64m.bin          # cursor A/A
python3 analyze2-p5.py --order < <A/A raw>   # 中位 ≈0、符号平衡 → machinery 无偏；记录 sd_AA 推 MDE′

# (2) 配对 ABBA 回归（负值利于 ladderA）：
./paired2.sh ./ladder-cand ./ladder-base ax 4096 1024 1024 5 16 /tmp/sluice600/data64m.bin        # read_exact_at 组合
./paired2.sh ./ladder-cand ./ladder-base ax 4096 1024 1024 5 16 /tmp/sluice600/data64m.bin tail  # eofmix
./paired2.sh ./ladder-cand ./ladder-base ac 4096 1024 1024 5 16 /tmp/sluice600/data64m.bin        # read_exact
./paired2.sh ./ladder-cand ./ladder-base wx 4096 1024 1024 5 16 /tmp/sluice600/write64m.bin       # write_all_at（E5 本轮未测，新证据）
./paired2.sh ./ladder-cand ./ladder-base wc 4096 1024 1024 5 16 /tmp/sluice600/write64m.bin       # write_all（同上）
./paired2.sh ./ladder-cand ./ladder-base a2 4096 1024 1024 5 16 /tmp/sluice600/data64m.bin        # 负控制：非组合原语必须 PARITY
python3 analyze2-p5.py --order < <raw-file>
```

- 臂语义（ladder.cpp:108-117,1459-1492）：`a2`=Sluice `read_at`（负控制）、
  `a1x/ax`=`read_exact_at`、`a1c/ac`=`read_exact`、`w1x/wx`=`write_all_at`、
  `w1c/wc`=`write_all`。
- 尺寸扫描（ask 的 0B/1B/64B/512B/4KiB/64KiB）沿用 P1 预注册 ops 表
  （P1_MEASUREMENT_PLAN.md §4：0–512B → ops=4096/warmup=4096；4KiB →
  1024/1024；64KiB → 512/512；1MiB → 256/128；reps=5, pairs=16）：

```sh
for sz_ops in "0 4096 4096" "1 4096 4096" "64 4096 4096" "512 4096 4096" "4096 1024 1024" "65536 512 512"; do
  set -- $sz_ops
  ./paired2.sh ./ladder-cand ./ladder-base ax $1 $2 $3 5 16 /tmp/sluice600/data64m.bin   # read_exact_at 尺寸扫描
done
# read_exact/write_all 同型换 ac/wc；写轨道用 write64m.bin + wx/wc
```

- 纪律：A/A 先行；fixture 在 tmpfs 或 ext4，绝不 /mnt/*；串行执行；按
  ENV-P2C1.txt 形状写 env-pin；WSL2 噪声带自定，绝不与 E5 ns 值比较
  （handoff §4/§7.5/§8.6）。

### 5.2 仓库内 bench 目标（非默认组，粗粒度观测）

`xmake/bench.lua:1-32`（构建：`xmake build -g bench -y`；运行：环境变量配置）：

```sh
cd <WT>
xmake f -c -m release --liburing=n -y && xmake build -g bench -y
mkdir -p /tmp/sluice-perf-bench
SLUICE_BENCH_JSONL=/tmp/sluice-perf-bench/w01.jsonl \
SLUICE_BENCH_TMP_DIR=/tmp/sluice-perf-bench \
SLUICE_BENCH_DISK_DIR=/home/hoo/.cache/sluice-bench \
SLUICE_BENCH_WARMUP=64 SLUICE_BENCH_MEASURED=7 \
xmake run bench_w01_direct
# 单 case 过滤：SLUICE_BENCH_CASE_FILTER=W01-read_exact_at-4k（bench_common.hpp:491-500）
```

case 清单（bench_w01_direct.cpp:833-858）：open_close、file_info、read_at
4k-seq/4k-rand/64k-seq/1m-seq、write_at、sync_data/sync_all、
`W01-read_exact_at-4k`、`W01-write_all_at-4k`、cold 盘腿（disk 可用时）。
注意：**in-tree bench 无 read_exact/write_all 独立 case**（gaps #4）；
`SLUICE_BENCH_STATS=1` 开启记账（默认关，bench_common.hpp:441-447）。
bench_w02_pipeline / bench_w03_external_loop 同构建组可用（async 面，非本轮 #600/#608 载体）。

## 6. Worktree 设置要点（/tmp/sluice-val-*、/tmp/sluice-perf-*）

1. 从本地 master 建验证 worktree（不 touch 主工作区；脚本统一清理，**不要
   自行 `git worktree remove`**）：
   `git -C /home/hoo/Projects/Sluice worktree add --detach /tmp/sluice-val-release 84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`
   （E5 先例：r3 §0、r4 §2、perf SUMMARY §1 均为独立 detach worktree。）
2. 每 worktree 在内部配置构建（相对 builddir cwd 行为，handoff §7.7）。
   多配置可单 worktree 顺序 `xmake f -c -m <mode> …`（P2C1-matrix.sh 先例）
   或每配置独立 worktree；release 静态档 sha256 参照
   `p2-raw/P2C1/matrix-release-archive-sha256.txt`（E5 身份，WSL2 不可比）。
3. fixture 与计时文件放 /tmp（tmpfs）或 ext4 native 路径；禁止 /mnt/*。
4. ladder kit 拷贝到 /tmp/sluice-perf-ladder/ 后再改 build-both.sh 中的
   worktree 路径（原文件硬编码 /home/jnhu/Source/…，gaps #6）。
5. 每会话写 env-pin（kernel、xmake/g++ 版本、负载、fs、io_uring 开关），
   镜像 `closure/perf-p2c1/ENV-P2C1.txt`（handoff §8.8）。

## 7. Gaps（找不到/不可比/未执行项，逐条注明搜索位置）

1. **独立消费者探针源码未归档**：R3 21-case（/tmp/r3c1_sdk_consumer_probe.cpp）、
   R4 16-case（/tmp/r4p2c1_sdk_consumer_probe.cpp）均为评审者自写、只存在于
   E5 /tmp。搜索范围：`scripts/`、`tests/package/`（仅 verifier 内置
   CLEAN_ROOM 消费者）、`research/direct-beyond-parity/` 全树
   （grep `sdk_consumer_probe`/`consumer_probe.cpp`，仅命中记录文本 r3/r4/
   P0 与 docs/evidence、docs/roadmap 的引用）。WSL2 执行者须按 §2.6 行为集
   自写。
2. **strace driver 源码未归档**：/tmp/r4p2c1_strace_driver.cpp 同上（r4 §8.3
   仅留期望形状）。命令形状已录于 §2.7；result schema 无 strace 字段，
   故记录在本报告。
3. **ask 路径勘误**：ask 写 `docs/research/direct-beyond-parity/…`；实际
   证据树在顶层 `research/direct-beyond-parity/`（`docs/research/` 下无该
   目录；`find` 验证）。已按实际路径勘察，内容一致（#610 归档提交
   ac995641 的 name-only 列表同为 `research/direct-beyond-parity/…`）。
4. **in-tree bench 无 read_exact/write_all 独立 case**：bench_w01_direct
   组合 case 仅 `W01-read_exact_at-4k`/`W01-write_all_at-4k`
   （bench_w01_direct.cpp:845-846）；`read_exact`/`write_all` 观测须用
   归档 ladder 臂 `ac`/`wc`（§5.1）。
5. **58/79 为静态枚举**：本轮未执行任何构建/测试（纪律），计数来自
   `xmake/tests.lua` 注册条件的逐项文件存在性镜像（receipts/cmd-01）。
   实跑计数（`xmake test` 汇总行）须在验证 worktree 构建后确认；与 E5
   58/58、79/79 的一致性是枚举一致，非复现声明。
6. **perf-p2c1/build-both.sh 不可原样执行**：硬编码
   `/home/jnhu/Source/Sluice-perf-p2c1-{base,head}` 与冻结 SHA
   77e5399a/70d48f5e（E5 已合并入 master）；WSL2 须改指本机 worktree 并按
   合并后主干重锚对比对象（handoff §0"re-anchor"规则）。ladder-base/cand
   的 E5 sha256 与 E5 指令数（240→174 等）是 GCC 16.2.1 口径，WSL2
   g++ 15.2.0 下必须重导出，不得抄录（handoff §2/§7.3）。
7. **"Direct Primitive 测试 / Composition 测试"非仓库注册目标组**：仓库
   只有单套件 target 与 `xmake test` 名称过滤（`xmake test -h` 实测，
   receipts/cmd-03）；§2.3/§2.4 的套件清单是本审计按 handoff §2
   primitive/composition 定义与 C1/SEM-05 killer 归属归纳的，非官方分组。
8. **`--sanitizers=` 与 `--hardened` 未入矩阵**：两者存在于 xmake.lua
   （:27-51, :70-89）但 E5 冻结矩阵未使用；如需可作为补充 leg，本矩阵
   不给未验证的期望值。
