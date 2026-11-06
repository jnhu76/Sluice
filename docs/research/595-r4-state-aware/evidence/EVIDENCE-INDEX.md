# #595 R4 证据索引（EVIDENCE-INDEX）

- 根目录：`/home/jnhu/sluice-595-e5/r4`；本索引由终审裁决员在 r4 树内生成，所有 sha256 为生成时实测。
- 裁决文档：`RESULTS.md`（含 §16 八问裁决、§17 复核订正）；可粘贴摘要：`evidence/pasteBlock.txt`。
- 注意：`analysis/MAIN_SUMMARY.json` 的文件 sha 随 `generated_utc` 改变；内容 sha（剔除该字段）见下。SVG 图含 matplotlib 生成时间元数据，sha 会随重跑变化；PNG 与 CSV 内容稳定。

## 0. 顶层文档

| 文件 | sha256 | 说明 |
|---|---|---|
| `RESULTS.md` | `416627588a02d87e12ceebe408c46d1870430811021a946db99de14d13ada990` | 终审定稿（§1–§17） |
| `PREREGISTERED.md` | `fbb2e7852e87f9fbe4bf0e1e12e110d9d2ad279f9d486141b16d61b79f01f0f6` | 门槛/家族/seed 唯一来源 |
| `BASELINE.md` | `704a372ab5519a2f157c20c3044d044dd22c0381b25e06d77110d73fad6245d0` | 环境与冻结对象 |
| `DESIGN.md` | `ff4ec20de6a9f7d8ce0f452d56cfde75de5c0f414ce57bf4a26bdce9c48e53dc` | 信息审计/状态-动作表/参考规则 |
| `evidence/pasteBlock.txt` | `3c7d88a3eb9a0ba10c6d0ccfdef12b59314984c077b66b023c1f03b38a56f465` | BEGIN_SLUICE_595_R4_STATE_AWARE 块 |

## 1. 正确性回执

| 文件 | sha256 | 说明 |
|---|---|---|
| `evidence/correctness/RECEIPT.md` | `c9d973070b2d3733c23f87896b9d96b172e15195624a892d5d61d99fc98b2dcb` | 18 场景 oracle、M1–M3 变异、恢复 GREEN、digestBinding |
| `evidence/correctness/base_green/digests.sha256` | `d10f9c79884d78b8962b7df0d67201bdd80cd8a859737dc94e8cc00e365dc464` | BASE GREEN 构建时原样 digest |
| `evidence/correctness/restored/restored_green.out` | `f3ad7e4cf729b5d7b3b34a1165b604f355cc693cace4579617a915e886661af5` | 恢复后全量 rc=0 |
| `evidence/correctness/tree_proof/tree_restored.txt` | `c0b45d2c7868824a1797e7a2720425dbc9b040106370dfcd8f51cbea47f1be44` | HEAD=f3b25329、porcelain=0、header digest |
| `evidence/correctness/uring_ready_reuse_correctness_test.cpp` | `5c10be9ee44f8471b34426ae663a78e7dc58633d0852808181f738b9136361d3` | 归档的新测试源（与 BASE/恢复同一源） |
| `evidence/correctness/LOCK_RECEIPT.md` | `a1e4b41ad72d00de5071d19928ccfdca8bcd3547a716abbc9bd07efeda3b60eb` | 正确性轮锁收据 |
| `logs/LOCK_RECEIPT.md` | `c4da5b7e917611d3daf13ea526bd77b72daec3884bbac4c60f3f2050d03be9b3` | 实现/构建轮锁收据（含最终重建） |
| `evidence/frozen-binaries/MANIFEST.txt` | `607fce1a77b9e689a567da9253842d74ba0a9eeb3b7aca5bccefeaa85522625b` | 冻结二进制参考副本清单 |
| `evidence/frozen-binaries/bench_w03_external_loop` | `f029607fc1a657efe0ba96203115f27a90d598f12c274e9360b6029a5e019009` | W03 冻结二进制 |
| `evidence/frozen-binaries/bench_w02_pipeline` | `daee6b6c1ab618a29d8e1afc120f8da5e478756b2def7c77c712f54a6a4e1931` | W02 冻结二进制 |
| `evidence/B-SUPPLEMENT-APPENDIX.md` | `d0d96a60112d97e8e180071ba7b3e68cbaa9a7bb71a66491b4b44b0c3d4f88bb` | B 补充块事前附录（先于计时） |
| `evidence/AA.md` | `efa18ec0fd67e92366854e25383cfc0692f0f3ca35da8e6299e03250a0607b55` | A/A 校准记录 |

### 1b. 正确性运行输出（BASE / 变异 / 恢复）

| 文件 | sha256 | 说明 |
|---|---|---|
| `evidence/correctness/base_green/uring_ready_reuse_correctness_test.out` | `f8404435d0a82e7f270a81348063a4026c0130cbaea669d7653df845dcc249a0` | BASE GREEN 新套件全量 rc=0 |
| `evidence/correctness/base_green/existing_suites.out` | `d53363232544d6cd2385a40eb7fa4dddc747b3e19fc4c6001380735a47212789` | 既有四套件回归 rc=0 |
| `evidence/correctness/mutants/M1_duplicate_accept_span_shapes.out` | `68c6e20758c2d4a8aa40c3ffeeb555cbac14a43fe3e85e828ea8f6fb7fd359ea` | M1 重复接受，321 failures rc=1 |
| `evidence/correctness/mutants/M2_zero_progress_prefilled_residue.out` | `4965da9a89ca1e6c08c1d29deea515cd028d4697d81f02576b72fb5e6381e730` | M2 零进展，确定性 FAIL + R timeout rc=124 |
| `evidence/correctness/mutants/M2_zero_progress_residue_after_cap.out` | `1d2700ec1eaaf8a4315179a257bc2930281eecf137d8a2bf2910660fcfd602f4` | M2 residue_after_cap rc=124 |
| `evidence/correctness/mutants/M3_boundary_ignored_residue_after_cap.out` | `25f6e1bfeafa9ffb6175409a89198a49b72b7c58df8f311f8fa2397e4fa09bff` | M3 authority 边界，10 failures rc=1 |

## 2. A/A 校准（3 块 × 24 pairs）

| 目录 | summary.json | results.jsonl | analysis.log | 半宽 CPU/tput/p99 (pp) |
|---|---|---|---|---|
| `raw/aa/AA-W03-D16` | `dacce2fc35e0b1895ce064d2401d1b1687f6f99c26c6c6a290f362f94cbb6722` | `a46a1a82c64086d3384742b4246bf7f8907fc1df712e5c85c6c51b72421a3c24` | `dacce2fc35e0b1895ce064d2401d1b1687f6f99c26c6c6a290f362f94cbb6722` | 1.0245 / 1.0286 / 2.0971 |
| `raw/aa/AA-W03-D2SERIAL` | `312c6b2e84fba9fea7d221ffcbb7aac9fc2bb2d9df936390bb37ed638b116783` | `ef76e240ce4ff5ef255d27adf8baa1ec0e01cac65341789a17fdab70662b120a` | `312c6b2e84fba9fea7d221ffcbb7aac9fc2bb2d9df936390bb37ed638b116783` | 0.5059 / 0.5131 / 2.7086 |
| `raw/aa/AA-W02-D2` | `b5511c0b318df442efa2d850cec1a81638335cc6e825023b8abd11b1a3174b2f` | `0fe61bfc6800cb78be2a1e8ff92de577d93d180c185547f9fe5c9ee8368474ed` | `b5511c0b318df442efa2d850cec1a81638335cc6e825023b8abd11b1a3174b2f` | 0.5518 / 0.5584 / 1.3262 |

- 三块均 PASS（门限 5/3/3 pp）；分析脚本用冻结 bootstrap(10000, seed 5950) 逐位复现各 summary.json 半宽（差异 <1e-9）。

## 3. 核心矩阵（raw/matrix）

| 格 | blocks | header.json | results.jsonl |
|---|---|---|---|
| `raw/matrix/MAIN-W03-D16` | 24 | `942c5fad023ef2c9328a9b099a8a88b58a9387a519aa187c9383d070558e30d6` | `f02ae4d1be17019163e68b19101ca7cf89b1f95128d5afa6256bc1b4ee9fe5f0` |
| `raw/matrix/CORE-W03-D2SERIAL` | 24 | `04ad4c1152dfc086532af0ba648160060ec382ba3aae892c334d0fd7857b9e70` | `871c68c75345fc263a0db16754226ee2778c489a11e5e3e14dc0beb3d672edcb` |
| `raw/matrix/CORE-W03-D4` | 24 | `db0ce7317310f5aa2f5f7d272b2d652f0f8aaf38ad33f0844f92433ca4ddc919` | `ce75eb8494686dba24965af8fe810f6dd2d3bd22a2d4102ffc30ed2f8b7da790` |
| `raw/matrix/CORE-W03-D64` | 24 | `d5f6d8702f67d4123128d5209d7d34c41020cfd2f9ac8344b3bc70d3feef8426` | `047c351e5b2541b11ab0cf5e122cc4dabfdcbd1d8ea60fc7cb85586626d4ef47` |
| `raw/matrix/CORE-W02-D1` | 24 | `47ffa9931a6f183f6cb6703d8780aa13375ff28b23d820aac828f82ea8188f9a` | `44a72ad74611f0d4227260cec6c64eefdeb8adfe1652a3b255e22c887b9a1373` |
| `raw/matrix/CORE-W02-D2` | 24 | `9613e38c0fb6f23abc1157feb61394bb79c0b301ea3d2f93efe1da756fa5c21f` | `7e370429e004575538457b0571f11ce6bc1ffcaff1cf75f80d0d0d98a96404cd` |
| `raw/matrix/CORE-W02-D16` | 24 | `d3ae2b7be5c8c81c1a7597c93661fc2e11574d1a1a74c35a12b6453e83b41346` | `82df3aeb298137742435a05d2634eb9bca96c93c6d7a5f9ea71edf1dfdc98d8b` |
| `raw/matrix/TRAJECTORY` | 24 | `844925c154314b36790e63ba5b4a0af3238e648d1f29d34ef56810c0391aff62` | `d90014c906d82cadf2a9de590baea6e5f6c41e8b97eae5ed26dde6b99dfcd589` |
| `raw/matrix/TRAJECTORY/attempts.jsonl` | 72 failed attempts | - | `562e4ae8ee3b1b7fe58711f7d131b2526008cd019462eed743e5bdc4ff65a5ec` |

- MAIN 24/24（192 invocations）；6 个 CORE 格各 24/24（96 each）；TRAJECTORY 0/24 完整块（24 块 done_with_failed_slots，552 valid invocations）。
- 汇总：`analysis/MAIN_SUMMARY.json` 文件 sha `3d56dd2c22ae1d32165c38d3198549f17c52a89826845494561eb0c8bbf4b30c`；内容 sha（剔除 generated_utc）`317bf9a5120a34de120dc9cbe138ae456f653fa3ba55b354a23ec97eb2e512dd`；`generated_utc` 见文件。
- 分析日志：`analysis/analysis_run.log` sha `3ff9c5b86ce50dd9cdb988ffc7959255418a11806d45b148247f6e18d62864c5`；逐格表：`analysis/per_cell_table.md` sha `62c76304a98615b94901fc2b213e2c62cfe84174c537bb3b1f6f02c98ee4acea`；重建回执：`analysis/REBUILD_EVIDENCE.md` sha `0c7b04b136ab22350311e1e2116db862e40b5de53aa2ab147ae55489f16e53ad`。

## 4. 扩展与 B 补充（raw/extension, raw/bsupp）

| 格 | blocks | header.json | results.jsonl | 说明 |
|---|---|---|---|---|
| `raw/extension/PROT-64K-D16` | 24 | `6222f5b433ddcc3605be6f17d4e83ca4aac15de96e918e0c7a6bec220a724e17` | `55e395a9554b404b1b1fba207aeeaf4ca38a61fb5528e85c70544124494cfb32` | 64KiB d16 eligibility 门 PASS；尾延迟描述性 |
| `raw/extension/PROT-NONTMPFS` | 24 | `20ed40fe4636ba3f474a84f2ae5c820e6fcbdd29ac6981d88a9fce14dc5551f3` | `3e298781aae344fe329f00bb67bd6c854b5e7fb1123c56f59572ba30b93921fa` | btrfs warm buffered；S p95/p99 FAIL |
| `raw/extension/PROT-SMT-2-12` | 8 | `2ecc2f9284e5757566138040c019add99d04d76fe7937bf3a4d0349b43c30ecb` | `6694cc6caaaff30b2807c6d17fdfaf8e3153f42821041277ab818ac0966c2ae2` | 同核 SMT 放置，safety-only 8 tuples |
| `raw/extension/PROT-XPHYS-2-8` | 8 | `51e0d5a7a9bbe71abc84f7a75eaaa3cdf75651941e4c387cc942d25928ad734c` | `da390210a41a358dbe2c0607784bfd87baa73e26c0841e988a503324c07a333d` | 异核放置，safety-only 8 tuples |
| `raw/extension/PROT-THREADPOOL` | 0 | `dba89a493d5b72e1b6d45a45ccf9b8fd2b4d3bc3e6077213a3d97b84069a757d` | `f4ac7ee002f96f2afc5a3a8c887c7c69c9145f91fdc98193f772331d2f1b6e6c` | threadpool 功能负控（无 block 文件，结果在 results.jsonl） |
| `raw/bsupp/B-SUPP-W03-D16` | 24 | `8078b71484643c2c32a6946c3b73c4b997a389e6fd2d719f5940dc252401e3ba` | `6fce43acd2819868326eed07ed048f209f6f3b365c33f5a7e75e97b3df32411f` | B/R 描述性补充块（不进门槛） |

- 扩展汇总：`analysis/extension/EXTENSION_SUMMARY.json` sha `0fd5818cdfce5fed30090f4ce6c5b74a67fbeb4e057dbb94f15258b707012abe`；`analysis/extension/EXTENSION_RESULTS.md` sha `1c8528e0fced6d36f8f3ceb081a84d53569baf568defd3184fa8c0cba02d9366`。

## 5. 诊断（raw/diagnostics；非计时证据）

| 文件 | sha256 | 用途 |
|---|---|---|
| `raw/diagnostics/header.json` | `d6fe1454ba8577b672a76aaae104861650af33a39d4cea2debac1c3b31729e64` | 诊断运行头 |
| `raw/diagnostics/results.jsonl` | `2b62e5784e3b1388944f8e029552da2f1f1099bea2ffe70576638d9f7e17ff71` | 诊断 invocation 结果 |
| `raw/diagnostics/TRAJECTORY__S__T1.diag` | `49057ce653c3566994dfca3f04a6265555728426399492da1d0e1b596ce6a825` | T1 S 决策行（60,000） |
| `raw/diagnostics/TRAJECTORY__SHADOW__T1.diag` | `8429f5658d6986729a4d9470dca8a3e6ffeffa4b161da76bd3f597763554076f` | T1 SHADOW 决策行（60,000） |
| `raw/diagnostics/TRAJECTORY__S__T2.diag` | `7fc80c051f6330e5fc7653a2020e6aa9510f5cbb6c81693b40b3eb73e6eaa1e3` | T2 S 决策行（94） |
| `raw/diagnostics/TRAJECTORY__SHADOW__T2.diag` | `452bcf13e4152faa79090454c3894ff56342baa212d8e86f7a7cd4b74e5af457` | T2 SHADOW 决策行（24） |
| `raw/diagnostics/TRAJECTORY__S__T3.diag` | `c0f030da6e641db50dcdba4c21be5dd79d88beb6da8ffc325a22e4ad218c99b3` | T3 S 决策行（29,995） |
| `raw/diagnostics/TRAJECTORY__SHADOW__T3.diag` | `4e0ac97c25fc3a8522964cbaef0c78fcfd0610933a533bf8354f31a19793bae8` | T3 SHADOW 决策行（7,504） |

- `raw/diagnostics/jsonl/` 另有 8 个 per-run JSONL（b00-p0-*）；逐行格式见 DESIGN §4。大体积 `.diag` 只作复核入口，不复制进交付物。

## 6. 图与图表数据

| 文件 | sha256 | 说明 |
|---|---|---|
| `figures/F1_net_effect.svg` | `fa1ed9d2147ee637fc9a850773ad41046c074908b44900be2fc42ccd80fbf26b` | 矢量图（含生成时间元数据，sha 随重跑变化） |
| `figures/F1_net_effect.png` | `42868c39924f9a8c34eedb57981c6e0e6e86a521ca24581b717c920dfde121aa` | 位图（内容稳定） |
| `figures/F2_behavior_change.svg` | `50416cbd0ca4af8343b0d900e8bdec8da939e2cd224c737fbbbb993eda74c58d` | 矢量图（含生成时间元数据，sha 随重跑变化） |
| `figures/F2_behavior_change.png` | `356abbd8896a3ae701f763412fcd0cbb5368122079c6d0511a2ab4d615283973` | 位图（内容稳定） |
| `figures/F3_decomposition.svg` | `2f3359a34809bc363f0865e3e6857632479909fe2324473ffc455a0e0926ab1d` | 矢量图（含生成时间元数据，sha 随重跑变化） |
| `figures/F3_decomposition.png` | `f9fe24cd87194ef8191bd397917599adc712ad60c9dcb4f288a04a1baf31cd94` | 位图（内容稳定） |
| `figures/F4_cpu_tail.svg` | `86aaed0bfe1bac28c1e20daa073f17180e4927e0d20e86a7ded9abb3933de8d4` | 矢量图（含生成时间元数据，sha 随重跑变化） |
| `figures/F4_cpu_tail.png` | `4530623ec76085ee29fb3a4d056c8e24be3ec5cbc646d41911c5f78ca9607070` | 位图（内容稳定） |
| `figures/F5_trajectory.svg` | `fe566186f511ece867b5354b2e2f8db8ef573fe9a7b782a2b26fd8d9a5ab5d99` | 矢量图（含生成时间元数据，sha 随重跑变化） |
| `figures/F5_trajectory.png` | `2200fed3ed791a83ff111aac450f8dbc548a6b15ee79cd61fbc377d0f2f67aba` | 位图（内容稳定） |
| `figures/figure-data.csv` | `c2c3294b9f8b124914602d16c9f93f178aafb14be2fb3b98e3f581aae02c8096` | 370 行；F5 state 行已订正 |
| `figures/CAPTIONS.md` | `c4fc4c3e09fc6138f272a7760f7e50b7ce799f27189a058615ab429d563c19d1` | 五图四句图注 |

- 脚本：`scripts/matrix_analysis.py` sha `74e92ec25496741dccd024fee74de52b63a8c63521167944f56ef519c32230b2`；`scripts/make_figures.py` sha `294384497644380e273f5f620f39e59efd81b0a225c2d7d5075e343c17231312`；`scripts/render_status_table.py` sha `1c3aef2ee23acf2d5a50e28141ece0c9fa2f030471ea34b9da926471cb62aed7`；`scripts/render_results.py` sha `2cf6d78b93efa090633cb4283854feb53d9fc2f2a4c12a66e26bf4f23832d6ed`。

## 7. 冻结对象与提交

| 项 | 值 |
|---|---|
| 主仓 master | `271b81bfc9d64eecc0266fe7066b9040525dd710`，porcelain=0 |
| R4 worktree | `/home/jnhu/sluice-595-e5/r4/worktree`，branch `research-595-r4`，HEAD `f3b25329bc8a64a201961c426a58adacfb8fdf08`，porcelain=0 |
| R4 冻结基线 | `d9d140eea180f29f629992aa293109acdeff1e03` |
| 实现补丁 | `d66f3bcf52d01c5ee19ecd6285d67586d4467a44ecc8d608b1b5a8e9b267cf49`（d9d140ee..f3b25329） |
| 上游补丁 | r3-completion `2596f204…`；delta_w02 `4aa7b306…`；delta_w03 `57f21e02…` |
| `bench/r3_ready_batch.hpp` | `363f346ac3b26076322cbe0126a9d276aedbcb1d2ecd7607c108f8c32621dd81` |
| 冻结二进制 w03 / w02 | `f029607fc1a657ef…` / `daee6b6c1ab618a2…`（全量见 frozen-binaries/MANIFEST.txt） |
| 预注册 | `fbb2e7852e87f9fbe4bf0e1e12e110d9d2ad279f9d486141b16d61b79f01f0f6` |

## 8. NOT_RUN / INCOMPLETE（如实索引）

- §10.5 `r3d2pos` 正控：NOT_RUN（raw/ 无该 case）。
- §10.5 W03-d16 enter/CQE 对账（每臂 3 次 strace）：NOT_RUN（只有 DIAG-64K-STRACE 与 PROT-THREADPOOL strace）。
- §8.1(a) 静态审计 `logs/audit.txt`：文件不存在；§8.1(b) W03-d16 strace clone/timer/eventfd：NOT_RUN；§8.1(c) valgrind/massif 分配斜率：NOT_RUN。
- PROT-SATURATION（研究臂）：NOT_RUN_WITH_REASON（冻结 effective_arm 规则）；OPEN_LOOP：NOT_RUN_WITH_REASON（无冻结 harness）；PROT-ODIRECT-COLD：NOT_RUN_WITH_REASON（无安全 harness）；W02-d3：NOT_RUN_WITH_REASON（冻结对象集无该 case）。
- TRAJECTORY：0/24 完整块，INCOMPLETE（SHADOW-T1 `shadow_outstanding_sum==0`，72 条失败尝试）。
- 全 `xmake test` 82 用例矩阵：未跑（正确性回执 §6 已注明）；逐请求 ECDF：未保存逐请求延迟。

## 9. 复现

```
cd /home/jnhu/sluice-595-e5/r4
python3 scripts/matrix_analysis.py        # -> analysis/MAIN_SUMMARY.json (stdlib only)
python3 scripts/make_figures.py           # -> figures/F1..F5 + figure-data.csv + CAPTIONS.md
python3 scripts/render_status_table.py    # -> analysis/per_cell_table.md
python3 scripts/render_results.py         # -> RESULTS.md
```

计时证据复现见 `PREREGISTERED.md` §11 与 `raw/*/RECEIPT.md`；本索引不重跑计时。

