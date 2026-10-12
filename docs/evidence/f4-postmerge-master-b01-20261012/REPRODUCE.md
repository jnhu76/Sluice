# F4 postmerge merged-master B-01 receipt — 2026-10-12

Status: CANDIDATE until this PR is independently reviewed, merged and the resulting master is rechecked. This is a scoped F4 package cut, not F5 full alignment.

MASTER_EXECUTION_SHA = c29523d0c54c7bc85e2d5e7db73887acb991308c
MASTER_TESTED_SHA = c29523d0c54c7bc85e2d5e7db73887acb991308c
MASTER_TREE_SHA = 769dadfaa3dc134d5f70024ea1a462288ed0fa56
MASTER_PRODUCTION_INPUT_HASH = 6f65ff6870e333ae5416f7356752ee361241b5b9bc522b9104ca3519b87e6d40
MASTER_TEST_INPUT_HASH = c86b53760c22ee3445ccd4b696fe00fbd5177d004e4c681484d34b5053df228c
MASTER_BUILD_INPUT_HASH = 2d982a5f6ff1c8f37d994c9487f354d350d33d891229f30c818a702cef55e8e2

| Profile | Mode | Core / async / headers | Installed content SHA256 |
|---|---|---|---|
| no-liburing | debug | 2 / 33 / 56 | `1d311a4ac6b676825c65b1dd3cdcbda50a6457a1f2063683042637bbf4b7a206` |
| no-liburing | release | 2 / 33 / 56 | `91cf1f1f85cfbdace32d1d71c14cf4bb3708ef9f7b682cc2ed5eb36532307af9` |
| real-liburing | debug | 2 / 33 / 56 | `a1d89cb724f422686c38bd744dcce2b4969ee0da68cd57de447f5f8cc28e9a60` |
| real-liburing | release | 2 / 33 / 56 | `b9a8e8870a91dbd115548ff7eb22d066f3fe913feca195460cdd40c77fc97705` |

All four freshly built/installed packages run W01 core-only, the actual #511-fixed master W02 consumer, W03 ThreadPool and real-uring, eight retained contract consumers, public closure and Future/WaitPolicy/default_wait_policy/Completion/RequestScope/host linkage. No-liburing uring is explicit UNAVAILABLE. W04 is candidate-only and unsupported. Both profiles have new actual Clang ASanUBSan/TSan runs; current GCC runtime linker failure is separately captured.

Every installed header is inventoried; archive ar/nm defined/undefined/SHA256 outputs, actual consumer link flags and compiler macro views are in raw logs. The 11 legacy source files are absent. Each retired interface has its own negative header result. Thirteen linkage-dependent surfaces use the same original B0 object for a positive old-library control and a negative new-master relink. Four header-only surfaces have no exclusive callable ABI. No legacy shim/resource authority was introduced.

| Surface | Header | Master installed missing / include rejection | ABI |
|---|---|---|---|
| F4-01 | `sluice/blocking_io_pool.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-02 | `sluice/detail/blocking_io_pool_impl.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-03 | `sluice/buffer.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-04 | `sluice/buffered_readable.hpp` | PASS in all four configurations | header-only, no exclusive callable ABI |
| F4-05 | `sluice/copy.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-06 | `sluice/copy_strategy.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-07 | `sluice/fault.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-08 | `sluice/file.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-09 | `sluice/io_context.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-10 | `sluice/iovec.hpp` | PASS in all four configurations | header-only, no exclusive callable ABI |
| F4-11 | `sluice/limit.hpp` | PASS in all four configurations | header-only, no exclusive callable ABI |
| F4-12 | `sluice/memory_io_context.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-13 | `sluice/observed.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-14 | `sluice/reader.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-15 | `sluice/sync.hpp` | PASS in all four configurations | header-only, no exclusive callable ABI |
| F4-16 | `sluice/wal.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |
| F4-17 | `sluice/writer.hpp` | PASS in all four configurations | PASS strong symbol / relink closure |

Historical experiment ab2cea2784db05be8b05b5a776c757856beb4395 / tree07c1a3d5c40207e1a519592b9d2e10333674f2ed is a distinct identity, never relabeled master. Exact production/test/build comparison is in input-comparison.json. Source/build equality can explain package closure; changed test inputs remain explicitly different and the current consumers were run independently. Historical F1/F3/F4 files and original W02 UAF/race traces remain unchanged.

Owner Clean Break records, already effective for the same enumerated scope:
- https://github.com/jnhu76/Sluice/pull/508#issuecomment-6123532317
- https://github.com/jnhu76/Sluice/pull/510#issuecomment-6123534897

EXTERNAL_USE remains UNKNOWN. This receipt requests no repeated Phase M or 17-header risk decision. TAIL CLI has a separate #474/#513 gate. #475 retained mechanisms, shared posix_retry/file_semantics and canonical File/Request ownership remain.

Raw archive: `381f94469185b64f00ce3c4fcaf3867b72531c393f0ad049bdaacc8613070b68.tar.gz`; SHA256 `381f94469185b64f00ce3c4fcaf3867b72531c393f0ad049bdaacc8613070b68`; 1725 indexed files. Independent technical ACCEPT at code92463a2e2bbedf28babb50a3fc4d54079b1db7d3 is in independent-review.md; this is a separate agent review, not a forged human/GitHub approval.

```sh
python3 scripts/verify_f4_postmerge.py --source /absolute/new-master-worktree --master-sha <actual-full-master-SHA> --output /absolute/nonexistent-output
python3 scripts/verify_f4_postmerge.py --verify-evidence /absolute/evidence
python3 tests/package/f4_retirement/test_postmerge.py /absolute/evidence
```

The actual first execution producer was8999d180872dbc7465b669d7dc8d88958f0b383c. Stronger verification adds source snapshots and metadata sealing only; no new execution is claimed from sealing. All 21 omission/tampering controls now reject. Raw command IDs, stdout/stderr, exit codes and digests bind each PASS. Contents include installed binaries/headers and exact inputs. Postmerge adoption is published only after actual new-master recheck; no V1_RELEASE_READY claim.
