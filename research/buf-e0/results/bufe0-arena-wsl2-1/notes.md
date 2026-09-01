# bufe0-arena-wsl2-1 — secondary arena-regime probe notes

16 runs (phase A x {4K, 64K} x slots 8 x 4 arms x R7/R14), gates green.
Exploratory-secondary (prereg §5), NOT primary evidence: default glibc
allocator (no pinning) — the regime a long-running process actually
experiences for repeated construction/free cycles.

## Result

b0 construction with arena recycling: 4K = 60 ns/buffer (0 faults —
arena pages stay resident, memset only), 64K = 760 ns/buffer (0 faults).
Compare the pinned fresh-page regime (formal session): 4K = 4795 ns,
64K = 26928 ns per buffer. The pinned regime is therefore the CONSERVATIVE
upper bound on B0's lifecycle cost: real processes sit between (cold
start = pinned; steady arena reuse = this probe). This double-bounds the
F01 conclusion: eager-init's extra cost is either shifted (fresh pages)
or shrunk to pure memset (~60-760 ns/buffer, arena) — never a material
application-level tax (amplifier: replica-b1 == replica-b0).

b1/b2/b3 in arena regime are not meaningful comparisons (their arms are
defined by the fresh-page design); recorded for completeness only.

## Environment classification

Same as the formal session: QUALIFIED_BUT_VIRTUALIZED,
ENVIRONMENT-LIMITED absolute numbers.
