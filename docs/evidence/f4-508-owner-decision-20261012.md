# F0 §1.7 owner-review packet — #508: BlockingIoPool

Status: technical candidate; human owner PENDING. External use UNKNOWN for every row.

Measured PR HEAD: `6e5fedf37132964fd224ebc1fdfc4e18741972e2`. Raw evidence SHA256: `d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38`.
Execution baseline: `f3521b45ead3963162e518758ac1820004adcfc2`; H0: `a38df5e9a7bee3603a439857f036de2b5a136bf2`.
Combined tested SHA: `ab2cea2784db05be8b05b5a776c757856beb4395`; tree: `07c1a3d5c40207e1a519592b9d2e10333674f2ed`.

Authority: root v1-r4 PROD-03/W-01–W-03/MIG-01/02/03; F0 §1.7/1.8; adopted Phase M; #475 HOLD/KEEP.

H0 contains all 17 headers in its public source include tree. Its libraries have no explicit header installation rules. The H0 API reference calls selected APIs stable-ish and requires deliberate deprecation; README calls the software experimental. Publication does not give every header the same stability promise. Source, API reference, README and rules are preserved verbatim. The pool implementation fragment fails standalone include but compiles through its public parent; a direct external include still breaks on removal.

No compatibility shim is added. Canonical per-operation blocking I/O is not a generic task pool or stream-product replacement. Existing external source consumers must change includes, types and calls; previously compiled consumers may fail to relink. A static executable already linked does not automatically lose its copied code. No zero-external-users claim is made.

The existing W-02 example contained a real borrow-lifetime defect. Original ASan UAF and TSan race diagnostics are retained. #511 moves buffers before RequestScope; #512 uses that corrected consumer against the exact unchanged candidate library trees. Adoption of the consumer correction is a separate evidence prerequisite. W-04 remains OPTIONAL_CANDIDATE and support is NO.

| Surface | Header | Retired API | Migration or no equivalent | Proposed policy | Owner |
|---|---|---|---|---|---|
| F4-01 | `sluice/blocking_io_pool.hpp` | BlockingIoPool, BlockingIoPoolOptions, PoolStats, Task<T>, try_submit, submit, wait_idle, shutdown, worker_count, queue_depth, make_blocking_io_pool, detail::enqueue_job | general task pool; no v1 equivalent | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-02 | `sluice/detail/blocking_io_pool_impl.hpp` | Task<T>::State, template, definitions, Task<T>::get, make_bound_job, try_submit, submit | implementation fragment requires public parent; no v1 equivalent | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |

This removes two installed headers and the pool core object. Task<T>, PoolStats, pool factory, submission/admission and shutdown callables cease to be available. External task-pool users may need architectural refactoring; there is no v1 generic-pool equivalent. Historical source exposure is expressly disclosed, not silently erased.

## Per-interface machine records

These rows bind baseline compile/link/behavior witnesses and candidate negative probes to profile/mode, command IDs, exit codes and log hashes in evidence.json. Header-only rows explicitly have no exclusive callable ABI. Group strong-symbol audits and real baseline-object negative relinks cover compiled legacy callables.

```json
[
  {
    "SURFACE_ID": "F4-01",
    "HEADER": "sluice/blocking_io_pool.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "0b8e360e683a14594665c1fe94a06ab78ab55ad52792f5846d0a02d1b71f2e39",
      "standalone": 47,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/error.hpp",
        "sluice/result.hpp",
        "sluice/detail/blocking_io_pool_impl.hpp"
      ],
      "types": [
        "BlockingIoPool",
        "BlockingIoPoolOptions",
        "PoolStats",
        "Task<T>"
      ],
      "callables": [
        "try_submit",
        "submit",
        "wait_idle",
        "shutdown",
        "worker_count",
        "queue_depth",
        "make_blocking_io_pool",
        "detail::enqueue_job"
      ],
      "description": "See verbatim H0 API reference and README; no per-header uniform stability claim; H0 lacks package header installation rules"
    },
    "CURRENT_INSTALLED_FACT": {
      "B0/no-liburing/debug": true,
      "B0/no-liburing/release": true,
      "B0/real-liburing/debug": true,
      "B0/real-liburing/release": true
    },
    "CURRENT_IN_TREE_CONSUMER": {
      "command": 1833,
      "raw_reference": "run-02/evidence.json exact git grep stdout; does not imply zero external users"
    },
    "EXTERNAL_USE": "UNKNOWN",
    "OLD_API_COMPILE": {
      "B0/no-liburing/debug": "PASS",
      "B0/no-liburing/release": "PASS",
      "B0/real-liburing/debug": "PASS",
      "B0/real-liburing/release": "PASS"
    },
    "OLD_API_LINK": {
      "B0/no-liburing/debug": "PASS",
      "B0/no-liburing/release": "PASS",
      "B0/real-liburing/debug": "PASS",
      "B0/real-liburing/release": "PASS"
    },
    "OLD_BEHAVIOR_WITNESS": {
      "B0/no-liburing/debug": "PASS",
      "B0/no-liburing/release": "PASS",
      "B0/real-liburing/debug": "PASS",
      "B0/real-liburing/release": "PASS"
    },
    "CANDIDATE_HEADER_ABSENT": {
      "C508/no-liburing/debug": true,
      "C508/no-liburing/release": true,
      "C508/real-liburing/debug": true,
      "C508/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C508/no-liburing/debug": "PASS",
      "C508/no-liburing/release": "PASS",
      "C508/real-liburing/debug": "PASS",
      "C508/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C508/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508/real-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/real-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      }
    },
    "RETAINED_CANONICAL_WORKLOAD": {
      "result": "W01/W02/W03 ThreadPool and real-uring PASS; no-liburing uring UNAVAILABLE; W04 support NO",
      "raw_reference": "run-02/evidence.json packages; command IDs, exit codes and log hashes",
      "consumer_lifetime_fix": "#511; measured library production unchanged"
    },
    "MIGRATION_OR_NO_EQUIVALENT": "general task pool; no v1 equivalent",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-02",
    "HEADER": "sluice/detail/blocking_io_pool_impl.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "0fd7c0ef9b0a12f224c5dea81a2b2a6fae5f4023fee47629e4daa4b33fedf6bc",
      "standalone": 48,
      "standalone_status": "FAIL",
      "public_includes": [],
      "types": [
        "Task<T>::State",
        "template",
        "definitions"
      ],
      "callables": [
        "Task<T>::get",
        "make_bound_job",
        "try_submit",
        "submit"
      ],
      "description": "See verbatim H0 API reference and README; no per-header uniform stability claim; H0 lacks package header installation rules",
      "parent_include": 49,
      "parent_status": "PASS"
    },
    "CURRENT_INSTALLED_FACT": {
      "B0/no-liburing/debug": true,
      "B0/no-liburing/release": true,
      "B0/real-liburing/debug": true,
      "B0/real-liburing/release": true
    },
    "CURRENT_IN_TREE_CONSUMER": {
      "command": 1834,
      "raw_reference": "run-02/evidence.json exact git grep stdout; does not imply zero external users"
    },
    "EXTERNAL_USE": "UNKNOWN",
    "OLD_API_COMPILE": {
      "B0/no-liburing/debug": "PASS",
      "B0/no-liburing/release": "PASS",
      "B0/real-liburing/debug": "PASS",
      "B0/real-liburing/release": "PASS"
    },
    "OLD_API_LINK": {
      "B0/no-liburing/debug": "PASS",
      "B0/no-liburing/release": "PASS",
      "B0/real-liburing/debug": "PASS",
      "B0/real-liburing/release": "PASS"
    },
    "OLD_BEHAVIOR_WITNESS": {
      "B0/no-liburing/debug": "PASS",
      "B0/no-liburing/release": "PASS",
      "B0/real-liburing/debug": "PASS",
      "B0/real-liburing/release": "PASS"
    },
    "CANDIDATE_HEADER_ABSENT": {
      "C508/no-liburing/debug": true,
      "C508/no-liburing/release": true,
      "C508/real-liburing/debug": true,
      "C508/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C508/no-liburing/debug": "PASS",
      "C508/no-liburing/release": "PASS",
      "C508/real-liburing/debug": "PASS",
      "C508/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C508/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508/real-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C508510/real-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      }
    },
    "RETAINED_CANONICAL_WORKLOAD": {
      "result": "W01/W02/W03 ThreadPool and real-uring PASS; no-liburing uring UNAVAILABLE; W04 support NO",
      "raw_reference": "run-02/evidence.json packages; command IDs, exit codes and log hashes",
      "consumer_lifetime_fix": "#511; measured library production unchanged"
    },
    "MIGRATION_OR_NO_EQUIVALENT": "implementation fragment requires public parent; no v1 equivalent",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  }
]
```

## F0 §1.7 Owner Decision Record — unsigned template

An identified human phase owner must actively publish or confirm this record. Fill each value and confirm a policy for every table row; no checkbox is preselected. A material code/risk scope change requires renewed confirmation. This agent has not signed the record.

```ini
OWNER = <human account and phase-owner role>
DECISION_DATE = <date of active confirmation>
DECISION_SCOPE = PR #508; enumerated surfaces F4-01, F4-02
REVIEWED_PR_HEAD = <full final PR SHA actively reviewed>
REVIEWED_EVIDENCE_SHA256 = d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38
ACCEPTED_BREAKAGE_SURFACES = <enumerate per-row accepted surfaces>
DEPRECATION_POLICY = <per-row ACCEPT_BREAKAGE_NOW / BOUNDED_DEPRECATION_WINDOW / BLOCK_AND_REVISE>
MIGRATION_DISCLOSURE = <confirm each migration/no-equivalent and historical exposure consequence>
APPROVED_FOR_SCOPED_REMOVAL = <human decision; presently PENDING>
```
