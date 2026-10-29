# #575 Native E5 Atomic-RMW Research — Evidence Archive Manifest

Evidence-only archive of the closed, unmerged research campaign for
[#575](https://github.com/jnhu76/Sluice/issues/575): whether the two
test-only atomic RMW counters (`ThreadPoolBackend::syscall_count_`,
`UringAsyncBackend::submit_flushes_`) should be removed from Release.

| Field | Value |
|---|---|
| Source branch | `research/575-native-e5-atomic-rmw` (origin; untouched by this PR) |
| Source HEAD SHA | `e9baafb9d117cbaf387dde9f4433ebbacb4a73b3` |
| Source PR | #584 "research(575): native E5 atomic RMW elimination evidence — NO_CHANGE_JUSTIFIED" — state `CLOSED`, `mergedAt` null (verified 2026-10-30 via `gh pr view 584 --json state,mergedAt`); no product code ever landed from it |
| Campaign product baseline | `3df10dc44d84799cccb18d29f20149d2983137ec` (v0.0.3 master) |
| Execution host | E5 (Intel Xeon E5-2666 v3, Fedora 44, GCC 16.2.1), per R0 |
| Verdict | `THREADPOOL_VERDICT = NO_CHANGE_JUSTIFIED`, `URING_VERDICT = NO_CHANGE_JUSTIFIED`, `ISSUE_575_VERDICT = NO_CHANGE_JUSTIFIED`, `PRODUCT_CHANGE_AUTHORIZED = NO`, `MERGED = NO` (see `VERDICT.md`) |

## Scope decision

This PR archives research evidence only. It adds no product source, build
or test changes and touches nothing outside this directory. The two
product-tree changes that existed on the research branch are **not**
replayed here: the UR positive-count oracle found by the campaign was
already productized on master as a test-only PR (#585, squash-merged as
`1df8b4b82f9078dab824a4559684c40503d3e2cd`), and the matching
`xmake/tests.lua` registration is already on master
(`xmake/tests.lua:578`). Whether the dead counters themselves are removed
from production remains an owner decision (see `VERDICT.md`, "What remains
an owner decision"); this archive neither merges nor forecloses it.

## Included evidence (110 files, sha256 below)

Categories: research documents and decision records (R0–R9, README,
VERDICT, campaign `manifest.json`); codegen receipts (`assembly/base/`:
optimized GIMPLE, objdump, linked-binary lock-add site census, baseline
binary sha256 record); arm/mutation records (`patches/`: the
production-code component of each measured arm plus mutant and control
diffs); raw measurement receipts (`raw/`: per-run `provenance.txt` with
per-arm binary sha256/kernel/compiler/CPU/governor, `ab.jsonl`, `d8.jsonl`,
`meta.jsonl`, `perf.csv`, and parity-suite result records).

Every archived file is byte-identical to the source-branch blob; 109 of
110 additionally match the campaign's own `manifest.json` inventory
(`manifest.json` cannot list itself). The four `manifest.json` entries not
present under this directory are exactly the excluded harness scripts
listed below.

| Path (relative to this directory) | SHA-256 |
|---|---|
| `R0-IDENTITY.md` | `309dd8ee480c58a76f253fcf17bfe91d261d49b4586cbc09ca7470ae24041c5b` |
| `R1-AUTHORITY-AND-DAG.md` | `281aae6f7a87592dabbb86e2d53b881b3715cec567b904f14512f66166093bca` |
| `R2-BASELINE-CODEGEN.md` | `a871b60e8806c7fe1a34fcacf847faee3c59bfb8c08d8b8a2d1cefd017028a33` |
| `R3-ARMS.md` | `6ed6f06c3e851722a2dc244466776b799c5a2bf0c803f346248e8a194467ca8b` |
| `R4-CORRECTNESS.md` | `83f398cff8a8c0f046ea603c4a61ec2fa501208f9b1ac9dbef7057a71335243c` |
| `R4-MUTATION-RECEIPTS.md` | `e4b983f32c8bc1c5a9a84364510cb14983a9c4475ea4d6b400812579fc2238c7` |
| `R5-CALIBRATION.md` | `1d04040d436de8369550e06c3f80b2dce500d7dacd3e39ea7bce2e9fb7bc6dda` |
| `R6-THREADPOOL.md` | `f1246de4d7498eaaebf86d5cc2910fa4c3ab710e825f55994fba46fde562bb49` |
| `R7-URING.md` | `3bc29107c7048ed0a374f29be33f9c01d58933c0d0fb6995fcbb3dfaee0b5500` |
| `R8-CAUSAL-ANALYSIS.md` | `a13b075146043766298d7b1e67decea6dad137610b987415bdf1072385b2a9f2` |
| `R9-REVIEWS.md` | `7965c4d23a2afdef58c5e9996ff79dee23c221625f582dd5e3d7b8897d9417ed` |
| `README.md` | `3c504cb3588f5233e5cc5f4292f4474d518fa50968f7d987b73bac01ce7b39bc` |
| `VERDICT.md` | `2e1614cdbd9058f07ef311912b000574b2903baa1c1791e9c75d3175d95a8194` |
| `assembly/base/gimple-threadpool_backend.worker_loop.optimized` | `4162dd6dde5303affb4d96c425b73a8b5d8ecc007ad2c38113432f1a6dad94f2` |
| `assembly/base/gimple-uring_transport.submit_transport_locked.optimized` | `a2f5daa27ae9e50598de370838ed6eaa80982d2c11df844260da9d0cf269ccec` |
| `assembly/base/linked-bench_w02-lockadd-sites.txt` | `912f46e449a55b5fa723d5a59931e97990bfe44a3647bfae00451a8b7f897d72` |
| `assembly/base/objdump-threadpool_backend.release.txt` | `02610481f375cb3d20b38043d296b14a4b308c2f5757c020442117ac8564ff7d` |
| `assembly/base/objdump-uring_transport.release.txt` | `caed922c1f0a318c7e76f6e765cb6fc2dd3e9e78171df43519e4c7be7ff7ba63` |
| `assembly/base/sha256-baseline.txt` | `621707d88eb4c12fcd4c960e883a5b6dbea7ec23f5a210afe61696756ee051b7` |
| `manifest.json` | `03364b0009cbdaa569ac6e9f7f77087e92a3ed8a539074c3213c780e4ae04588` |
| `patches/arm-identity.txt` | `6f171b3167859194b264b588353560119417fb6b9d77c6b6cc1e1ba2223c4275` |
| `patches/mutant-tp-m1.diff` | `960455a3e541a2b96ce6165ad1d3ae1ac5f5f3829102b881578e9ccc047af5ce` |
| `patches/mutant-ur-m1.diff` | `5bdd0875ff19389d0afe2f5210005f9b4e2f66c391d0bcc0ee3e086331488311` |
| `patches/tp-ctrl-plain-increment.diff` | `907184536b1a6bd18352933ac8ed9be049163bd8d1221b12e003f79980fc82d8` |
| `patches/tp-full.diff` | `4bd2a67df73cd3eeab32b20522e0f5bf2c712341281c5d682720b6493ccb2c1b` |
| `patches/tp-oponly.diff` | `74b07bc9488e0352d782e1489fb8fd1e496dd9d72986bda622bd58448cd8fd23` |
| `patches/ur-ctrl-plain-increment.diff` | `0259c8bc2ced5e54ce5c62b337576f68584ed4296021aa5ce2265dd86ac25195` |
| `patches/ur-full.diff` | `37ba7460a013d23cfbf4898d660e2254e265eaa5394fc6b0d3c2c85f50e9ab00` |
| `patches/ur-oponly.diff` | `4ac343cfc178ae6acab0f5ca5bd90e48774c639dbe9248dc6e16abfc76aafe27` |
| `raw/ab/ab-tp/ab.jsonl` | `72e635f58eb952ce6d2d01fb3adbe3d54ceb9aaee4b913f170e97d7aef11b325` |
| `raw/ab/ab-tp/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-tp/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-tp/provenance.txt` | `8113ad5a69e5db549a7669d574eee097a8b06672aa043f5b257021003da72cb7` |
| `raw/ab/ab-tp-attrib/ab.jsonl` | `936141af5022837f8aa7f0079238a24a6492e6165f17e6c144a92ef06ad3681a` |
| `raw/ab/ab-tp-attrib/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-tp-attrib/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-tp-attrib/provenance.txt` | `0643a3a2de61cf5737a8b5bb4293cd66c5791b35bdc3e5542279339fd0a72a28` |
| `raw/ab/ab-tp-stats/ab.jsonl` | `5755b3a4c8c27645a5d6c12946ccd629208e5a313ee656e339b81b45001f1cbd` |
| `raw/ab/ab-tp-stats/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-tp-stats/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-tp-stats/provenance.txt` | `d06b4ccfc2a975cb54a01d896482304328a579df4952fe2002014f768b70e4f5` |
| `raw/ab/ab-ur/ab.jsonl` | `12065d4cdfbe367e3969868388b4a47f81059e4a4b13946bfbace62cf33dd49f` |
| `raw/ab/ab-ur/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-ur/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-ur/provenance.txt` | `784f4014c5862b775a3041b34cf6488ea3ab5e6041aa289d1c9d2670a3779240` |
| `raw/ab/ab-ur-attrib/ab.jsonl` | `0b08c0ef205cd26622f5ff1dc3f239e5616cc5b001fa7c5a1207a214ce5a7aa0` |
| `raw/ab/ab-ur-attrib/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-ur-attrib/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-ur-attrib/provenance.txt` | `92334e296b5dfeef5c0b8f0fd4461ec741ce23cbeb155c557ec3e0600fbc02b1` |
| `raw/ab/ab-ur-d8/d8.jsonl` | `b0ccaa14fcaffb704485e033bb53f916503be8e83a9f6b5a0b3cd54bd9bce53a` |
| `raw/ab/ab-ur-stats/ab.jsonl` | `63ceb7713f4f9cf6406b60919dc2dffcfed469abe9d9250bb9b47c1c33f91d9b` |
| `raw/ab/ab-ur-stats/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-ur-stats/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-ur-stats/provenance.txt` | `b2a09e61b08c6f02cfb07b1a6bd6c6b0e2af4d8f48d055c3da35b8272e9e3bba` |
| `raw/ab/ab-w01/ab.jsonl` | `4b49fc0b94b0f6a94034701505303d0268fe44b1f75d81f6e97826995b8d666d` |
| `raw/ab/ab-w01/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-w01/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/ab/ab-w01/provenance.txt` | `5ddeafe7a01edee05c92153da53a1a2db024f8b17ef848401130ce5fb0650cbd` |
| `raw/calibration/cal-aa-tp/ab.jsonl` | `ecc826a8bdf014fbd85b40121c003a17dfca5535313e332029fa236b7f9d0df6` |
| `raw/calibration/cal-aa-tp/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal-aa-tp/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal-aa-tp/provenance.txt` | `264d3c0014744b0d8d5290404d75582739c60d7e9c5fd1daab8b9a432f9e7a58` |
| `raw/calibration/cal-aa-ur/ab.jsonl` | `38b21eb9a2df52d31bab80788b894be9137932b7ff03aa6a8e22f6995c5a5b75` |
| `raw/calibration/cal-aa-ur/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal-aa-ur/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal-aa-ur/provenance.txt` | `8dd911c7d973a201b716a2c6983f3484bd8532f4d022b0e74b5c43b17ca38763` |
| `raw/calibration/cal2-aa-tp/ab.jsonl` | `2984869091f6093410f6567c0de42dc62157746a388dcfb357b36adc81d915aa` |
| `raw/calibration/cal2-aa-tp/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal2-aa-tp/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal2-aa-tp/provenance.txt` | `0dff0d0fd23deb82578c3986f492071aee172d76af9c7e6236bdf784c11854a4` |
| `raw/calibration/cal2-aa-ur/ab.jsonl` | `67cb23decb1927cde7fbb20b5a8aec013ad46d9a0f8b5c6015610dfba401f14d` |
| `raw/calibration/cal2-aa-ur/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal2-aa-ur/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal2-aa-ur/provenance.txt` | `01d1238036360fc74081132cc05e98ceb8731599dc0c2faa16287ab7ab454655` |
| `raw/calibration/cal3-aa-tp/ab.jsonl` | `326922266aff8272ea293aac5ed7c268f523a2f2b0581b8239f6ffd67b60c53d` |
| `raw/calibration/cal3-aa-tp/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal3-aa-tp/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal3-aa-tp/provenance.txt` | `b6c384e7a899f02d7f4b5c2565d55d4921dfb88732916c7c26f1e96056ef0b56` |
| `raw/calibration/cal4-aa-tp-shared/ab.jsonl` | `3b74618f55f49ff1b1bcf00b118df9c6edefc5f273cfae11068a3ad7c2344d25` |
| `raw/calibration/cal4-aa-tp-shared/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal4-aa-tp-shared/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal4-aa-tp-shared/provenance.txt` | `c54fcd70d45eb4525bf32446acd97cda01ff7a6523b0edd0de7ca670a29034c0` |
| `raw/calibration/cal5-aa-tp/ab.jsonl` | `2fc36ecc18e550f6d78c3dfdf120f2b584f307ebacf143a9e0d1b60faa9fa0a6` |
| `raw/calibration/cal5-aa-tp/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal5-aa-tp/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal5-aa-tp/provenance.txt` | `826e31d5447e5a2c198b003594b70ed87f2b3e019f5e1d0b0abffeb1291485d6` |
| `raw/calibration/cal5-aa-ur/ab.jsonl` | `ce4fa8fa139bfe38eef451e4579022711836609e8ce4fb9f6fd94585a886ac73` |
| `raw/calibration/cal5-aa-ur/meta.jsonl` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal5-aa-ur/perf.csv` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `raw/calibration/cal5-aa-ur/provenance.txt` | `6efbdbd1d8f45cd9bed5d40ca4b9134c5641b5506d6a218b7c73a4a6f2356658` |
| `raw/parity/par_base_s0_threadpool-4k-d16-w4.jsonl` | `86058b9111b5dbd162d2921157131f4848f10d5f6a4dc921ba55b264135a2cdd` |
| `raw/parity/par_base_s0_uring-4k-d16.jsonl` | `a242272a246ec0ef5321f3c98f27dafc90507267c1ab1528df0515b0429b29f8` |
| `raw/parity/par_base_s1_threadpool-4k-d16-w4.jsonl` | `79b1773a17f0ad7c760964973d64db2ba45f3be1754aff7452ac3ee4b6a5a2cd` |
| `raw/parity/par_base_s1_uring-4k-d16.jsonl` | `954f42cd07ff1221d5df2cb277ccf1dcc0ce07cb8b52ddbd99d0a0b7f9ea56fe` |
| `raw/parity/par_tp-full_s0_threadpool-4k-d16-w4.jsonl` | `122833a10640c42fc890a0a4b6e4dacc32174e4c02e14e0171478e7db5f552ec` |
| `raw/parity/par_tp-full_s0_uring-4k-d16.jsonl` | `6801c98462502e0109cc1f263aecd48aa7a4500f60a7c9c5a6af255532179552` |
| `raw/parity/par_tp-full_s1_threadpool-4k-d16-w4.jsonl` | `cb83361c389cc7d44bf1816577822382f15037a04358686ab62859713ca19a0c` |
| `raw/parity/par_tp-full_s1_uring-4k-d16.jsonl` | `758b3ed944042814ed8ba150e92c7965381eb6736cec9c223db82819c35c6296` |
| `raw/parity/par_tp-oponly_s0_threadpool-4k-d16-w4.jsonl` | `71282d824632eb6a5e065571e469925cdf808e78d3564e9c1e97ecd0ea450512` |
| `raw/parity/par_tp-oponly_s0_uring-4k-d16.jsonl` | `404dc5349da056bede26fdeaa0a45b92bf3eb2da7e1782b63aeca930c657b3a8` |
| `raw/parity/par_tp-oponly_s1_threadpool-4k-d16-w4.jsonl` | `0f33a970bdbc55dfc8ec41a35740ee9cb83977407681fc3bd2686aabba03c78f` |
| `raw/parity/par_tp-oponly_s1_uring-4k-d16.jsonl` | `60e160cb1df138c4ef6fa275478183eed5530955002e5711c170c0372b61828f` |
| `raw/parity/par_ur-full_s0_threadpool-4k-d16-w4.jsonl` | `f5c01d692e6a120a7d3fb9d27b72ce14822ffc0d95dd5bea05c47f1d46a6b27e` |
| `raw/parity/par_ur-full_s0_uring-4k-d16.jsonl` | `1bc57b46075b3f7c3e70431de91c18b86e6c75c0f24c322f24e305dabbe2e4ea` |
| `raw/parity/par_ur-full_s1_threadpool-4k-d16-w4.jsonl` | `e75b43fd1017dd3b3557db028136e7cc16deab580a1fa783b1a7ce91325b8977` |
| `raw/parity/par_ur-full_s1_uring-4k-d16.jsonl` | `f5d337f2475b5a48378479b8120c082968ef6a4194f770219e0f80d84d824f00` |
| `raw/parity/par_ur-oponly_s0_threadpool-4k-d16-w4.jsonl` | `55f0bbf549ecda11757927040d33882f12585e3d1ed88fec949ac6d8c55aad35` |
| `raw/parity/par_ur-oponly_s0_uring-4k-d16.jsonl` | `ddaf82b3ef4dded0b98bdbee4457b83a72dab031f1a95b6bd3323cb5f0ff5d4b` |
| `raw/parity/par_ur-oponly_s1_threadpool-4k-d16-w4.jsonl` | `7b9d6413e4b087cd68bc89ebc6430a5e5f3eef976bd4dcf6614e026adf7b4f6b` |
| `raw/parity/par_ur-oponly_s1_uring-4k-d16.jsonl` | `230e9241847823d0b0488e5be61ba77b43f00df85052bea34a4e6384219e5bc9` |

## Excluded items (6)

| Excluded item | Class | Reason |
|---|---|---|
| `docs/research/mw-hw-575-native-e5/scripts/run_575_ab.sh` | Rebuildable harness | Frozen paired interleaved A/B runner (v3). Collector tooling, not evidence: repository rules archive the receipts a collector produces, not the collector. Protocol is fully specified in R5 and per-run invocations/config are in `raw/*/provenance.txt`. Retrievable verbatim from the source HEAD (sha256 below). |
| `docs/research/mw-hw-575-native-e5/scripts/run_575_d8.sh` | Rebuildable harness | One-shot D8 supplement runner driving the pre-existing M2 `pipeline_ab.cpp` harness. Same collector rationale; its recorded outputs are archived (`raw/ab/ab-ur-d8/`). |
| `docs/research/mw-hw-575-native-e5/scripts/reduce_575.py` | Rebuildable harness | Paired-statistics reduction script (with self-test). Its results are already materialized in R6–R8 tables and raw receipts; the method is documented in R5/R6. |
| `docs/research/mw-hw-575-native-e5/scripts/layout_probe.cpp` | Rebuildable harness / fixture | One-shot `sizeof` probe (~6 lines). Its outputs are materialized as the R3 layout-delta tables. |
| `tests/uring_flush_count_oracle_test.cpp` (added on source branch, blob `28e77926`) | Prototype product-tree code | Superseded on master: productized via test-only PR #585 (squash `1df8b4b8`, master blob `990cb80c`); the two versions differ only in the leading comment (verified by diff this PR). R3 records it as the arm-identical shared component of every arm, so no measured result depends on the branch copy. An evidence archive must not modify the product tree. |
| `xmake/tests.lua` (modification on source branch) | Product build config | The exact `uring_flush_count_oracle_test` registration already exists on master via #585 (`xmake/tests.lua:578`); replaying it would be a no-op product change outside this archive's scope. |

Excluded-harness source-blob checksums (retrieve with
`git show e9baafb9d117cbaf387dde9f4433ebbacb4a73b3:<path>`):

| Path | Source-blob SHA-256 |
|---|---|
| `docs/research/mw-hw-575-native-e5/scripts/run_575_ab.sh` (source blob) | `d85b458a73de06ae03d38c5cc7d4ad45c9728a211184f1ab608e4dbe731ec643` |
| `docs/research/mw-hw-575-native-e5/scripts/run_575_d8.sh` (source blob) | `2f5134721cb462ce9a223d8d977398043d5e2a5ad0d01fe1c0e80c83ac24c7d1` |
| `docs/research/mw-hw-575-native-e5/scripts/reduce_575.py` (source blob) | `d80035129c726d279e2c46032c54ca773e01b1881f22f7b7776d5165669e8b26` |
| `docs/research/mw-hw-575-native-e5/scripts/layout_probe.cpp` (source blob) | `f1fc33a9756d87da2f822044fc3b31a47623b2e11cd343bbcdab42d4c7a09ba9` |

## Reproduction guidance

1. **Read path:** every measured claim cites its receipt file; R0–R9 form
   the document index (`README.md`), `VERDICT.md` is the final decision
   record.
2. **Arms:** the measured binaries are reconstructable by applying
   `patches/tp-oponly.diff`, `patches/tp-full.diff`, `ur-oponly.diff`,
   `ur-full.diff` (plus control/mutant diffs) to the campaign baseline
   `3df10dc4`; per-arm binary sha256s to check against are in each
   `raw/*/provenance.txt`.
3. **Raw data:** `raw/ab/ab.jsonl` (paired per-rep samples), `perf.csv`
   (perf-stat counters), `meta.jsonl`, `raw/ab/ab-ur-d8/d8.jsonl`, and
   `raw/parity/*.jsonl` (deterministic parity suite records) are the
   primary measurements behind R5–R8.
4. **Harness:** the four excluded scripts are retrievable verbatim from
   the source HEAD SHA above; R5 records the protocol evolution (v1 → v3)
   they implement, including the two-warmup and shared-tmpdir-path rules
   and the path-name-bias finding.
5. **Oracle test:** the merged master version runs as
   `uring_flush_count_oracle_test` (registered in `xmake/tests.lua:578`).

Archived by `research/archive-575-evidence`; no source-branch content was
modified or deleted.
