# TAX-0 measurement sessions (append-only)

Each directory is an immutable session (environment.json + manifest.json +
raw/ per-run perf/bench output + summary). Lineage:

- `tax0b-zladder-pilot-wsl2` — pilot correctness sweep (60/60 OK)
- `tax0b-zladder-wsl2-formal1..3` — superseded ladder protocols (raw/
  omitted: formal2/3 measured a z1/z1b two-enter variant fixed before
  formal4; formal1 hit the runner bugs documented in the formal4 notes).
  Summaries/manifests retained as history.
- `tax0b-zladder-wsl2-formal4` — CANONICAL ladder session (60/60 OK);
  citable numbers, see its notes.md
- `tax0c-profile-wsl2` — TAX-0C perf-record reports (raw .data deleted
  after text extraction, see its notes.md)
- `tax0d-ablation-wsl2-1` — TAX-0D F01/F02 causal A/B session

Sessions are never overwritten; new measurements create new directories.
