# 回执：debug 验证单元 — Debug 构建 + 全量测试（含 Direct Primitive 与 Composition 子集）

- 日期：2026-10-29（UTC 15:46 收尾）
- 主机：WSL2，内核 `6.18.54.1-microsoft-standard-WSL2`，8 核
- 工具链：xmake v3.1.2+HEAD.5d21e9d
- 代码基线：`84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`（master HEAD，= origin/master）
- 专属 Worktree：`/tmp/sluice-val-debug`（detach 于上述提交；`git rev-parse HEAD` 核实一致；工作区干净）
- 原始日志：`/tmp/sluice-val-debug-logs/{configure,build,test-full,test-direct,test-composition}.log`

## 命令与结果（均在 /tmp/sluice-val-debug 内执行）

### 1. 配置（debug，无 liburing）

```
cd /tmp/sluice-val-debug
time xmake f -c -m debug --liburing=n -y
```

- exit=0；耗时 0.927s wall。
- 既有环境警告：`warning: Ascend SDK not found`（与本单元断言无关）。

### 2. 构建

```
time xmake -y
```

- exit=0；`build ok, spent 1.557s`；wall 1.989s。
- 注：`xmake -y` 仅构建默认目标（测试目标均 `set_default(false)`，见 `xmake/tests.lua`）；测试二进制由 `xmake test` 按需构建（构建记录含在 test-full.log 内，83 个 compile/link 步骤）。

### 3. 全量测试

```
time xmake test -y
```

- exit=0；**`100% tests passed, 0 test(s) failed out of 58, spent 3.994s`**；wall 40.406s（含全部测试目标首次构建）。
- 与 E5 冻结参照 58/58（`WSL2_COMMAND_MATRIX.md` §成功判据、r3 §1；静态枚举 `--liburing=n` → 58 个）一致。

### 4. Direct Primitive 子集（read_at/write_at/read/write 四原语 + admission 面）

矩阵原命令（裸测试名）实际行为不符：

```
xmake test -y blocking_file_read_test blocking_file_write_test ...   # → "nothing to test"（exit=0）
```

实测调整（单名试验确认）：xmake v3.1.2 位置参数按 `targetname/testname` 全名匹配，裸名不匹配（`-h` 示例与实际不符）；且 zsh 需引号防止 `*` 展开：

```
time xmake test -y 'blocking_file_read_test/*' 'blocking_file_write_test/*' \
  'blocking_file_state_test/*' 'blocking_file_sequential_test/*' \
  'direct_cursor_position_test/*' 'file_access_precedence_test/*' \
  'file_close_semantics_test/*' 'semantic_range_test/*' \
  'semantic_validation_precedence_test/*'
```

- exit=0；**9/9 passed, 0 failed, spent 0.153s**；wall 2.458s。
- 逐项：blocking_file_read_test 0.078s、blocking_file_sequential_test 0.069s、blocking_file_state_test 0.065s、blocking_file_write_test 0.056s、direct_cursor_position_test 0.048s、file_close_semantics_test 0.031s、semantic_range_test 0.023s、file_access_precedence_test 0.067s、semantic_validation_precedence_test 0.078s。
- `file_close_semantics_test` 为 SLUICE_FILE_INTERNAL_TESTING seam 构建目标（`xmake/tests.lua:60-74`），实跑通过。

### 5. Composition 子集（read_exact_at/write_all_at/read_exact/write_all 组合面）

```
time xmake test -y 'direct_composition_test/*' 'direct_composition_fault_test/*' \
  'semantic_short_io_reference_test/*' 'semantic_effect_outcome_test/*'
```

- exit=0；**4/4 passed, 0 failed, spent 0.044s**；wall 2.340s。
- 逐项：direct_composition_fault_test 0.029s、direct_composition_test 0.026s、semantic_effect_outcome_test 0.015s、semantic_short_io_reference_test 0.017s。

## 全量 58 项报告（debug, --liburing=n，全部 passed）

app_copy_consumption_test, app_copy_dir_fsync_test, app_grep_consumption_test, app_hash_consumption_test, async_sync_admission_test, blocking_file_read_test, blocking_file_sequential_test, blocking_file_state_test, blocking_file_sync_all_test, backend_contract_conformance_threadpool_test, blocking_file_sync_data_test, blocking_file_write_test, direct_composition_fault_test, direct_composition_test, direct_cursor_position_test, direct_w01_consumer_probe, explicit_file_ref_test, external_loop_stop_order_test, file_close_semantics_test, file_info_identity_test, file_access_precedence_test, file_move_borrow_test, file_open_contract_test, file_resource_test, file_read_test, file_sync_all_test, progress_notification_suppression_test, file_write_test, file_sync_data_test, progress_source_ownership_test, request_core_consumer_probe, request_core_init_closure_test, public_request_test, public_request_release_violation_test, request_core_protocol_test, request_core_publication_test, request_core_ownership_test, request_error_domain_threadpool_test, semantic_durability_reference_test, semantic_effect_outcome_test, semantic_errno_mapping_test, semantic_open_test, semantic_range_test, semantic_short_io_reference_test, semantic_reference_case_test, semantic_validation_precedence_test, threadpool_progress_pass_bound_test, threadpool_request_core_integration_test, uring_submit_boundary_test, threadpool_progress_race_test, request_scope_test, app_tail_consumption_test, submit_concurrency_test, threadpool_external_loop_test, shutdown_lifecycle_test, app_copy_pipeline_oracle_test, app_tail_lifecycle_oracle_test, app_tail_cli_oracle_test。

（完整逐项耗时见 `/tmp/sluice-val-debug-logs/test-full.log` 的 `report of tests` 段；最慢三项 app_tail_cli_oracle_test 3.989s、app_tail_lifecycle_oracle_test 2.511s、app_copy_pipeline_oracle_test 2.470s。）

## 相对勘察员矩阵的调整记录

1. Worktree 路径：矩阵示例写 `/tmp/sluice-val-release`，本单元专属 Worktree 为 `/tmp/sluice-val-debug`（单元指令优先），已在其中执行全部命令。
2. 模式：矩阵 direct/composition 两条示例为 `-m release`；本单元范围为 Debug 构建 + 全量测试，direct/composition 子集亦在 debug 模式下执行（`xmake/tests.lua` 按模式同型注册，矩阵 buildAndTest 条目自注 "same static test set … per mode"）。
3. 测试名过滤：矩阵假设裸测试名可作 `xmake test` 位置参数；实测 xmake v3.1.2 下返回 `nothing to test`，须用引号包裹的 `<target>/*` 全名通配（`xmake test -h` 示例与实际行为不符）。子集覆盖的测试集合与矩阵列出的目标一一对应，无增删。
4. 突变目标（`*_mut_*`）不带 `add_tests`（`xmake/tests.lua` 突变注册处均条件跳过），未混入常规测试运行；本单元非突变单元，未执行突变。

## 结论

- Debug 全量：58/58 passed，0 failed —— 与 E5 参照一致。
- Direct Primitive 子集：9/9 passed。Composition 子集：4/4 passed（均为全量 58 的子集，重复实跑取证）。
- 未发现真实失败；未放宽任何断言；未在主工作区执行任何构建。
