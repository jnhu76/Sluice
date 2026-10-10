# #402 F3/W-04 conditional exit gate — before/after decision record

> **Current status:** The owner-directed #402 issue-body policy alignment **was applied on 2026-10-10** to its F3 Evidence and Exit Gate lines after comparing the actual root PROD-02/W-04/MIG-03 optionality. The blocks below preserve the precise **before/after** for review; “Current” means *before that issue-body change*. This is **not** adoption of this PR's substantive v1-r4/F0 exclusion amendment, which remains review-pending, nor any W-04 supported PASS. GitHub issue history and [#458 delegated direction](https://github.com/jnhu76/Sluice/issues/458#issuecomment-6099156773) identify the owner-directed provenance.

**Status:** #402 F3 optionality interpretation applied by owner-directed issue edit; substantive root/F0 policy in PR #488 still PROPOSED. Owner-directed Clean Minimal V1 target: [#458 decision 6099156773](https://github.com/jnhu76/Sluice/issues/458#issuecomment-6099156773). Current root PROD-02 / W-04 / MIG-03 makes narrow sequential host support optional, while [#402](https://github.com/jnhu76/Sluice/issues/402) F3 Exit Gate currently unconditionally says `W04_SUPPORTED_HOST_PROFILE = PASS`. This creates a cycle for reference-app migration to canonical W-01/W-02/W-03.

## Minimal #402 change after review

**Current F3 Evidence line**
```text
- supported W-04 HOST-03 full matrix;
```

**Proposed**
```text
- if the narrow W-04 host profile is actually shipped/claimed OPTIONAL_SUPPORTED,
  its full HOST-03/W-04 verification matrix; otherwise an explicit
  OPTIONAL_SUPPORTED=NO (deferred/experimental) disposition and negative
  release-package/support-claim audit;
```

**Current F3 Exit Gate line**
```text
W04_SUPPORTED_HOST_PROFILE = PASS
```

**Proposed**
```text
W04_SUPPORTED_HOST_PROFILE =
  PASS_IF_SHIPPED_AS_SUPPORTED
  | N/A_WITH_DEFERRED_OR_EXPERIMENTAL_PROFILE_AND_NEGATIVE_PACKAGE_PROOF
```

N/A is **not** a fictitious passing W-04 verification. An optional candidate may be a separately labeled experimental/deferred surface, and cannot be advertised supported without W-04. The F3 `RETAINED_HOST_SUBSTRATE_HAS_NECESSITY_AND_LIFETIME_EVIDENCE` and `HOST_RUNTIME_CONSUMERS_MIGRATED_OR_ISOLATED` gates remain mandatory for their actual retained mechanisms/consumers.

**Current per-app #474/#458 exit trigger**: `D2/W-04 implementation verification passes AND F3 host arm exists` as a prerequisite for every app port.

**Proposed**: Each app consumer closes upon migration to the explicit W-01/W-02/W-03 mechanism it actually needs, with independently discriminating behavior and every-exit borrow/settlement/stop oracles. HASH/GREP → direct File/blocking; COPY bounded Requests/Scope; TAIL app-owned driver with explicit stop/settlement. W-04 support is independent and cannot become a forced requirement for these apps. Retain separate D2 obligations if W-04 is actually claimed supported.

## No changes authorized by this proposal

- No bypass of W-03 backend/core firewall or full ThreadPool/io_uring required backend conformance.
- No `OPTIONAL_SUPPORTED` or D2 verification PASS without actual W-04 evidence.
- No silent multi-worker → single-owner reinterpretation, no no-op `--workers` retained as pretend parallelism.
- No source deletion, F1 baseline rewrite or package contraction by editing this phase policy.
- No override of adopted #475 A/B/C installed-header hold/keep policy without its specific reconsideration conditions.
- No F3 family gate PASS merely because the host is deferred; other gate arms and consumer oracles still matter.

**After owner-directed #402 alignment:** independently review root/F0 v1-r4 candidate and exact #402 issue-body text against PROD-02/ADR-0003; update #458 per-app exit ownership and final Phase M md/JSON before closing M. Do not flip a gate solely because the wording changed.
