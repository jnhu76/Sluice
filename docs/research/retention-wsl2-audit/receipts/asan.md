# Receipt — 验证单元「asan」：ASan 构建 + 全量测试

- 日期：2026-10-29
- 验证人：subagent 验证员·asan（动态工作流）
- Worktree：`/tmp/sluice-val-asan` @ `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`
  （`git rev-parse HEAD` 确认一致；`git status --short` 干净，无本地改动）
- 主工作区未执行任何构建（仅写入本报告目录）。

## 环境信息（实机采集）

| 项 | 值 |
| --- | --- |
| xmake | 3.1.2+HEAD.5d21e9d |
| C++ 编译器（实际使用） | g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| 内核 | 6.18.54.1-microsoft-standard-WSL2 |
| CPU | 8 逻辑核 |
| xmake 配置结果 | `mode = "asan"`，`liburing = false`，`plat/arch = linux/x86_64`（`.xmake/linux/x86_64/xmake.conf`） |

## 执行命令与结果（均在 /tmp/sluice-val-asan 内执行）

### 1. 配置

```
$ xmake f -c -m asan --liburing=n -y
checking for platform ... linux (x86_64)
checking for sluice_liburing_capability ... ok
warning: Ascend SDK not found
xmake f -c -m asan --liburing=n -y  0.56s user 0.14s system 70% cpu 0.999 total
```

退出码 0。

### 2. 构建

```
$ xmake -y
[ 19%]: <sluice_core> compiling.asan src/file_resource.cpp
[ 38%]: <sluice_core> compiling.asan src/blocking_file.cpp
[ 56%]: <sluice_core> archiving.asan libsluice_core.a
create ok!
compile_commands.json updated!
[100%]: build ok, spent 1.607s
xmake -y  1.71s user 0.36s system 100% cpu 2.069 total
```

退出码 0。说明：全部测试目标在 `xmake/tests.lua` 中声明为
`set_default(false)`，因此 `xmake -y` 只构建默认库目标
`sluice_core`；测试二进制由下一条 `xmake test` 自动构建。这与
勘察员命令矩阵的意图一致（构建 + 测试覆盖完整命令序列），非偏差。

### 3. 全量测试（ASan 下构建并运行）

```
$ xmake test -y
…（xmake test 先以 compiling.asan / linking.asan 构建全部 58 个测试目标）…
[100%]: app_tail_cli_oracle_test/app_tail_cli_oracle_test .... passed 4.098s

100% tests passed, 0 test(s) failed out of 58, spent 4.103s
xmake test -y  269.59s user 42.09s system 445% cpu 1:10.02 total
```

退出码 0。

- testsTotal = 58，testsPassed = 58，failed = 0（汇总行原文：
  `100% tests passed, 0 test(s) failed out of 58, spent 4.103s`）。
- 与 E5 参考基线 58/58（r3 §1）一致。
- 总墙钟 1:10.02，其中绝大部分为 ASan 下编译约 40 个测试二进制；
  纯测试执行 4.103s。最慢测试：app_tail_cli_oracle_test 4.098s。
- 日志中无任何 `AddressSanitizer` / `LeakSanitizer` / `deadly signal` /
  `SEGV` 报告（grep 零命中）。
- 逐测试结果清单（58 条 "… passed …"）已从日志去 ANSI 后核对：
  运行集合与通过集合完全一致（58 = 58）。

## ASan 真实生效的独立证据

1. 策略声明：`xmake.lua:7-9` — `if is_mode("asan") then
   set_policy("build.sanitizer.address", true) end`，与勘察员矩阵注记一致。
2. 实际编译命令（`xmake -r -v direct_composition_test` 采集，原文截取）：

   ```
   g++ -c -m64 -g -Wall -Werror -std=c++20 -I/tmp/sluice-val-asan/include
   -fsanitize=address -o build/.objs/direct_composition_test/linux/x86_64/asan/tests/direct_composition_test.cpp.o
   tests/direct_composition_test.cpp
   ```

3. 二进制符号验证：`nm build/linux/x86_64/asan/direct_composition_test`
   → `U __asan_init` 及 21 个 asan 符号（链接到 ASan 运行时）。
4. `xmake show` 报告 `mode: asan`；`xmake.conf` 中 `liburing = false`。

## 与勘察员命令矩阵的偏差记录

- 矩阵首条命令为 `cd /tmp/sluice-val-release`，与本单元指派的专属
  Worktree（`/tmp/sluice-val-asan`，且纪律要求"绝不动别人的 Worktree"）
  矛盾。按本单元任务书以 `/tmp/sluice-val-asan` 执行。除此之外三条
  xmake 命令逐字执行，且与仓库 xmake 实际行为相符（58/58 命中 E5
  参考值），无其他调整。
- `xmake -y` 仅构建默认目标的观察如上说明，属仓库既有设计而非失败。

## 结论

ASan 配置下全仓 58/58 测试全部通过，0 失败，无任何 ASan/LSan 报告。
状态：**PASS**。
