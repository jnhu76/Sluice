# Sluice Research Results

This file keeps only durable results from the pre-reset research era. The research process, preregistrations, campaign machinery, raw session directories, plots, and intermediate reports were intentionally removed from the current tree. Git history remains the archive.

These results are historical evidence, not current architecture authority. Current C++ code defines the implementation. Future verification and optimization work should be derived from the reduced codebase rather than from the old campaign structure.

## 1. Control-plane and runtime cost

### Semantic capability cost was small in the measured pilot

The TAX-0 semantic-floor ladder measured the explicit semantic floor at roughly **+37 instructions/op** over raw liburing across the sampled size/depth cells. In the same WSL2 campaign, the higher-level backend abstraction added roughly **~2,015 instructions/op** as a mostly fixed per-operation cost, and the continuation/runtime layer added roughly **~800 instructions/op** in representative cells.

The WSL2 measurements were environment-limited and are retained only as a cost-shape result, not as a universal magnitude claim.

### Native Host-0 showed a regime split

On the native Fedora/x86-64 Host-0 campaign:

- small, control-plane-dominated 4 KiB I/O showed material backend overhead;
- the backend layer used roughly **2.8–5.1x** the semantic-floor instructions/op in the small-I/O cells;
- for **64 KiB–2 MiB** application-relevant I/O, no material backend tax was detected in the tested cells;
- runtime-layer composite verdicts remained gray because instruction overhead and wall-time behavior did not move consistently together;
- broad cross-host performance claims were not established.

Durable conclusion: fixed per-operation machinery matters most when useful I/O work is small. It is largely amortized by larger operations.

## 2. Buffer construction and alignment

### Eager initialization was mostly first-touch cost shifting

Replacing eagerly zero-initialized slot buffers with uninitialized storage reduced construction work, but the saved page-touch cost reappeared during first useful I/O. In the realistic copy amplifier, uninitialized construction did not produce a material end-to-end benefit.

Durable conclusion: **do not optimize buffer initialization merely to avoid construction-time zero fill without a measured whole-lifecycle benefit.**

### Alignment was real in microbenchmarks but not a stable application lever

The WSL2 buffer study isolated a large user-buffer alignment effect. Native replication confirmed the same structural signature at much smaller magnitude: the tested +16-byte page offset was slower than aligned alternatives in 4 KiB–64 KiB READ microbenchmarks, typically around **1.14–1.42x** on Host-0 rather than the much larger WSL2 effect.

A stricter application-level causal test then found **no material copy-workload benefit** from changing exposed pointer phase in the tested 4 KiB–64 KiB, depth {1,2} cells.

Durable conclusion: **alignment is a real low-level variable, but the tested effect was microbenchmark-only for the production copy workload. No alignment control knob or runtime adaptation was earned.**

## 3. Chunk size was a much larger application-level lever

The Host-0 chunk-size × depth sweep found a host-local sweet region rather than a universal optimum:

- depth 1: no stable plateau inside the tested range;
- depth 2: local plateau entry at **1.5 MiB**;
- depth 4: plateau entry at **1.5 MiB**, and this was the only plateau that stayed flat through the tested 4 MiB boundary;
- depth 8: local plateau entry at **768 KiB**;
- the largest sampled point was often still competitive, so no global optimum was established.

Durable conclusion: chunk size had a much larger effect on application throughput than the alignment treatment. The campaign did not justify changing the production default automatically.

## 4. G1-Control experiments did not establish a general control thesis

### Fixed-file resource identity

The fixed-file experiment supported fixed-file use as a product capability and confirmed that Linux owns the physical lifetime of already-bound kernel resources. It did **not** establish the broader G1-Control thesis, and it did not show that Sluice needed new lifetime machinery beyond disciplined local handling.

### Copy transformation boundary

The copy experiment showed that a composed copy operation can be a legal transformation boundary while primitive operation semantics remain rigid. Explicit fallback and mechanism reporting were useful, but the result was implementable as a **thin local branch**.

Durable conclusion: **a generic capability framework was not earned.** Performance adjudication was blocked by measurement instability on the available host, so no kernel-transfer performance claim survived.

### Batch

The Batch experiment found that the current Batch abstraction did **not** grant group-admission semantics. The compliant performance rerun was blocked by the frozen filesystem requirement on the available host. Structural analysis showed that primitive consecutive submits already receive transport batching from the backend in relevant cases.

Durable conclusion: **G1-Control was not established; a thin semantic floor was sufficient; no new generalized Batch control layer was earned.**

## 5. Optimization guidance carried forward

The old campaigns support only a narrow engineering rule set:

1. Profile the current reduced implementation first.
2. Optimize measured local hotspots, not architectural stories.
3. Keep per-operation fixed costs visible in small-I/O regimes.
4. Do not promote a microbenchmark effect into the public control surface without application-level evidence.
5. Prefer a thin local mechanism when it solves the observed case; do not generalize it without a second real use.
6. Treat wall time and instruction count as separate signals; improvement in one does not guarantee improvement in the other.

No old research campaign, candidate ladder, or roadmap is carried forward as authority.
