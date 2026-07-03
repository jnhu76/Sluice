# Release checklist — v0.1-mvp

**Status: CPPIO-CORE-014B.** Checklist for tagging the blocking cppio-core MVP
as `v0.1-mvp`. Do not tag until every box is verifiable.

## 1. Scope

`v0.1-mvp` is the **blocking, measurable, Zig-`std.Io`-inspired C++ I/O core**.
It is explicitly not async, not io_uring-as-default, and makes no universal
performance claim. The experimental io_uring spike (013) ships as opt-in,
build-gated, and skip-clean-without-liburing.

## 2. Included features

```text
blocking FileReader/FileWriter
Reader/Writer abstractions
buffering wrappers
fault injection
observability
copy strategy
vector I/O
WAL scalar/vector
flush/sync/durability split
IoContext boundary
microbench harness
experimental uring spike
```

## 3. Excluded features

```text
production io_uring backend
async runtime
cancellation model
networking
timers
default backend switch
universal performance claims
```

## 4. Required test commands

```bash
xmake f -m debug
xmake build cppio_core
xmake build -g test
xmake test            # expect: 100% passed, 0 failed (35 tests as of 014)
```

Re-run in release to catch optimization-flagged issues:

```bash
xmake f -m release
xmake build -g test
xmake test
```

## 5. Required example smoke runs

```bash
xmake build -g examples
xmake run mvp_copy_pipeline
xmake run mvp_limited_copy
xmake run mvp_wal_vector
xmake run mvp_copy_strategy
xmake run mvp_wal_durable
xmake run mvp_io_context_copy
```

All must exit 0 and verify their bytes (the MVP examples self-check output).

## 6. Required bench smoke runs

Build-only (no winner claimed):

```bash
xmake f -m release
xmake build -g bench
```

Optional CSV run (local observation, not a claim):

```bash
bash scripts/run_core_microbenches.sh release /tmp/v0.1-mvp-bench.csv
```

## 7. Required doc checks

- `README.md` accurately states current capabilities and MVP status.
- `docs/mvp-closeout.md` says the MVP is complete and io_uring is post-MVP.
- `docs/zig-std-io-parity-audit.md` is honest (no `High` where it should be
  `Partial` — e.g. the `flush` row).
- `docs/optimization-decision-matrix.md` has no universal claims.
- `docs/io-uring-spike.md` states the spike is experimental and non-default.
- Known-limitations blocks are current.

## 8. Known limitations

```text
io_uring remains experimental unless real liburing validation supports promotion.
No production io_uring backend yet.
No async runtime.
No cancellation model.
No networking.
No timers.
No default backend switch.
No universal performance conclusion.
Zig stdlib remains design reference only.
```

## 9. Tagging instructions

Only when sections 4–8 all pass:

```bash
git tag -a v0.1-mvp -m "cppio-core v0.1-mvp: blocking measurable Zig-inspired I/O core"
git push origin v0.1-mvp   # if pushing tags
```

Do **not** tag if any test/example/doc check fails. The tag is a release
boundary, not a checkpoint — once tagged, the MVP API surface is frozen.
