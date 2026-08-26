# Sluice Formal Verification Tooling

This directory is the canonical home for all TLA+ model-checking tooling.

```text
scripts/formal/
├── README.md              # this file
├── verify.py              # unified orchestrator (doctor/list/check/suite/smoke/all)
├── bootstrap.py           # download + cache tla2tools.jar
├── tla2tools.lock.json    # pinned jar version + SHA-256
├── lib/
│   ├── __init__.py
│   └── tlc.py             # jar resolution + TLC version reporting (verify.py)
├── verify-e7-publication.sh
├── verify-e8-ownership-transfer.sh
├── verify-async-queue.sh
├── verify-e13-select-core.sh
├── verify-e13-select-safety.sh
└── ... (one verifier per suite)
```

## Quick start

```bash
# 1. Download and cache the official tla2tools.jar
python3 scripts/formal/bootstrap.py

# 2. Verify the environment is healthy
python3 scripts/formal/verify.py doctor

# 3. List all suites
python3 scripts/formal/verify.py list

# 4. Run structural checks (no TLC)
python3 scripts/formal/verify.py check

# 5. Run the PR smoke tier
python3 scripts/formal/verify.py smoke

# 6. Run everything
python3 scripts/formal/verify.py all
```

## Commands

| Command | Purpose |
|---------|---------|
| `doctor` | Check Java, jar, manifest, verifier executables, temp dirs |
| `list` | Print the suite inventory (`--markdown` for a table) |
| `check` | Structural checks only — no TLC execution |
| `suite <id>` | Run one suite's authoritative verifier |
| `smoke` | Run the measured-fast PR tier |
| `all` | Run every suite and print a unified summary |

## Jar resolution order

1. `TLA2TOOLS_JAR` environment variable
2. `~/.cache/sluice/formal/tla2tools.jar` (managed by `bootstrap.py`)
3. `<repo>/tla2tools.jar` (legacy untracked jar — compatibility only)

The checksum is always verified against `tla2tools.lock.json` unless
`TLA2TOOLS_JAR` points at a local override (which emits a warning).

## Source-tree safety

Every TLC invocation happens inside an isolated `mktemp` workspace
(`sluice-formal.<suite>.<random>`). The workspace is copied from the suite's
`.tla`/`.cfg` files, TLC runs there, and the workspace is cleaned up on exit.
TLC never writes `MC.out`, `states/`, `metadir`, or `*_TTrace*` into the source
tree.

## Adding a new suite

1. Create `spec/tla/<suite-id>/` with the `.tla`, `.cfg`, and a `README.md`.
2. Add a verifier script at `scripts/formal/verify-<suite-id>.sh`.
3. Add an entry to `spec/tla/manifest.json`.
4. Update `docs/verification/formal-models.md`.
5. Run `python3 scripts/formal/verify.py check` to confirm consistency.
