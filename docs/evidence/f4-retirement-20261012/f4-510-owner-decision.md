# F0 §1.7 owner-review packet — #510: Legacy File/Reader/Writer/stream

Status: technical candidate; human owner PENDING. External use UNKNOWN for every row.

Measured PR HEAD: `0529c2b6f8f221aab3112c963ee0491a632229f8`. Raw evidence SHA256: `d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38`.
Execution baseline: `f3521b45ead3963162e518758ac1820004adcfc2`; H0: `a38df5e9a7bee3603a439857f036de2b5a136bf2`.
Combined tested SHA: `ab2cea2784db05be8b05b5a776c757856beb4395`; tree: `07c1a3d5c40207e1a519592b9d2e10333674f2ed`.

Authority: root v1-r4 PROD-03/W-01–W-03/MIG-01/02/03; F0 §1.7/1.8; adopted Phase M; #475 HOLD/KEEP.

H0 contains all 17 headers in its public source include tree. Its libraries have no explicit header installation rules. The H0 API reference calls selected APIs stable-ish and requires deliberate deprecation; README calls the software experimental. Publication does not give every header the same stability promise. Source, API reference, README and rules are preserved verbatim. The pool implementation fragment fails standalone include but compiles through its public parent; a direct external include still breaks on removal.

No compatibility shim is added. Canonical per-operation blocking I/O is not a generic task pool or stream-product replacement. Existing external source consumers must change includes, types and calls; previously compiled consumers may fail to relink. A static executable already linked does not automatically lose its copied code. No zero-external-users claim is made.

The existing W-02 example contained a real borrow-lifetime defect. Original ASan UAF and TSan race diagnostics are retained. #511 moves buffers before RequestScope; #512 uses that corrected consumer against the exact unchanged candidate library trees. Adoption of the consumer correction is a separate evidence prerequisite. W-04 remains OPTIONAL_CANDIDATE and support is NO.

| Surface | Header | Retired API | Migration or no equivalent | Proposed policy | Owner |
|---|---|---|---|---|---|
| F4-03 | `sluice/buffer.hpp` | BufferedReader, BufferedWriter, read_some, consume_buffered, write_some, flush | no v1 stream buffering product | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-04 | `sluice/buffered_readable.hpp` | BufferedReadable, peek_buffered, consume_buffered | no v1 stream peek/consume equivalent | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-05 | `sluice/copy.hpp` | CopyOptions, CopyDecision, copy_all | application may loop canonical operations; no stream copy adapter | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-06 | `sluice/copy_strategy.hpp` | CopyStrategy, CopyOptions, CopyDecision, to_string | no v1 copy strategy product | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-07 | `sluice/fault.hpp` | MemoryReader, MemoryWriter, FaultPlan, FaultReader, FaultWriter, read_some, write_some, flush | test doubles unadmitted; no installed replacement | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-08 | `sluice/file.hpp` | FileReader, FileWriter, open, close, read_some, read_at, write_some, write_at, read_vec, write_vec, sync_data, sync_all | file_resource.hpp + blocking/file.hpp partially replace scalar resource I/O; vectored/stream excluded | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-09 | `sluice/io_context.hpp` | IoContext, BlockingIoContext, OpenReaderOptions, OpenWriterOptions, open_reader, open_writer | canonical File open plus AsyncIoContext request submission; no stream factory equivalent | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-10 | `sluice/iovec.hpp` | IoSlice, ConstIoSlice | vectored product excluded; no v1 equivalent | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-11 | `sluice/limit.hpp` | CopyLimit, unlimited, bytes, nothing, kind, remaining | no v1 copy limit product | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-12 | `sluice/memory_io_context.hpp` | MemoryIoContext, seed, open_reader, open_writer | test double excluded; no installed equivalent | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-13 | `sluice/observed.hpp` | ReaderStats, WriterStats, ObservedReader, ObservedWriter, read_some, read_vec, write_some, write_vec, flush | no v1 stream observation adapter | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-14 | `sluice/reader.hpp` | Reader, read_some, read_exact, stream_to, read_vec, read_vec_all | canonical scalar reads only; generic stream abstraction excluded | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-15 | `sluice/sync.hpp` | SyncableWriter, sync_data, sync_all | canonical blocking/request file sync partially replaces file durability; generic writer interface excluded | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-16 | `sluice/wal.hpp` | wal::WalWriter, write_record, write_record_vec, read_record, flush, sync, written_lsn, flushed_lsn, durable_lsn | no v1 WAL codec or LSN product | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |
| F4-17 | `sluice/writer.hpp` | Writer, write_some, write_all, flush, write_vec, write_all_vec | canonical scalar writes only; generic stream abstraction excluded | ACCEPT_BREAKAGE_NOW (recommendation) | PENDING |

Functional relationships: native FileReader/FileWriter resource ownership and Reader/Writer polymorphic stream calls retire together; IoContext/BlockingIoContext factories and MemoryIoContext leave with that stream closure. Copy/Buffer strategies and peek/consume buffers have no supplied v1 stream adapter. WAL codec/LSN tracking has no v1 equivalent. Fault/observed wrappers and memory doubles lose installed exposure. IoSlice/ConstIoSlice, CopyLimit and SyncableWriter auxiliary types also disappear. Canonical File plus blocking/file.hpp and AsyncIoContext offer partial scalar-file and durability migration only. Vectored/generic-stream operations are excluded. Removed sluice/async/file.hpp is not a migration target.

## Per-interface machine records

These rows bind baseline compile/link/behavior witnesses and candidate negative probes to profile/mode, command IDs, exit codes and log hashes in evidence.json. Header-only rows explicitly have no exclusive callable ABI. Group strong-symbol audits and real baseline-object negative relinks cover compiled legacy callables.

```json
[
  {
    "SURFACE_ID": "F4-03",
    "HEADER": "sluice/buffer.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "27902c6340acf69bc39abf687ac8e8f7214f1f4446ccf97b42c3b7383776c161",
      "standalone": 50,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/buffered_readable.hpp",
        "sluice/reader.hpp",
        "sluice/writer.hpp",
        "sluice/measurement.hpp"
      ],
      "types": [
        "BufferedReader",
        "BufferedWriter"
      ],
      "callables": [
        "read_some",
        "consume_buffered",
        "write_some",
        "flush"
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
      "command": 1835,
      "matches": "src/buffer.cpp:1:#include <sluice/buffer.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "no v1 stream buffering product",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-04",
    "HEADER": "sluice/buffered_readable.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "bea0bc780b9d539ceb514a53135b9a0af7fea11fc05e8e2badd3e64a7ea6d87b",
      "standalone": 51,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/result.hpp"
      ],
      "types": [
        "BufferedReadable"
      ],
      "callables": [
        "peek_buffered",
        "consume_buffered"
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
      "command": 1836,
      "matches": "include/sluice/buffer.hpp:3:#include <sluice/buffered_readable.hpp>\nsrc/copy.cpp:2:#include <sluice/buffered_readable.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": "HEADER_ONLY_NO_EXCLUSIVE_CALLABLE_ABI",
    "RETAINED_CANONICAL_WORKLOAD": {
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "no v1 stream peek/consume equivalent",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-05",
    "HEADER": "sluice/copy.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "c462ef05c2fb7289e5ff8435993cd2806cf4b2e202a29839f03647901920cabd",
      "standalone": 52,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/copy_strategy.hpp",
        "sluice/limit.hpp",
        "sluice/measurement.hpp",
        "sluice/reader.hpp",
        "sluice/writer.hpp"
      ],
      "types": [
        "CopyOptions",
        "CopyDecision"
      ],
      "callables": [
        "copy_all"
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
      "command": 1837,
      "matches": "src/copy.cpp:1:#include <sluice/copy.hpp>\nsrc/reader.cpp:3:#include <sluice/copy.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "application may loop canonical operations; no stream copy adapter",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-06",
    "HEADER": "sluice/copy_strategy.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "8e5d563e3960a78a2f152b031f2eb8ecb1a20945a371560f3c4f7f05e1842a66",
      "standalone": 53,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/limit.hpp"
      ],
      "types": [
        "CopyStrategy",
        "CopyOptions",
        "CopyDecision"
      ],
      "callables": [
        "to_string"
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
      "command": 1838,
      "matches": "include/sluice/copy.hpp:3:#include <sluice/copy_strategy.hpp>\nsrc/copy_strategy.cpp:1:#include <sluice/copy_strategy.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "no v1 copy strategy product",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-07",
    "HEADER": "sluice/fault.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "a006e8008e6f121675d9f690594ceef0c61715eddbacb17ff52d2f87e489384f",
      "standalone": 54,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/reader.hpp",
        "sluice/writer.hpp"
      ],
      "types": [
        "MemoryReader",
        "MemoryWriter",
        "FaultPlan",
        "FaultReader",
        "FaultWriter"
      ],
      "callables": [
        "read_some",
        "write_some",
        "flush"
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
      "command": 1839,
      "matches": "include/sluice/memory_io_context.hpp:3:#include <sluice/fault.hpp>\nsrc/fault.cpp:1:#include <sluice/fault.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "test doubles unadmitted; no installed replacement",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-08",
    "HEADER": "sluice/file.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "5517918d18d5c09a2c09bcb35b91a64d6c8af2f40e4e707f5fed5c769adc5bee",
      "standalone": 55,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/measurement.hpp",
        "sluice/reader.hpp",
        "sluice/sync.hpp",
        "sluice/writer.hpp"
      ],
      "types": [
        "FileReader",
        "FileWriter"
      ],
      "callables": [
        "open",
        "close",
        "read_some",
        "read_at",
        "write_some",
        "write_at",
        "read_vec",
        "write_vec",
        "sync_data",
        "sync_all"
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
      "command": 1840,
      "matches": "src/file.cpp:1:#include <sluice/file.hpp>\nsrc/io_context.cpp:2:#include <sluice/file.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "file_resource.hpp + blocking/file.hpp partially replace scalar resource I/O; vectored/stream excluded",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-09",
    "HEADER": "sluice/io_context.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "fc55ad46bb38e2f0d87d351eb2718859af9ed7d60b30ed19cf18b0ae8fb7f2ad",
      "standalone": 56,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/measurement.hpp",
        "sluice/reader.hpp",
        "sluice/result.hpp",
        "sluice/writer.hpp"
      ],
      "types": [
        "IoContext",
        "BlockingIoContext",
        "OpenReaderOptions",
        "OpenWriterOptions"
      ],
      "callables": [
        "open_reader",
        "open_writer"
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
      "command": 1841,
      "matches": "include/sluice/memory_io_context.hpp:4:#include <sluice/io_context.hpp>\nsrc/io_context.cpp:1:#include <sluice/io_context.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "canonical File open plus AsyncIoContext request submission; no stream factory equivalent",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-10",
    "HEADER": "sluice/iovec.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "44ff89ded6989797480414d71546da448a55bfedd9c5e6d12c573fc3ebe809f5",
      "standalone": 57,
      "standalone_status": "PASS",
      "public_includes": [],
      "types": [
        "IoSlice",
        "ConstIoSlice"
      ],
      "callables": [],
      "description": "See verbatim H0 API reference and README; no per-header uniform stability claim; H0 lacks package header installation rules"
    },
    "CURRENT_INSTALLED_FACT": {
      "B0/no-liburing/debug": true,
      "B0/no-liburing/release": true,
      "B0/real-liburing/debug": true,
      "B0/real-liburing/release": true
    },
    "CURRENT_IN_TREE_CONSUMER": {
      "command": 1842,
      "matches": "include/sluice/reader.hpp:3:#include <sluice/iovec.hpp>\ninclude/sluice/writer.hpp:3:#include <sluice/iovec.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": "HEADER_ONLY_NO_EXCLUSIVE_CALLABLE_ABI",
    "RETAINED_CANONICAL_WORKLOAD": {
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "vectored product excluded; no v1 equivalent",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-11",
    "HEADER": "sluice/limit.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "d63b17cfb7e8b6e26925e856f9f8f37c3c04bdbd31522c9702c273deba94c6e8",
      "standalone": 58,
      "standalone_status": "PASS",
      "public_includes": [],
      "types": [
        "CopyLimit"
      ],
      "callables": [
        "unlimited",
        "bytes",
        "nothing",
        "kind",
        "remaining"
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
      "command": 1843,
      "matches": "include/sluice/copy.hpp:4:#include <sluice/limit.hpp>\ninclude/sluice/copy_strategy.hpp:3:#include <sluice/limit.hpp>\ninclude/sluice/reader.hpp:4:#include <sluice/limit.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": "HEADER_ONLY_NO_EXCLUSIVE_CALLABLE_ABI",
    "RETAINED_CANONICAL_WORKLOAD": {
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "no v1 copy limit product",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-12",
    "HEADER": "sluice/memory_io_context.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "921d10881809968b5c158c6afcb1b2552b407a03e8af2483abc6e83a5f17c327",
      "standalone": 59,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/fault.hpp",
        "sluice/io_context.hpp",
        "sluice/result.hpp"
      ],
      "types": [
        "MemoryIoContext"
      ],
      "callables": [
        "seed",
        "open_reader",
        "open_writer"
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
      "command": 1844,
      "matches": ""
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "test double excluded; no installed equivalent",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-13",
    "HEADER": "sluice/observed.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "dea24e0b09c082e1f9930f4900a2399ea4dfdfc7fc308aa067074f15926df2f7",
      "standalone": 60,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/reader.hpp",
        "sluice/writer.hpp",
        "sluice/measurement.hpp"
      ],
      "types": [
        "ReaderStats",
        "WriterStats",
        "ObservedReader",
        "ObservedWriter"
      ],
      "callables": [
        "read_some",
        "read_vec",
        "write_some",
        "write_vec",
        "flush"
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
      "command": 1845,
      "matches": "src/observed.cpp:1:#include <sluice/observed.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "no v1 stream observation adapter",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-14",
    "HEADER": "sluice/reader.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "98b2a4741a21412198f10d899b1ed2c84a83cbb154e3f57af731592ea02dbc6f",
      "standalone": 61,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/iovec.hpp",
        "sluice/limit.hpp",
        "sluice/measurement.hpp",
        "sluice/result.hpp"
      ],
      "types": [
        "Reader"
      ],
      "callables": [
        "read_some",
        "read_exact",
        "stream_to",
        "read_vec",
        "read_vec_all"
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
      "command": 1846,
      "matches": "include/sluice/buffer.hpp:4:#include <sluice/reader.hpp>\ninclude/sluice/copy.hpp:6:#include <sluice/reader.hpp>\ninclude/sluice/fault.hpp:3:#include <sluice/reader.hpp>\ninclude/sluice/file.hpp:4:#include <sluice/reader.hpp>\ninclude/sluice/io_context.hpp:4:#include <sluice/reader.hpp>\ninclude/sluice/observed.hpp:3:#include <sluice/reader.hpp>\ninclude/sluice/wal.hpp:3:#include <sluice/reader.hpp>\nsrc/reader.cpp:1:#include <sluice/reader.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "canonical scalar reads only; generic stream abstraction excluded",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-15",
    "HEADER": "sluice/sync.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "14bfa82f0efc73213536ec29dbf1c720f6ee374cfeba2e0d0b9f7bff3be5bf21",
      "standalone": 62,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/result.hpp"
      ],
      "types": [
        "SyncableWriter"
      ],
      "callables": [
        "sync_data",
        "sync_all"
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
      "command": 1847,
      "matches": "include/sluice/file.hpp:5:#include <sluice/sync.hpp>\ninclude/sluice/wal.hpp:4:#include <sluice/sync.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": "HEADER_ONLY_NO_EXCLUSIVE_CALLABLE_ABI",
    "RETAINED_CANONICAL_WORKLOAD": {
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "canonical blocking/request file sync partially replaces file durability; generic writer interface excluded",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-16",
    "HEADER": "sluice/wal.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "37f5c78335a8d97492044334cbb376025c0e1bf0b94d0121e23cdfe7aeffc300",
      "standalone": 63,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/reader.hpp",
        "sluice/sync.hpp",
        "sluice/writer.hpp",
        "sluice/result.hpp"
      ],
      "types": [
        "wal::WalWriter"
      ],
      "callables": [
        "write_record",
        "write_record_vec",
        "read_record",
        "flush",
        "sync",
        "written_lsn",
        "flushed_lsn",
        "durable_lsn"
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
      "command": 1848,
      "matches": "src/wal.cpp:1:#include <sluice/wal.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "no v1 WAL codec or LSN product",
    "BREAKAGE_CONSEQUENCE": "source include and named types/callables cease to compile; existing linked legacy consumers may fail to relink",
    "PROPOSED_POLICY": "ACCEPT_BREAKAGE_NOW",
    "TECHNICAL_VERDICT": "TECHNICALLY_SAFE_FOR_REQUIRED_V1_SCOPE",
    "HUMAN_OWNER_VERDICT": "PENDING"
  },
  {
    "SURFACE_ID": "F4-17",
    "HEADER": "sluice/writer.hpp",
    "V001_PUBLIC_FACT": {
      "present": true,
      "source_sha256": "0f5fe68b190c06bfd75a943707d2028c45297a8ec2c761aa82530122eff1b66b",
      "standalone": 64,
      "standalone_status": "PASS",
      "public_includes": [
        "sluice/iovec.hpp",
        "sluice/result.hpp"
      ],
      "types": [
        "Writer"
      ],
      "callables": [
        "write_some",
        "write_all",
        "flush",
        "write_vec",
        "write_all_vec"
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
      "command": 1849,
      "matches": "include/sluice/buffer.hpp:5:#include <sluice/writer.hpp>\ninclude/sluice/copy.hpp:7:#include <sluice/writer.hpp>\ninclude/sluice/fault.hpp:4:#include <sluice/writer.hpp>\ninclude/sluice/file.hpp:6:#include <sluice/writer.hpp>\ninclude/sluice/io_context.hpp:6:#include <sluice/writer.hpp>\ninclude/sluice/observed.hpp:4:#include <sluice/writer.hpp>\ninclude/sluice/wal.hpp:5:#include <sluice/writer.hpp>\nsrc/reader.cpp:2:#include <sluice/writer.hpp>\nsrc/writer.cpp:1:#include <sluice/writer.hpp>\n"
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
      "C510/no-liburing/debug": true,
      "C510/no-liburing/release": true,
      "C510/real-liburing/debug": true,
      "C510/real-liburing/release": true,
      "C508510/no-liburing/debug": true,
      "C508510/no-liburing/release": true,
      "C508510/real-liburing/debug": true,
      "C508510/real-liburing/release": true
    },
    "CANDIDATE_API_COMPILE_REJECTED": {
      "C510/no-liburing/debug": "PASS",
      "C510/no-liburing/release": "PASS",
      "C510/real-liburing/debug": "PASS",
      "C510/real-liburing/release": "PASS",
      "C508510/no-liburing/debug": "PASS",
      "C508510/no-liburing/release": "PASS",
      "C508510/real-liburing/debug": "PASS",
      "C508510/real-liburing/release": "PASS"
    },
    "CANDIDATE_SYMBOL_ABSENT": {
      "C510/no-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/no-liburing/release": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/debug": {
        "symbol_audit": "PASS",
        "link_negative": "PASS"
      },
      "C510/real-liburing/release": {
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
      "C510/no-liburing/debug": {
        "w01_direct": {
          "compile_link": 1125,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1126,
            "status": "PASS"
          },
          "binary_nm": 1127,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1128,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1129,
            "status": "PASS"
          },
          "binary_nm": 1130
        },
        "w03_external_loop": {
          "compile_link": 1131,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1132,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1133,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1134,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1135,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1136,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1137
        },
        "contract_request_lifetime": {
          "compile_link": 1138,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1139,
            "status": "PASS"
          },
          "binary_nm": 1140
        },
        "contract_admission": {
          "compile_link": 1141,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1142,
            "status": "PASS"
          },
          "binary_nm": 1143
        },
        "contract_cancel_effect": {
          "compile_link": 1144,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1145,
            "status": "PASS"
          },
          "binary_nm": 1146
        },
        "contract_observer": {
          "compile_link": 1147,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1148,
            "status": "PASS"
          },
          "binary_nm": 1149
        },
        "contract_progress": {
          "compile_link": 1150,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1151,
            "status": "PASS"
          },
          "binary_nm": 1152
        },
        "contract_shutdown": {
          "compile_link": 1153,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1154,
            "status": "PASS"
          },
          "binary_nm": 1155
        },
        "contract_durability": {
          "compile_link": 1156,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1157,
            "status": "PASS"
          },
          "binary_nm": 1158
        },
        "contract_backend_availability": {
          "compile_link": 1159,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1160,
            "status": "PASS"
          },
          "binary_nm": 1161
        }
      },
      "C510/no-liburing/release": {
        "w01_direct": {
          "compile_link": 1210,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1211,
            "status": "PASS"
          },
          "binary_nm": 1212,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1213,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1214,
            "status": "PASS"
          },
          "binary_nm": 1215
        },
        "w03_external_loop": {
          "compile_link": 1216,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1217,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1218,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1219,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1220,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1221,
            "status": "UNAVAILABLE"
          },
          "binary_nm": 1222
        },
        "contract_request_lifetime": {
          "compile_link": 1223,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1224,
            "status": "PASS"
          },
          "binary_nm": 1225
        },
        "contract_admission": {
          "compile_link": 1226,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1227,
            "status": "PASS"
          },
          "binary_nm": 1228
        },
        "contract_cancel_effect": {
          "compile_link": 1229,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1230,
            "status": "PASS"
          },
          "binary_nm": 1231
        },
        "contract_observer": {
          "compile_link": 1232,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1233,
            "status": "PASS"
          },
          "binary_nm": 1234
        },
        "contract_progress": {
          "compile_link": 1235,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1236,
            "status": "PASS"
          },
          "binary_nm": 1237
        },
        "contract_shutdown": {
          "compile_link": 1238,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1239,
            "status": "PASS"
          },
          "binary_nm": 1240
        },
        "contract_durability": {
          "compile_link": 1241,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1242,
            "status": "PASS"
          },
          "binary_nm": 1243
        },
        "contract_backend_availability": {
          "compile_link": 1244,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1245,
            "status": "PASS"
          },
          "binary_nm": 1246
        }
      },
      "C510/real-liburing/debug": {
        "w01_direct": {
          "compile_link": 1295,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1296,
            "status": "PASS"
          },
          "binary_nm": 1297,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1298,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1299,
            "status": "PASS"
          },
          "binary_nm": 1300
        },
        "w03_external_loop": {
          "compile_link": 1301,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1302,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1303,
            "status": "PASS"
          },
          "binary_nm": 1304,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1305,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1306,
            "status": "PASS"
          },
          "binary_nm": 1307
        },
        "contract_request_lifetime": {
          "compile_link": 1308,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1309,
            "status": "PASS"
          },
          "binary_nm": 1310
        },
        "contract_admission": {
          "compile_link": 1311,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1312,
            "status": "PASS"
          },
          "binary_nm": 1313
        },
        "contract_cancel_effect": {
          "compile_link": 1314,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1315,
            "status": "PASS"
          },
          "binary_nm": 1316
        },
        "contract_observer": {
          "compile_link": 1317,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1318,
            "status": "PASS"
          },
          "binary_nm": 1319
        },
        "contract_progress": {
          "compile_link": 1320,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1321,
            "status": "PASS"
          },
          "binary_nm": 1322
        },
        "contract_shutdown": {
          "compile_link": 1323,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1324,
            "status": "PASS"
          },
          "binary_nm": 1325
        },
        "contract_durability": {
          "compile_link": 1326,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1327,
            "status": "PASS"
          },
          "binary_nm": 1328
        },
        "contract_backend_availability": {
          "compile_link": 1329,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1330,
            "status": "PASS"
          },
          "binary_nm": 1331
        }
      },
      "C510/real-liburing/release": {
        "w01_direct": {
          "compile_link": 1380,
          "compile_link_status": "PASS",
          "source_sha256": "623f5fab9e9d9d11c5ea9770eb3693fc9b6d0347428610226dad6526a8cb9da2",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w01_direct.cpp",
          "runtime": {
            "command": 1381,
            "status": "PASS"
          },
          "binary_nm": 1382,
          "no_scheduler_fiber": "PASS"
        },
        "w02_pipeline": {
          "compile_link": 1383,
          "compile_link_status": "PASS",
          "source_sha256": "61377266063272582612f8a9c8623048ede25452e00f4483da62a1242590d986",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w02_pipeline.cpp",
          "runtime": {
            "command": 1384,
            "status": "PASS"
          },
          "binary_nm": 1385
        },
        "w03_external_loop": {
          "compile_link": 1386,
          "compile_link_status": "PASS",
          "source_sha256": "3334308e4e2092c9a2173a25e70d0939750e7a532eecd1920c32bd56db94ce56",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w03_external_loop.cpp",
          "threadpool": {
            "command": 1387,
            "status": "PASS"
          },
          "non_busy_progress": "PASS",
          "uring": {
            "command": 1388,
            "status": "PASS"
          },
          "binary_nm": 1389,
          "no_scheduler_fiber": "PASS"
        },
        "w04_stackful_candidate": {
          "compile_link": 1390,
          "compile_link_status": "PASS",
          "source_sha256": "279ab9dbf7845f323e39943fa5fee9076ba2147e85d87bb4e388ad78be9f81a4",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/w04_stackful_candidate.cpp",
          "runtime": {
            "command": 1391,
            "status": "PASS"
          },
          "binary_nm": 1392
        },
        "contract_request_lifetime": {
          "compile_link": 1393,
          "compile_link_status": "PASS",
          "source_sha256": "0d3fc4cfe6b7a518e3ad2d37ea5160e46460f7e358bce9354d64e0ef3af1809a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_request_lifetime.cpp",
          "runtime": {
            "command": 1394,
            "status": "PASS"
          },
          "binary_nm": 1395
        },
        "contract_admission": {
          "compile_link": 1396,
          "compile_link_status": "PASS",
          "source_sha256": "192fe5360f25ab0fb9817a68d9ae5deee4f0ab7897a4eac0edf7f19820fd552a",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_admission.cpp",
          "runtime": {
            "command": 1397,
            "status": "PASS"
          },
          "binary_nm": 1398
        },
        "contract_cancel_effect": {
          "compile_link": 1399,
          "compile_link_status": "PASS",
          "source_sha256": "8c36908ab2e02e3477189c0ad12dc0be2d265a5156261fac39ee8ff16c5a25a6",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_cancel_effect.cpp",
          "runtime": {
            "command": 1400,
            "status": "PASS"
          },
          "binary_nm": 1401
        },
        "contract_observer": {
          "compile_link": 1402,
          "compile_link_status": "PASS",
          "source_sha256": "987f5bc70e9a518f258a28538aeb025f6db27fb2c99c265f48cf467dd5a3c74e",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_observer.cpp",
          "runtime": {
            "command": 1403,
            "status": "PASS"
          },
          "binary_nm": 1404
        },
        "contract_progress": {
          "compile_link": 1405,
          "compile_link_status": "PASS",
          "source_sha256": "4bf7ef296bdc48f86e2b0793df44b3868352810c9f92b035990d5a277d5270c1",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_progress.cpp",
          "runtime": {
            "command": 1406,
            "status": "PASS"
          },
          "binary_nm": 1407
        },
        "contract_shutdown": {
          "compile_link": 1408,
          "compile_link_status": "PASS",
          "source_sha256": "b9b7f8283e1e40c56f370538c10f5f82eeb55e1f6a9a25d29880377164d07979",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_shutdown.cpp",
          "runtime": {
            "command": 1409,
            "status": "PASS"
          },
          "binary_nm": 1410
        },
        "contract_durability": {
          "compile_link": 1411,
          "compile_link_status": "PASS",
          "source_sha256": "eb81e243e3f7a70d252ab36641437cb2c997f17022187a91fe6fb52cbb11c594",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_durability.cpp",
          "runtime": {
            "command": 1412,
            "status": "PASS"
          },
          "binary_nm": 1413
        },
        "contract_backend_availability": {
          "compile_link": 1414,
          "compile_link_status": "PASS",
          "source_sha256": "c7348c85e118f678f45e4fc4002c8af096dfaceed20f407195207439a61b7e33",
          "source_origin": "/home/jnhu/f4-retirement-verifier-20261012/worktrees/tool/tests/package/consumers/contract_backend_availability.cpp",
          "runtime": {
            "command": 1415,
            "status": "PASS"
          },
          "binary_nm": 1416
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
    "MIGRATION_OR_NO_EQUIVALENT": "canonical scalar writes only; generic stream abstraction excluded",
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
DECISION_SCOPE = PR #510; enumerated surfaces F4-03, F4-04, F4-05, F4-06, F4-07, F4-08, F4-09, F4-10, F4-11, F4-12, F4-13, F4-14, F4-15, F4-16, F4-17
REVIEWED_PR_HEAD = <full final PR SHA actively reviewed>
REVIEWED_EVIDENCE_SHA256 = d4f58f1230caa764c87300ebaf299617edf8adb54c8d0c33cd468f56272efa38
ACCEPTED_BREAKAGE_SURFACES = <enumerate per-row accepted surfaces>
DEPRECATION_POLICY = <per-row ACCEPT_BREAKAGE_NOW / BOUNDED_DEPRECATION_WINDOW / BLOCK_AND_REVISE>
MIGRATION_DISCLOSURE = <confirm each migration/no-equivalent and historical exposure consequence>
APPROVED_FOR_SCOPED_REMOVAL = <human decision; presently PENDING>
```
