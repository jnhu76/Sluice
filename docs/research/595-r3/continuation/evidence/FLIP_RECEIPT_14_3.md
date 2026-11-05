# §14.3 翻转回执：semanticsReady false → true（#595 R3 续跑轮）

回执出具：2026-11-05T14:33Z（`date -u`）。verifier：ZCode（独立验证会话，
非 `BUILD_COMPLETION.md` 的实现者自述）。主预注册：
`r3-completion-20261105T083818Z/evidence/PREREGISTERED_R3_COMPLETION.md`
（sha256 前缀 `1a9c1e12`，冻结）。

## 1. 七项最低内容

### 1.1 verifier 身份/日期

ZCode 独立验证会话（`evidence/correctness/session.log`，锁记录
acquired 2026-11-05T14:30:48Z / released 14:30:51Z）。

### 1.2 被验证对象指纹

| 项 | 值 |
|---|---|
| 冻结 patch（主预注册 §3.2） | `2596f204035f…`（reverse-check rc=0；未改动） |
| worktree HEAD | `0a22a79195f1b913145fcd443496e7f70b08cdcf` |
| `git diff HEAD`（冻结态） | `c1a31a30a9f0…`（15 行 porcelain） |
| `git diff HEAD`（含本轮 bench-only 增量） | `73fa116ff668ea58143ab6666d24684f0eea7f39fcb9e6c03572ae00a7df5560`（16 行 porcelain） |
| 增量 delta | `delta_w03.patch`=`57f21e02…`（统一延迟口径）、`delta_w02.patch`=`4aa7b306…`（W02 R3 臂 + 统一口径） |
| 语义/库二进制 | `libsluice_async.a`=`c7eed8da…`（隔离重建逐位相同）；`uring_ready_batch_deterministic_test`=`c54a9222…`；`research`=`1372b346…`；`fault`=`18395c0a…`；`shutdown_lifecycle_test`=`3ccec804…` —— 全部与 §3.2/BUILD_COMPLETION §8 一致 |
| 计时 bench 二进制（新，增量后） | `bench_w03_external_loop`=`bb142d69788a…`；`bench_w02_pipeline`=`5104ce250e07…`（builddir `/tmp/r3c2-build`） |

增量性质声明：delta 仅触碰两个 bench .cpp；adapter（`bench/r3_ready_batch.hpp`）、
库源、测试源零改动，由"lib 与全部正确性二进制隔离重建逐位相同"证明。
统一延迟口径与 W02 新臂已按所有者授权预先登记于
`PREREG_ADDENDUM_CONTINUATION.md`（计时前冻结）。

### 1.3 正确性（全部持 `MEASUREMENT.lock`，`evidence/correctness/session.log`）

- 26 场景确定性套件（冻结二进制 `c54a9222…`）：聚合 rc=0；且逐场景
  `--only=` 全 26 项 rc=0（场景清单见 session.log 21–46 行）。
- 3 变异体 RED（冻结变异二进制）：`includes_not_ready` rc=1、`residue_dropped`
  rc=1、`redispatch_consumed` rc=134（router exhaustion abort）——与完成轮
  变异签名一致。
- RESTORED GREEN：同源隔离 builddir 重建逐位相同（`c54a9222…`）rc=0。
- 既有回归（冻结二进制）：research/fault/submit_boundary/progress_race/
  flush_count_oracle/external_loop/public_consumer/request_core_integration/
  `shutdown_lifecycle_test` 全部 rc=0（含锁记录）。
- 前后状态一致：HEAD、porcelain、diff sha 会话前后不变。

### 1.4 A0/suffix 回执

`A0_AND_SUFFIX_RECEIPT.md` 存在（本目录），内容含 A0 源码形状（vs baseSha）、
suffix 静态规则、d16 计数对账（entry==flush==10515、suffix==untouched==9160、
1355×8+9160=20000、Σk·counts=60000），证据哈希绑定（`86e08bbd`/`2872c9a1`）。

### 1.5 OPEN-1.4 / OPEN-1.14 处置（引自 BUILD_COMPLETION §11，状态未变）

- **OPEN-1.4**：PROVED（记录窗口）——`have_ring_` atomic；批量 ring 访问全在
  `dispatch_mtx_` 内并锁内重查；close/poison 交错由
  acceptance_barrier/mixed_acceptance/shutdown_accepted/post_poison_single 覆盖。
  残余：A0 基线路径仍可能返回 `backend_error`；未跑 TSan。
- **OPEN-1.14**：PARTIAL——机制界成立（无新 timer/thread/queue/per-request
  heap；栈上 `array<Request,8>`）；RSS/alloc/resource 计量门 NOT_RUN。
  该残余不阻塞语义翻转，但 RESOURCE-B 块仍须按 §10 独立给出。

### 1.6 声明

**`semanticsReady = true`。允许 B/A1 计时。**

适用范围：
1. 主预注册 §14.2 全部激活块（MAIN-W03-D16-A1/B、CROSS-W03-D4/D64、
   MECH-*、PROT-W03-*、PROT-64K/THREADPOOL/SMT/XPHYS、RESOURCE-B），
   以增量 A 的新 bench 二进制（`bb142d69…`）与统一完整请求延迟口径执行；
   §9.2/§9.3/§9.7 门槛阈值不变。
2. `PREREG_ADDENDUM_CONTINUATION.md` §2 登记的 W02R3-D2 块（新 B 臂，二进制
   `5104ce25…`）：只作 B−A0 描述性对照 + 完整请求尾延迟报告，不入 Holm 家族。
3. 不变：PROT-SATURATION-B 仍 NOT_ESTABLISHED（无 saturation 臂）；PRODUCT-604
   仍受 #604 门约束；翻转前 B 数据（无）不作数；翻转后一切计时从 fresh A/A 开始。

### 1.7 自身绑定

本回执 sha256 与全目录清单见 `FLIP_MANIFEST.sha256`（含本文件、prereg 附录、
delta patch、A0/suffix 回执、correctness/ 会话日志与脚本、full diff）。

## 2. 翻转后立即义务（已在计划内）

1. fresh A/A：W03 d16（A0/A0，24 对，新二进制）与 W02 d2（A0/A0，24 对）。
2. W-03 d16 24-triplet（冻结 §6.2(b) 序：`permutations(['A0','A1','B'])` 经
   `random.Random(5950).shuffle`，4 轮，`orders[j%6][k]`）。
3. W02R3-D2：A0/B 交替 24 对。
