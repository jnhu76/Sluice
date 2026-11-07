# R2_PROVENANCE — 旧 B 同 SHA 复现的来源回执

| 项 | 值 |
|---|---|
| 树 | `/home/jnhu/sluice-595-e5/worktree` |
| HEAD | `271b81bfc9d64eecc0266fe7066b9040525dd710` |
| `git status --porcelain` | 6 行（5 M + 1 ??；`bench_w02_pipeline.cpp`、`uring_backend.hpp`、`uring_backend.cpp`、`uring_transport.cpp`、`xmake/tests.lua`、`tests/uring_595_bounded_progress_test.cpp`） |
| `git diff HEAD` sha256 | `b006dbbf751f5fb2bbe8fb9edb091ae66e57a181b8cf2470d880c9589e1f4072`（= R2 `PROTOCOL.md:20-22` 目标） |
| R2 patch | `/home/jnhu/sluice-595-e5/evidence/measurement/patch.diff` sha256 `7be9d0145a8c268a62d9fa90338b7cda95cefc5f8ad2976e4706cb192fb1eaf0` |
| `git apply --reverse --check patch.diff` | **exit 0** |
| 构建 | `xmake f -m release --liburing=y`（rc 0）；`xmake build -y bench_w02_pipeline uring_595_bounded_progress_test`（rc 0，`build ok, spent 0.05s`，无重编译） |
| bench 二进制 sha256 | `32beabcd891801eb65dab1d6e2df72aa0b89a98a2c16a114d398527cb821589a` |
| `libsluice_async.a` sha256 | `468d83923f0a6372fdad85f209dbf080d4830d381affc1ac1f8a0f8e217a229c` |
| `uring_595_bounded_progress_test` sha256 | `865de828f9b4bc95146caa15101921a28d7298247f8d2130d6d7be56db2b069a` |
| 构建后树状态 | status 6 行、`git diff HEAD` 仍为 `b006dbbf…`（源码未被改动） |
| 语义回执 | `SLUICE_595_ARM=B ./uring_595_bounded_progress_test` → `all checks passed`（exit 0） |

## 二进制与 patch 的绑定

- 源文件 mtime `11:36:10`、`libsluice_async.a` `11:36:33`、`bench_w02_pipeline` `11:39:01`：
  bench 在打过 patch 的静态库之后链接；`patch.diff` 的 mtime `11:53` 是该文件被复制/生成的时间，
  不是源码编辑时间。
- `strings bench_w02_pipeline | grep -c SLUICE_595_ARM` = 1；`strings libsluice_async.a | grep -c` = 1：
  两臂 selector 的字符串存在于链接进 bench 的静态库中。
- `uring_595_bounded_progress_test` 是 patch 新增的测试目标，它链接同一 `libsluice_async.a` 并
  强制 `SLUICE_595_ARM=B`；11 个 deferral/flush/residue oracle 全部通过，证明该库包含
  `bounded_progress_` 与 `defer_transport_flush_locked_` 门链。

## 说明

- `xmake build` 报告 up-to-date（0.05s），没有重新编译；这是构建系统基于内容哈希的决定，
  不是跳过重建。二进制 SHA `32beabcd…` 与 2026-11-04 R2 轮使用的 SHA 相同，因此本轮是
  **同一二进制**的重新配对测量，而不是另一个构建。
- 本回执不把 `causal-r2/worktree`（diff `f939602f…`、reverse-check exit 1）当作 R2 目标树；
  legacy 块只使用本文件的干净树。
- 原始日志：`raw/calibration/build/r2-provenance-before.txt`、`r2-provenance-after.txt`、
  `r2-build.log`、`r2-xmake-config.log`、`diag-r2/logs/bounded_progress_test.log`。
