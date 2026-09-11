# Sluice

Sluice 是一个 C++20 显式 I/O library/runtime。

它的目标是：**只暴露保持可观察 I/O 语义、正确性与真实资源边界所必需的信息；语义授权必须显式，执行机制与执行策略保持局部、可替换，并且只保留已经证明有价值的机制。**

[English](README.md)

## 项目宗旨

Sluice 的长期原则来自现有研究结论：

> **语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

```text
Minimal semantics.
Clear boundaries.
Explicit authority.
Named bounds.
Replaceable execution.
Minimum mechanism.
```

- [`docs/mission.md`](docs/mission.md) —— 冻结的规范性项目宗旨。
- [`docs/adr/0001-explicit-io-design-doctrine.md`](docs/adr/0001-explicit-io-design-doctrine.md) —— 研究结论支持的显式 I/O 设计准则。
- [`docs/adr/0002-explicit-file-api-architecture.md`](docs/adr/0002-explicit-file-api-architecture.md) —— 冻结 File-centric 语义、API 责任边界与可替换 execution 架构。
- [`docs/roadmap/explicit-file-conformance.md`](docs/roadmap/explicit-file-conformance.md) —— 从 ADR-0002 推导的当前 master 符合性台账与架构补齐 roadmap。
- [`research/RESULTS.md`](research/RESULTS.md) —— 当前保留的研究证据与结论。

## 架构一览

<p align="center">
  <img src="docs/assets/sluice-architecture.svg" alt="Sluice 架构概览" width="100%">
</p>

SVG 只负责压缩展示实现形态。[`docs/architecture.md`](docs/architecture.md) 描述当前代码现实；mission 与 ADR 定义规范性边界；conformance roadmap 记录当前 master 哪些节点已经符合这些边界、哪些 gap 仍需关闭。

当前 `sluice_core` / `sluice_async` 的 build 拆分不代表两套长期独立 I/O 语义。ADR-0002 冻结一个 canonical `File` resource 与共享 operation semantics，并把 Blocking、ThreadPool、io_uring 定义为显式、可替换 execution。

`File` 本身**不携带** blocking/async mode，也不选择 backend。execution 由调用/API boundary 显式选择，因此 caller 是否阻塞、是否产生 outstanding request、cancellation、lifetime 与 bounded-resource cost 都不会被隐藏。

## 研究已经冻结的设计护栏

现有研究不支持“更多显式信息自然带来更多 generic control / specialization / performance”的项目级假设。

长期保持：

```text
resource identity != fixed-resource optimization authority
operation grouping != fused / atomic admission authority
backend capability != semantic authority
hint / information != authority
```

Copy 研究表明，显式 composed operation 可以成为合法 transformation boundary，但一个 thin local branch 已足以表达得到证明的能力，generic capability framework 没有被赚到。

Batch 研究表明，知道 operations 属于同一 Batch 不等于获得 group-admission authority。

性能研究同样要求把 semantic contract 与 execution policy 分开：alignment、chunk size、queue depth、worker count、backend mechanism 等可以显著影响性能，但不能因为 benchmark 结果就自动升级成 public semantics。

## 当前实现

当前 master 已经拥有 canonical `sluice::File` resource，并显式表达 open/close/access 语义。File-facing async positional Read、positional Write 与 SyncData 已经沿既有 runtime seam 工作，没有把 File semantic authority 交给 Scheduler 或 backend。

仓库仍保留 historical blocking `FileReader` / `FileWriter` 世界，部分应用也仍直接消费 raw-fd explicit operation。这些被记录为 conformance gap，而不是第二套长期 File model。

异步部分包含 caller-owned completion、有界 request state、scheduler/runtime、取消、同步设施与 honest backend execution。repository-provided synthetic AsyncBackend 已经删除；生产 execution 保留 ThreadPool 与可用时的 io_uring。

当前架构工作明确分成三阶段：

```text
Phase A  先让 ADR 架构在代码中真实成立
Phase B  再证明并比较不同 execution 的优劣
Phase C  最后优化或增加 execution backend / capability
```

Phase A 的进度以 roadmap 为准。

## 应用

- [`sluice-copy`](apps/sluice-copy/README.md)
- [`sluice-hash`](apps/sluice-hash/README.md)
- [`sluice-grep`](apps/sluice-grep/README.md)
- [`sluice-tail`](apps/sluice-tail/README.md)

## 构建

Sluice 使用 [Xmake](https://xmake.io)，需要 C++20 编译器。

```bash
git clone https://github.com/jnhu76/Sluice.git
cd Sluice
xmake f -m release -y
xmake
```

当前可用 target 以 `xmake.lua` 和 `xmake/` 为准。

## 文档

- [`docs/mission.md`](docs/mission.md) —— 项目宗旨。
- [`docs/adr/0001-explicit-io-design-doctrine.md`](docs/adr/0001-explicit-io-design-doctrine.md) —— 研究结论对应的显式 I/O 设计准则。
- [`docs/adr/0002-explicit-file-api-architecture.md`](docs/adr/0002-explicit-file-api-architecture.md) —— 规范性的 File API 与 execution 架构。
- [`docs/roadmap/explicit-file-conformance.md`](docs/roadmap/explicit-file-conformance.md) —— 实现符合性台账与架构补齐 roadmap。
- [`docs/architecture.md`](docs/architecture.md) —— 从当前 master 推导出的架构快照。
- [`research/RESULTS.md`](research/RESULTS.md) —— 保留的研究结论。

## License

Sluice 使用 [MIT License](LICENSE)。
