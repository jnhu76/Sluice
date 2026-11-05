# R3 确定性测试的命名变异开关

三个开关都是编译期宏，缺省不定义。BASE 目标不定义任何宏；每个变异目标只多定义一个
宏（逐目标 define 见 `raw/correctness/compile-defines.txt`）。宏所在文件：
`/home/jnhu/sluice-595-e5/r3/worktree/src/async/uring_backend.cpp`。
构建目标定义：`xmake/tests.lua:590-621`。

## M1 — 把 transport 未就绪输入纳入组

- 宏：`SLUICE_R3_MUTANT_BATCH_INCLUDES_NOT_READY_TRANSPORT`
- 位置：`src/async/uring_backend.cpp:525`
- 目标场景：`sq_full_residue`（`tests/uring_ready_batch_deterministic_test.cpp:882`）
- 目标断言：`a batch at SQ-full transport residue accepts nothing`（test:912）
- RED：`raw/correctness/mut1-includes-not-ready-only-sq-full.log`，exit=1

```cpp
#if !defined(SLUICE_R3_MUTANT_BATCH_INCLUDES_NOT_READY_TRANSPORT)
        if (dispatch_retry_remains_locked_()) {
            report.stop = ReadyBatchStop::recovery;
            report.untouched = ready.size();
            return report;
        }
        const std::size_t sq_budget =
            static_cast<std::size_t>(::io_uring_sq_space_left(&ring_state_->ring));
        if (sq_budget == 0) {
            report.stop = ReadyBatchStop::sq_budget;
            report.untouched = ready.size();
            return report;
        }
        const std::size_t limit = std::min({ready.size(), ready_batch_cap, sq_budget});
#else
        const std::size_t limit = std::min(ready.size(), ready_batch_cap);
#endif
```

映射说明：`submit_ready_reads` 的输入由调用者断言为 ready，后端不能观测调用者
ReadyGroup；后端可强制的就绪边界是 transport 残渣 / SQ 预算门。M1 去掉该门，
把「SQ 已满且账本仍有未证明 SQE」当作可接受状态。

## M2 — 丢掉真实 residue，假装已提交

- 宏：`SLUICE_R3_MUTANT_BATCH_RESIDUE_NOT_ADVERTISED`
- 位置：`src/async/uring_backend.cpp:614`
- 目标场景：`partial_submit`（test:948）
- 目标断言：`partial kernel consumption advertises dispatch residue`（test:965）
- RED：`raw/correctness/mut2-residue-dropped-only-partial.log`，exit=1

```cpp
    if (report.accepted != 0)
        note_outstanding();
#if defined(SLUICE_R3_MUTANT_BATCH_RESIDUE_NOT_ADVERTISED)
    advertise = false;
#endif
    if (advertise) {
        report.residue_advertised = true;
        signal_ready_progress();
    }
```

## M3 — 重复发出已消费的 SQE

- 宏：`SLUICE_R3_MUTANT_BATCH_REDISPATCHES_CONSUMED`
- 位置：`src/async/uring_backend.cpp:583`
- 目标场景：`d2_positive`（test:453）
- 目标断言：`a fully consumed group advertises no residue`（test:543）
- RED：`raw/correctness/mut3-redispatch-consumed-only-d2-positive.log`，exit=1

```cpp
                for (std::size_t i = 0; i < dispatchable; ++i) {
                    const detail::RequestReservation emitted{report.keys[i].slot,
                                                             report.keys[i].generation};
                    if (!dispatch_one_locked(emitted)) {
                        break;
                    }
#if defined(SLUICE_R3_MUTANT_BATCH_REDISPATCHES_CONSUMED)
                    dispatch_->push_back(emitted);
#endif
                }
```

`dispatch_one_locked`（`src/async/uring_transport.cpp:130`）已经把该 reservation 的
SQE 写入内核并 claim execution；重新压回 `dispatch_` 后，下一 pass 会看到
「SQE 已发出」的重复项，全消费批次因此仍报告 residue。全量运行的附带 abort 与
RED 见证的区分见 `CORRECTNESS.md` §3/§6。

## 构建/运行命令

```bash
cd /home/jnhu/sluice-595-e5/r3/worktree
xmake build -y uring_ready_batch_deterministic_test \
    uring_ready_batch_det_mut_includes_not_ready \
    uring_ready_batch_det_mut_residue_dropped \
    uring_ready_batch_det_mut_redispatch_consumed

B=./build/linux/x86_64/release
$B/uring_ready_batch_deterministic_test --only=sq_full_residue
$B/uring_ready_batch_det_mut_includes_not_ready --only=sq_full_residue
$B/uring_ready_batch_deterministic_test --only=partial_submit
$B/uring_ready_batch_det_mut_residue_dropped --only=partial_submit
$B/uring_ready_batch_deterministic_test --only=d2_positive
$B/uring_ready_batch_det_mut_redispatch_consumed --only=d2_positive
```

完整命令与退出码见 `commands.txt` 与 `raw/correctness/*.log`。
