#!/usr/bin/env bash
# run-e12-tlc-all.sh -- 逐个跑 E12-D AsyncCondition 的全部 13 次 TLC 模型检验，
# 实时输出到 stdout（并 tee 到日志），每个模型后给出 PASS/FAIL 判定。
# 可选地追加运行 E12-E Queue 形式化 gate (verify-e12-queue-formal.sh)。
#
# 用法:
#   bash scripts/run-e12-tlc-all.sh                 # 默认: AsyncCondition + Queue (jar=repo 根)
#   bash scripts/run-e12-tlc-all.sh async           # 只跑 AsyncCondition (原有行为)
#   bash scripts/run-e12-tlc-all.sh queue           # 只跑 Queue formal gate
#   bash scripts/run-e12-tlc-all.sh all             # 两者都跑 (默认)
#   TLA2TOOLS_JAR=/path/tla2tools.jar bash scripts/run-e12-tlc-all.sh
#   JFLAGS_EXTRA="-Dtlc2.tool.impl.fp.fpset.impl.MSBDiskFPSet.init=false" \
#       bash scripts/run-e12-tlc-all.sh
#
# 预期结果 (AsyncCondition):
#   #1  正确性模型        -> PASS (No error has been found)
#   #2  reach1            -> FAIL (NoReachOrdThenReq 可达, 即期望违反)
#   #3  reach2            -> FAIL (NoReachReqThenOrd 可达, 即期望违反)
#   #4-#13 NEG-C1..NEG-C10 -> FAIL (各自违反其命名不变式)
# 预期结果 (Queue, via verify-e12-queue-formal.sh):
#   Model A (E12Queue)      -> PASS (12 invariants)
#   Model B (E12QueueClosed)-> PASS (7 invariants)
#   NEG-QUEUE-1..7          -> FAIL (各自违反其命名不变式)
#   WRONG-PROPERTY gate     -> OK (defect specificity)
# 退出码: 0=全部符合预期; 1=有模型结果与预期不符或 TLC 无法启动。
set -uo pipefail

# 选择运行目标: async | queue | all (默认 all)。
TARGET="${1:-all}"
case "$TARGET" in
  async|queue|all) : ;;
  *) echo "usage: $0 [async|queue|all]" >&2; exit 2 ;;
esac

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$here/.."
spec="$repo/docs/spec/e12_async_condition"
JAR="${TLA2TOOLS_JAR:-$repo/tla2tools.jar}"
LOG="${TLC_LOG:-$repo/build/tlc-e12-all.log}"

if [ ! -f "$JAR" ]; then
  echo "error: tla2tools.jar not found at $JAR" >&2
  echo "  set TLA2TOOLS_JAR=/path/to/tla2tools.jar" >&2
  exit 2
fi
if ! command -v java >/dev/null 2>&1; then
  echo "error: java not found on PATH" >&2
  exit 2
fi

mkdir -p "$(dirname "$LOG")"
: > "$LOG"   # truncate

cd "$spec" || { echo "error: spec dir $spec missing" >&2; exit 2; }

JVM=("-XX:+UseParallelGC" "-cp" "$JAR" "tlc2.TLC" "-nowarning")
JFLAGS_EXTRA=("${JFLAGS_EXTRA[@]+"${JFLAGS_EXTRA[@]}"}")

# 判定辅助 ---------------------------------------------------------------
tlc_launched()  { grep -q '^Starting\.\.\.' "$1"; }      # TLC 真正跑起来了
tlc_passed()    { grep -q 'Model checking completed. No error has been found' "$1"; }
named_violated() { grep -Eq "Invariant ${2} is violated" "$1"; }
any_violated()  { grep -Eq 'Invariant .+ is violated|Property .+ is violated' "$1"; }

# 单次运行: run_one <编号> <标签> <期望(PASS|FAIL)> <期望不变式(FAIL时)> <model> <cfg>
run_one() {
  local num="$1" label="$2" expect="$3" inv="$4" model="$5" cfg="$6"
  local out; out="$(mktemp -t e12tlc.${model}.XXXXXX)"
  echo "================================================================"
  echo "[$num/13] $label"
  echo "  model: $model   cfg: $cfg"
  echo "  expect: $expect${inv:+ (must violate $inv)}"
  echo "--- TLC output (live) ---"
  # 实时输出 stdout, 同时存文件
  java "${JVM[@]}" "${JFLAGS_EXTRA[@]}" -config "$cfg" "$model" 2>&1 | tee "$out" | tee -a "$LOG"
  echo "--- end [$num] ---"

  local verdict="??" rc=0
  if ! tlc_launched "$out"; then
    verdict="ERROR(tlc did not launch)"; rc=1
  elif [ "$expect" = PASS ]; then
    if tlc_passed "$out"; then verdict="PASS(ok)"; else verdict="FAIL(expected PASS but invariant violated)"; rc=1; fi
  else  # expect FAIL
    if tlc_passed "$out"; then
      verdict="FAIL(expected a violation but model PASSED)"; rc=1
    elif [ -n "$inv" ]; then
      if named_violated "$out" "$inv"; then verdict="PASS(violated $inv as expected)"; else
        verdict="FAIL(violated wrong/no property; expected $inv)"; rc=1; fi
    else
      if any_violated "$out"; then verdict="PASS(violated as expected)"; else verdict="FAIL(no violation found)"; rc=1; fi
    fi
  fi
  rm -f "$out"
  printf '[$num/13] %-42s => %s\n' "$label" "$verdict" | tee -a "$LOG.summary"
  # 清理 TLC 残留 trace 文件
  find . -maxdepth 1 -name '*TTrace*' -delete 2>/dev/null
  return $rc
}

# 主流程 -----------------------------------------------------------------
: > "$LOG.summary"
echo "E12 全量 TLC 检验  (jar=$JAR, target=$TARGET)" | tee -a "$LOG.summary"
echo "started: $(date)"                                 | tee -a "$LOG.summary"
echo "log: $LOG"

overall=0

run_async_condition() {
  echo "================================================================"
  echo "E12-D AsyncCondition 全量 TLC 检验 (13 runs)" | tee -a "$LOG.summary"
  cd "$spec" || { echo "error: spec dir $spec missing" >&2; exit 2; }
  run_one 1  "正确性 safety"                  PASS ""                       E12AsyncCondition          E12AsyncCondition.cfg            || overall=1
  run_one 2  "reach1 OrdThenReq"              FAIL NoReachOrdThenReq        E12AsyncCondition          E12AsyncCondition.reach1.cfg     || overall=1
  run_one 3  "reach2 ReqThenOrd"              FAIL NoReachReqThenOrd        E12AsyncCondition          E12AsyncCondition.reach2.cfg     || overall=1
  run_one 4  "NEG-C1 NonOwnerWait"            FAIL InvConditionWaiterDoesNotOwnMutex  E12AsyncConditionNegC1  E12AsyncConditionNegC1.cfg      || overall=1
  run_one 5  "NEG-C2 NotifyAnyNonRegistered"  FAIL InvConditionResolvedFinality        E12AsyncConditionNegC2  E12AsyncConditionNegC2.cfg      || overall=1
  run_one 6  "NEG-C3 ReturnOwnedNoGrant"      FAIL InvReturnedOwnsMutex                E12AsyncConditionNegC3  E12AsyncConditionNegC3.cfg      || overall=1
  run_one 7  "NEG-C4 CancelReacquireEpoch"    FAIL InvTerminalAttemptFinality          E12AsyncConditionNegC4  E12AsyncConditionNegC4.cfg      || overall=1
  run_one 8  "NEG-C5 NotifyAllNoDrain"        FAIL InvConditionQueueWellFormed         E12AsyncConditionNegC5  E12AsyncConditionNegC5.cfg      || overall=1
  run_one 9  "NEG-C6 ReacquireNonFIFO"        FAIL InvFIFOGrant                        E12AsyncConditionNegC6  E12AsyncConditionNegC6.cfg      || overall=1
  run_one 10 "NEG-C7 DestroyWithActiveWaiters" FAIL InvDestructionPrecondition         E12AsyncConditionNegC7  E12AsyncConditionNegC7.cfg      || overall=1
  run_one 11 "NEG-C8 WaitReleaseBeforeRegister" FAIL InvNoLostNotifyWindow             E12AsyncConditionNegC8  E12AsyncConditionNegC8.cfg      || overall=1
  run_one 12 "NEG-C9 HandoffNonFIFO"          FAIL InvEligiblePreMutexQueue            E12AsyncConditionNegC9  E12AsyncConditionNegC9.cfg      || overall=1
  run_one 13 "NEG-C10 SeparateQueues"         FAIL InvOrdinaryAndReacquireFIFO         E12AsyncConditionNegC10 E12AsyncConditionNegC10.cfg     || overall=1
}

# E12-E Queue 形式化 gate: 调用 verify-e12-queue-formal.sh (独立脚本，自带
# 判定/命名属性校验/wrong-property gate/fresh outdir)。这里只转发退出码。
run_queue() {
  echo "================================================================"
  echo "E12-E Queue 形式化 gate (verify-e12-queue-formal.sh)" | tee -a "$LOG.summary"
  if [ ! -x "$repo/scripts/verify-e12-queue-formal.sh" ]; then
    echo "  (warn: verify-e12-queue-formal.sh missing or not executable; skipping)" | tee -a "$LOG.summary"
    return 0
  fi
  TLA2TOOLS_JAR="$JAR" "$repo/scripts/verify-e12-queue-formal.sh" 2>&1 | tee -a "$LOG"
  local rc=${PIPESTATUS[0]}
  if [ "$rc" -ne 0 ]; then overall=1; fi
}

case "$TARGET" in
  async) run_async_condition ;;
  queue) run_queue ;;
  all)   run_async_condition; run_queue ;;
esac

echo "================================================================"
echo "finished: $(date)" | tee -a "$LOG.summary"
if [ "$overall" -eq 0 ]; then
  echo "RESULT: 全部检验符合预期 (target=$TARGET)" | tee -a "$LOG.summary"
else
  echo "RESULT: 有模型不符合预期  (见上方 FAIL 行; target=$TARGET)" | tee -a "$LOG.summary"
fi
echo "汇总: $LOG.summary"
exit $overall
