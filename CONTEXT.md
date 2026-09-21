# Sluice Context

Sluice is converging toward a C++20 explicit file-I/O library with direct
execution, a bounded host-independent request core, and optional I/O hosts.

## Start here

- [v1 Architecture and Contract Reference](docs/explicit-io-v1-final-decision.md)
  is the sole normative root: product scope, dictionary, semantics, lifetime,
  progress, shutdown, threading, evidence and migration gates.
- [v1 conformance ledger](docs/roadmap/v1-conformance.md) tracks implementation
  and evidence. Adoption of the target does not establish conformance.
- [AGENTS.md](AGENTS.md) gives repository working instructions.

`include/` and `src/` establish current C++ behavior; `apps/` are real consumers;
`xmake.lua` and `xmake/` establish current build targets. Inspect them before
claiming an implementation fact.

Old mission/ADRs and conformance records are retained historical rationale under
their supersession banners. Their earlier FROZEN/CLOSED status does not close a
v1 requirement. Research conclusions remain evidence, not independent authority.

Implement in reviewable slices with the tests/models needed for each changed
protocol, update the ledger, and retire replaced mechanisms after consumer
migration. Public behavior changes require a root amendment. Performance work
starts from measured workloads/hotspots.
