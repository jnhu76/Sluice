# F0 §1.7 Decision Table — #506 BlockingIoPool Retirement

| Interface | v0.0.1 public | In-tree consumer | Migration / equivalent | Breakage impact | Owner approves deletion |
|-----------|---------------|------------------|------------------------|-----------------|-------------------------|
| `sluice/blocking_io_pool.hpp` | Yes | None | None; consumers migrate to per-op blocking calls or async runtime | Compile + link failure | ☐ |
| `sluice/detail/blocking_io_pool_impl.hpp` | Yes (installed detail) | None | None; implementation substrate of retired pool | Compile failure (usually co-broken with public header) | ☐ |

**Owner signature / date:** _________________________

**Conditions of approval:**
- [ ] Reviewed B-01 evidence at `docs/evidence/b01-506-blocking-io-pool-retirement.md`.
- [ ] Reviewed per-interface risk adjudication at `docs/evidence/f0-1-7-506-blocking-io-pool-risk.md`.
- [ ] Confirmed no in-tree consumer remains.
- [ ] Confirmed retained canonical W-01/W-02/W-03 surfaces are sufficient for v1.
