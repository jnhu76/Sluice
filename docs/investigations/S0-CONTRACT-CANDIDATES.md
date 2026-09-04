# S0-CONTRACT-CANDIDATES — Safety S0B input ledger

**Status:** LIVE — open candidates for the #289 S0B contract / correctness
authority / C++ reality freeze.

**Authority:** none. Every row below is an **UNCONFIRMED hypothesis** observed
during the S0-DOCS documentation audit (#290, 2026-09-04). None was fixed
beyond pure documentation-role/stale-prose repair, because each involves
"what behavior should be" rather than "what documentation role/path should
be" — that adjudication belongs to S0B (#289), not to a docs pass.

**Rule for S0B:** for each row, freeze the documented contract ↔ internal
correctness authority ↔ actual C++ mapping before any fix; characterize the
drift class (doc-too-strong / doc-too-weak / implementation drift /
undocumented / unknown) with test or static evidence.

## C-01 — api.md public surface vs post-v0.0.1 header drift

- **Document/header claim:** `docs/reference/api.md` (and `api.zh-CN.md`)
  describe the public API "as of the `v0.0.1` reference baseline".
- **Suspected C++ behavior:** master has moved past `v0.0.1` — e.g. PR #287
  removed the speculative `CopyStrategy` deferred public contract
  (semantic-diet-0), a post-baseline public-surface change.
- **Affected authority:** `docs/reference/api.md`, `docs/reference/api.zh-CN.md`
  vs `include/sluice/`.
- **Why safety-relevant:** S0B contract freeze needs to know whether the
  reference describes current headers or a historical snapshot; a stale
  reference can hide contract drift in both directions.
- **Evidence inspected:** #227 stable-checkpoint section; PR #287 merge
  (`bc97ac35`); api.md header line.
- **Status:** UNCONFIRMED.
- **Recommended S0B scope:** diff `docs/reference/api.md` against
  `include/sluice/` headers; classify every divergence doc-vs-code.

## C-02 — "Last verified against" anchors on sync architecture docs

- **Document/header claim:** `docs/architecture/sync-io-architecture.md` and
  `sync-durability-model.md` carry `Last verified against: v0.0.1` (re-pointed
  from the nonexistent `v0.1.0` tag by #290; the *verification itself* was not
  re-run).
- **Suspected C++ behavior:** `src/` sync implementation may have drifted from
  these documents since the baseline was actually checked.
- **Affected authority:** sync architecture CURRENT docs vs `src/file.cpp` etc.
- **Why safety-relevant:** durability contracts (`flush` ≠ durability,
  `sync_data`/`sync_all`) are Boundary/Safety-critical semantics.
- **Evidence inspected:** header lines only; no re-verification performed.
- **Status:** UNCONFIRMED.
- **Recommended S0B scope:** re-verify or re-stamp these two documents against
  current `src/`; record the verification command/date.

## C-03 — Request lifecycle prose vs request_arena implementation

- **Document/header claim:** `AGENTS.md` §3.2/§3.3, ADR-explicit-io-request-contract,
  and `async-request-lifecycle.md` state the stable-identity / terminal-winner /
  reap-only-publication invariants.
- **Suspected C++ behavior:** not suspected wrong — this is the core S0B row
  set by design: each invariant needs a named C++ authority owner and an
  independent test witness mapped row-by-row.
- **Affected authority:** the whole request-lifecycle authority chain.
- **Why safety-relevant:** generation/ABA defense, exactly-once terminal, and
  borrow lifetime are the load-bearing correctness kernel.
- **Evidence inspected:** none beyond reading; this is planned S0B work.
- **Status:** UNCONFIRMED (planned).
- **Recommended S0B scope:** the S0B table in #289 §2, rows identity /
  accept-rollback / terminal winner / publication / borrow / shutdown.

## C-04 — failure-model T1–T7 ↔ assert sites ↔ failure-envelope.json

- **Document/header claim:** `docs/architecture/failure-model.md` (T1–T7),
  `scripts/gates/assert-hygiene.allowlist`, and
  `docs/verification/failure-envelope.json` each encode failure-class facts.
- **Suspected C++ behavior:** unverified whether every allowlisted assert site,
  every T-class, and every envelope row still correspond to real C++ sites and
  tests after the post-freeze changes.
- **Affected authority:** failure model + verification gate wiring.
- **Why safety-relevant:** Debug/Release fail-fast coverage claims depend on
  this correspondence.
- **Evidence inspected:** gate scripts; no site-by-site audit performed.
- **Status:** UNCONFIRMED.
- **Recommended S0B scope:** site-by-site correspondence audit
  (model ↔ allowlist ↔ envelope ↔ code).

## C-05 — verification README evidence-status table vs actual gate wiring

- **Document/header claim:** `docs/verification/README.md` classifies evidence
  availability (CI gate / available / environment-dependent / planned).
- **Suspected C++ behavior:** unverified whether every "CI gate" row is
  actually wired in `.github/workflows/` and `scripts/gates/pre-push.sh`
  after recent gate changes.
- **Affected authority:** verification README + CI wiring.
- **Why safety-relevant:** claiming a gate runs when it does not is exactly the
  silent-coverage class Safety must eliminate.
- **Evidence inspected:** README + pre-push.sh; no per-row wiring audit.
- **Status:** UNCONFIRMED.
- **Recommended S0B scope:** row-by-row wiring verification during the S0B
  verification-authority pass.
