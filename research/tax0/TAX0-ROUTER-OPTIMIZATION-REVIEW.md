# TAX-0 / T0-U-ROUTER — Evidence-Driven Performance Optimization Review

本报告记录一次完整的性能优化战役，并把其中可复用的方法抽象为通用流程。
Sluice 的 router 优化是案例；真正要保留下来的，是“如何证明热点、如何选方案、
如何设计 benchmark、如何统计结果、如何把结果安全地落到生产代码”的方法。

关联：#227（roadmap）/ #250（TAX-0）/ #254（EXP-U0）/ #255（fix selection）/ #256（candidate shootout）。

---

## 0. Executive summary

### 0.1 这次到底优化了什么

`UringAsyncBackend` 用 64-bit cookie 关联 SQE/CQE 与内部 request router。
当前 production 路径需要在固定 router 中查找匹配 cookie；在本次测量基线中，
活跃 request 往往集中在数组高位，而 production 从低位向高位扫描，因此在
`request_capacity = C` 明显大于实际 active depth `D` 时，会为大量空 entry
付出额外 CPU 指令。

EXP-U0 只改变扫描方向，保持 cookie、router、匹配谓词、请求生命周期与 workload
不变，测得容量相关指令斜率从约 `+6 instr/op/C` 降到约 0，因而把该容量税
因果归因到 completion-time router scan。

### 0.2 候选方案与最终选择

| candidate | 方案 | 语义状态 | 结果 |
| --- | --- | --- | --- |
| R0 | 当前 production：高位活跃 entry + forward scan | baseline | 有明显 capacity tax |
| R1 | 保持放置策略，仅改为 reverse scan | 保持现有 identity / stale-CQE 契约 | **最终选择** |
| R2 | 改成低位优先放置，继续 forward scan | 保持契约 | 与 R1 端到端 practical tie |
| R3 | 增加有界 cookie→router-index 表 | 保持契约；增加固定 metadata 与 insert/erase 维护 | micro 更强，端到端未形成优势 |
| R4 | reusable index / raw pointer / packed identity 等 | 会削弱或复杂化现有 stale-CQE / ABA / no-wrap 身份约束 | benchmark 前拒绝 |

### 0.3 最重要的性能结果

Layer B 是 production-selection authority：真实 io_uring、READ/WRITE、tmpfs/btrfs、
7 组 `D/C` geometry、同场次 R0 归一化。

| candidate | GM instructions | 约等于 | GM cycles | 约等于 | worst instr | worst cycles |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| R1 reverse scan | **0.8397** | **−16.03%** | **0.9093** | **−9.07%** | +1.58% | +4.17% |
| R2 low-index placement | 0.8387 | −16.13% | 0.9072 | −9.28% | +0.46% | +4.54% |
| R3 bounded table | 0.8443 | −15.57% | 0.9023 | −9.77% | +1.62% | +3.25% |

没有候选在冻结规则下形成 ≥0.02 normalized-ratio 的明确领先；三者构成
practical tie，因此按预注册的 simplicity order 选择 **R1**。

高 capacity / 低 active-depth 的格子收益更明显。例如 READ/tmpfs `D=8,C=512`：

- R0：约 7857 instr/op
- R1 normalized：0.6161
- instruction reduction：约 **38.4%**

测试矩阵中的高容量格整体约出现 **35–40% total-instruction reduction**；
`D == C` 时接近 1.00，说明本次修掉的主要就是“capacity 大于实际活跃深度”
带来的扫描税，而不是所有 I/O 成本。

### 0.4 当前 production 是否已经变快

**没有。**

#256 完成的是研究候选、验证工具与 fix selection；R1 仍属于
`SLUICE_ASYNC_INTERNAL_TESTING` research seam。master 的 production path
仍保持原有行为。因此本文的 `−16% instructions / −9% cycles` 是
**候选在真实 io_uring shootout 中的实测收益**，不是 production landing 后的
最终 before/after 数字。

### 0.5 Copy 是否已经变快

**COPY-AB-1（application-level copy A/B）已于 2026-08-31 完成测量；
同日完成 evidence corrective（判定统计按冻结规则重算，raw 数据未重跑）。**

同一 copy engine（`run_pipelined_copy_with_backend`，真实 `sluice-copy`
bounded pipeline）、同一 256 MiB 确定性源、唯一变量为 R0/R1 router mode
（#256 seam 注入真实 `UringAsyncBackend`）；tmpfs + warm btrfs、
{4K,64K,1M} × {P=1,8,32} × {C=P+1,128,512}、Q=64、9 个 blocked-randomized
rounds、seed `0x434F5059`；A/A 校准 + 外部冻结 materiality rule（2×A/A 噪声
与 2% 取大，作用于 per-cell paired median）；全部六 artifact 通过外置
manifest validator 的 campaign seal（跨工件一致性 + paired 重算）。

结果——先给决策统计（authoritative：per-cell paired-median 分类），
再给描述统计（descriptive：GM ratio-of-arm-medians，不参与分类）：

- instructions/byte：**18/27 cells material（每个 fs），且恰好是全部
  18 个 capacity-skewed cells（C=128: 9/9，C=512: 9/9）**；C=P+1 的
  9 个 cells 0/9 material（btrfs 上两个深 pipeline 近容量 cell 为
  +2.4–2.5% 的 material regression，tmpfs 同几何 +1.8% 在带内）。
  描述 GM **0.898 (tmpfs) / 0.889 (btrfs)**。
- cycles/byte：决策门槛 ratio ≤ 0.8177（纠正后包络），仅
  C=512 × {4K,64K} 的 6/27 cells material（paired-median ratio
  0.760–0.815）；描述 GM 0.922 / 0.920。
- wall：**0/27 cells material（两个 fs）**；描述 GM 0.977 / 0.993。
- ThreadPool 对照组（production backend，无 router/无 C 参数）：
  harness-stability control（无 C 维度可操纵）；instr/byte rel. sd
  ≤0.3%，harness 稳定。

判定：**COPY-AB-1 PASS — BENEFIT ONLY IN CAPACITY-SKEWED REGIMES**
（corrective 后 SURVIVED）——R1 的应用级收益真实存在，但严格由
`C >> active depth` 驱动；near-capacity sizing 下 R1 ≈ R0。这与 EXP-U0
的因果模型定量吻合。router 因果继承自 EXP-U0/#254 与 #256 shootout；
COPY-AB-1 是应用效果验证，不独立记录 router 迭代 witness。

> Copy application CPU cost: SUPPORTED — SCOPE-BOUNDED（instruction 维度
> material，范围 = capacity-skewed cells，paired-median 决策）。
> Copy wall throughput: NOT MATERIAL / NOT YET SHOWN（测试矩阵内）。

---

## 1. Evidence model

报告区分三类证据，避免把测量、外部资料和工程解释混成一种“事实”。

| evidence class | authority | 用途 |
| --- | --- | --- |
| Internal measurement | Sluice runner → machine-readable artifact → validator | 本报告所有 Sluice 性能数字 |
| External evidence | 原始论文、官方项目/文档、上游实现 | prior art 与 AI/SOTA 背景 |
| Engineering interpretation | 由上述证据推导出的设计判断 | 候选解释、适用范围、后续计划 |

内部性能 claim 必须遵循：

```text
FACT
  ↓
SCOPE
  ↓
CLAIM
  ↓
STATUS
```

不允许从一个单点 benchmark 直接跳到“系统更快”。

---

## 2. 通用优化流程：从热点到 production

这次战役可以抽象为以下通用流程：

```text
Measurement qualification
        ↓
Profile / topology audit
        ↓
Causal attribution
        ↓
Prior art + semantic admission
        ↓
Candidate freeze
        ↓
Micro benchmark + end-to-end benchmark
        ↓
Fail-closed validation
        ↓
Mechanical selection
        ↓
Human adversarial review
        ↓
Production implementation
        ↓
Canonical before/after
        ↓
Application-level A/B
        ↓
Residual-cost decomposition
        ↓
Next hotspot or stop
```

### 2.1 为什么不是“看到 O(N) 就改”

复杂度只能告诉我们“哪里值得怀疑”，不能告诉我们“这笔成本在当前 workload
里值多少钱”。本次静态审计早就能看到 router scan 是 O(C)，但直到 EXP-0 / EXP-U0
才证明：

1. ThreadPool arm 没有同类 capacity slope；
2. Uring arm 有稳定 capacity tax；
3. 只改变 scan direction 就几乎消除该 slope；
4. lookup iteration witness 与性能斜率同步变化。

所以 optimization authorization 来自 **causal evidence**，不是代码形状。

### 2.2 为什么 prior art 不能直接照抄

liburing/fio/Tokio/tokio-uring/Monoio/Glommio/Seastar/Boost.Asio 等系统对
`user_data`、operation identity、lifetime、stale completion 的契约不同。

别人能把 pointer 或 reusable slab index 直接塞进 `user_data`，不代表 Sluice
可以无代价照搬。Sluice 当前依赖 no-wrap cookie 与 stale-CQE miss 语义来避免
旧 completion 冒领新 request。候选首先要过 semantic admission，然后才有资格跑分。

---

## 3. Benchmark 设计：为什么要两层

### 3.1 Layer A：机制隔离

目标不是预测最终系统速度，而是解释候选本身的物理成本：

```text
install cookie
    ↓
lookup completion cookie
    ↓
retire cookie
```

A2 修正后 measured region 内 allocation 被实测为 0；所有候选消费相同 trace。

A2 官方 GM：

| candidate | GM instr | GM cycles |
| --- | ---: | ---: |
| R1 | 0.4329 | 0.5184 |
| R2 | 0.4329 | 0.5013 |
| R3 | **0.3699** | **0.4242** |

这说明 R3 在隔离的 router lifecycle 里总体 CPU 工作更少，但不能据此直接选 R3。
R3 还要支付 insert/erase、metadata footprint 和真实 cache 行为；这些只能由
end-to-end 层裁决。

一个重要细节：R3 的优势不是“C 越大一定越快”。R1 的 reverse scan 更接近
D-dependent cost，而 R3 更接近 capacity-independent lookup + fixed maintenance。
因此 crossover 取决于 D、C、维护成本与 cache 行为。例如 A2 数据里：

- `D=8,C=512`：R1 比 R3 更低；
- `D=128,C=512`：R3 明显低于 R1。

所以报告不把 R3 简化为严格 worst-case `O(1)`，只描述为：

> 在本次有界 table、load factor 和受测 geometry 下，lookup 呈近似
> capacity-independent；完整 lifecycle 仍包含 bounded table 的 insert/erase 与 probing 成本。

### 3.2 Layer B：真实系统裁决

Layer B 使用真实 liburing，覆盖：

- READ 4 KiB / WRITE 4 KiB；
- tmpfs / warm btrfs；
- `D8/C8,32,128,512`；
- `D32/C32,128,512`；
- `Q = D`，避免把 dispatch backlog 混进 router fix selection；
- 128 MiB/process/cell；
- 2 warmup + 9 measured reps；
- blocked randomized rounds；
- same-session R0 normalization。

生产选型以 Layer B 为准；Layer A 只负责解释机制。

---

## 4. 统计、归一化与选型

### 4.1 同场次归一化

每个 cell 内先计算：

```text
normalized_instr  = candidate instr/op  / R0 instr/op
normalized_cycles = candidate cycles/op / R0 cycles/op
```

解释：

- `1.00`：与 R0 持平；
- `0.84`：candidate 只需要基线 84% 的成本，即减少约 16%；
- `1.05`：比基线多约 5%。

同场次归一化比跨天比较绝对值更能抵御环境漂移。

### 4.2 为什么用几何平均

不同 cell 的结果是比例，整体聚合使用 geometric mean：

```text
GM = (r1 × r2 × ... × rn)^(1/n)
```

它适合聚合 ratio，也避免大型绝对值 cell 对算术平均造成不合理支配。

### 4.3 当前 winner rule

冻结规则主要包含：

1. correctness / semantic gate；
2. 每个合法 cell instructions 与 cycles regression 不得超过 +5%；
3. 以 overall GM instructions 为主；
4. cycles 不得形成明显反向退化；
5. 进入 practical tie 时比较两轴 GM 与 worst-cell tails；
6. practical tie 内按预注册 simplicity order 选更简单的实现。

这里的 tie threshold 应精确写成：

> `T = 0.02 normalized-ratio band`，即 **2 percentage points 的绝对 ratio 差**，
> 不是严格的 multiplicative “relative 2%”。

当前 `reps=9` 也不意味着“能分辨 2%”。本次尚未用 A/A noise floor 和 power
analysis 标定最小可检测效应，2pp 是预注册工程 guard，而不是统计显著性结论。

### 4.4 下一版统计升级

本次 blocked design 已经天然具有配对结构。后续应优先统计：

```text
per randomized block:
    log(candidate / R0)
        ↓
paired effect
        ↓
cell / session / campaign aggregation
```

并采用 preserving hierarchy 的 bootstrap（session → cell → round），而不是把
所有 raw rows 当 IID 样本直接重采样。

---

## 5. 从 bench result 到代码选择

### 5.1 为什么不是 R3

R3 在 micro 层总体最省指令，但 Layer B：

- R1 GM instr = 0.8397
- R2 GM instr = 0.8387
- R3 GM instr = 0.8443

三者没有形成冻结规则要求的明确领先。

这说明：

> **更好的局部 lookup complexity 不等于更好的系统实现。**

R3 的 lookup 收益被 table insert/erase、metadata/cache cost 部分抵消；当真实 I/O
完整路径加入后，端到端没有形成足以支付额外复杂度的优势。

### 5.2 为什么最后是 R1

R1 和 R2 在端到端几乎相同，但 R1：

- 不改变 cookie identity；
- 不改变 router placement/reuse policy；
- 不增加 metadata；
- 不增加维护路径；
- 只改变已有 search 的 traversal direction。

因此在 performance practical tie 时，R1 具有更小的 semantic surface、实现复杂度
和维护成本。

这次选型真正证明的不是“reverse loop 神奇地最快”，而是：

> **在受测矩阵内，复杂候选没有产生足以支付复杂度的端到端优势，因此最小修复胜出。**

---

## 6. Production landing：现在还缺什么

#256 已经把：

```text
CAUSE ATTRIBUTED
    ↓
OPTIMIZATION EARNED
    ↓
CANDIDATE SHOOTOUT COMPLETE
    ↓
FIX SELECTED = R1
```

跑完。

但生产闭环还缺：

```text
R1 production implementation
    ↓
correctness gates
    ↓
canonical EXP-0 before/after
    ↓
shootout key-cell confirmation
    ↓
application-level A/B
    ↓
recovered tax + residual tax
```

只有完成这条链后，才能把“R1 candidate 在 shootout 中减少约 16% instructions”
升级为“production Sluice 实际减少 X%”。

---

## 7. Copy workload：如何回答“copy 是否变快”

### 7.1 当前答案

**尚未测量，不能声称 copy 已提升。**

本次 Layer B 已经证明 R1 在 READ 与 WRITE 的受测 session 上方向一致，因此 copy
值得优先做 application-level validation；但 copy 是组合 workload，不能用两个 micro
或单 I/O arm 的百分比直接相加。

### 7.2 为什么可能有收益

如果 copy pipeline 使用 Uring backend，每个 read/write completion 都需要 cookie
resolution。只要 `C > D`，R1 会减少这部分 completion routing CPU 工作。

粗略地说，如果 copy 总 CPU 成本中有比例 `f` 来自本次可消除的 router capacity tax，
而 R1 回收其中比例 `r`，那么整体 CPU 改善受 Amdahl 型上界约束：

```text
overall_cpu_reduction ≈ f × r
```

但 `f` 目前未知，因此不能从 shootout 直接推出 copy 的百分比。

### 7.3 Production R1 落地后的 copy A/B

建议至少覆盖：

| dimension | cells |
| --- | --- |
| backend | Uring；ThreadPool 作为 negative/control arm |
| file size | 64 MiB / 1 GiB（至少一小一大） |
| block size | 4 KiB / 64 KiB / 1 MiB |
| depth | 1 / 8 / 32 |
| request capacity | `C=D` 与 `C>>D` 两类 |
| filesystem | tmpfs + 本地真实 filesystem |
| metrics | wall throughput、instr/byte、cycles/byte、user/sys time、CPU% |

关键问题不是“某一格有没有快”，而是检查形态：

1. `C == D`：预期几乎无 router-fix 收益；
2. `C >> D`：预期 CPU instructions 明显下降；
3. 大块/慢设备：wall throughput 可能几乎不变，因为 I/O 本身占主导；
4. 内存/缓存命中、CPU-bound copy：更可能把 CPU 减少转换成 wall-time/throughput 收益。

验收必须同时报告：

```text
CPU work reduction
≠
wall throughput improvement
```

这会直接回答“我们是把 runtime 变薄了，还是用户实际 copy 也更快了”。

---

## 8. 如何提高优化上限

性能上限至少分三层。

```text
B0  blocking sync I/O
B1  naive raw io_uring
B2  competent raw io_uring
B3  current Sluice
B4  optimized-current Sluice
```

### Ceiling 1 — avoidable implementation tax

```text
B3 → B4
```

当前 router capacity tax 属于这一层。目标是删除不必要工作，而不是改变 Sluice
的核心语义。

### Ceiling 2 — abstraction/runtime residual tax

```text
B4 vs B2
```

当明显 implementation tax 清理后，与 competent raw io_uring 比较，才能知道 Sluice
为了生命周期、调度、发布、取消等语义付出的 residual runtime tax。

### Ceiling 3 — workload architecture

当 runtime tax 已接近测量底线，应停止继续磨 0.x% 的局部代码，转向：

- batching；
- registered/fixed buffers；
- submission/completion coalescing；
- page/cache-aware I/O；
- application-runtime co-design；
- 减少 wake、publication、copy、context-switch 次数。

### Stop condition

每一层都应该允许“停止优化”。例如：

```text
if expected_recoverable_gain < measurement_floor
or no material hotspot remains
    → stop local TAX optimization
    → move to next ceiling / application architecture
```

这样飞轮不会退化成无限寻找 0.3% hotspot。

---

## 9. AI 与自动化：把 AI 放在正确的位置

AI 最适合扩大搜索空间，而不是拥有正确性与性能 verdict。

### 9.1 四种适合 AI 的角色

| role | AI 做什么 | 裁决者 |
| --- | --- | --- |
| Hypothesis generator | 根据 profile、source topology、历史 evidence 提出下一笔 tax | causal experiment |
| Candidate generator | 生成实现候选、ablation、alternative data structures | semantic gates + benchmark |
| Adversarial experiment designer | 主动寻找 benchmark blind spot 与 falsification case | preregistration + human review |
| Invariant/test generator | 生成 death/property/DST cases | mechanical correctness gates |

### 9.2 不应交给 AI 的权力

AI 不应自行决定：

- semantic equivalence 已成立；
- benchmark 没有作弊；
- evidence 可以删除异常 cell；
- speedup 足够 merge；
- production correctness 可以由性能结果覆盖。

裁判仍由：

```text
semantic admission
+ correctness tests
+ same-work witness
+ measurement qualification
+ fail-closed validator
+ statistics
+ human adversarial review
```

组成。

### 9.3 与当前 SOTA 的关系

可参考的不同方向包括：

- **AlphaEvolve**：LLM + evaluator + evolutionary search；
- **AlphaDev**：RL / search 发现底层算法实现；
- **SWE-Perf / PERFOPT-Bench**：真实仓库 performance optimization 与 verified speedup；
- **SysLLMatic**：profiling-guided iterative source optimization；
- **Meta LLM Compiler**：compiler/IR 专用 foundation model；
- **MLGO**：ML 替代 compiler heuristic；
- **Coz / AutoFDO / Propeller / BOLT / OpenTuner / STOKE / Souper / e-graph**：
  非 LLM 但可作为 profiler、search space、evaluator 或 lower-level backend。

对于 Sluice，更合适的长期结构是：

```text
AI proposes
    ↓
semantic admission
    ↓
mechanical correctness
    ↓
benchmark
    ↓
statistics / Pareto frontier
    ↓
human-reviewed production decision
```

不要把 instructions、cycles、memory、worst regression、semantic surface 压成一个
opaque fitness score；更适合维护 Pareto frontier，再按预注册策略选 production winner。

---

## 10. 这次流程暴露了什么问题

这次战役真正有价值的一部分，是 benchmark/validator 自己也被对抗性 review 发现问题。

主要修复包括：

1. runner cell serialization/variable-shadowing；
2. micro validator path 缺少真实 fixture 覆盖；
3. multi-session aggregate key 错误；
4. Layer A measured window 内存在 allocation，但 artifact 曾声明为 0；
5. hex seed 解析/传递链错误；
6. selector cycles guard 方向错误与 cycle-tail sealing 不完整；
7. post-A2 又发现被 veto 的 singleton leader 可能被重新包装成 “practical tie” 选中。

结论没有因此翻转，但方法学发生了重要升级：

> **先验证尺子，再验证被测系统。**

---

## 11. 改进后的通用 Playbook

### Stage -1A — Measurement integrity（必须 PASS）

- perf counter 可用；
- CPU/core placement 明确；
- governor/environment fingerprint 记录；
- measured region bookkeeping/allocation 已检查；
- runner → artifact → validator roundtrip PASS；
- frozen manifest 独立于 artifact；
- source SHA / binary hash / dirty paths 可追溯。

任何一项失败，不进入正式 measurement。

### Stage -1B — Statistical calibration

- A/A noise floor；
- confidence interval method；
- paired/block-aware estimator；
- minimum detectable effect / power；
- guardrail / tie threshold 的测量依据。

未完成时可以做 causal/exploratory experiment，但不得把“低于噪声”当成已经被统计证明的结论。

### Stage 0 — Attribution

- profile/topology 找 suspect；
- 写可证伪 hypothesis；
- 单机制 ablation；
- 测出 recoverable-cost ceiling。

### Stage 1 — Prior art

固定记录：identity、lifetime、lookup、memory、allocation、stale handling、worst-case contract。

### Stage 2 — Semantic admission

语义不合法的候选在 benchmark 前淘汰。

### Stage 3 — Candidate freeze

冻结：candidate set、matrix、metrics、randomization、winner rules、validator。

### Stage 3.5 — Dry run

完整跑通：

```text
binary → runner → artifact → validator → aggregate → selector
```

并执行 mutation/property/truth-table tests。

### Stage 4 — Official measurement

- blocked randomized；
- same-work；
- raw evidence only；
- 禁止手改 artifact。

### Stage 5 — Validation

- matrix/session 集合相等；
- raw→derived 独立重算；
- mutation fail-closed；
- provenance 完整。

### Stage 6 — Selection

- semantic/correctness first；
- performance envelope second；
- worst regression visible；
- practical tie 不强行排名；
- complexity 只有在 performance 没形成明确优势时才作为 tie-break。

### Stage 7 — Production landing

- 单独 production PR；
- canonical before/after；
- application A/B；
- recovered/residual tax ledger。

### Stage 8 — Stop or iterate

- residual hotspot material → 下一轮；
- 低于 measurement floor → 停止该层优化；
- runtime residual 已小 → 转向 workload/application co-design。

---

## 12. Evidence index

Internal evidence：

- `research/tax0/TAX0-EXP-U0-ROUTER-CAUSALITY.md`
- `research/tax0/TAX0-ROUTER-PRIOR-ART.md`
- `research/tax0/TAX0-ROUTER-FIX-SELECTION.md`
- `research/tax0/TAX0-ROUTER-REFREEZE-A2.md`
- `research/tax0/TAX0-COPY-AB1-DESIGN.md`
- `research/tax0/TAX0-COPY-AB1.md`
- `docs/results/performance-attribution/tax0router-fix-micro.json`
- `docs/results/performance-attribution/tax0router-fix-shootout-{read,write}-{tmpfs,btrfs}.json`
- `docs/results/performance-attribution/tax0-copy-ab1-aa-{tmpfs,btrfs}.json`
- `docs/results/performance-attribution/tax0-copy-ab1-{tmpfs,btrfs}.json`
- `docs/results/performance-attribution/tax0-copy-ab1-control-{tmpfs,btrfs}.json`
- `scripts/bench/tax0router-validate.py`
- `scripts/bench/tax0-copy-ab1-run.py`
- `scripts/bench/tax0-copy-ab1-validate.py`
- `scripts/bench/perf-evidence-validate.py`

External directions：

- AlphaEvolve — Google DeepMind
- AlphaDev — Google DeepMind
- SWE-Perf — arXiv:2507.12415
- PERFOPT-Bench — arXiv:2607.07744
- SysLLMatic — arXiv:2506.01249
- Meta LLM Compiler — arXiv:2407.02524
- LLVM MLGO — `https://llvm.org/docs/MLGO.html`

---

## 13. Final status

### 已经证明

- Uring 存在 material capacity-dependent router tax；
- EXP-U0 将该 tax 因果归因到 forward router scan；
- R1/R2/R3 都能显著回收这一容量项；
- 真实 io_uring shootout 中三者 practical tie；
- R1 因最小实现/语义表面积胜出；
- 受测矩阵中 R1 平均约减少 **16% instructions/op**、**9% userspace cycles/op**；
- 高 capacity / 低 active-depth cell 总指令可减少约 **35–40%**；
- COPY-AB-1（真实 sluice-copy pipeline，唯一变量 R0/R1）：instr/byte
  决策统计（per-cell paired median）**18/18 capacity-skewed cells
  material**，C=P+1 时 0/9；描述 GM **0.898 (tmpfs) / 0.889 (btrfs)**；
  cycles 仅 C=512 × {4K,64K} 的 6/27 cells 过门槛（≤0.8177）；wall
  0/27 NOT MATERIAL —— 判定 **PASS — BENEFIT ONLY IN CAPACITY-SKEWED
  REGIMES**（PR #257，evidence corrective 后 SURVIVED）。

### 尚未证明

- production Sluice 已经获得上述收益；
- copy wall throughput 已提升（COPY-AB-1 明确 NOT MATERIAL，0/27）；
- 结论跨 CPU/kernel/compiler/机器保持相同比例；
- tie 2pp 与 guardrail 5pp 已由 noise floor / power analysis 校准；
- optimized Sluice 已接近 competent raw io_uring ceiling。

下一步应是：

```text
R1 production PR
    ↓
canonical before/after
    ↓
copy / application A/B
    ↓
B4 vs B2 residual tax
    ↓
决定继续 TAX optimization 还是进入 Ceiling 3
```

这套方法的核心不是“找到一段更快的代码”，而是把性能优化变成一条可证伪、
可复现、可审计的决策链：**先证明成本，再证明原因，再比较合法方案，最后才让
benchmark 结果进入 production。**
