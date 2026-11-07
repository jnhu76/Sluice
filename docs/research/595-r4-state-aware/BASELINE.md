# #595 R4 — BASELINE（环境清单 / 冻结对象 / 目录布局 / 锁协议）

配套 `PREREGISTERED.md`（同刻冻结）。本文件是描述性基线记录，不承载门槛。
所有 sha256 与环境值均为本轮实测（标注命令来源）。

## 1. 环境清单（对照 R0-IDENTITY 核验）

R0-IDENTITY 平台记录：`/home/jnhu/sluice-595-e5/evidence/platform.txt`
（PLATFORM_RECORD，frozen 2026-11-04T11:29:58+08:00）。下表左列 = 该记录的冻结项，
右列 = 本轮预注册时点实测（2026-11-06，命令见括号）。**全部一致**。

| R0-IDENTITY 冻结项 | 本轮实测（一致 ✓） |
|---|---|
| host：E5 原生裸机（无 SSH 跳变、无虚拟化） | 同（`virt=none`；会话原生） |
| cpu_model：Intel Xeon E5-2666 v3 @ 2.90GHz | ✓（`grep -m1 "model name" /proc/cpuinfo`） |
| 20 CPU（10 core × 2 thread），1 NUMA | ✓（`nproc`=20） |
| smt_pairs：core2: 2,12；core8: 8,18 | ✓（cpu2 `thread_siblings_list`=2,12；cpu8=8,18） |
| kernel 7.2.11-200.fc44.x86_64 | ✓（`uname -r`） |
| gcc 16.2.1 20260819 (Red Hat 16.2.1-2) | ✓（`g++ --version`） |
| liburing liburing-devel-2.13-2.fc44（header /usr/include，so /lib64） | ✓（`rpm -q liburing-devel`） |
| perf 7.2.11，perf_event_paranoid=2 | ✓（`/proc/sys/kernel/perf_event_paranoid`=2） |
| xmake v3.1.2+HEAD.5d21e9d | ✓（`xmake --version`） |
| governor=schedutil；freq 1.2–3.5 GHz | ✓（cpu2 scaling_governor/min/max） |
| tmpfs /tmp：rw,nosuid,nodev,seclabel,size≈31G,inode64 | ✓（`findmnt -no OPTIONS /tmp`；`df -h /tmp` 31G free） |
| repo origin git@github.com:jnhu76/Sluice.git | 未变（本轮不推送） |
| BASE_SHA/ANCHOR_622 = 271b81bf | 主仓 master 仍 271b81bf，porcelain=0 |

计时轮核验项（每会话执行并记录，见 PREREGISTERED §10.1/§14）：governor、cpu2
scaling_cur_freq、cpu2/cpu12 占用、pin 2/12 的进程清单、loadavg、`df -h /tmp`、
锁 inode/dev（6941075 / 0:53）不变。

本轮预注册时点负载快照：loadavg `0.16 0.12 0.14`；cpu2/cpu12 自开机累计 busy
≈0.7%（/proc/stat 原始计数器记录于会话日志）。

## 2. 冻结对象与补丁 sha256

### 2.1 提交链

| 项 | 值 |
|---|---|
| 主仓 master | `271b81bfc9d64eecc0266fe7066b9040525dd710`（porcelain=0，未动） |
| R3 证据 worktree | 0a22a791 + 三补丁脏树（diff sha `73fa116f…`，未动） |
| R4 worktree | `/home/jnhu/sluice-595-e5/r4/worktree`，分支 `research-595-r4` |
| 冻结基线提交 | `d9d140eea180f29f629992aa293109acdeff1e03`（= 0a22a791 + r3-completion + delta_w02 + delta_w03；重建验收门见 `evidence/BASELINE-FREEZE.md` §3） |
| 实现提交 | `f3b25329bc8a64a201961c426a58adacfb8fdf08`（经 6a13d2d5；HEAD，porcelain=0） |

### 2.2 补丁 sha256（双源核验过）

| 补丁 | sha256 |
|---|---|
| r3-completion.patch | `2596f204035f9695db47184b20ad8ece7b2168c54d8589b2060782aa7959bbc0` |
| delta_w02.patch | `4aa7b306fc8469ef0b597af3def761f340221dcb86503a135bf6caa4d4dec88e` |
| delta_w03.patch | `57f21e022e7b9d0df97ea74398f37717902c1fe97586efe0849c2d19b5d42736` |
| d9d140ee → f3b25329 全量 diff | `d66f3bcf52d01c5ee19ecd6285d67586d4467a44ecc8d608b1b5a8e9b267cf49` |

关键源内容 sha256：`bench/r3_ready_batch.hpp` =
`363f346ac3b26076322cbe0126a9d276aedbcb1d2ecd7607c108f8c32621dd81`（磁盘=git blob
@f3b25329，正确性轮 tree_proof 复核）。

### 2.3 冻结二进制（sha256 实测；参考副本 `evidence/frozen-binaries/MANIFEST.txt`）

| 二进制 | sha256 |
|---|---|
| bench_w03_external_loop | `f029607fc1a657efe0ba96203115f27a90d598f12c274e9360b6029a5e019009` |
| bench_w02_pipeline | `daee6b6c1ab618a29d8e1afc120f8da5e478756b2def7c77c712f54a6a4e1931` |
| r4_arm_smoke（仅冒烟） | `07dc3176a1d66bacc91eb2c217fb9a66f292cfc9a11afbb6eae5a0b5ef77bf3b` |

构建出处：f3b25329 提交后持锁全量重建（`logs/final_rebuild_smoke.log`，
`logs/LOCK_RECEIPT.md` 第 8 条；`xmake f -m release`，builddir r4/build）。
绑定规则见 PREREGISTERED §1.2（每 invocation 重算比对；禁止重建/替换）。

### 2.4 正确性基线

`evidence/correctness/RECEIPT.md`：18 场景确定性 oracle（BASE GREEN / M1-M3 变异
RED / 恢复 GREEN），十臂判定全过；新测试源 sha256
`5c10be9ee44f8471b34426ae663a78e7dc58633d0852808181f738b9136361d3`；既有四套件 +
r4_arm_smoke（57 PASS/0 FAIL）回归锚定。

## 3. r4 目录布局

```
/home/jnhu/sluice-595-e5/r4/
├── PREREGISTERED.md      本轮性能预注册（门槛唯一来源）
├── BASELINE.md           本文件
├── DESIGN.md             信息审计/状态-动作表/参考规则映射（描述性）
├── worktree/             research-595-r4 @f3b25329（冻结；只读于计时轮）
├── build/                xmake builddir（冻结二进制所在；禁止重建）
├── raw/                  计时块原始数据（header.json/results.jsonl/summary.json/
│                         logs/attempts.jsonl/RECEIPT.md；块命名=PREREG §11.1）
├── analysis/             bootstrap/Holm/COMPLETION_GATE.json
├── evidence/             BASELINE-FREEZE.md、frozen-binaries/、correctness/、smoke/
│                         PREREG_MANIFEST.txt
├── logs/                 全部构建/冒烟/锁收据日志
└── figures/              图表（如有）
```

写入边界：只写 `r4/` 树与 `r4/worktree` 研究代码（计时轮内 worktree 冻结只读）；
主工作树与他人 worktree 只读；不推送、不写 GitHub。

## 4. 锁协议

- 锁文件：`/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock`（inode 6941075，dev 0:53，
  size=0；R3 基线冻结轮创建，#595/#604 共享互斥）。
- 一切计时/构建/tracing 持锁：`flock` 独占（fd 9，`-w 1800`）；≥15 分钟计时串行
  且独占整机，期间禁止并行编译/压缩/归档/其他实验。
- LOCK_RECEIPT 内容（每会话）：获取/释放 UTC 与等待时长、`fuser -v`、第二 open
  file description `flock -n` 排他性检查（必须 blocked）、会话命令清单、释放后
  复核（立即可取、inode/size 不变）；会话前后 load、cpu2/cpu12 占用、`df -h /tmp`。
- 先例收据：`logs/LOCK_RECEIPT.md`（实现轮 8 项持锁命令）、
  `evidence/correctness/LOCK_RECEIPT.md`、`evidence/BASELINE-FREEZE.md` §6。
