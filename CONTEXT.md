# Sluice Context

Sluice is a C++20 explicit file-I/O library with direct execution, a bounded
request core with ThreadPool and io_uring backends, and a bounded
owned-driver scope. No host runtime is in the supported surface.

## Start here

- [v2 Architecture and Contract](docs/explicit-io-contract.md) (revision
  v2-r1) is the sole normative root: product scope, dictionary, semantics,
  lifetime, progress, shutdown, threading, evidence and acceptance.
- [v2 conformance ledger](docs/roadmap/v2-conformance.md) tracks
  implementation and evidence against the v2 root, including the
  release-gate status. Adoption of the target does not establish
  conformance.
- [AGENTS.md](AGENTS.md) gives repository working instructions.

`include/` and `src/` establish current C++ behavior; `apps/` are real consumers;
`xmake.lua` and `xmake/` establish current build targets. Inspect them before
claiming an implementation fact.

The superseded v1 root, its conformance ledger and formal evidence map, the
old mission/ADRs and the Phase M/F campaign records are retained historical
rationale in Git history under their supersession notices. Their earlier
FROZEN/CLOSED status does not close a v2 requirement. Research conclusions
remain evidence, not independent authority.

Implement in reviewable slices with the tests/models needed for each changed
protocol, update the v2 ledger, and retire replaced mechanisms after consumer
migration. Public behavior changes require a root amendment. Performance work
starts from measured workloads/hotspots.
