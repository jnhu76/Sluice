Round-0 matrix defect: `cancel_race_with_parked_owner_settles_and_leaves_the_fd_quiescent`

Captured at head `6411cff3` (RG5 oracle commit), first full-matrix pass of
this campaign. Both liburing=y cells failed `uring_progress_race_test`
(debug-y 73/74, release-y 73/74, `xmake test` exit 255); the same binary
passed standalone in the debug configuration.

Root cause: test-observability defect in the new RG5 oracle, not a product
defect. The oracle demanded observing `live_control_sqes_for_test() >= 1`
— the control cancel SQE in flight between submission and kernel
completion. Direct reproduction of the failing cell
(`/tmp/rg6-build-release-y` binary, stderr):

    cancel-race parked owner: intent=1 terminal=1 retired=1 woken=0
    (submitted=0 kind=0 completed=1 elapsed=5) quiescent=1 idle=1
    FAIL: cancel_race_with_parked_owner_settles_and_leaves_the_fd_quiescent

Every end-state cell is correct (physical canceled terminal, control
retired, fd quiescent, idle verdict, progress in 5 ms); only the transient
in-flight observation missed, because the release-speed kernel completed
the cancel CQE pair before the observing thread was scheduled. The old
binary failed 5/5 reproductions; the submission-serving interleaving the
dropped observation claimed to pin is already pinned deterministically by
the fiction-kernel control-race oracles (k10a/k10b).

Fix: commit `5fd8e109` — the oracle now discriminates wake delivery from
stranding by the bounded-wait deadline (a stranded owner cannot return
progress before the 8000 ms deadline-fueled recheck, so `elapsed < 4000`
with `completed == 1` is wake-delivered) plus stable end-state
assertions. Post-fix stress: release 10/10, debug 10/10, ASan+UBSan 5/5,
all under concurrent TLC model-checker load. The release matrix was then
re-run in full from the fixed head; `matrix/` holds that final record.

The files in this directory are the failing-cell logs from the pre-fix
run, kept as the record that the matrix caught the defect.
