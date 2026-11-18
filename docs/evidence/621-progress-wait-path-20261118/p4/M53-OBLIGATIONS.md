# M53-OBLIGATIONS — M-53 三项迁移义务审计

| 字段 | 值 |
|---|---|
| 日期 | 2026-11-18 |
| BASE | `5d94f233967c3aef993eb19ba9e3252c532b0951`（master） |
| 审计对象 | M-53：cookie O(R) 逆向扫描 → `user_data` 路由索引编码 |
| 审计范围 | 只读 `src/async/uring_transport.cpp`、`src/async/uring_internal.hpp`、`src/async/uring_completion.cpp`、`src/async/uring_backend.cpp`、`include/sluice/async/detail/uring_backend.hpp` 与相关测试；只读 `p2/P2-COST-MAP-R1.md`、`p1/AA-CALIBRATION-2.md`、`docs/research/native-linux-m2/h1-identity-matrix.md` 及 `docs/research/native-linux-m2/patches/h1.patch` |
| 未做 | 没有修改 `src/`/`include/`/`tests/`；没有重新构建；没有运行 d64 A/A 或 A/B；没有运行 H1 candidate patch 的 envelope 测试；没有 kernel trace |
| 结论 | **O1 PROVEN，O2 PROVEN，O3 INCONCLUSIVE → 总体 INCONCLUSIVE**；d64 资格在 0.4% 效应量级上不可行（见 §6） |

---

## 0. 结论摘要

| # | 义务 | 判词 | 一句话依据 |
|---:|---|---|---|
| 1 | TransportLedger 物理位置单调 | **PROVEN** | append 强制 `physical_position == (last+1) % ring_entries`，两个 append 点都在 `io_uring_get_sqe` 后立即用 `(sqe_tail-1)&ring_mask`；reconcile/retire 只弹 FIFO 前缀，保持顺序。反例仅针对“把物理槽位本身当作 user_data 索引”的额外解读：SQ 槽位在 kernel 消费后即可复用，早于 CQE；安全索引应是 router slot。 |
| 2 | 控制 CQE exact-submitted | **PROVEN** | 控制 SQE 消费记账在 `dispatch_mtx_` 内先于任何 CQE 处理；`mark_consumed_control_locked_` 要求 `prepared` + 精确 handle；控制 CQE 要求 `submitted` + `live_control_sqes_ != 0`；bit 63 标签与 `allocate_cookie_` 的 `< CONTROL_TAG` 守卫使控制/数据编码不可能碰撞。 |
| 3 | cancel 定向（编码下） | **INCONCLUSIVE** | 现行 63-bit 单调 cookie 下 cancel 定向是可证的（cookie 不复用、slot+generation 精确匹配、poison 保留 route）；但 M-53 的打包编码（H1：`tag \| slot15 \| cookie48`）**不在 HEAD**：当前 `validate_config_` 仍允许 `request_capacity` 到 `UINT32_MAX`（`uring_backend.cpp:150-159`），`allocate_cookie_` 仍在 `CONTROL_TAG`（2^63）而不是 2^48 处 fail-fast（`uring_transport.cpp:530-537`），`io_uring_prep_cancel64` 仍以 raw cookie 为目标（`:679`）。H1 矩阵/patch 在旧 BASE `8fdcd99c` 上报告 envelope 控制在 E5 7/7 通过，但本次未运行、也未应用到 HEAD。 |

**总体判词**：`INCONCLUSIVE`。O1/O2 的现行实现不变量已经可证；O3 的“打包编码下”部分尚无可执行的 HEAD 证明。即使 O3 关闭，§6 的 d64 预算也表明 17–20 对只对应 δ=2%，若 d64 效应接近 d16 的 0.4%，需要约 178（80% 功效）/346（t 半宽 ≤ 效应/2）对，加上 A/A 至少约 524 对。

---

## 1. 审计口径：三个“索引”不是同一个东西

M-53 的现行实现是 `find_live_router_cookie_` 的逆向 O(R) 扫描：

- `src/async/uring_transport.cpp:551-557`：`for (i = router_.size(); i-- > 0;) if (router_[i].in_use && cookie == cookie) return i;`
- 调用点：`src/async/uring_completion.cpp:105,148`（控制 CQE、操作 CQE），另有 `uring_transport.cpp:283`（消费的控制）、`:388`（poison Class-A）。

M-53 的候选设计（H1）把 **router slot** 编进 `user_data`：bit 63 控制标签，bits [48..62] 为 15-bit router slot，bits [0..47] 为 48-bit cookie；查找时解码 slot 并重新比较完整 identity（`docs/research/native-linux-m2/h1-identity-matrix.md:6-30`；`bench/mechanisms/h1_router_index_bench.cpp:10-14,45-63`）。

本审计必须区分三个位置/索引：

| 名称 | 生产者 | 定义 | 生命周期 | 是否可编进 user_data 作为 CQE 路由索引 |
|---|---|---|---|---|
| **SQ 物理槽位** | kernel/liburing | `(sq.sqe_tail - 1) & sq.ring_mask`（`uring_transport.cpp:196-199,688-692`） | kernel 消费 SQE 后即可复用，**早于**操作完成/CQE | **否**：CQE 未到时槽位可被新 SQE 复用（§2.4 反例） |
| **TransportLedger 逻辑位置** | backend | `(head_ + offset) % capacity_`；append/reconcile/retire 维护的 FIFO 顺序 | 从 append 到 submit 消费；消费即 `pop_front` | **否**：CQE 到达时 ledger entry 已不在；它只用于 submit/control 记账 |
| **Router slot** | backend | `cookie_free_list_.back()`（`uring_transport.cpp:161-162`） | 从 dispatch 到 `retire_router_entry_`；terminal 与 control 都退休后才放回 free list | **是**（H1 编码），安全性来自 router residency + full identity 复核，不是 ledger 物理位置 |

因此，三项义务的正确角色是：

1. **O1** 是 ledger 内部顺序不变量：保证 submit 的 `rc` 前缀记账对应 kernel 消费的前缀，从而控制 SQE 的“已提交”标记落在正确的 ledger entry/route 上。
2. **O2** 是控制 SQE 的提交/完成状态机：`prepared → submitted → none`，以及 bit 63 编码隔离。
3. **O3** 是 router slot 的身份/复用/poison 定向：packed `(slot, cookie)` 在 slot 复用、generation 变化、poison 后仍能唯一命中或安全丢弃。

---

## 2. O1 — TransportLedger 物理位置单调

### 2.1 义务

`append` / `reconcile` / `retire` 是否保证物理槽位单调，使 user_data 编码索引安全。

### 2.2 源码事实

**append 强制连续性。** `TransportLedger::append`（`src/async/uring_internal.hpp:170-191`）：

```cpp
const std::uint32_t expected_physical =
    last_sequence_ == 0
        ? physical_position
        : static_cast<std::uint32_t>(
              (static_cast<std::uint64_t>(last_physical_position_) + 1u) % capacity_);
if (size_ >= capacity_ || physical_position >= capacity_ || next_sequence_ == 0 ||
    next_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
    physical_position != expected_physical || next_sequence_ != last_sequence_ + 1u) {
    ... std::terminate();
}
```

第一次 append 豁免 expected 检查（`last_sequence_ == 0`）；此后每次必须等于上一次 `+1 mod capacity_`，并伴随严格 sequence `next == last+1`。

**两个 append 点都立即取 SQE 物理位置。** `dispatch_one_locked`（`uring_transport.cpp:141-149,196-199`）和 `issue_running_cancel_locked_`（`:645-652,688-692`）都在 `io_uring_get_sqe` 成功后立刻计算：

```cpp
const std::uint32_t physical_position =
    static_cast<std::uint32_t>((sq.sqe_tail - 1u) & sq.ring_mask);
transport_ledger_->append(..., physical_position, ...);
```

`io_uring_get_sqe` 的契约：当 `sqe_tail - khead < ring_entries` 时返回 `sqes[(sqe_tail & ring_mask)]`，然后 `sqe_tail++`（本机 liburing 2.13 头文件 `/usr/include/liburing.h:1917-1928`；`io_uring_load_sq_head` 返回 kernel 的 `khead`，`:1683-1706`）。所以 `(sqe_tail - 1) & ring_mask` 就是刚取得的 SQE 槽位。

**容量一致。** ledger 用 `ring_state_->ring.sq.ring_entries` 构造（`uring_backend.cpp:177-178`）。io_uring 的 `ring_entries` 是 2 的幂，`ring_mask = ring_entries - 1`，故 `% capacity_` 与 `& ring_mask` 同值。

**reconcile/retire 只弹前缀。** `account_transport_result_locked` 在 `rc > 0` 时弹前 `rc` 项（`uring_transport.cpp:242-256`）；`reconcile_ledger_with_kernel_locked_` 弹前 `size - io_uring_sq_ready` 项（`:298-335`）；`pop_front` 检查 `out.sequence == retired_prefix_sequence_ + 1`（`uring_internal.hpp:193-211`），不跳过、不重排。

**没有非 ledger 的 SQE。** 全仓 `io_uring_get_sqe` 只有三处：`dispatch_one_locked`、`issue_running_cancel_locked_`、构造期 `probe_statx_support_`（`uring_backend.cpp:240-247`）。probe 在 `have_ring_ = true` 之前完成 `submit_and_wait` 并 `cqe_seen`，所以 ledger 启用时它已消费。`dispatch_one_locked` 的 claim 失败路径在**任何** append 之前回滚 `--sq.sqe_tail`（`uring_transport.cpp:151-158`），不会留下未记账的 SQE。

### 2.3 归纳证明

不变量：在 `dispatch_mtx_` 内、每次 submit/reconcile 之前，

```text
ledger.size() == io_uring_sq_ready(ring) == sqe_tail - khead
```

且 ledger 的第 i 项对应 `khead + i` 这个物理 SQE（模 `ring_entries`）。

- **Base**：构造后 probe 已消费，`ledger.size() == 0 == sqe_tail - khead`。
- **Append**：`io_uring_get_sqe` 使 `sqe_tail++`；append 使 `size++`；`khead` 不变。等式两边同时 +1，物理位置 = `(sqe_tail-1)&mask`，与上一项连续。
- **Submit 成功**：liburing 的 `__io_uring_flush_sq` 返回 `tail - khead`（liburing 2.13 `queue.c:203-219`），即提交前的 `sq_ready`；`io_uring_enter` 消费 c 个并推进 `khead`，`io_uring_submit` 返回 c（`queue.c:422-441`）。ledger 弹前 c 项，两边同时 -c。
- **Submit 失败**：`reconcile` 用 `size - sq_ready` 弹前 c 项；若 kernel 部分消费 c，`khead` 已推进 c，等式恢复；若未消费，c=0。
- **Rollback**：claim 失败时 `sqe_tail--`，`size`/`khead` 不变，等式在 append 前恢复。
- **Retire**：`pop_front` 弹最老项；剩余项仍是物理位置的连续后缀。

因此物理槽位在 ledger 生命周期内严格连续（模容量），且 reconcile/retire 不破坏该顺序。**O1 的 ledger 单调性成立。**

### 2.4 反例（针对更强解读）与限制

**反例：物理槽位不是稳定的 CQE 路由索引。** 设 ring depth 4：

1. 请求 A 取得物理槽位 3，提交；kernel 消费 A，`khead` 前进，A 的 CQE 尚未 reap。
2. 之后再有 4 次 dispatch，`sqe_tail` 绕回，新的物理槽位 3 被请求 B 使用；B 的 `user_data` 若只编码物理槽位，会与 A 相同。
3. ledger 的 append 检查全部通过（3 → 0 → 1 → 2 → 3），`last_physical_position_` 也保持连续；**单调性不能阻止槽位复用**。
4. A 的 CQE 到达时，编码索引指向 B；若不做完整 identity 复核，会把 A 的结果错交给 B；若做复核，A 的 live CQE 会被当 miss 丢弃，A 永远不完成。

所以“物理位置单调 ⇒ user_data 编码索引安全”**不成立**。ledger 的实际作用是 §2.3 的前缀记账；CQE 路由索引必须用 **router slot**（H1 设计），其安全性来自：

- router slot 只在 `retire_router_entry_` 放回 free list；该函数要求 `control_state == none`（`uring_transport.cpp:567-579`）；
- 操作终止只有在 `control_state == none` 时才退休（`uring_completion.cpp:94-96`）；有未完成控制时 route 保留（`:139-141`）；
- 控制 CQE 先到则只清 control，不产生 terminal；操作 CQE 先到则 terminal 已交付但 route 因 control 未清而保留（`uring_completion.cpp:117-142`）；
- 因此原始操作 CQE 到达前 router slot 不会被复用；复用后旧 identity 由 cookies 不复用 + 完整比较拒绝。

**限制/脆弱点：**

- 第一次 append 的 `physical_position` 不做 expected 检查（`last_sequence_ == 0` 分支，`uring_internal.hpp:172-176`），依赖调用点的公式。若未来新增第三个 append 点，必须复制 `(sqe_tail-1)&ring_mask`。
- `Entry` 已不保存 `physical_position`（commit `b4b8b04b`，`#609`）；单调性由 append + FIFO 弹出维持，不能在 pop 时独立复核。
- 该证明依赖 liburing `io_uring_get_sqe` 的 `khead` 空间检查（`/usr/include/liburing.h:1917-1928`）。若换成 `IORING_SETUP_SQPOLL` 或非标准 submit 路径，必须重证（当前 `io_uring_queue_init(..., 0)`，`uring_backend.cpp:171`）。

### 2.5 O1 测试证据（本次运行）

- `backend_contract_conformance_uring_test` 的 `full_ring_zero_consumed_hard_error_settles_every_entry`（`tests/backend_contract_conformance_test.cpp:1917-1970`）验证满 ring（size 8、sq_ready 8）在零消费硬错误下的 ledger 基数；
- `wrapped_head_partial_consumption_keeps_logical_cardinality`（`:1973-2055`）验证 head 绕回后 3 个 pending entries 与部分消费前缀；
- `hard_submit_failure_resolves_per_entry_kernel_visibility`（`:1800-1866`）验证部分提交后可见前缀保留真实 CQE、不可见后缀 poison；
- 本次运行输出：`all 20 io_uring backend conformance tests passed`，exit 0。

**O1 判词：PROVEN（ledger 物理位置单调）**。记录反例：若把物理槽位本身编码为 CQE 索引，该设计不成立；M-53 必须编码 router slot。

---

## 3. O2 — 控制 CQE exact-submitted

### 3.1 义务

控制 SQE 的提交/完成记账是否精确；控制 CQE 与数据 CQE 的编码不会碰撞。

### 3.2 状态机与源码坐标

**发出控制 SQE。** `issue_running_cancel_locked_`（`src/async/uring_transport.cpp:620-693`）：

- 只对 live route 操作（`:624-627`）；
- 已有 `control_state != none` 则返回，保证每个 route 同时最多一个控制（`:628-634`）；
- 目标必须是 `route.cookie`，且 `!= 0 && < CONTROL_TAG`（`:636-643`）；
- `io_uring_prep_cancel64(sqe, target_cookie, 0)`（`:679`）；
- `io_uring_sqe_set_data64(sqe, make_control_cookie(target_cookie))`（`:680`）；
- `route.control_state = prepared`（`:681`）；
- `transport_ledger_->append(Kind::cancel_control, physical_position, target_cookie, h)`（`:688-692`）。

**提交消费记账。** `account_transport_result_locked` 在 `rc > 0` 时弹前 `rc` 项；对每个 `cancel_control` 调 `mark_consumed_control_locked_`（`uring_transport.cpp:250-255`）；`reconcile` 对每个消费前缀项做同样处理（`:328-333`）。`mark_consumed_control_locked_`（`:281-296`）：

- 用精确 cookie 找到 live route（`find_live_router_cookie_`，`:551-557`）；
- 要求 handle 的 slot/generation 完全相等，且 `control_state == prepared`（`:284-287`）；
- 否则 `std::terminate()`（不静默错标）；
- 成功则 `control_state = submitted`（`:294`）、`live_control_sqes_++`（`:295`）。

**控制 CQE。** `handle_one_cqe` 控制分支（`src/async/uring_completion.cpp:99-144`）：

- `is_control_cookie(user_data)` 以 bit 63 判定（`uring_internal.hpp:48-50`）；
- `target_cookie = control_target_cookie(user_data)` 去掉 bit 63（`:56-58`）；
- target 0 直接 drop（`uring_completion.cpp:103-104`）；
- `find_live_router_cookie_` 精确查找；miss 直接 drop（`:105-107`）；
- **exact-submitted 校验**：要求 `control_state == submitted` 且 `live_control_sqes_ != 0`，否则 terminate（`:110-116`）；
- 清 `control_state = none`、`live_control_sqes_--`、`release_control`（`:117-122`）；
- 只有 `terminal_delivered` 时才 retire route（`:139-141`）。

**串行化。** 提交记账在 `submit_transport_locked` 内、持有 `dispatch_mtx_`（`uring_transport.cpp:218-235`）；`handle_one_cqe` 进入时取同一 `dispatch_mtx_`（`uring_completion.cpp:99-100`）。kernel 可能已把控制 CQE 放入 CQ，但用户态在提交记账完成前不会处理它，所以 `submitted` 一定先于 CQE 被观察。

**编码不碰撞。** 操作 cookie 域是 `[1, 2^63)`：

- `allocate_cookie_` 在 `next_cookie_ == 0 || next_cookie_ >= CONTROL_TAG` 时 terminate（`uring_transport.cpp:526-538`）；
- `CONTROL_TAG = 1<<63`，`COOKIE_MASK = CONTROL_TAG-1`（`uring_internal.hpp:45-46`）；
- `make_control_cookie` 只 OR bit 63（`:52-54`），`control_target_cookie` 只 mask bit 63（`:56-58`）；
- 操作 SQE 的 user_data 是 raw cookie（`:189`），故 bit 63 恒 0；控制 SQE/CQE 的 bit 63 恒 1；`user_data == 0` 被 drop（`uring_completion.cpp:145-146`）。不存在控制 cookie 等于数据 cookie 的编码。

H1 打包形式仍保留 bit 63：15-bit slot + 48-bit cookie = 63 bits，operation identity `< 2^63`；控制 identity = `CONTROL_TAG | operation identity`（`docs/research/native-linux-m2/h1-identity-matrix.md:11-30`）。因此 O2 的“编码不碰撞”在打包形式下仍成立（slot 解码正确性另属 O1/O3）。

### 3.3 反例检查（均不成立或为 fail-fast）

| 假设反例 | 结果 |
|---|---|
| 重复控制 CQE | 第一次把 `control_state` 清 none；第二次要么 route 已 retire → miss drop，要么仍 live → `submitted` 检查失败 → terminate。不会二次 release/二次发布。 |
| 错误 target 的控制 CQE（指向另一个 live route） | 若该 route 没有 `submitted` 控制 → terminate（拒绝伪造）；若指向非 live → miss drop。不会误释放。 |
| 控制 CQE 先于提交记账 | 同一 `dispatch_mtx_` 串行化；CQE 处理在 submit 返回后，`submitted` 已置位。若状态不符则 terminate。 |
| 操作 CQE 被当作控制 CQE | 只有操作 cookie 的 bit 63 为 1 才可能；`allocate_cookie_` 禁止。 |
| 控制 SQE 已被 kernel 消费但 ledger 未标记 | O1 的前缀记账保证 `rc`/reconcile 弹出的是 kernel 消费前缀；若计数不符有 `consumed > size` / `unconsumed > size` terminate 守卫（`uring_transport.cpp:244-249,320-326`）。 |

### 3.4 O2 测试证据（本次运行）

- `tests/uring_pending_control_parity_test.cpp`：`sticky_lifecycle_oracle`（`:133-191`）、`repeated_cancel_oracle`（`:195-228`）、`retire_and_slot_reuse_oracle`（`:233-278`）、`concurrent_cancel_storm`（`:310-395`）、`poison_with_pending_intent`（`:401-452`）、`shutdown_after_pending_intent_settles`（`:457-493`）。该套件用 `pending_control_snapshot_for_test()`（`src/async/uring_test_seams.hpp:101-118`）在同一锁快照内比较 derived count 与权威 router 扫描。
- `tests/backend_contract_conformance_test.cpp:1689-1740`（`canceled_metadata_op...`）、`:1750-1800`（`success_and_cancel_converge...`）、`:1816-2048`（ledger 满/绕回）。
- `tests/uring_request_core_integration_test.cpp:431-481`（original CQE first）、`:483-537`（control CQE first）、`:539-629`（cancel racing completion）、`:992-1045`（poison + running cancel）。
- 本次运行输出：pending-control parity 7/7、conformance 20/20、integration 21/21，均 exit 0。

**O2 判词：PROVEN。**

---

## 4. O3 — cancel 定向

### 4.1 义务

cancel CQE/请求在编码下能被唯一定向（generation / slot reuse / poison）。

### 4.2 现行 raw-cookie 编码下的证明

现行编码是 `user_data = op_cookie`（`uring_transport.cpp:189`），控制为 `CONTROL_TAG | op_cookie`（`:680`）。

1. **请求定向**：`cancel_key` 在 `dispatch_mtx_` 内先 `dispatch_->remove_exact(h)`，再 `core_->cancel(key)`（`uring_transport.cpp:584-599`）。只有 core 返回 `requested` 才 `issue_running_cancel_locked_`；`won_before_execution` 走 release/publication，不发控制（`:591-596`）。`issue_running_cancel_locked_` 用 `find_live_router_index_(h)` 以 **slot+generation 精确**找 route（`:624`；`find_live_router_index_` 在 `:540-549`）。
2. **kernel 目标唯一**：`io_uring_prep_cancel64(sqe, target_cookie, 0)` 的 target 是 `route.cookie`（`:636,679`）。cookie 由 `allocate_cookie_` 单调分配、从不复用（`:526-537`），`RouterEntry` retire 时清零并把 slot 放回 free list（`:559-582`）；新请求复用了 slot 但得到新 cookie。因此 kernel 按 `user_data` 精确匹配时，旧 cancel 不可能命中新请求。
3. **slot reuse**：route 在 `terminal_delivered` 且 `control_state == none` 时才退休（`uring_transport.cpp:567-579`；`uring_completion.cpp:94-96,139-141`）。控制未完成时 route 保留，所以 cancel 目标不会在控制 CQE 前被复用。
4. **generation**：旧 generation 的取消由 core 先判 `not_found`/非 running；`find_live_router_index_` 的 slot+generation 比较是第二道；`mark_consumed_control_locked_` 的 handle 比较是第三道（`:284-287`）。
5. **poison**：`reconcile` 把 kernel 已消费的前缀移出 ledger；`poison_and_recover_locked` 只处理剩余（kernel 不可见）entry（`uring_transport.cpp:298-335,337-443`）。未消费控制被清除/释放，可能退休；poison 后 `fatal_error_` 门控所有 submit（`:219,621-622`；`poll`/`dispatch_after_accept` 检查），且 overflow flush 的 `io_uring_get_events` 不提交 SQE，故不会有事后控制 CQE。已消费控制的 route 保留 `submitted`，其 CQE 仍能精确命中。
6. **控制 CQE 不伪造 terminal**：控制分支只 release control，不写操作 outcome（`uring_completion.cpp:117-122`；`SLUICE_URING_CORE_MUTANT_CANCEL_CQE_AS_ORIGINAL_TERMINAL` 是 mutant，不是生产路径，`:123-135`）。操作结果只能由操作 CQE/poison Class-A 产生。

现行测试覆盖：`cancel_cqe_first_never_becomes_the_terminal`（`tests/uring_request_core_integration_test.cpp:483-537`）、`cancel_racing_completion_keeps_the_confirmed_count`（`:539-601`）、`stale_cookie_cannot_reach_a_reused_slot`（`:631-700`）、`real_kernel_slot_reuse`（`:1269-1310`）、`poison_with_running_cancel_keeps_the_real_completion`（`:992-1045`）；本次 integration 21/21 exit 0。

**结论：现行 raw-cookie 编码下的 cancel 定向是可证的。**

### 4.3 打包编码（M-53/H1）下的缺口

H1 的编码是 `user_data = bit63 tag | slot15 | cookie48`，cancel 目标也必须是 packed identity（`docs/research/native-linux-m2/h1-identity-matrix.md:15-30`，行 1/5/6/7；`bench/mechanisms/h1_router_index_bench.cpp:10-14,45-63,109-116`）。要把它变成 HEAD 上的可证义务，至少需要：

| # | 要求 | HEAD 现状 |
|---:|---|---|
| 1 | `request_capacity <= 32768` 校验（15-bit slot） | **缺失**：`validate_config_` 允许到 `UINT32_MAX`（`src/async/uring_backend.cpp:150-159`） |
| 2 | cookie horizon 在 2^48 fail-fast | **缺失**：`allocate_cookie_` 仍在 `CONTROL_TAG`（2^63）处 terminate（`src/async/uring_transport.cpp:530-537`） |
| 3 | operation SQE user_data = packed identity | **缺失**：`io_uring_sqe_set_data64(sqe, op_cookie)`（`:189`） |
| 4 | `RouterEntry.cookie` 与 ledger entry.cookie 存 packed identity | **缺失**：存 raw cookie（`:192,199,691`） |
| 5 | `io_uring_prep_cancel64` target = packed identity | **缺失**：target = `route.cookie`（raw）（`:636,679`） |
| 6 | 控制 SQE/CQE = `tag \| packed identity` | **缺失**：`make_control_cookie(target_cookie)`，target 是 raw cookie（`:680`） |
| 7 | `find_live_router_cookie_` 解码 slot + 完整 identity 复核 | **缺失**：仍为 O(R) 逆向 raw-cookie 扫描（`:551-557`） |
| 8 | `mark_consumed_control_locked_` / poison / 两个 CQE 路径统一解码 | 仍按 raw cookie 查找（`:283,388`；`uring_completion.cpp:105,148`） |

没有 #1/#2，packed identity 的 slot 域会被 cookie 的高位污染（`slot | (cookie >> 48)`），`find_live_router_cookie_` 的 slot 解码会 miss，live CQE 被静默丢弃；没有 #5/#6，cancel 目标与操作 SQE 的 user_data 不相等，`IORING_OP_ASYNC_CANCEL` 会匹配失败（-ENOENT）或无法释放控制引用，route 被 pin 到 shutdown。因此 **“在编码下”的 cancel 定向在 HEAD 上没有实现，也没有 HEAD 可执行证据。**

### 4.4 归档证据的定位

- `docs/research/native-linux-m2/h1-identity-matrix.md` 记录 H1 candidate 的 16 行 producer/consumer/verification 矩阵，并列出 7 个 envelope controls（`:49-68`）；`:30` 注明 `request_capacity` 上限 32768、cookie horizon 2^48。
- `docs/research/native-linux-m2/patches/h1.patch` 是 baseline `8fdcd99c` 的 candidate patch，包含 `OPERATION_COOKIE_LIMIT = 1<<48`、`ROUTER_SLOT_CAPACITY = 1<<15`、`make_operation_identity`、`find_live_router_cookie_` 直接解码等改动。
- 该矩阵称 envelope controls 在 E5 上 7/7 通过（`:49-68`），但**本次没有运行**该 patch 的测试，也**没有把它应用到 HEAD `5d94f233`**。它只能作为设计/历史证据，不能作为 HEAD 的可执行证明。

**O3 判词：INCONCLUSIVE（编码下）；现行 raw-cookie 定向 PROVEN，但 raw-cookie 不是 M-53 的编码。** 若 owner 采用 H1 打包，实现 #1–#8 并在 HEAD 上重跑 envelope controls + 现有 `uring_*` 套件后，O3 才可改判 PROVEN。若 owner 改而编码 **SQ 物理槽位**，§2.4 的反例使其直接 DISPROVEN。

---

## 5. 总体判词

| 义务 | 判词 | 是否阻塞 M-53 |
|---|---|---|
| O1 ledger 物理位置单调 | PROVEN | 否（但必须编码 router slot，不是物理槽位） |
| O2 控制 CQE exact-submitted | PROVEN | 否 |
| O3 编码下 cancel 定向 | INCONCLUSIVE | 是：packed 编码不在 HEAD，缺少 2^48/32768 守卫与全消费者更新 |

**总体：INCONCLUSIVE。** 建议 ledger 行表述为：

> O1/O2 已在 HEAD `5d94f233` 上证；O3 的 packed 形式未实现/未在 HEAD 验证。M-53 仍不具备 GO 资格；即使 O3 关闭，d64 资格预算在 0.4% 效应量级需要约 178/346 对（§6），17–20 对只覆盖 δ=2%。

---

## 6. d64 资格预算修正

### 6.1 输入

- **d16 数字**（`p2/P2-COST-MAP-R1.md:499-511`）：P-A d16 `find_live_router_cookie_` leaf self **34/8,369 = 0.406%**（Wilson 95% CI 0.291–0.567%），inclusive 0.59%，≈61.3 cyc/op；P-C d16 **47/16,472 = 0.285%**（CI 0.215–0.379%），≈58.0 cyc/op；P-B d1 = 0；P-D/P-E 无 uring。d64 **NOT_OBSERVABLE**；P1-A 历史 d64 1.74%/≈116 cyc/op 只作线索（`P2-COST-MAP-R1.md:509,599`）。
- **d64 带**（`p1/AA-CALIBRATION-2.md:222-238,397-408,485-506`）：C7 `W02-uring-4k-d64` 的 cycles/task 配对差 SD = **1.889 pp**（n=16），bootstrap hw **0.893%**，t-hw **1.007%**，MDE80 **1.416%**；判词 **ENVIRONMENT_BLOCKED**（CPU/wall；CPU A/A 均值 +5.04% 不正），cycles 仅“条件性诊断带”，用前必须 cycles-only A/A。C7 CPU SD 9.782、MDE80 7.331%，不可用于 M-53。

### 6.2 公式与复算

沿用 P1/P2 的 t 公式（`p1/AA-CALIBRATION-2.md:57-61,427-433`；实现见 `tools/recompute_independent.py:228-260`）：

```text
n80(δ)   = min n: (t_{0.975,n-1} + t_{0.80,n-1}) * SD / sqrt(n) <= δ
n_half(δ)= min n: t_{0.975,n-1} * SD / sqrt(n) <= δ/2
```

本次用 `python3 -I` 导入同一归档实现，SD=1.889，复算结果：

| δ | n80（80% 功效） | n_half（95% t 半宽 ≤ δ/2） | 备注 |
|---:|---:|---:|---|
| 5% | 4 | 5 | 仅对照 |
| 3% | 6 | 9 | — |
| **2%** | **10** | **17** | 16 对 t-hw 1.007% > 1.0%，**差一对**；建议 17–20 对 A/A |
| 1.2% | 22 | 41 | 36 对 80% 够、hw≤0.6% 不够（需 41） |
| **1%** | **30** | **58** | 36 对 hw 0.639% > 0.5%，**不够**；A/A 与 A/B 各 58 对 |
| 0.5% | 114 | 222 | 不现实 |
| **0.406%**（P-A d16） | **172** | **336** | 与 `P3-ADMISSION-R1.md:52,218` 一致 |
| **0.4%**（约 0.4%） | **178** | **346** | 0.4 比 0.406 略小，取整更多 |
| **0.285%**（P-C d16） | **347** | **678** | — |
| 1.74%（P1-A d64 线索） | 12 | 21 | 线索，不是本轮实测；P2 未复现 d64 |

关键 16 对事实：`t_hw(16)=1.0066%`、`MDE80(16)=1.4157%`；`t_hw(17)=0.9712%`、`t_hw(20)=0.8841%`、`t_hw(58)=0.4967%`。所以 **17–20 对只满足 δ=2% 的“半宽 ≤ 1%”**；δ=1% 需要 58 对才能 hw ≤ 0.5%。

### 6.3 若 d64 效应 ≈ 0.4% 需要多少对

按同一 C7 cycles SD=1.889 的**假设**（不是实测 d64 效应）：

- δ=0.4%：**n80 = 178 对**（80% 功效）；**n_half = 346 对**（95% t 半宽 ≤ 0.2%）。
- 若用 d16 P-A 的点估计 0.406%：**172 / 336 对**。
- 若用 d16 P-C 的 0.285%：**347 / 678 对**。
- 资格预算：ledger 的 M-53 判据是“d64 A/A 半宽 ≤ 效应/2”。若 A/A 与 A/B 是两组独立配对，**最低总数 = 346（A/A）+ 178（A/B）= 524 对**；若 A/B 也要求 CI 半宽 ≤ δ/2，则 **346 + 346 = 692 对**。相比 17–20 对，这是约 **26–39×** 的预算。
- 对照：若 δ=2%，A/A 17–20 + A/B 10–17（建议 17–20），合计约 34–40 对；若 δ=1%，A/A 58 + A/B 30（或 58 若 A/B 也要 hw），合计 88–116 对。

### 6.4 读法与限制

1. **0.4% 不是 d64 实测效应。** P2 R1 明确 d64 NOT_OBSERVABLE；0.406%/0.285% 是 d16 leaf self 份额。把它们当 d64 的计划 δ 是**乐观代理/上界假设**，不是测量结论。若 d64 效应更大（例如历史线索 1.74%），预算会降到 12/21 对；但该线索未被 P2 复现，不能直接用于预注册。
2. **C7 cycles 带是条件性的。** C7 的 CPU/wall `ENVIRONMENT_BLOCKED`，A/A 均值不正；cycles 是唯一居中的指标（mean −0.489%，bootstrap CI 含 0），但 16 对 t-hw 1.007% 刚好超过 2%/2=1.0%。用前必须做 cycles-only A/A（17–20 对）并冻结 SD/hw。
3. **SD 不可跨格搬运。** 1.889 是 C7 d64 的 cycles 配对差 SD；若实际 d64 运行/负载改变，必须用新 A/A 的 SD 重算 n80/n_half，不能直接套 178/346。
4. **C7 CPU 不可用于 M-53。** CPU SD 9.782、n_half(2%)=370，当前环境 NOT_POWERED；不要用 CPU 指标降低预算。
5. **预算不含失败重跑/多重比较/环境复跑。** 实际预算应向上取整并留余量（AA-2:433）。
6. **结论**：17–20 对只在 δ=2% 成立；δ=1% 需 58 对；若 d64 效应接近 d16 的 0.4%，资格所需为 **178/346 对**（0.406% 时 172/336），A/A+A/B 至少约 **524 对**。在这个效应量级上，M-53 的 d64 资格不具可行性；应先证明 d64 效应显著大于 0.4%，或改指标/改设计，而不是按 17–20 对排 A/B。

---

## 7. 本次执行的检查

| 检查 | 命令 | 结果 |
|---|---|---|
| Pending-control parity | `./build/linux/x86_64/release/uring_pending_control_parity_test` | exit 0；7/7 `ok`，`all uring pending-control parity tests passed` |
| Uring conformance | `./build/linux/x86_64/release/backend_contract_conformance_uring_test` | exit 0；20/20 `ok`，`all 20 io_uring backend conformance tests passed` |
| Uring integration | `./build/linux/x86_64/release/uring_request_core_integration_test` | exit 0；21/21 通过，`0 not run` |
| 工作树源码干净 | `git diff --quiet HEAD -- src include tests; echo $?` | `0`（无差异） |
| d64 预算复算 | `cd docs/evidence/621-progress-wait-path-20261118/tools && python3 -I` 导入 `recompute_independent.py`，调用 `required_n_power`/`required_n_halfwidth`（SD=1.889） | 输出见 §6.2：2%→10/17；1%→30/58；0.406%→172/336；0.4%→178/346；0.285%→347/678 |

**未执行**：

- 没有重新构建；三个测试二进制是 `build/linux/x86_64/release/` 中 2026-11-12 21:42 的已有产物。源码 mtime 为 2026-11-12 12:45，且 `git diff` 为空，但本次没有 clean rebuild。
- 没有运行 d64 A/A、A/B、perf/bench。
- 没有运行 H1 candidate patch 的 `tests/uring_identity_envelope_test.cpp`，也没有把 `h1.patch` 应用到 HEAD。
- 没有 kernel trace 验证 `IORING_OP_ASYNC_CANCEL` 的 user_data 匹配；O3 的 kernel 匹配结论来自 liburing/io_uring 接口契约与源码，不是本机 trace。

---

## 8. 本次读取的源码/证据

**生产源码**

- `src/async/uring_internal.hpp:45-58`（control tag、mask、make/mask helpers）、`:146-240`（TransportLedger：append 170-191、pop 193-211、at 213-221、members 231-239）。
- `src/async/uring_transport.cpp:43-102`（dispatch_after_accept）、`:130-209`（dispatch_one_locked）、`:218-235`（submit）、`:237-279`（account）、`:281-296`（mark consumed control）、`:298-335`（reconcile）、`:337-524`（poison）、`:526-538`（allocate_cookie_）、`:540-557`（router lookups）、`:559-582`（retire）、`:584-618`（cancel_key）、`:620-693`（issue_running_cancel）、`:695-711`（service pending controls）。
- `src/async/uring_completion.cpp:23-97`（finalize）、`:99-144`（control CQE、exact-submitted 110-116）、`:145-198`（data CQE）、`:200-278`（reap/overflow）。
- `src/async/uring_backend.cpp:139-160`（config validation）、`:171-178`（ring init、ledger construction）、`:340-373`（quiescence）、`:616-689`（poll；当前行号见文件）。
- `include/sluice/async/detail/uring_backend.hpp:178-191`（RouterEntry）、`:206-208`（TransportLedger）、`:304-326`（counters）。
- `src/async/uring_test_seams.hpp:83-118`（inject_cqe、pending snapshot）。

**测试**

- `tests/uring_pending_control_parity_test.cpp:42-51`（submit fiction）、`:133-191,195-228,233-278,310-395,401-452,457-493`。
- `tests/backend_contract_conformance_test.cpp:1569-1595`（fiction_submit）、`:1689-1740`（control target）、`:1750-1800`（success+cancel）、`:1800-1866`（partial submit visibility）、`:1917-1970`（full ring）、`:1973-2055`（wrapped head）。
- `tests/uring_request_core_integration_test.cpp:431-481,483-537,539-629,631-700,992-1045,1269-1310`。

**文档/研究证据**

- `docs/evidence/621-progress-wait-path-20261118/p2/P2-COST-MAP-R1.md:499-511,599`（M-53 d16/d64）。
- `docs/evidence/621-progress-wait-path-20261118/p1/AA-CALIBRATION-2.md:27-28,57-61,222-238,397-408,485-506`（C7 d64 带与 M-53 预算）。
- `docs/evidence/621-progress-wait-path-20261118/p3/P3-ADMISSION-R1.md:52,218,294-310`（17–20 与 0.4% 预算的既有表述）。
- `docs/research/native-linux-m2/h1-identity-matrix.md:6-30,32-47,49-79`（H1 编码、消费者矩阵、envelope controls、限制）。
- `docs/research/native-linux-m2/patches/h1.patch`（H1 candidate patch，baseline `8fdcd99c`；未应用）。
- `bench/mechanisms/h1_router_index_bench.cpp:1-14,45-63,109-116,236-270`（router slot 编码与 negative control）。
- liburing 2.13：`/usr/include/liburing.h:1683-1706,1917-1928`；`src/queue.c`（本次从 `liburing-2.13` tag 取得，`__io_uring_flush_sq:203-219`、`__io_uring_submit:422-441`、`io_uring_submit:453-456`）。

---

## 9. 剩余限制

1. **O1 的物理单调证明依赖 liburing `io_uring_get_sqe` 的 `khead` 检查**；当前 flags=0、非 SQPOLL。若未来引入 SQPOLL 或自定义 submit，本证明不自动成立（issue #621 正文也提示 SQPOLL 会改变 rc/可见性假设）。
2. **O1 的第一次 append 不校验 expected 位置**；只有两个 append 点，均正确使用 `(sqe_tail-1)&ring_mask`。新增 append 点必须遵守同一公式。
3. **O2 的 exact-submitted 是 fail-fast 守卫，不是恢复机制**；任何状态不符会 terminate。这是现行 invariant 契约；若 owner 要求可恢复，需要新的错误路径设计。
4. **O3 的 packed 编码未在 HEAD 实现**；H1 matrix/patch 是旧 baseline 的候选证据，本次未运行。不能把 H1 matrix 的 E5 7/7 当作 HEAD 的测试通过。
5. **d64 效应未测**；178/346 对是“若 d64 效应 = 0.4% 且 SD 复现 1.889”的算术下限，不是 M-53 的真实效应估计。C7 cycles 是 `ENVIRONMENT_BLOCKED` 下的条件带，使用前必须 cycles-only A/A。
6. **没有 kernel trace**：cancel64 的 user_data 匹配、control CQE 的 kernel echo 都由接口契约和状态机证明，不是本机观测。
7. **测试二进制未重建**：三个测试通过是对 2026-11-12 已有 release 二进制 + 干净源码树的证据；不是本次 clean build 的证据。

---

*本文件结束。*
