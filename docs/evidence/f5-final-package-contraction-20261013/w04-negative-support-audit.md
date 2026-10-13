# F5 W-04 conditional support audit — negative package and support-claim record

Disposition: `N/A_WITH_NEGATIVE_PACKAGE_AND_SUPPORT_CLAIM_AUDIT` (the normalized
#402 F5 gate spelling; not W-04 PASS).

- `OPTIONAL_W04_SUPPORTED = NO`. The stackful single-owner host
  (`stackful_io_host.hpp` + `fiber.hpp`/`fiber_ctx.hpp`) remains an
  OPTIONAL_CANDIDATE surface: installed so the candidate closure is consumable
  exactly as documented, labeled unsupported, gated on the H-09 W-04/HOST-03
  acceptance that has not been run in F5.
- Negative package evidence: `stackful_io_host.hpp`, `fiber.hpp` and
  `fiber_ctx.hpp` are the only host-side installed headers after the F5
  contraction; nothing that could be read as a supported multi-worker host
  (`scheduler.hpp`, `select.hpp`, the public sync suite) remains installed
  (B-01 v6 `summary.json` installed_header_list, both ring profiles). The
  `StackfulIoHost` consumer (`w04_stackful_candidate.cpp`) links and runs from
  the fresh prefixes; the F1-era negative `NEGATIVE_EXPERIMENTAL_HEADERS_NOT_
  INSTALLED` is superseded by tree-level absence (experimental headers no
  longer exist; nothing can install them).
- Negative support-claim evidence: README.md, README.zh-CN.md, docs/README.md,
  CHANGELOG.md, CONTEXT.md and the four app READMEs contain no supported-host,
  supported-multiworker or supported-W-04 claim; README's architecture section
  explicitly marks the host layer conditional ("Scheduler/Fiber, when
  supported, is an optional host above that core"). The published
  visuals/pages caption ("条件性 target：仅 supported optional single-owner
  stackful profile；本页不宣称当前实现已支持或通过 W-04") is unchanged from
  the F3 audit (SHA256 `8313c21a…` recorded there).
- `UNQUALIFIED_SUPPORTED_HOST_CLAIMS = 0`, `UNQUALIFIED_MULTIWORKER_CLAIMS =
  0`, `RETAINED_CONSUMER_SURVIVORS_OF_REMOVED_SURFACES = 0` (canonical
  consumers and the four apps consume only the canonical request path;
  per-member undefined-symbol audit in `closure-manifest.json`).
