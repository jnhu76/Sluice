# R5 §8.5 — #578 R0 裁决摘要

日期：2026-10-29。完整报告：`closure/DUCKDB_FIO_R0_PREREG.md`（同文已随
dududu R0 Research PR 入库，引用 Sluice #578）。证据：
`closure/r5-native/`（串行 lane 原始输出）、`closure/r5-prep.md`（协议冻结）、
`closure/r5-async-host.md`（§8.4 异步准入）。全部条目标注
[RUN]/[TRANSCRIBED]/NOT_RUN_WITH_REASON。

## Verdict

**`R0_READY_FOR_NEXT_OWNER_GATE`**

## §8.5 八问速答

1. **fio Native Baseline 真实建立 [RUN]**：fio 3.40（fio-3.40-3.fc44）、
   6 个 frozen section（engine×iodepth×bs×读写型×cache×dataset，节间不可比）、
   3×8 GiB /dev/urandom dataset 逐轮 SHA、btrfs
   `compress=zstd:1,ssd,discard=async` 身份、governor schedutil 未改；settled
   相邻对差 ≤2.3%；round-1 全节慢样本（分配后滞回）已披露未删除；randrw 仅
   2 轮欠采样声明。基线：sync-4k direct≈buffered≈cold-buffered
   ≈3.2–3.3k IOPS；libaio/io_uring qd32 ≈4.0k 不可分；seqread-1m
   330–389 MiB/s（唯一带宽形态）。
2. **DuckDB Native Baseline 真实建立 [RUN]**：SHA-scoped 上游构建
   （`069cc9f9…` = 远端 `refs/tags/v1.5.6`，无 patch），dataset
   lineitem.parquet 207,130,013 B（SHA `b7583e65…0ef911`），官方 harness +
   `benchmark_runner`。`--version` 显示串为已知展示性缺口（depth-1 无 tag
   对象），身份以 SHA 为准。
3. **A/A 噪声可接受 [RUN]**：73 对（W1 29 / W1b 29 / W2 15，各工作负载 ≥15
   对）。of-record：W1b（taskset -c 2-9 --threads=4）CV 4.46%、相邻对差
   mean 4.90%/max 13.86%；W2（4-query oracle suite 整进程 wall）CV 1.67%、
   max 4.08%。未绑核默认配置（W1）CV 7.39%/max 23.87% 记录为未来 A/B 必须
   规避的噪声域。分辨力：W1b n=15 约 ±3–5%，W2 约 ±1%；无预注册效应量的
   power analysis（诚实缺口）。
4. **SQL/字节 Oracle 通过 [RUN]**：oracle.sql 两轮 4/4 CSV 哈希彼此一致且等
   于 prep-record；官方 result oracle（`226829357828.867781`，gate 在
   `benchmark_runner.cpp:122`）62/62 次 runner 调用零 INCORRECT；q2 组值符合
   TPC-H sf=1 标准值（6,001,215 行）。
5. **同步 Read Seam 已定位并符号化 [RUN]**：`pread(2) ←
   LocalFileSystem::Read ← ThriftFileTransport::read ← ParquetReader ←
   PhysicalTableScan ← PipelineExecutor`（strace -k），与
   `docs/duckdb-stable-io-map.md` §1.1 一致；冷=热请求流逐字节相同（492
   pread/37.17MB/0 短读）；344 次（70%）≤4KiB 元数据微读仅占 0.13% 字节；
   全部读非 512/4096 对齐（O_DIRECT 无法原样直通）；每线程独立 fd
   （28–31 openat）。内核级 perf 归因 NOT_RUN_WITH_REASON
   （perf_event_paranoid=2，放宽属安全设置变更未授权）。
6. **同步 Direct Adapter**：同步 seam 是 v1.5.6 唯一生产 I/O 形态（R0 D2
   范围可准入）；但 **O_DIRECT 在本机/本负载无可证明收益**——fio 证明
   4k 随机读 direct≈buffered≈cold（无隐藏设备性能），且观测流非对齐需
   bounce buffer。真实杠杆是 syscall/元数据削减（AC-1 形态：向量化读或
   元数据批读），为下一 owner gate 的实验建议，**非加速结论、未采纳 API**。
7. **真异步 Host Protocol 无合法完成驱动边界 [RUN+TRANSCRIBED]**：
   v1.5.6 无 pending 任务态、无外部完成驱动者、调度器层 TASK_BLOCKED 无复活
   通道；`ReadAheadIOTask` 等符号全树 0 命中（#556 v2.0-dev 谱系）。三分类：
   Sync Direct 可准入 / Submit+Wait 正确但非 True Async（每 in-flight I/O
   钉住一个 worker）/ 真完成驱动 = `REQUIRES_HOST_CHANGE`。非侵入探针
   （分支 `r5-async-host-probe` @ `4eafbc0`，P1/P2/P3）把源码推理升级为机器
   证据，与 `docs/duckdb-stable-io-map.md` §2.4 独立一致。
8. **下一阶段**：向量化/元数据批读 seam 的 A/B（明确非 Direct-I/O），同一
   oracle 门禁 + W1b/W2 噪声模型 + ≥15 对 + 绑核声明线程数 + 分配后先跑
   settling pass；真异步需 `r5-async-host.md` §10 的 host 变更最小形态 +
   D0' 对照臂，属 Owner 决策。

## 边界声明

R0 是**原生地板 + seam 身份 + 准入边界**结论，不是任何 Sluice/DuckDB/adapter
的应用加速成果——本轮未构建 adapter、未采纳公开 API、未改 v2-r1 契约、未
merge 任何 PR、未运行任何候选机制的 A/B。dududu R0 Research PR 保持 OPEN。
