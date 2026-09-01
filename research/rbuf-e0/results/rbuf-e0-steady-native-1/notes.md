# rbuf-e0-steady-native-1 — notes

RBUF-E0 formal steady-state matrix session (#272), Host-0, 2026-09-02, prereg
RBUF-E0-PREREGISTRATION.md (FROZEN). Seeded blocked-interleaved ordering
(random.Random(0xE1E1E1E121212121 + round)), R=7, perf-stat-wrapped,
driver-side dst sha256 gate per run, 0 gate errors.

Verdicts live in analysis.json and RBUF-E0-REPORT.md. Secondary
observations recorded there: U2 direction flips across steady cells
(sub-material everywhere); amortization-session teardown region carries
2.5-14.4 s arm-independent filesystem/page-cache cost absent from transfer
spans (steady-session teardown ~0.26 s) — mechanism observed, mechanism
attribution not claimed.
