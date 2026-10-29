# R5 prep — #578 fio + DuckDB R0（准备阶段记录）

日期：2026-10-29。主机：E5（Fedora 44，kernel 7.2.11-200.fc44.x86_64，Xeon
E5-2666 v3，20 CPU，62 GB）。本文件是**准备阶段**记录：按任务书 §8.3 与硬性
边界 #6/#7，本阶段未做任何正式计时测量，未跑 A/A；全部条目标注
[RUN]/[TRANSCRIBED]/[UNCERTAIN]/[NOT_RUN_WITH_REASON]。

## 载体

- dududu 研究分支 `sluice-r0-closure`，基线 = 远端 live
  `origin/main` = `b94041d918672d113ea0ca667ba8ccdabf329c80`（fetch 后核验，
  未用陈旧 origin/main）。已推送远程（新分支，无 PR；R0 实验 PR 留给
  §8.5 阶段）。
- 分支三提交：
  - `bbce768` bench(fio): freeze e5 native storage baseline job set
  - `732a8fb` bench(duckdb): pin the sql oracle and io exposure protocol
  - `cc053cf` bench(duckdb): record dataset and oracle prep receipts
- 未 merge 任何 PR；未改任何上游源码；冻结 worktree 未触碰。

## fio [RUN]

- 安装：`dnf install -y fio` → **fio 3.40（fio-3.40-3.fc44，x86_64，Fedora 44
  官方仓库）**。sudo 仅用于此安装，未写入任何结果文本。
- 工具 smoke [RUN]：psync / libaio / io_uring 三个 engine 在本内核各完成一次
  2 秒级临时文件 I/O（工具可用性验证；结果丢弃，不是测量）。
- Job file：`benchmarks/fio-r0/`（6 个 section，全部 `fio --parse-only` 解析
  通过 [RUN]）。身份维度显式分节：engine（psync/libaio/io_uring）、iodepth
  （1/32）、bs（4k/1M）、读写型（randread / seq read / randrw 75/25）、cache
  （direct / buffered 分开命名）、dataset（3×8 GiB /dev/urandom，固定
  randseed=20261029）。README 明文规定：不同 section 是不同负载，任何数字
  不得跨节比较或替代；dataset 内容取不可压缩随机字节以排除 btrfs
  compress=zstd:1 的压缩路径干扰；randrw 会改写 dataset，须重做 prep。
- [NOT_RUN_WITH_REASON] 正式 fio 测量与 A/A：串行测量 lane（边界 #6）。
- [NOT_RUN_WITH_REASON] cold-cache buffered 协议：需 root drop_caches，超出
  本阶段 sudo 授权范围（仅限安装测量工具）；direct=1 各节不受影响。

## DuckDB [RUN]

- 受控初始化：`git submodule update --init --depth 1 third_party/duckdb` →
  HEAD = `069cc9f9b5be802405797faecc284961b07c70ef`；与远端
  `refs/tags/v1.5.6` 经 `git ls-remote` 相等（本机核验）；porcelain 干净。
- Native build [RUN]：`cmake --preset release` + `cmake --build build/cmake
  --target duckdb_native --parallel 16`（上游 CMake 选项，SHA-scoped 目录
  `build/duckdb-native/069cc9f9…/`）。GCC 16.2.1 / CMake 4.3.0 / Ninja
  1.13.2 编译通过。
- 入口点 [RUN]：`ctest -R duckdb` → **5/5 PASSED**（shell / unittest /
  benchmark.list / benchmark.smoke 含上游 result oracle / build.sha-refresh）。
- [UNCERTAIN→已解释] `duckdb --version` 显示 `v0.0.1 (Unknown Version)`：
  depth-1 无 tag 对象，configure 期 `git describe` 失败的展示性回退；commit
  SHA 正确，tag 对象拉取两次因网络失败。身份以 SHA 为准（README 上游 pinning
  一节已预记录此坑）。
- Dataset [RUN]：官方 harness 生成
  `build/duckdb-bench/duckdb_benchmark_data/lineitem.parquet`
  （207,130,013 B，SHA-256 `b7583e65198aaa9522067fd849f9d08f2668df2e7608fef548ab386cab0ef911`）。
  CLI 再生（dbgen sf=1 + COPY）哈希逐字节一致 → 本机可再生。
  `BENCHMARK_DIR` 写入落在 runner CWD 的 `duckdb_benchmark_data/`（v1.5.6
  `interpreted_benchmark.cpp:108`），生成后两个 submodule porcelain 干净，
  与 r0-report 声明一致。
- SQL Query Oracle + Stable Hash [RUN]：`benchmarks/duckdb-r0/oracle.sql`
  四条确定性查询（纯 DECIMAL/INTEGER、唯一键全序），两轮运行 4/4 CSV 哈希
  完全一致；行数 6,001,215 与 q2 returnflag 组符合 TPC-H sf=1 标准值。哈希
  见 `benchmarks/duckdb-r0/results/prep-record.md`（已提交）。
- I/O Exposure 方案（已验证，非计时）[RUN]：`strace -f -y` 温热单进程直聚合
  查询 → lineitem.parquet 上 **492 次 pread64 / 37,172,128 字节 / 0 短读**
  （exact-count 语义在 LocalFileSystem::Read → pread 接缝处可见）；尺寸谱：
  147×256 B 与 46–68 B 元数据尾、~62 KB 块、~634 KB 大块。trace 留存在 E5
  忽略目录 `build/duckdb-bench/oracle-out/`。冷热对比与按列归因留串行 lane。
- [NOT_RUN_WITH_REASON] Native A/A 与任何计时：串行 lane；本阶段零计时。

## 复用与不重复实现

全部工作复用 PR #1（MERGED）既有设施：SHA-scoped DuckDB ExternalProject
（cmake/DuckDB.cmake）、官方 benchmark harness 与 result oracle
（tests/cmake/RunDuckdbBenchmarkSmoke.cmake）、stable-io-map 的接缝结论。
本阶段新增仅为 fio job set、oracle.sql、协议 README 与 prep receipts，无新
桥接、无 parser、无 DuckDB patch、无 Sluice 改动。D1–D4 admission 边界维持
docs/experiment-matrix.md 原状。

## 交接给串行测量 lane

1. fio：按 `benchmarks/fio-r0/README.md` 运行模板逐节独占执行，manifest 记
   fio 版本、findmnt、dataset SHA、governor（schedutil 未改）。
2. DuckDB：A/A ≥15 对按 §8.3；dataset/oracle 身份直接引用 prep-record。
3. 冷缓存协议（fio buffered 与 DuckDB 观测）需 Owner 对 drop_caches 明确
   授权，否则按 warm-only 记录并声明局限。
4. R0 裁决与 `DUCKDB_FIO_R0_PREREG.md` 属后续阶段，本文件不预写结论。
