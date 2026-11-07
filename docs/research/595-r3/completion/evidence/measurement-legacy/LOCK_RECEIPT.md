# LOCK_RECEIPT — MEASUREMENT.lock 使用记录

- 锁文件：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（inode 6941075，dev 0:53）。
- 会话获取：2026-11-05T11:03:31Z，等待 0s；释放：2026-11-05T11:08:21Z。
- 获取时 `fuser -v` 输出：`51956 51965`（持锁进程）。
- `/proc/locks` 在本平台未列出该 flock 条目（header holders=[]）；探针会话用第二个 open file description 做 `flock -n` 排他性检查，结果：`EXCLUSIVITY_CHECK: blocked as expected`。
- 所有构建（R2 xmake）、计时块（3 × 48 invocations）与 R2 strace 诊断均在获取与释放之间执行；会话日志时间戳覆盖 11:03:31Z–11:08:21Z，探针 11:12:45Z–11:12:49Z。

## 原始记录

- `raw/calibration/lock-log.jsonl`
- `raw/calibration/bucket-probe/logs/lock-fuser.txt`
- `raw/calibration/bucket-probe/logs/lock-exclusivity.txt`
