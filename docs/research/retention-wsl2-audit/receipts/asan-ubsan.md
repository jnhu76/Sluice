# Receipt: asan-ubsan (ASan+UBSan 联合构建 + 全量测试)

- 单元: 验证员·asan-ubsan
- 日期: 2026-10-29
- Worktree: `/tmp/sluice-val-asan-ubsan`
- Commit: `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` (`git rev-parse HEAD` 实测)
- 结论: **PASS** — 58/58 测试通过，0 失败；UBSan `runtime error` 计数 0；AddressSanitizer 报告 0。与 E5 参考 58/58 一致。

## 命令调整说明

勘察员矩阵写的是 `/tmp/sluice-val-release` 路径；本单元专属 Worktree 为
`/tmp/sluice-val-asan-ubsan`，故仅调整了路径（命令与 `-m asanubsan` 模式完全一致）。
模式定义与矩阵注释一致：`xmake.lua:16-19` 的 `asanubsan` 分支同时设置
`build.sanitizer.address` 与 `build.sanitizer.undefined` 两个 policy。

## 执行记录

### 1. 配置

```
cd /tmp/sluice-val-asan-ubsan && xmake f -c -m asanubsan --liburing=n -y
```

输出（tail）：`checking for platform ... linux (x86_64)`；`checking for
sluice_liburing_capability ... ok`；`warning: Ascend SDK not found`（预期，本机无
Ascend）。耗时 0.986s。

### 2. 构建

```
cd /tmp/sluice-val-asan-ubsan && xmake -y
```

`build ok, spent 1.745s`（增量：Worktree 内已有缓存，仅重编 3 个 TU 后归档
`libsluice_core.a`）。为排除增量掩盖问题，追加验证：

- 二次 `xmake -y` → `build ok, spent 0.02s`（无待编译目标，构建完整）。
- `grep -o '\-fsanitize[^ "]*' compile_commands.json | sort | uniq -c` →
  `387 -fsanitize=address` / `387 -fsanitize=undefined`：全部 387 条编译命令
  均带双 sanitizer 标志。
- 抽查二进制符号：`nm build/linux/x86_64/asanubsan/app_tail_cli_oracle_test |
  grep -ciE '__asan_init|__ubsan_handle'` → `11`，测试二进制确实链入两个
  sanitizer 运行时。

### 3. 全量测试

```
cd /tmp/sluice-val-asan-ubsan && time xmake test -v 2>&1 | tee /tmp/sluice-val-asan-ubsan/asanubsan.log
```

日志末行（实测）：

```
100% tests passed, 0 test(s) failed out of 58, spent 4.142s
```

- testsPassed = 58, testsTotal = 58, failed = 0。
- 墙钟耗时 54.146s（user 322.33s，多核 ~678% —— ASan 开销可见）。
- 日志：`/tmp/sluice-val-asan-ubsan/asanubsan.log`（含每个 CASE PASS 输出，
  末组为 `app_tail_cli_oracle_test`，19 个 tail cli oracle 子用例全过）。

### 4. Sanitizer 命中扫描

```
grep -c 'runtime error' /tmp/sluice-val-asan-ubsan/asanubsan.log   # → 0 (exit 1, 无匹配)
grep -cE 'AddressSanitizer|ERROR: ' /tmp/sluice-val-asan-ubsan/asanubsan.log  # → 0 (exit 1, 无匹配)
```

UBSan `runtime error` 计数为 0，AddressSanitizer 错误报告为 0。

## 判定

| 检查 | 预期 | 实测 | 结果 |
| --- | --- | --- | --- |
| `-m asanubsan` 双 policy（xmake.lua:16-19） | 双 sanitizer 生效 | 387/387 编译命令带两标志 | PASS |
| 构建成功 | build ok | build ok（含无操作复验） | PASS |
| 测试通过数 | 58/58，0 failed | 58/58，0 failed | PASS |
| `grep -c 'runtime error'` | 0 | 0 | PASS |
| AddressSanitizer 报告 | 0 | 0 | PASS |

未修改任何产品代码；所有写操作仅限本 receipt 与 `/tmp` 验证 Worktree 内的构建产物。
