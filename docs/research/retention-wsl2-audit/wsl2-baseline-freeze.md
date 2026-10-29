# WSL2 验证基线冻结（retention audit）

- 冻结时间：2026-10-29T22:37+08:00（ Asia/Shanghai，本机 `date -Is` 于 receipts 内）
- 冻结对象 commit：`84e1b7d578c2c58a3c46033487d9e1ac7c639b8b`（= `origin/master` = 本地 `master` HEAD，两者经 `git rev-parse` 一致）
- binaryIdentity：`pending-release-build`（占位，由 Release 验证员的 sha256 回填至 receipts）

## 1. Host / Kernel

| 项 | 值 | 证据 |
| --- | --- | --- |
| hostname | `Hu` | receipts/01-host-kernel.txt |
| kernel | `Linux 6.18.54.1-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC Tue Oct 6 19:13:07 UTC 2026 x86_64 GNU/Linux` | 同上（`uname -a`） |
| OS | Ubuntu 26.04.1 LTS (resolute) | 同上（`/etc/os-release`） |

## 2. 工具链

| 工具 | 版本 | 证据 |
| --- | --- | --- |
| gcc | 15.2.0 (Ubuntu 15.2.0-16ubuntu1) | receipts/02-toolchain.txt |
| g++ | 15.2.0 (Ubuntu 15.2.0-16ubuntu1) | 同上 |
| clang | 21.1.8 (Ubuntu clang version 21.1.8 (6ubuntu1), target x86_64-pc-linux-gnu) | 同上 |
| glibc (ldd) | 2.43 (Ubuntu GLIBC 2.43-2ubuntu2.4) | 同上 |
| xmake | v3.1.2+HEAD.5d21e9d | 同上（存在，非 blocker） |

无工具缺失；无 blocker。

## 3. 文件系统

| 路径 | 设备 | 类型 | 结论 |
| --- | --- | --- | --- |
| /home/hoo/Projects/Sluice | /dev/sdd | ext4 | 通过：ext4 原生，非 /mnt/* Windows 挂载 |
| /tmp | tmpfs | tmpfs | 通过：tmpfs，非 9p/drvfs |

证据：receipts/03-filesystem.txt（`df -T` + `findmnt -T`）。两处均不涉及 9p/drvfs。

## 4. liburing

- 版本：**2.14**（`pkg-config --modversion liburing`，exit=0）
- 来源：Ubuntu 包 `liburing-dev 2.14-1` + `liburing2 2.14-1`（dpkg）
- 头文件：`/usr/include/liburing.h`；库：`/usr/lib/x86_64-linux-gnu/liburing.so*` 与 `.a`（含 ffi 变体）
- 证据：receipts/04-liburing.txt

## 5. SDK 头文件（include/）

`include/` 下仅一个目录 `sluice/`。`include/sluice/` 顶层 8 个条目（3 目录 + 5 头文件）：

- `async/`、`blocking/`、`detail/`（目录）
- `effect.hpp`、`error.hpp`、`file_resource.hpp`、`measurement.hpp`、`result.hpp`

递归 `.hpp` 总数：**26**（完整清单见 receipts/05-sdk-headers.txt）。

与预期"约 11 个"的差异：顶层条目 8（若按顶层算 −3）；若按递归 .hpp 计 26（+15）。差异原因：SDK 采用子目录组织（async/blocking/detail），预期值可能按某种中间口径估计；此处如实记录两种口径的实际数字，未做裁剪。

## 6. Merge SHA（C1 / P2C1）

`git log --merges --oneline -40` 中**不存在** #602/#612 的 merge commit（最近 merge 为 3df10dc4 / #583）。二者均为 GitHub squash 直推的单亲提交，经 `git log --grep` 定位并用单亲 + `merge-base --is-ancestor` 验证：

| 项 | SHA | 性质 | 证据 |
| --- | --- | --- | --- |
| C1（#600 C1, PR #602） | `43cb8029ca60b572c91a9aa9dccb17d4300e2f88` | squash 单亲（parent ac995641…），origin/master 祖先 | receipts/06-merge-shas.txt |
| P2C1（#608, PR #612） | `84e1b7d578c2c58a3c46033487d9e1ac7c639b8b` | squash 单亲（parent 4b962abb…），即 origin/master HEAD 本身 | 同上 |

## 7. 边界遵守声明

本轮仅执行只读审计命令与报告写入（本目录及 receipts/）；未在主工作区构建、未 merge/close PR、未删除任何分支/Worktree/证据、未执行任何破坏性 git 操作。

## 已知限制

- binaryIdentity 为占位值 `pending-release-build`，待 Release 验证员的 sha256 回填。
- 本基线为环境快照；Release 构建/测试结果不在本轮范围内。
