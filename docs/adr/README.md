# Architecture Decision Records

This directory contains the retained architectural decisions that still describe the current C++ system.

The implementation under `include/` and `src/` remains authoritative. ADRs explain durable design choices; they are not a history log or an execution roadmap.

## Retained ADRs

- [ADR-024S-sync-runtime-contract.md](ADR-024S-sync-runtime-contract.md)
- [ADR-application-runtime.md](ADR-application-runtime.md)
- [ADR-async-io-model.md](ADR-async-io-model.md)
- [ADR-async-primitive-lifetime-failfast.md](ADR-async-primitive-lifetime-failfast.md)
- [ADR-cancel-request-epoch.md](ADR-cancel-request-epoch.md)
- [ADR-execution-model.md](ADR-execution-model.md)
- [ADR-explicit-io-completion-authority.md](ADR-explicit-io-completion-authority.md)
- [ADR-explicit-io-request-contract.md](ADR-explicit-io-request-contract.md)
- [ADR-public-request-handle.md](ADR-public-request-handle.md)

Future ADR changes should describe current architectural decisions only. Historical campaign context belongs in Git history, not in current documentation.
