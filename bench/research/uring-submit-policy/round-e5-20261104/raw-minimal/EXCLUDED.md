# EXCLUDED.md — what stays only in the full E5 archive, and why

Layer-B principle: keep everything an independent reviewer needs to recompute
the scientific decision; leave bulk deep-provenance in the full archive. The
full archive (`/home/jnhu/sluice-595-e5/evidence/`, manifest `MANIFEST.sha256`
here, backup tarball per `FULL_ARCHIVE.md`) retains **every byte**; nothing was
deleted.

| Excluded from Git | Category | Reason | Where it remains |
|---|---|---|---|
| `<phase>/jsonl/*.jsonl` (per-invocation bench JSONL, ~384 KiB/phase) | deep provenance | The decision-relevant content (per-rep ops, cpu_s, wall, quantiles, enter counters, byte-oracle gate) is already embedded verbatim in the committed `results.jsonl` `metrics`/`reps` blocks; the bench JSONL is the same data one level deeper | full archive `measurement/raw/<phase>/jsonl/`, hashes in manifest |
| `<phase>/logs/*.log` (bench stdout, ~192 KiB/phase) | run logs | Progress lines and gate prints; every invocation's validity is already proven by `exit: 0` + the embedded stats in `results.jsonl`; kept for manual inspection only | full archive `measurement/raw/<phase>/logs/`, hashes in manifest |
| `aa-probe-cpu8` per-invocation jsonl/logs beyond results/header | deep provenance | Probe receipt; its analysis number (p99 8.05 pp) is recorded in PREREGISTERED.md and reanalysis receipt | full archive |
| build outputs (`build/` trees), xmake caches | build products | Reproducible from `patch.diff` + `build-notes.md`; only the 3 release binary hashes are evidence and they are recorded in build-notes + manifest | worktree (hashes recorded) |
| kernel/liburing system files | environment | Recorded by `platform.txt` (full archive) | full archive |

Not excluded, although arguably derivable: all `results.jsonl` and
`header.json` for every one of the 23 phases — including `harness-smoke` and
`threadcheck-w02` — because the protocol text references them as receipts
(smoke = validation-only, threadcheck = accounting consistency) and the audit
recomputes accounting facts from them.

Rule observed: no committed file was regenerated, filtered, or edited;
`results.jsonl` here are byte-identical to the full archive (checked by
SHA256SUMS here against the same hashes recorded in the full manifest).
