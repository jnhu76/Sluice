# R1 — Call DAG, identity matrix and complexity ledger

Rebuilt from source at `46157fd1f4a1a4afec22d8834c86462b245b576b` (frozen in
`environment.md`). Line numbers refer to that SHA.

## R1-A. Call DAG and lookup call sites

### Submit path

```
AsyncIoContext::submit_read/write/sync_*/file_info/size
→ UringAsyncBackend::submit_request                     uring_backend.cpp:424
   → validate_op; core_->reserve()                      (RequestCore authority)
   → prepared_ops_[slot] init                           uring_backend.cpp:479
   → {dispatch_mtx_} delivery_[slot] record init        uring_backend.cpp:488
   → core_->accept(reservation, descriptor, borrow)     uring_backend.cpp:514
   → dispatch_after_accept(h)                           uring_transport.cpp:43
      → {dispatch_mtx_}
      → dispatch_->push_back(h)
      → dispatch_one_locked(front)*                     uring_transport.cpp:130
         → io_uring_get_sqe (flush via submit_transport_locked if full)
         → core_->claim_execution                        uring_transport.cpp:152
         → router_slot ← cookie_free_list_.back(); pop   uring_transport.cpp:161  (LIFO)
         → op_cookie ← allocate_cookie_()                uring_transport.cpp:164  (next_cookie_++)
         → io_uring_prep_{read|write|fsync|statx}        uring_transport.cpp:166-187
         → io_uring_sqe_set_data64(sqe, op_cookie)       uring_transport.cpp:189
         → router_[router_slot] ← {cookie,handle,in_use} uring_transport.cpp:190-194
         → transport_ledger_->append(operation, pos, op_cookie, handle) :199
         → dispatch_->remove_exact(h)                    uring_transport.cpp:201
      → submit_transport_locked                          uring_transport.cpp:218
         → io_uring_submit → account_transport_result_locked :237
            rc>0  : pop `rc` ledger entries; cancel_control →
                    mark_consumed_control_locked_        uring_transport.cpp:281
                    → SCAN find_live_router_cookie_(cookie)          [S1]
                    → verify handle + control_state==prepared else terminate
                    → control_state=submitted; live_control_sqes_++
            rc<=0 retryable (EINTR/EAGAIN/EBUSY) or fatal:
                    reconcile_ledger_with_kernel_locked_ uring_transport.cpp:298
                    → same mark_consumed_control per consumed entry  [S1]
                    fatal → poison_and_recover_locked    uring_transport.cpp:337
      → service_pending_controls_locked_                 uring_transport.cpp:695
         full router_ forward scan, gated by pending_control_count_ != 0   [S5]
         → issue_running_cancel_locked_(route.handle)    uring_transport.cpp:620
            → SCAN find_live_router_index_(h)                       [S2]
            → io_uring_prep_cancel64(sqe, target_cookie, 0)
            → user_data ← CONTROL_TAG | target_cookie
            → transport_ledger_->append(cancel_control, pos, target_cookie, h)
```

### Completion path

```
AsyncIoContext::poll                                       async_io_context.cpp:409
→ UringAsyncBackend::poll                                  uring_backend.cpp:616
   → {dispatch_mtx_} dispatch drain + submit + service_pending_controls
   → reap_cqes                                             uring_completion.cpp:200
      → io_uring_peek_batch_cqe(BATCH=32)
      → per CQE (lock taken per CQE):
        handle_one_cqe(user_data, res)                     uring_completion.cpp:99
        → {dispatch_mtx_}
        control CQE (bit 63): target = user_data & COOKIE_MASK
           → SCAN find_live_router_cookie_(target)                   [S3]
           → require control_state==submitted + live_control_sqes_!=0
           → release_control; retire if terminal delivered
        operation CQE:
           → SCAN find_live_router_cookie_(user_data)                [S4]
           → finalize_operation_terminal_                  uring_completion.cpp:23
              → core_->offer_terminal; release_execution
              → publication_pending_->push_back(handle)
              → control_state==none → retire_router_entry_ uring_transport.cpp:559
                 entry zeroed; cookie_free_list_.push_back(index); live_cookies_--
   → overflow branch: io_uring_get_events; reconcile; on failure poison
   → publication pass: publish_one × pass_bound → deliver_event (no lock)
   → owed sweep, gated by owed_count_ != 0 (H2b, productized)
```

### Cancel path

```
AsyncIoContext::cancel → backend_->cancel_identity        async_io_context.cpp:371
→ cancel_key(key)                                         uring_transport.cpp:584
   → {dispatch_mtx_} dispatch_->remove_exact; core_->cancel
   → won_before_execution → release + publication (no scan)
   → requested → issue_running_cancel_locked_
        → SCAN find_live_router_index_(h)                            [S2]
```

### Poison / Class-A recovery path

```
poison_and_recover_locked(error)                           uring_transport.cpp:337
   → retire undispatched dispatch_ entries (no scan)
   → per surviving ledger entry:
       SCAN find_live_router_cookie_(physical.cookie)                [S6]
       → verify handle slot+generation else terminate
       → operation without terminal → failure terminal, release, publish
       → control_state != none → release_control
       → terminal delivered && control none → retire_router_entry_
```

### Scan inventory (lock column: all scans run under `dispatch_mtx_`)

| ID | Site | Kind | Direction | Frequency | Hit behavior | Miss behavior |
|---|---|---|---|---|---|---|
| S1 | `mark_consumed_control_locked_` :283 | by cookie | reverse | once per consumed cancel_control ledger entry (submit-path and reconcile) | verify handle+prepared, mark submitted | terminate (invariant) |
| S2 | `issue_running_cancel_locked_` :624 | by handle | **forward** | once per running cancel (public cancel or pending-control service) | read route, prep cancel64 | return silently (cancel of already-settled request) |
| S3 | `handle_one_cqe` control :105 | by cookie | reverse | once per control CQE | verify submitted, release control | drop CQE, return |
| S4 | `handle_one_cqe` operation :148 | by cookie | reverse | once per operation CQE | finalize terminal | drop CQE (stale/late), return |
| S5 | `service_pending_controls_locked_` :704 | full array | forward | per dispatch/poll pass while pending_control_count_>0 | re-issue per pending route | — |
| S6 | `poison_and_recover_locked` :388 | by cookie | reverse | once per surviving ledger entry at poison | Class-A retirement | terminate (invariant) |

Cold full scans not in the lookup family: `any_event_owed_locked_`
(destructor/`internal_work_retired`), destructor quiescence audit, owed sweep
(counter-gated since PR #565).

### Structural facts that determine scan cost

1. `router_.size() == UringConfig::request_capacity == C` — every scan iterates
   the **capacity array**, not the live set. Misses always cost C iterations.
2. `cookie_free_list_` is LIFO, initialized `[0..C-1]`, popped from the back and
   pushed on retirement — the live working set concentrates at the **high**
   index end (dispatch order C-1, C-2, ...; retired top slots are reused first).
3. `find_live_router_cookie_` scans **in reverse** (from C-1 down): aligned with
   the LIFO live region. Expected hit probes ≈ live-region span / 2, bounded by
   the recent high-water mark of live entries, not by C.
4. `find_live_router_index_` scans **forward** (from 0): **mis**aligned — it
   traverses ~C-D dead entries before reaching the live region. Expected hit
   probes ≈ (C-D) + D/2 at live depth D. This is the only scan whose expected
   cost grows with C even at tiny D.
5. `allocate_cookie_` is a monotonic counter in `[1, 2^63)` — cookies are never
   reused within a backend's lifetime; router slots are reused (LIFO). Stale
   cookies therefore cannot alias a live entry's cookie; identity is exact.
6. `RouterEntry` is 32 bytes (cookie 8, handle = slot u32 + generation u64 +
   padding, state bytes): 2 entries per 64-byte line; C=1024 ⇒ 32 KiB / 512
   lines (confirmed by the archived disassembly stride `add $0x20,%rax`).

### Work-elimination findings (before any secondary index)

- **WE-1: `TransportLedger::Entry` can carry `router_index`.** S1 and S6 run on
  ledger entries that were appended in `dispatch_one_locked`/`issue_running_cancel_locked_`
  where the router index was in hand. In the baseline the cookie is the lookup
  key and the handle is post-verified; under WE-1 the carried index becomes the
  routing hint and the **cookie becomes the post-verified field** (exact 64-bit
  equality retained, handle verification unchanged), which is precisely why
  identity authority survives. Storing the index eliminates the S1 and S6
  scans with no new index structure, no capacity change, no cookie change. The
  ledger already carries `handle` and `cookie` — this adds a derived physical
  position, not a semantic fact.
- **WE-2: reverse the S2 scan direction.** A one-line change aligns the
  by-handle lookup with the LIFO live region, turning its expected cost from
  (C-D)+D/2 into ≈ live-span/2 — same family as the cookie scans. No new state.
- S3/S4 (per-CQE lookups) have no in-hand alternative source: the kernel echoes
  only the 64-bit cookie. Eliminating them requires a mapping structure — that
  is the R3 question, gated by R2.
- S5 is counter-gated and only fires when a cancel SQE could not be allocated
  (SQ ring full); not a steady-state cost.

## R1-B. Identity matrix (baseline, frozen SHA)

| Identity object | Domain / format | Producer | Consumers | Verification |
|---|---|---|---|---|
| Operation cookie (`op_cookie`) | `[1, 2^63)` — last issuable `2^63-1` (guard rejects `>= CONTROL_TAG` pre-increment) — monotonic, never reused, `next_cookie_` starts 1 | `allocate_cookie_` :526 | SQE `user_data` :189; `RouterEntry.cookie` :192; ledger op entries :199; cancel64 target :679 | exact 64-bit equality in every lookup |
| Control cookie | `CONTROL_TAG \| target_cookie`, bit 63 set, `target ∈ [1, 2^63)` | `issue_running_cancel_locked_` :680 | control CQE routing | `is_control_cookie` → strip tag → target cookie lookup + `control_state==submitted` |
| `RouterEntry` (router slot) | index ∈ `[0, C)`, reused LIFO, zeroed on retire :579 | `dispatch_one_locked` install :190; `retire_router_entry_` reset | all lookups | `in_use && cookie ==` (cookie lookups) or `in_use && slot+generation ==` (handle lookups) |
| Request slot + generation (handle) | `RequestReservation{SlotIndex u32, Generation u64}` from RequestCore `reserve()`; generations never recur (`release_slot_` bumps before re-adding) | RequestCore | router entries, ledger, dispatch/publication queues, RequestKey | exact slot+generation pair comparison |
| CQE `user_data` | kernel echo of SQE `user_data`: op cookie or control cookie | kernel | `handle_one_cqe` :99 | bit-63 tag split, then exact cookie lookup; miss ⇒ silent drop |
| Transport ledger entry | {sequence (monotonic), physical SQ position, kind, cookie, handle, class_a flag} | append at dispatch/cancel :199 :691 | submit accounting :250, reconcile :328, poison :377 | sequence monotonicity + physical-position continuity enforced in append/pop |
| Retired router entry | `RouterEntry{}` (cookie=0, in_use=false) + index back on free list | `retire_router_entry_` :559 | subsequent dispatch reuse | a retired entry cannot match: `in_use==false`; its old cookie is unique forever (monotonic counter) |
| Late / stale CQE | `user_data` of a retired or never-live cookie | kernel (late), tests (injected) | `handle_one_cqe` | scan miss ⇒ drop; cannot false-hit because cookies are never reused |
| Cookie exhaustion | `next_cookie_` reaching 0 or ≥ 2^63 | `allocate_cookie_` :530 | process | explicit fail-fast before wrap (domain is a physical obligation, TERM/BOUND) |
| Control-tag reserved range | `[2^63, 2^64)` | `CONTROL_TAG` uring_internal.hpp:45 | cookie allocation guard, CQE routing | cookies must stay below the tag; crossing terminates |

Key separation: **Request slot/generation** identity is owned by RequestCore and
keys the public RequestId; **cookie** identity is owned by the backend and keys
kernel transport. A router entry binds one cookie to one handle for its live
interval. H1's disqualification was narrowing the cookie field; nothing above
depends on any packing.

## R1-C. Complexity ledger

Parameters: `C` = request_capacity (router array size), `D` = live outstanding
(live_cookies_), `H` ≈ recent high-water live span (LIFO keeps live entries
within a top region of ~H slots), `Q` = lookup count, `U` = router install/retire
count, `M` = miss/stale completion count.

| Path | Worst | Expected (steady) | Amortized note |
|---|---|---|---|
| S4 op-CQE lookup (hit) | C iterations (live region at array bottom after pathological churn) | ≈ H/2 + 1 probes, each 1 load + branch on a hot line | ≤ D probes when C==D |
| S3 control-CQE lookup (hit) | C | ≈ H/2 + 1 | same region as S4 |
| S4 miss (stale CQE) | C | C (full sweep, all `in_use==false` rejects) | the only true O(C) hot-path event; requires M>0 |
| S2 cancel lookup (hit) | C | (C-D) + D/2 — **grows with C at fixed D** | misaligned direction (WE-2) |
| S2 miss | C | C | cancel of already-settled request |
| S1 consumed-control lookup | C | ≈ H/2 (target is live by construction; miss terminates) | eliminable by WE-1 (index in ledger entry) |
| S6 poison Class-A lookup | C × ledger size | ≈ H/2 per entry; entries already resolved vs kernel head | eliminable by WE-1 |
| S5 pending-control sweep | C | C per pass while any control is pending | gated by counter; fires only after SQ-full cancel deferral |
| Router install (`dispatch_one_locked`) | O(1) | O(1): free-list pop, entry write, ledger append | one cache line write + ledger line |
| Router retire | O(1) | O(1): zero entry, free-list push | — |
| `remove_exact` (dispatch queue) | O(dispatch size) | O(size) with shift | unrelated to router scan |
| Owed sweep (poll) | O(C) | gated by owed_count_≠0 (zero-op completions only) | already optimized (PR #565) |

Lower-bound observation for R2: at the production-shaped configuration the
harness sets `request_capacity = depth` (bench_w02_pipeline.cpp:108-120,
bench_w03_external_loop.cpp:104-116) — i.e. **C == D** in every standard W-02 /
W-03 run, so expected S3/S4 hit probes are D/2 with D ≤ 64 in the standard
matrix. C ≫ D requires a configuration (large request_capacity with shallow
pipeline) that the standard harness never produces; M2's C=1024/d=N campaign
used a purpose-built research harness, not the production shape.

### Can lookups be deleted or merged without a new index?

Yes, partially — WE-1 removes S1/S6 completely (ledger already carries the
identity triple; the index is a derived physical position captured where it is
known), and WE-2 removes the C-dependence of S2. S3/S4 are irreducible without a
mapping structure because the kernel returns only the cookie. Whether S3/S4
elimination is worth an index is exactly what R2's trigger gate must price.
