# Receipts — 验证单元「ubsan」

- 日期: 2026-10-29
- Worktree: `/tmp/sluice-val-ubsan`（detach 于 `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`，即 master HEAD）
- 平台: WSL2, linux 6.18.54.1-microsoft-standard-WSL2 x86_64
- 结论: **PASS** — UBSan 全量测试 58/58 通过，`runtime error` 计数 0

## 命令与输出

### 0. Worktree 校验

```
$ git -C /tmp/sluice-val-ubsan rev-parse HEAD
84e1b7d578c2c58a3c46033487d9e1ac7c639b8b
```

### 1. 配置

```
$ cd /tmp/sluice-val-ubsan && xmake f -c -m ubsan --liburing=n -y
checking for platform ... linux (x86_64)
checking for sluice_liburing_capability ... ok
warning: Ascend SDK not found
# 耗时: 1.010s（user 0.57s）
```

注：命令矩阵原文路径为 `/tmp/sluice-val-release`，按本单元专属 Worktree
要求改为 `/tmp/sluice-val-ubsan` 执行（含日志路径），其余参数未变。

### 2. 构建

```
$ cd /tmp/sluice-val-ubsan && xmake -y
[100%]: build ok, spent 1.686s
# 耗时: 2.124s wall（user 1.72s），增量构建
```

### 3. 全量测试

```
$ cd /tmp/sluice-val-ubsan && xmake test -v 2>&1 | tee /tmp/sluice-val-ubsan/ubsan.log
[100%] app_tail_cli_oracle_test/app_tail_cli_oracle_test ... passed 4.001s
100% tests passed, 0 test(s) failed out of 58, spent 4.007s
# wall 耗时: 47.836s（user 282.05s, system 42.11s）
```

- 汇总行: `ubsan.log:867` — `100% tests passed, 0 test(s) failed out of 58, spent 4.007s`
- 与 E5 参考一致: 58/58 passed, 0 failed
- 明细子测试合计（日志内 "all N ... tests passed" 行）与 58 个 xmake 测试目标一一对应

### 4. UBSan runtime error 检查（r3 §1 判据）

```
$ grep -c 'runtime error' /tmp/sluice-val-ubsan/ubsan.log
0
# 退出码 1（无匹配），计数必须为 0 —— 满足
```

## 日志中的非失败注意事项（如实记录）

`ubsan.log` 中出现如下 stderr 文本，均为测试故意触发的 contract-violation
自测输出（watchdog / shutdown 生命周期验证），对应 xmake 测试目标均判定
passed：

- `terminate called without an active exception`（ubsan.log:656,658,660,662,741,776,778）
- `sluice::async::AsyncIoContext: relocating or destroying a context with ... is a contract violation`（ubsan.log:659,775,777）
- `TIMEOUT [withheld_completion_fails_by_deadline_and_late_completion_passes] the withheld read stays pending after 876947 poll passes (no failed poll: polls healthy)`（ubsan.log:796）— 位于 `shutdown_lifecycle_test` 的 stderr 转储内（ubsan.log:773 起该目标 stderr 段），xmake 判定该目标 passed（ubsan.log:778 行之后 `shutdown_lifecycle_test ... passed 1.508s`）

## 附注

- `-m ubsan` 模式启用 `build.sanitizer.undefined`（`xmake.lua:13-14`），
  `--liburing=n` 生效（测试日志含 `real-uring not built in noliburing profile`，ubsan.log:818 附近）。
- 日志存档: `/tmp/sluice-val-ubsan/ubsan.log`（该验证 Worktree 由脚本统一清理，本文件为证据摘录）。
