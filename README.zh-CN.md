# Sluice

Sluice 是一个 C++20 显式文件 I/O 库，正向统一 File 契约、显式 direct/request
execution 和可选 I/O 控制流宿主收敛。

[English](README.md)

## 规范权威

**[Sluice v1 架构与契约参考](docs/explicit-io-v1-final-decision.md)** 是 v1 目标的
唯一规范根。它也是后续工作的术语字典与设计支架，规定文件语义、调用方式、请求与
观察者生命周期、进展、关闭、线程安全、资源预算和验收要求。

ADR 根据其中的稳定 requirement ID 细化实现选择；C++ 说明当前行为；测试与模型
提供有范围的证据。三者都不能独立改变根规范。

- [v1 符合性台账](docs/roadmap/v1-conformance.md)：记录实现缺口、阶段门槛与证据；
  批准目标文档不代表代码已经符合。
- [AGENTS.md](AGENTS.md)：仓库工作规则。
- [代码架构快照](docs/architecture.md)：带基线的历史实现视图，不是 v1 目标图。
- [研究结论](research/RESULTS.md)：保留的论据与证据。

长期原则保持为：**语义最少，边界清晰，权威显式，资源有界，执行可换，机制最小。**

## 目标架构

canonical `File` 拥有 native resource 与 access 事实。direct completed-return、
显式 outstanding request、受支持的 host completed-return 共用操作语义。

Direct execution 不依赖请求表或 runtime。显式 `IoContext` 拥有有界 RequestCore、
backend 和 progress source；move-only Request 表达尚待履行的操作责任。
Scheduler/Fiber 如被支持，应处于核心之上的可选 host 层，backend 不负责 Scheduler
waiter routing。

请求公开完成、消费结果、回收 slot 是不同事件；取消等待不结束 buffer borrow。
执行资源关闭后仍保留未消费的结果，context 销毁则要求所有 public binding 已释放。
具体契约与 Linux v1 范围以根规范为准。

## 当前实现与迁移

保留基线已有 canonical File/blocking 操作、caller-owned Completion、AsyncIoContext、
ThreadPool/io_uring backend 和 runtime。它们的存在不代表新目标已经实现。
后续迁移按根规范 A–G 阶段推进，在新台账中记录代码位置、配置与验证证据。

旧 mission、ADR-0001/0002、配套架构图和
[旧 conformance roadmap](docs/roadmap/explicit-file-conformance.md)
不再承担 v1 规范权威。其 CLOSED/CONFORMANT 结论只适用于原来审查的基线和契约。

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

当前 target 以 `xmake.lua` 和 `xmake/` 为准。构建成功不代表全部 v1 目标配置已经实现。

## 许可证

[MIT License](LICENSE)。
