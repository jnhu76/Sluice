# Sluice

Sluice 是一个 C++20 显式文件 I/O 库：统一 File 契约、显式 direct/request
执行、有界进展，支持面内不含 host runtime。

[English](README.md)

## 规范权威

**[Sluice v2 架构与契约](docs/explicit-io-contract.md)**（修订 v2-r1）是唯一
规范根。它也是后续工作的术语字典与设计支架，规定文件语义、调用方式、请求与
观察者生命周期、进展、关闭、线程安全、资源预算和验收要求。原 v1 根与其台账
已被取代，Git 历史保留其文本。

ADR 根据其中的稳定 requirement ID 细化实现选择；C++ 说明当前行为；测试与模型
提供有范围的证据。三者都不能独立改变根规范。

- [v2 符合性台账](docs/roadmap/v2-conformance.md)：对照 v2 根记录实现状态、
  证据、开放项与发布门槛；批准目标文档不代表代码已经符合。
- [docs/README.md](docs/README.md)：文档地图——哪些是现行文档，哪些在 `docs/archive/` 归档。
- [AGENTS.md](AGENTS.md)：仓库工作规则。
- [v1 交互式架构演示](visuals/sluice-v1/)：v1 时期的可视化，仅作溯源保留，
  不是当前契约。
- [代码架构快照](docs/archive/architecture/architecture.md)：带基线的历史实现视图，不是 v2 目标图。
- [研究结论](research/RESULTS.md)：保留的论据与证据。

长期原则保持为：**语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

## 目标架构

canonical `File` 拥有 native resource 与 access 事实。direct completed-return
与显式 outstanding request 共用操作语义。

Direct execution 不依赖请求表或 runtime。显式 `IoContext` 拥有有界
RequestCore、一个 backend 和 progress source；move-only Request 表达尚待履行
的操作责任。backend seam 是内部的：v2 只随附 ThreadPool 与 io_uring 两个
backend，用户自写 backend 不是受支持配置。stackful host 及其 fiber/cancel
组件在 v2 之外（由实现 PR 物理删除）；有界管线改用 owned-driver 的
`RequestScope`。

请求公开完成、消费结果、回收 slot 是不同事件；取消等待不结束 buffer borrow。
执行资源关闭后仍保留未消费的结果，context 销毁则要求所有 public binding 已释放。
具体契约与 Linux 范围以根规范为准。

## 当前实现与收敛

当前代码树包含 canonical File/blocking 操作、Request/AsyncIoContext 请求核心
（ThreadPool 与 io_uring backend）以及 W-02 的 RequestScope。可选 stackful
host 候选在 v2 之外，等待实现 PR 物理删除（属主决策 D1）；在此之前它是无契约
的存量代码，不是保留选项。实现状态、开放缺口与发布门槛记录在
[v2 符合性台账](docs/roadmap/v2-conformance.md)；v1 收敛战役（Phase M/F）是已
完成的历史，不再约束任何工作。

旧 mission、ADR-0001/0002、配套架构图和
[旧 conformance roadmap](docs/archive/explicit-file-roadmap/explicit-file-conformance.md)
是已被取代的历史记录。其 CLOSED/CONFORMANT 结论只适用于原来审查的基线和契约；
被取代的 v1 根与其台账现在同样如此。

## 应用

- [sluice-copy](apps/sluice-copy/README.md)
- [sluice-hash](apps/sluice-hash/README.md)
- [sluice-grep](apps/sluice-grep/README.md)
- [sluice-tail](apps/sluice-tail/README.md)

## 构建

Sluice 使用 [Xmake](https://xmake.io)，需要 C++20 编译器。

```bash
git clone https://github.com/jnhu76/Sluice.git
cd Sluice
xmake f -m release -y
xmake
```

当前 target 以 `xmake.lua` 和 `xmake/` 为准。构建成功不代表全部 v2 目标配置已经实现。

## 许可证

[MIT License](LICENSE)。
