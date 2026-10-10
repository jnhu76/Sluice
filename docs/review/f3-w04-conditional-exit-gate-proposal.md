# #402 F3/W-04 conditional exit gate — applied before/after decision record

> **Status: APPLIED 2026-10-10 to the live #402 issue body**, per owner-directed Clean Minimal V1 decision [#458 comment 6099156773](https://github.com/jnhu76/Sluice/issues/458#issuecomment-6099156773). The following is an exact **Before edit → Applied after edit** log of #402's F3 Evidence/Exit Gate. Root v1-r4/F0 exclusion is now adopted via PR #488 at `d4973100a31dccaa098cf4227f84e6c9c0afdee7`; this phase-gate alignment does not assert supported W-04 or D2 implementation verification.

Root PROD-02/W-04/MIG-03 already defined the narrow supported host as optional. **Before edit**, #402 mistakenly made support unconditional, potentially forcing a W-04 product just to migrate HASH/GREP/COPY/TAIL.

## Exact F3 evidence change

**Before edit:**

```text
- supported W-04 HOST-03 full matrix；
```

**Applied after edit (verbatim #402 F3 Evidence line):**

```text
- if a narrow W-04 profile is **shipped/claimed OPTIONAL_SUPPORTED**, the supported W-04/HOST-03 full matrix；otherwise explicitly record `OPTIONAL_SUPPORTED=NO` (deferred or experimental) and pass a **negative package/support-claim audit**, without claiming W-04 verification；
```

## Exact F3 Exit Gate change

**Before edit:**

```text
W04_SUPPORTED_HOST_PROFILE = PASS
```

**Applied after edit (verbatim live #402 token):**

```text
W04_SUPPORTED_HOST_PROFILE = PASS_IF_SHIPPED_AS_SUPPORTED | N/A_WITH_NEGATIVE_PACKAGE_PROOF
```

`N/A_WITH_NEGATIVE_PACKAGE_PROOF` is an *unverified optional-support exclusion with negative package/support-claim evidence*, NOT `W04_SUPPORTED_HOST_PROFILE=PASS`. A candidate/experimental host may have installed supporting headers without becoming a supported product, if its installed closure, documentation and support-claims are consistent. If future v1 shipping claims W-04 supported, it must pass complete W-04/HOST-03 obligations.

**Historical per-app #474/#458 exit trigger before owner decision**: `D2/W-04 implementation verification passes AND F3 host arm exists` as a prerequisite for every app port.

**Owner-selected target, tracked for F3 family migration**: Each app consumer closes upon migration to the explicit W-01/W-02/W-03 mechanism it actually needs, with independently discriminating behavior and every-exit borrow/settlement/stop oracles. HASH/GREP → direct File/blocking; COPY bounded Requests/Scope; TAIL app-owned driver with explicit stop/settlement. W-04 support is independent and cannot become a forced requirement for these apps. Retain separate D2 obligations if W-04 is actually claimed supported.

## Constraints preserved by the issue-body alignment

- No bypass of W-03 backend/core firewall or full ThreadPool/io_uring required backend conformance.
- No `OPTIONAL_SUPPORTED` or D2 verification PASS without actual W-04 evidence.
- No silent multi-worker → single-owner reinterpretation, no no-op `--workers` retained as pretend parallelism.
- No source deletion, F1 baseline rewrite or package contraction by editing this phase policy.
- No override of adopted #475 A/B/C installed-header hold/keep policy without its specific reconsideration conditions.
- No F3 family gate PASS merely because the host is deferred; other gate arms and consumer oracles still matter.

**Post-adoption:** #488 exact-head independent review approved the root/F0 amendment and exact #402 issue-body text against PROD-02/ADR-0003. The final Phase M adoption md/JSON and #458 record carry per-app exit ownership and the remaining final-review/merge/closure receipt. Do not flip a gate solely because the wording changed.
