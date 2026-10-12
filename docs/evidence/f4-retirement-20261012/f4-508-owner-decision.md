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
      "matches": "src/blocking_io_pool.cpp:1:#include <sluice/blocking_io_pool.hpp>\n"
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
      "C508/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 671,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 672,
            "status": "PASS"
          },
          "binary_nm": 673,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 674,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 675,
            "status": "PASS"
          },
          "binary_nm": 676
        },
        "w03_external_loop": {
          "compile_link": 677,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 678,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 679,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 680,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 681,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 682,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 683
        },
        "contract_request_lifetime": {
          "compile_link": 684,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 685,
            "status": "PASS"
          },
          "binary_nm": 686
        },
        "contract_admission": {
          "compile_link": 687,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 688,
            "status": "PASS"
          },
          "binary_nm": 689
        },
        "contract_cancel_effect": {
          "compile_link": 690,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 691,
            "status": "PASS"
          },
          "binary_nm": 692
        },
        "contract_observer": {
          "compile_link": 693,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 694,
            "status": "PASS"
          },
          "binary_nm": 695
        },
        "contract_progress": {
          "compile_link": 696,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 697,
            "status": "PASS"
          },
          "binary_nm": 698
        },
        "contract_shutdown": {
          "compile_link": 699,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 700,
            "status": "PASS"
          },
          "binary_nm": 701
        },
        "contract_durability": {
          "compile_link": 702,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 703,
            "status": "PASS"
          },
          "binary_nm": 704
        },
        "contract_backend_availability": {
          "compile_link": 705,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 706,
            "status": "PASS"
          },
          "binary_nm": 707
        }
      },
      "C508/no-liburing/release": {
        "w01_direct": {
          "compile_link": 794,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 795,
            "status": "PASS"
          },
          "binary_nm": 796,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 797,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 798,
            "status": "PASS"
          },
          "binary_nm": 799
        },
        "w03_external_loop": {
          "compile_link": 800,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 801,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 802,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 803,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 804,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 805,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 806
        },
        "contract_request_lifetime": {
          "compile_link": 807,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 808,
            "status": "PASS"
          },
          "binary_nm": 809
        },
        "contract_admission": {
          "compile_link": 810,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 811,
            "status": "PASS"
          },
          "binary_nm": 812
        },
        "contract_cancel_effect": {
          "compile_link": 813,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 814,
            "status": "PASS"
          },
          "binary_nm": 815
        },
        "contract_observer": {
          "compile_link": 816,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 817,
            "status": "PASS"
          },
          "binary_nm": 818
        },
        "contract_progress": {
          "compile_link": 819,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 820,
            "status": "PASS"
          },
          "binary_nm": 821
        },
        "contract_shutdown": {
          "compile_link": 822,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 823,
            "status": "PASS"
          },
          "binary_nm": 824
        },
        "contract_durability": {
          "compile_link": 825,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 826,
            "status": "PASS"
          },
          "binary_nm": 827
        },
        "contract_backend_availability": {
          "compile_link": 828,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 829,
            "status": "PASS"
          },
          "binary_nm": 830
        }
      },
      "C508/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 917,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 918,
            "status": "PASS"
          },
          "binary_nm": 919,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 920,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 921,
            "status": "PASS"
          },
          "binary_nm": 922
        },
        "w03_external_loop": {
          "compile_link": 923,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 924,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 925,
            "status": "PASS"
          },
          "binary_nm": 926,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 927,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 928,
            "status": "PASS"
          },
          "binary_nm": 929
        },
        "contract_request_lifetime": {
          "compile_link": 930,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 931,
            "status": "PASS"
          },
          "binary_nm": 932
        },
        "contract_admission": {
          "compile_link": 933,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 934,
            "status": "PASS"
          },
          "binary_nm": 935
        },
        "contract_cancel_effect": {
          "compile_link": 936,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 937,
            "status": "PASS"
          },
          "binary_nm": 938
        },
        "contract_observer": {
          "compile_link": 939,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 940,
            "status": "PASS"
          },
          "binary_nm": 941
        },
        "contract_progress": {
          "compile_link": 942,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 943,
            "status": "PASS"
          },
          "binary_nm": 944
        },
        "contract_shutdown": {
          "compile_link": 945,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 946,
            "status": "PASS"
          },
          "binary_nm": 947
        },
        "contract_durability": {
          "compile_link": 948,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 949,
            "status": "PASS"
          },
          "binary_nm": 950
        },
        "contract_backend_availability": {
          "compile_link": 951,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 952,
            "status": "PASS"
          },
          "binary_nm": 953
        }
      },
      "C508/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1040,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1041,
            "status": "PASS"
          },
          "binary_nm": 1042,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1043,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1044,
            "status": "PASS"
          },
          "binary_nm": 1045
        },
        "w03_external_loop": {
          "compile_link": 1046,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1047,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1048,
            "status": "PASS"
          },
          "binary_nm": 1049,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1050,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1051,
            "status": "PASS"
          },
          "binary_nm": 1052
        },
        "contract_request_lifetime": {
          "compile_link": 1053,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1054,
            "status": "PASS"
          },
          "binary_nm": 1055
        },
        "contract_admission": {
          "compile_link": 1056,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1057,
            "status": "PASS"
          },
          "binary_nm": 1058
        },
        "contract_cancel_effect": {
          "compile_link": 1059,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1060,
            "status": "PASS"
          },
          "binary_nm": 1061
        },
        "contract_observer": {
          "compile_link": 1062,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1063,
            "status": "PASS"
          },
          "binary_nm": 1064
        },
        "contract_progress": {
          "compile_link": 1065,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1066,
            "status": "PASS"
          },
          "binary_nm": 1067
        },
        "contract_shutdown": {
          "compile_link": 1068,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1069,
            "status": "PASS"
          },
          "binary_nm": 1070
        },
        "contract_durability": {
          "compile_link": 1071,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1072,
            "status": "PASS"
          },
          "binary_nm": 1073
        },
        "contract_backend_availability": {
          "compile_link": 1074,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1075,
            "status": "PASS"
          },
          "binary_nm": 1076
        }
      },
      "C508510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1458,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1459,
            "status": "PASS"
          },
          "binary_nm": 1460,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1461,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1462,
            "status": "PASS"
          },
          "binary_nm": 1463
        },
        "w03_external_loop": {
          "compile_link": 1464,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1465,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1466,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1467,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1468,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1469,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1470
        },
        "contract_request_lifetime": {
          "compile_link": 1471,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1472,
            "status": "PASS"
          },
          "binary_nm": 1473
        },
        "contract_admission": {
          "compile_link": 1474,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1475,
            "status": "PASS"
          },
          "binary_nm": 1476
        },
        "contract_cancel_effect": {
          "compile_link": 1477,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1478,
            "status": "PASS"
          },
          "binary_nm": 1479
        },
        "contract_observer": {
          "compile_link": 1480,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1481,
            "status": "PASS"
          },
          "binary_nm": 1482
        },
        "contract_progress": {
          "compile_link": 1483,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1484,
            "status": "PASS"
          },
          "binary_nm": 1485
        },
        "contract_shutdown": {
          "compile_link": 1486,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1487,
            "status": "PASS"
          },
          "binary_nm": 1488
        },
        "contract_durability": {
          "compile_link": 1489,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1490,
            "status": "PASS"
          },
          "binary_nm": 1491
        },
        "contract_backend_availability": {
          "compile_link": 1492,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1493,
            "status": "PASS"
          },
          "binary_nm": 1494
        }
      },
      "C508510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1536,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1537,
            "status": "PASS"
          },
          "binary_nm": 1538,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1539,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1540,
            "status": "PASS"
          },
          "binary_nm": 1541
        },
        "w03_external_loop": {
          "compile_link": 1542,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1543,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1544,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1545,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1546,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1547,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1548
        },
        "contract_request_lifetime": {
          "compile_link": 1549,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1550,
            "status": "PASS"
          },
          "binary_nm": 1551
        },
        "contract_admission": {
          "compile_link": 1552,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1553,
            "status": "PASS"
          },
          "binary_nm": 1554
        },
        "contract_cancel_effect": {
          "compile_link": 1555,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1556,
            "status": "PASS"
          },
          "binary_nm": 1557
        },
        "contract_observer": {
          "compile_link": 1558,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1559,
            "status": "PASS"
          },
          "binary_nm": 1560
        },
        "contract_progress": {
          "compile_link": 1561,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1562,
            "status": "PASS"
          },
          "binary_nm": 1563
        },
        "contract_shutdown": {
          "compile_link": 1564,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1565,
            "status": "PASS"
          },
          "binary_nm": 1566
        },
        "contract_durability": {
          "compile_link": 1567,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1568,
            "status": "PASS"
          },
          "binary_nm": 1569
        },
        "contract_backend_availability": {
          "compile_link": 1570,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1571,
            "status": "PASS"
          },
          "binary_nm": 1572
        }
      },
      "C508510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1614,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1615,
            "status": "PASS"
          },
          "binary_nm": 1616,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1617,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1618,
            "status": "PASS"
          },
          "binary_nm": 1619
        },
        "w03_external_loop": {
          "compile_link": 1620,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1621,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1622,
            "status": "PASS"
          },
          "binary_nm": 1623,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1624,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1625,
            "status": "PASS"
          },
          "binary_nm": 1626
        },
        "contract_request_lifetime": {
          "compile_link": 1627,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1628,
            "status": "PASS"
          },
          "binary_nm": 1629
        },
        "contract_admission": {
          "compile_link": 1630,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1631,
            "status": "PASS"
          },
          "binary_nm": 1632
        },
        "contract_cancel_effect": {
          "compile_link": 1633,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1634,
            "status": "PASS"
          },
          "binary_nm": 1635
        },
        "contract_observer": {
          "compile_link": 1636,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1637,
            "status": "PASS"
          },
          "binary_nm": 1638
        },
        "contract_progress": {
          "compile_link": 1639,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1640,
            "status": "PASS"
          },
          "binary_nm": 1641
        },
        "contract_shutdown": {
          "compile_link": 1642,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1643,
            "status": "PASS"
          },
          "binary_nm": 1644
        },
        "contract_durability": {
          "compile_link": 1645,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1646,
            "status": "PASS"
          },
          "binary_nm": 1647
        },
        "contract_backend_availability": {
          "compile_link": 1648,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1649,
            "status": "PASS"
          },
          "binary_nm": 1650
        }
      },
      "C508510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1692,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1693,
            "status": "PASS"
          },
          "binary_nm": 1694,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1695,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1696,
            "status": "PASS"
          },
          "binary_nm": 1697
        },
        "w03_external_loop": {
          "compile_link": 1698,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1699,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1700,
            "status": "PASS"
          },
          "binary_nm": 1701,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1702,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1703,
            "status": "PASS"
          },
          "binary_nm": 1704
        },
        "contract_request_lifetime": {
          "compile_link": 1705,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1706,
            "status": "PASS"
          },
          "binary_nm": 1707
        },
        "contract_admission": {
          "compile_link": 1708,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1709,
            "status": "PASS"
          },
          "binary_nm": 1710
        },
        "contract_cancel_effect": {
          "compile_link": 1711,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1712,
            "status": "PASS"
          },
          "binary_nm": 1713
        },
        "contract_observer": {
          "compile_link": 1714,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1715,
            "status": "PASS"
          },
          "binary_nm": 1716
        },
        "contract_progress": {
          "compile_link": 1717,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1718,
            "status": "PASS"
          },
          "binary_nm": 1719
        },
        "contract_shutdown": {
          "compile_link": 1720,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1721,
            "status": "PASS"
          },
          "binary_nm": 1722
        },
        "contract_durability": {
          "compile_link": 1723,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1724,
            "status": "PASS"
          },
          "binary_nm": 1725
        },
        "contract_backend_availability": {
          "compile_link": 1726,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1727,
            "status": "PASS"
          },
          "binary_nm": 1728
        }
      }
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
      "matches": "include/sluice/blocking_io_pool.hpp:96:#include <sluice/detail/blocking_io_pool_impl.hpp>\nsrc/blocking_io_pool.cpp:2:#include <sluice/detail/blocking_io_pool_impl.hpp>\n"
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
      "C508/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 671,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 672,
            "status": "PASS"
          },
          "binary_nm": 673,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 674,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 675,
            "status": "PASS"
          },
          "binary_nm": 676
        },
        "w03_external_loop": {
          "compile_link": 677,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 678,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 679,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 680,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 681,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 682,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 683
        },
        "contract_request_lifetime": {
          "compile_link": 684,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 685,
            "status": "PASS"
          },
          "binary_nm": 686
        },
        "contract_admission": {
          "compile_link": 687,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 688,
            "status": "PASS"
          },
          "binary_nm": 689
        },
        "contract_cancel_effect": {
          "compile_link": 690,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 691,
            "status": "PASS"
          },
          "binary_nm": 692
        },
        "contract_observer": {
          "compile_link": 693,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 694,
            "status": "PASS"
          },
          "binary_nm": 695
        },
        "contract_progress": {
          "compile_link": 696,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 697,
            "status": "PASS"
          },
          "binary_nm": 698
        },
        "contract_shutdown": {
          "compile_link": 699,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 700,
            "status": "PASS"
          },
          "binary_nm": 701
        },
        "contract_durability": {
          "compile_link": 702,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 703,
            "status": "PASS"
          },
          "binary_nm": 704
        },
        "contract_backend_availability": {
          "compile_link": 705,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 706,
            "status": "PASS"
          },
          "binary_nm": 707
        }
      },
      "C508/no-liburing/release": {
        "w01_direct": {
          "compile_link": 794,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 795,
            "status": "PASS"
          },
          "binary_nm": 796,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 797,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 798,
            "status": "PASS"
          },
          "binary_nm": 799
        },
        "w03_external_loop": {
          "compile_link": 800,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 801,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 802,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 803,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 804,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 805,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 806
        },
        "contract_request_lifetime": {
          "compile_link": 807,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 808,
            "status": "PASS"
          },
          "binary_nm": 809
        },
        "contract_admission": {
          "compile_link": 810,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 811,
            "status": "PASS"
          },
          "binary_nm": 812
        },
        "contract_cancel_effect": {
          "compile_link": 813,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 814,
            "status": "PASS"
          },
          "binary_nm": 815
        },
        "contract_observer": {
          "compile_link": 816,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 817,
            "status": "PASS"
          },
          "binary_nm": 818
        },
        "contract_progress": {
          "compile_link": 819,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 820,
            "status": "PASS"
          },
          "binary_nm": 821
        },
        "contract_shutdown": {
          "compile_link": 822,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 823,
            "status": "PASS"
          },
          "binary_nm": 824
        },
        "contract_durability": {
          "compile_link": 825,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 826,
            "status": "PASS"
          },
          "binary_nm": 827
        },
        "contract_backend_availability": {
          "compile_link": 828,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 829,
            "status": "PASS"
          },
          "binary_nm": 830
        }
      },
      "C508/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 917,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 918,
            "status": "PASS"
          },
          "binary_nm": 919,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 920,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 921,
            "status": "PASS"
          },
          "binary_nm": 922
        },
        "w03_external_loop": {
          "compile_link": 923,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 924,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 925,
            "status": "PASS"
          },
          "binary_nm": 926,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 927,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 928,
            "status": "PASS"
          },
          "binary_nm": 929
        },
        "contract_request_lifetime": {
          "compile_link": 930,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 931,
            "status": "PASS"
          },
          "binary_nm": 932
        },
        "contract_admission": {
          "compile_link": 933,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 934,
            "status": "PASS"
          },
          "binary_nm": 935
        },
        "contract_cancel_effect": {
          "compile_link": 936,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 937,
            "status": "PASS"
          },
          "binary_nm": 938
        },
        "contract_observer": {
          "compile_link": 939,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 940,
            "status": "PASS"
          },
          "binary_nm": 941
        },
        "contract_progress": {
          "compile_link": 942,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 943,
            "status": "PASS"
          },
          "binary_nm": 944
        },
        "contract_shutdown": {
          "compile_link": 945,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 946,
            "status": "PASS"
          },
          "binary_nm": 947
        },
        "contract_durability": {
          "compile_link": 948,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 949,
            "status": "PASS"
          },
          "binary_nm": 950
        },
        "contract_backend_availability": {
          "compile_link": 951,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 952,
            "status": "PASS"
          },
          "binary_nm": 953
        }
      },
      "C508/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1040,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1041,
            "status": "PASS"
          },
          "binary_nm": 1042,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1043,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1044,
            "status": "PASS"
          },
          "binary_nm": 1045
        },
        "w03_external_loop": {
          "compile_link": 1046,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1047,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1048,
            "status": "PASS"
          },
          "binary_nm": 1049,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1050,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1051,
            "status": "PASS"
          },
          "binary_nm": 1052
        },
        "contract_request_lifetime": {
          "compile_link": 1053,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1054,
            "status": "PASS"
          },
          "binary_nm": 1055
        },
        "contract_admission": {
          "compile_link": 1056,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1057,
            "status": "PASS"
          },
          "binary_nm": 1058
        },
        "contract_cancel_effect": {
          "compile_link": 1059,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1060,
            "status": "PASS"
          },
          "binary_nm": 1061
        },
        "contract_observer": {
          "compile_link": 1062,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1063,
            "status": "PASS"
          },
          "binary_nm": 1064
        },
        "contract_progress": {
          "compile_link": 1065,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1066,
            "status": "PASS"
          },
          "binary_nm": 1067
        },
        "contract_shutdown": {
          "compile_link": 1068,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1069,
            "status": "PASS"
          },
          "binary_nm": 1070
        },
        "contract_durability": {
          "compile_link": 1071,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1072,
            "status": "PASS"
          },
          "binary_nm": 1073
        },
        "contract_backend_availability": {
          "compile_link": 1074,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1075,
            "status": "PASS"
          },
          "binary_nm": 1076
        }
      },
      "C508510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1458,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1459,
            "status": "PASS"
          },
          "binary_nm": 1460,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1461,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1462,
            "status": "PASS"
          },
          "binary_nm": 1463
        },
        "w03_external_loop": {
          "compile_link": 1464,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1465,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1466,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1467,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1468,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1469,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1470
        },
        "contract_request_lifetime": {
          "compile_link": 1471,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1472,
            "status": "PASS"
          },
          "binary_nm": 1473
        },
        "contract_admission": {
          "compile_link": 1474,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1475,
            "status": "PASS"
          },
          "binary_nm": 1476
        },
        "contract_cancel_effect": {
          "compile_link": 1477,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1478,
            "status": "PASS"
          },
          "binary_nm": 1479
        },
        "contract_observer": {
          "compile_link": 1480,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1481,
            "status": "PASS"
          },
          "binary_nm": 1482
        },
        "contract_progress": {
          "compile_link": 1483,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1484,
            "status": "PASS"
          },
          "binary_nm": 1485
        },
        "contract_shutdown": {
          "compile_link": 1486,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1487,
            "status": "PASS"
          },
          "binary_nm": 1488
        },
        "contract_durability": {
          "compile_link": 1489,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1490,
            "status": "PASS"
          },
          "binary_nm": 1491
        },
        "contract_backend_availability": {
          "compile_link": 1492,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1493,
            "status": "PASS"
          },
          "binary_nm": 1494
        }
      },
      "C508510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1536,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1537,
            "status": "PASS"
          },
          "binary_nm": 1538,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1539,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1540,
            "status": "PASS"
          },
          "binary_nm": 1541
        },
        "w03_external_loop": {
          "compile_link": 1542,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1543,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1544,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1545,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1546,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1547,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1548
        },
        "contract_request_lifetime": {
          "compile_link": 1549,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1550,
            "status": "PASS"
          },
          "binary_nm": 1551
        },
        "contract_admission": {
          "compile_link": 1552,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1553,
            "status": "PASS"
          },
          "binary_nm": 1554
        },
        "contract_cancel_effect": {
          "compile_link": 1555,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1556,
            "status": "PASS"
          },
          "binary_nm": 1557
        },
        "contract_observer": {
          "compile_link": 1558,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1559,
            "status": "PASS"
          },
          "binary_nm": 1560
        },
        "contract_progress": {
          "compile_link": 1561,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1562,
            "status": "PASS"
          },
          "binary_nm": 1563
        },
        "contract_shutdown": {
          "compile_link": 1564,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1565,
            "status": "PASS"
          },
          "binary_nm": 1566
        },
        "contract_durability": {
          "compile_link": 1567,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1568,
            "status": "PASS"
          },
          "binary_nm": 1569
        },
        "contract_backend_availability": {
          "compile_link": 1570,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1571,
            "status": "PASS"
          },
          "binary_nm": 1572
        }
      },
      "C508510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1614,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1615,
            "status": "PASS"
          },
          "binary_nm": 1616,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1617,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1618,
            "status": "PASS"
          },
          "binary_nm": 1619
        },
        "w03_external_loop": {
          "compile_link": 1620,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1621,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1622,
            "status": "PASS"
          },
          "binary_nm": 1623,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1624,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1625,
            "status": "PASS"
          },
          "binary_nm": 1626
        },
        "contract_request_lifetime": {
          "compile_link": 1627,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1628,
            "status": "PASS"
          },
          "binary_nm": 1629
        },
        "contract_admission": {
          "compile_link": 1630,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1631,
            "status": "PASS"
          },
          "binary_nm": 1632
        },
        "contract_cancel_effect": {
          "compile_link": 1633,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1634,
            "status": "PASS"
          },
          "binary_nm": 1635
        },
        "contract_observer": {
          "compile_link": 1636,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1637,
            "status": "PASS"
          },
          "binary_nm": 1638
        },
        "contract_progress": {
          "compile_link": 1639,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1640,
            "status": "PASS"
          },
          "binary_nm": 1641
        },
        "contract_shutdown": {
          "compile_link": 1642,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1643,
            "status": "PASS"
          },
          "binary_nm": 1644
        },
        "contract_durability": {
          "compile_link": 1645,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1646,
            "status": "PASS"
          },
          "binary_nm": 1647
        },
        "contract_backend_availability": {
          "compile_link": 1648,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1649,
            "status": "PASS"
          },
          "binary_nm": 1650
        }
      },
      "C508510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1692,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1693,
            "status": "PASS"
          },
          "binary_nm": 1694,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1695,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1696,
            "status": "PASS"
          },
          "binary_nm": 1697
        },
        "w03_external_loop": {
          "compile_link": 1698,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1699,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1700,
            "status": "PASS"
          },
          "binary_nm": 1701,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1702,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1703,
            "status": "PASS"
          },
          "binary_nm": 1704
        },
        "contract_request_lifetime": {
          "compile_link": 1705,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1706,
            "status": "PASS"
          },
          "binary_nm": 1707
        },
        "contract_admission": {
          "compile_link": 1708,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1709,
            "status": "PASS"
          },
          "binary_nm": 1710
        },
        "contract_cancel_effect": {
          "compile_link": 1711,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1712,
            "status": "PASS"
          },
          "binary_nm": 1713
        },
        "contract_observer": {
          "compile_link": 1714,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1715,
            "status": "PASS"
          },
          "binary_nm": 1716
        },
        "contract_progress": {
          "compile_link": 1717,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1718,
            "status": "PASS"
          },
          "binary_nm": 1719
        },
        "contract_shutdown": {
          "compile_link": 1720,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1721,
            "status": "PASS"
          },
          "binary_nm": 1722
        },
        "contract_durability": {
          "compile_link": 1723,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1724,
            "status": "PASS"
          },
          "binary_nm": 1725
        },
        "contract_backend_availability": {
          "compile_link": 1726,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1727,
            "status": "PASS"
          },
          "binary_nm": 1728
        }
      }
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
