# Rebuild evidence — figure script rerun (#595 R4)

- Working dir: `/home/jnhu/sluice-595-e5/r4`
- Command: `python3 scripts/make_figures.py`
- Start UTC: 2026-11-06T10:05:54Z; finish UTC: 2026-11-06T10:06:02Z
- Exit code: **0**
- stdout:
```
figures: 15 files -> /home/jnhu/sluice-595-e5/r4/figures
figure-data rows: 370 -> /home/jnhu/sluice-595-e5/r4/figures/figure-data.csv
captions -> /home/jnhu/sluice-595-e5/r4/figures/CAPTIONS.md
```
- Inputs: `analysis/MAIN_SUMMARY.json` sha256 `4838114648fd21783225915cdde16aef7d055b43ad9fbcda72ebf8221151d894`; analysis engine `scripts/matrix_analysis.py` sha256 `74e92ec25496741dccd024fee74de52b63a8c63521167944f56ef519c32230b2`

| output | sha256 | bytes |
|---|---|---|
| `figures/F1_net_effect.svg` | `ed45088dbb03e61ff0bdba89bc124fff8d0145830112860d176168687c466ec9` | 162961 |
| `figures/F1_net_effect.pdf` | `5ee7b8c113748fbdfcb27e4b58ff4a0e4701536d19c4eb2f6ae593836acd17ca` | 34583 |
| `figures/F1_net_effect.png` | `42868c39924f9a8c34eedb57981c6e0e6e86a521ca24581b717c920dfde121aa` | 153493 |
| `figures/F2_behavior_change.svg` | `f2f86a6c7abd6158ce85c79dbf51f57c0d196018713c4aae7e5b24c31e0dd9ed` | 114883 |
| `figures/F2_behavior_change.pdf` | `7fbac399d792e69370b93066a6b82a3c308faafa50088503c57f87f7ab4ac440` | 26210 |
| `figures/F2_behavior_change.png` | `356abbd8896a3ae701f763412fcd0cbb5368122079c6d0511a2ab4d615283973` | 150455 |
| `figures/F3_decomposition.svg` | `7e9508eb2ca8436ebe2ff1f077dc647fd7ec5c0d9d081b8da588463c7a7b6d0e` | 87521 |
| `figures/F3_decomposition.pdf` | `28bfd8024117e3691e82282418893d6b02158c3ff19cd922022eb3a1ad63d285` | 25907 |
| `figures/F3_decomposition.png` | `f9fe24cd87194ef8191bd397917599adc712ad60c9dcb4f288a04a1baf31cd94` | 98344 |
| `figures/F4_cpu_tail.svg` | `cc72b93e7c9c771b481a3e3c1fadc71d800f3522426befa240c0eab6877a0d7f` | 74502 |
| `figures/F4_cpu_tail.pdf` | `7dfb1f065f258c60dd8f5a2821f6b453dbb8bb351f682fbceb59297b44ff990a` | 22900 |
| `figures/F4_cpu_tail.png` | `4530623ec76085ee29fb3a4d056c8e24be3ec5cbc646d41911c5f78ca9607070` | 59339 |
| `figures/F5_trajectory.svg` | `2aba4d2874162d9e0e22489d481d5ccab43b6ca895c437eeeecc3ea2aafe8a47` | 81801 |
| `figures/F5_trajectory.pdf` | `6c3da9ce72a24ed6e3529706f0fd3f7afe2effa981e1ab6f354aff7964941765` | 25962 |
| `figures/F5_trajectory.png` | `2200fed3ed791a83ff111aac450f8dbc548a6b15ee79cd61fbc377d0f2f67aba` | 108751 |
| `figures/figure-data.csv` | `a104a3ebc304dd4b90e2750e0714962aaaa4bee30180669cdf8c0b05dd2a7a91` | 34695 |
| `figures/CAPTIONS.md` | `c4fc4c3e09fc6138f272a7760f7e50b7ce799f27189a058615ab429d563c19d1` | 6829 |
| `analysis/MAIN_SUMMARY.json` | `4838114648fd21783225915cdde16aef7d055b43ad9fbcda72ebf8221151d894` | 582538 |
| `scripts/make_figures.py` | `91b90687c3547b1875fd6fa04c25ec84d570fc666bad44846ff86e78d6f187a2` | 26424 |
| `RESULTS.md` | `fddb2875a6b6a7b5ae43d307be5fab973b34974b0f419dfbf224135973760888` | 43912 |

## 二次重建 —— 终审订正（F5 figure-data.csv）

- 触发：独立复核发现 `figure-data.csv` 的 F5 state 行 T1/T2 使用 `ready[-1]`/`present[-1]`（T3 值）且 `quantum2_share` 百分比数值配 `fraction` 标签。
- 修正：`scripts/make_figures.py` 的 F5 循环改为 `enumerate(segs)` + `ready[i]`/`present[i]`，`quantum2_share` 存 fraction。
- 重跑命令：`cd /home/jnhu/sluice-595-e5/r4 && python3 scripts/make_figures.py`（无 `-I`，与首次一致；系统 python 的 user-site 才有 matplotlib）
- 退出码：**0**；stdout：`figures: 15 files`；`figure-data rows: 370`；`captions -> ...`
- 结果：F5 state 行订正为 T1 1.0/1.0/0.0、T2 8.361702127659575/2.0425531914893615/0.9893617021276596、T3 7.923387231205201/2.0003333888981496/0.9997332888814803。
- 图内容未变：五张 PNG 的 sha256 与首次逐一相同（F1 `42868c39…`、F2 `356abbd8…`、F3 `f9fe24cd…`、F4 `4530623e…`、F5 `2200fed3…`）；SVG 仅因 matplotlib 生成时间元数据改变 sha。
- 新 sha256：

| 输出 | sha256 | 字节 |
|---|---|---|
| `figures/figure-data.csv` | `c2c3294b9f8b124914602d16c9f93f178aafb14be2fb3b98e3f581aae02c8096` | 370 行 |
| `scripts/make_figures.py` | `fd9a39f6bebb61610777064f7da7663319f25adc74b5dafb546097aad0b59936` | — |
| `figures/F5_trajectory.png` | `2200fed3ed791a83ff111aac450f8dbc548a6b15ee79cd61fbc377d0f2f67aba` | — |
| `figures/F5_trajectory.svg` | `c3da44cd8b87f7d1233168037f157a356dda2a9ff76c1c05a338961ce8fe15fc` | — |

## 三次重建 —— 独立复核会话终跑（2026-11-06T10:36–10:39Z）

- 前提：另一分析会话对本 ask 的全部产物做了独立核验（关键对比用从零重写的
  bootstrap 复算：MAIN R−A0 cpu −19.624%/上界 −19.113%、p99 上界 +23.577%；S−A0
  cpu −12.813%/−12.388%、p95 上界 +34.664%、p99 上界 +36.247%；S−R cpu +8.508%/
  +9.307%；D2SERIAL R−A0 cpu +6.940%/+7.648%——与 MAIN_SUMMARY 逐位一致；两次
  `matrix_analysis.py` 重跑除 `generated_utc` 外内容全等，判定确定性）。
- 订正：`make_figures.py` 文档串与 `render_results.py` §15 复现块改为实际可用命令
  （`python3`，非 `-I`：隔离模式排除 user site-packages 的 matplotlib；
  `-I` 下 `make_figures.py` 实测 `ModuleNotFoundError: No module named 'matplotlib'`），
  并把 `render_status_table.py`（新增：`analysis/per_cell_table.md` 的确定性渲染器，
  81 行 status rows，与既有表逐行一致仅 AA_status 多带半宽数字）纳入链条。
- 终跑命令与退出码（工作目录 `/home/jnhu/sluice-595-e5/r4`，2026-11-06T10:36:41Z 起）：
  1. `python3 scripts/matrix_analysis.py` → **exit 0**（`analysis/analysis_run.log`）
  2. `python3 scripts/make_figures.py` → **exit 0**（`analysis/figure_rebuild.log`；
     stdout：`figures: 15 files`、`figure-data rows: 370`、`captions -> …`）
  3. `python3 scripts/render_status_table.py` → **exit 0**（81 status rows）
  4. `python3 scripts/render_results.py` → **exit 0**（169 行 RESULTS.md，嵌入下列 sha）
- 确定性复核：五张 PNG 的 sha256 与首次/二次重建逐一相同（F1 `42868c39…`、
  F2 `356abbd8…`、F3 `f9fe24cd…`、F4 `4530623e…`、F5 `2200fed3…`）；
  `figure-data.csv` `c2c3294b…`、`CAPTIONS.md` `c4fc4c3e…` 与二次重建相同
  （F5 行 T1 1.0/1.0/0.0、T2 8.3617/2.0426/0.98936、T3 7.9234/2.0003/0.99973 复核入 CSV）。
- 终版 sha256：

| 输出 | sha256 |
|---|---|
| `analysis/MAIN_SUMMARY.json` | `3d56dd2c22ae1d32165c38d3198549f17c52a89826845494561eb0c8bbf4b30c` |
| `scripts/matrix_analysis.py` | `74e92ec25496741dccd024fee74de52b63a8c63521167944f56ef519c32230b2` |
| `scripts/make_figures.py` | `294384497644380e273f5f620f39e59efd81b0a225c2d7d5075e343c17231312` |
| `scripts/render_status_table.py` | `1c3aef2ee23acf2d5a50e28141ece0c9fa2f030471ea34b9da926471cb62aed7` |
| `scripts/render_results.py` | `2cf6d78b93efa090633cb4283854feb53d9fc2f2a4c12a66e26bf4f23832d6ed` |
| `analysis/per_cell_table.md` | `62c76304a98615b94901fc2b213e2c62cfe84174c537bb3b1f6f02c98ee4acea` |
| `RESULTS.md` | `1ed716083892891c010321bcbf53711274334f29b9c8fff3bc8b795fbe27d0f2` |
