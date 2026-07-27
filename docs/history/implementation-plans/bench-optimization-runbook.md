# Optimization runbook

**Status: SLUICE-CORE-011A.** This is the first document allowed to turn
microbench observations into cautious, evidence-linked optimization rules. It
sits between the raw CSV (010) and the decision matrix (011D).

```text
Observation ≠ Rule.
Rule without evidence is forbidden.
```

## 1. Purpose

The runbook tells maintainers how to interpret microbench output, decide whether
an observation justifies an optimization, and document that decision. The
decision matrix (011D) records the conclusions; this runbook records the method.

## 2. Prerequisites

- All core microbenches build and run in **release** mode (010H).
- The CSV has been produced by `scripts/run_core_microbenches.sh release`.
- The host/filesystem/caches are recorded (010 methodology §6).

## 3. Step 1 — Run and capture

```sh
bash scripts/run_core_microbenches.sh release core_microbench_results.csv
```

Record in the run's notes: `uname -srm`, filesystem of the temp dir
(`df -T "$TMPDIR"`), kernel, and any load on the machine.

## 4. Step 2 — Summarize

```sh
python3 scripts/summarize_core_microbench.py core_microbench_results.csv
```

The summarizer (011B) prints, per (case, size): each mode's elapsed_ns/iter and
a throughput derived from bytes/elapsed, plus the min/median across repeats if
the CSV has repeated rows.

## 5. Step 3 — Interpret

For each cell, ask:

1. **Is the observation stable?** Re-run; if the rank order flips, it is noise.
2. **Is it workload-specific?** A 1B small-write win may vanish at 4KB.
3. **Is it environment-specific?** tmpfs vs ext4 vs WSL2 overlay can invert
   results; record the filesystem.

Only if all three hold can a cell graduate to a candidate rule.

## 6. Step 4 — Decide

- **Do nothing** if no cell shows a stable, workload-relevant, environment-noted
  win that is large enough to matter for a real workload.
- **Document a candidate rule** in `docs/bench-decision-matrix.md` with a
  link to the CSV run, the host/filesystem, and the magnitude. Phrase it as
  "observed", never "always".
- **Never implement** an optimization based on a single run or a single cell.

## 7. Valid categories

The matrix may record decisions in three categories:

- **Accepted (implemented already)** — e.g. Auto == BufferedFirst (006 default).
- **Candidate (deferred)** — observed win, not yet implemented, link to evidence.
- **Rejected (deferred)** — observed no win or unstable; recorded so it isn't
  retried without new evidence.

## 8. No universal claims

```text
"All X is faster than Y" is forbidden.
"On this host, in this run, for this workload, X observed Y" is required.
```

The matrix's rules are scoped and evidence-linked. They are NOT portable
performance guarantees. Cross-machine or cross-filesystem re-measurement is
required before treating any rule as generally true.
