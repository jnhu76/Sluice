# COPY candidate remote reproduction

All captured C++ runs use commit c94c7770335e4e01e4f1f5876740cb0e792a4f88 on
Linux E5, kernel and compiler versions captured in each profile log. Build/run
commands are literal lines in each profile log. Always `cd` to that profile's
worktree before using xmake; `-P` alone is insufficient configuration isolation.
Create new detached worktrees rather than overwriting existing ones:

```sh
primary=/home/jnhu/Source/Sluice-f3-copy-request-scope
sha=c94c7770335e4e01e4f1f5876740cb0e792a4f88
profile=debug-n # repeat debug-y, release-n, release-y, asanubsan-n, tsan-n
mode=${profile%-*}
uring=${profile##*-}
tree=/home/jnhu/Source/Sluice-copy-reproduction-$profile
test ! -e "$tree" || exit 1
git -C "$primary" worktree add --detach "$tree" "$sha"
cd "$tree"
# For asanubsan/tsan, append --toolchain=clang (Clang22).
~/.local/bin/xmake f -P "$tree" -m "$mode" --liburing="$uring" -o "$tree/build-$profile"
for target in app_copy_pipeline_oracle_test app_copy_consumption_test app_copy_dir_fsync_test sluice-copy; do
    ~/.local/bin/xmake -P "$tree" -j 4 "$target" || exit 1
done
bin="$tree/build-$profile/linux/x86_64/$mode"
for target in app_copy_pipeline_oracle_test app_copy_consumption_test app_copy_dir_fsync_test; do
    timeout 60 "$bin/$target" || exit 1
done
python3 "$primary/docs/evidence/f3-copy-request-scope/cli_smoke.py" "$bin/sluice-copy"
```

For TSan, run only the four `SLUICE_ORACLE_FILTER` cases recorded in tsan-n.txt
instead of the full pipeline suite; the legacy stackful Runtime harness phase
is deliberately outside that instrumentation claim. Four normal profiles and
ASan/UBSan run all three complete suites. liburing=y profiles contain a real
UringAsyncBackend case and preserve its named output. The CLI subprocess always
uses the default ThreadPool backend; profile selection does not change that.
CLI failure witnesses require POSIX `resource` and run a genuine output-size
limit with SIGXFSZ ignored. They assert output bytes and temporary-file cleanup.

External-loop logs preserve 19 ThreadPool / 5 actual-ring cases at the same
baseline. dependency-audit.txt records preprocessing closure and binary symbols;
canonical AsyncIoContext's internal Completion header inclusion is not claimed
removed. Source consumer contraction is distinct from SDK install contraction.
