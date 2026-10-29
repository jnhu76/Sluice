# P2C1 静态复算记录 — 关闭 §2.3 NOT_RUN + §6.3/§9 条件2 的 +10.228 ns eofmix 残差定量归因

- 会话：P2C1 静态复算员（REVISE 条件 2）。日期 2026-10-28。主机 E5（Fedora fc44，
  binutils `GNU objdump version 2.46.1-1.fc44`）。
- 目的：`P2_CANDIDATE_P2C1.md` §2.3 披露的 measurement-segment 未复跑的 audit 约定
  per-path 指令复算；§6.3/§9 条件 2 要求的 eofmix 残差（D_cand_eof = +10.228 ns）
  每额外一步定量拆分；§3.5 claim cap 判定。
- 只读：未改任何源码/分支/文档；所有中间物在 /tmp/p2c1-recount/。

## 0. 工件与命令（全部本会话执行）

```
sha256sum /home/jnhu/Source/Sluice-600-wt/build/linux/x86_64/release/libsluice_core.a
  → d1e351a54ded3dfa1df6b1cd2e47622eefbd5d7027439cb31a314bed3a3007f0   (= 冻结 B1，与 ask 一致)
sha256sum /home/jnhu/Source/Sluice-direct-read-compose-prevalidated-steps/build/linux/x86_64/release/libsluice_core.a
  → 07d0680307c71cc412dfd021fd47c3098de57b580283df189f66dcdc2052049e   (= §2 候选归档)
sha256sum …/direct-cost-ladder/ladder    → 8945036746d03389… (= p2c1-base-eofmix-4k.txt 头记录)
sha256sum …/direct-cost-ladder/ladder-c1 → 8305d22ca4ff74db… (= 候选侧 cell 所用二进制)
ar x 提取 blocking_file.cpp.o：B1 .o sha256 3d84a523…3001b；候选 .o sha256 d5c58bb3…a8b
objdump -dr --demangle  → 本目录 4 个 .objdump.txt（落盘原始输出）
```

计数约定 = P1 audit 精确路径约定（`p1-audit/composition-read.md` 头：合法非空调用、
首步指定返回序列路径上的全部指令，含全部条件分支、syscall call、ret；对齐 nop 不计；
`nop/nopl/nopw/xchg %ax,%ax` 一律排除）。访存数按"引用内存的指令"计：load 类含
`mov …,(%r)`、`movdqa`、内存源 `cmp/cmpb/cmpq/test`；store 类含全部内存写。

锚点复核（先于任何新数字）：B1 read_exact_at 1-step **100**、每额外步 arc **44**；
read_exact **82/34** —— 与 P1 audit §1.2/§2.2 完全一致。候选 read_exact_at **68**、
arc **25**；read_exact **55/21** —— 与 P2_CANDIDATE §2.1 审核通过值完全一致。
八/八个锚点复现 ⇒ 本会话的约定应用与两份既有记录对齐（同时关闭 §2.6 所述
"68/25/55/21 未被后续会话独立重执行"的缺口，就静态计数而言）。

## 1. read_exact_at 四形状精确计数（本会话新数；地址为各自 .o 内偏移）

形状定义按 eofmix 的 ladder tail 轮转（`ladder.cpp:784-795`）：full=末整块 1 步满
返回→complete；half=跨 EOF 1 步短返回(2048)+回边+第 2 步 0 返回→eof；atEOF=offset
=file_bytes 1 步 0 返回→eof。cell 计时窗每 3 op 恰 4 次 pread、1 次回边（1.3333
pread/op，与 §2.5 strace 2.6667 timed+verify 一致）。

| 路径 | B1（指令序列） | 候选（指令序列） | Δ |
|---|---|---|---|
| 1-step full | **100** = 序 29 + 步 27 + complete 合并/状态/出口 23 + 共享尾 21 | **68** = 序 30 + 步 13 + complete 合并 5 + 物化 4 + 尾 16 | −32 |
| 每额外一步 arc | **44** = 短步合并 7 + 状态块 10 + 下一脚步体 27 | **25** = 短步合并+回边 12 + 下一脚步体 13 | −19 |
| at-EOF | **95** = 29+27+eof 合并 18+尾 21 | **69** = 30+13+eof 合并 7+eof 物化 7+尾 12 | −26 |
| half（=atEOF+arc） | **139** | **94** | −45 |

关键地址（供复核）：
- B1：步循环 0xfc0–0x102d；pread call 0x1020；不可能计数守卫 0x1200/0x1203；
  状态块 0x1250–0x127f（含死存 0x1277 `movaps …0x20(%rsp)`）；complete 出口
  0x1285–0x12a1→共享尾 0x10d3–0x1143；eof 出口 0x1216–0x1247→0x10d3。
- 候选：步循环 0xfc0–0xfed（pread call 0xfe5）；守卫 0x1130/0x1133；complete 物化
  0x1170–0x117e→尾 0x107d–0x10e0；eof 物化 0x11e8–0x120c→尾 0x108f–0x10e0；
  回边 0x1142–0x1157。全函数无 `movaps …0x20(%rsp)`（grep 0 命中，§2.2 复核成立）。
- read_exact（cursor）：B1 序 21+步 17+出口 44 = **82**、arc **34**（=7+10+17）；
  候选 **55**、arc **21**；at-EOF B1 **76**/候选 **56**；half B1 **110**/候选 **77**。

每额外一步访存数（arc）：
- B1 read_exact_at：**13 次**（4 load + 9 store）—— 状态块独占 7（1L/6S，含
  movdqa+movaps 往返与死 16B 存）+ 步体 6（3L/3S）。
- 候选 read_exact_at：**4 次**（3 load + 1 store）→ **−9 访存/步**（−1L/−8S）。
- B1 read_exact arc 11 次（4L/7S）→ 候选 2 次（2L/0S），同 −9/步。
- 1-step full 全路径访存：B1 ≈42（16L/26S，含内存源比较）→ 候选 ≈25（10L/15S）。

## 2. 实测单元的额外步数与静态超额（AX vs A1x，同二进制）

A1x 原生臂 = `n1_read_exact_at` 内联进 ladder `one_op`（ladder.cpp:138-177；
机器 0x403080–0x403168 + 共享尾 0x402e50→0x4028c0）。本会话数：full **54**、
arc **23**、atEOF **56**、half **79**（含尾 11；one_op 公共分派序 16 条两臂同
付、不计入差）。AX 臂包装=分派到 call 7 + 消费者尾 11 = 18（两二进制同）。

桥接复核（本会话）：ladder 与 ladder-c1 的 one_op 归一化(去地址/去符号) diff 仅
3 行 = write_all/read_exact/write_all_at 三个 call 目标地址；**A1x 内联块（含其
pread@plt 调用）逐条相同**；两二进制中链接的 sluice read_exact_at 与各自归档 .o
指令序列相同（242/242、174/174 行，分支相对位移逐一相等）——§2.2/§2.3 桥接与
public-op 结论在本会话独立复核成立。

| 形状 | AX_B1 | AX_cand | A1x | E_B1 | E_cand |
|---|---|---|---|---|---|
| full（1 步, complete） | 7+100+11=118 | 7+68+11=86 | 54 | **+64** | **+32** |
| half（2 步, eof） | 7+139+11=157 | 7+94+11=112 | 79 | **+78** | **+33** |
| atEOF（1 步, eof） | 7+95+11=113 | 7+69+11=87 | 56 | **+57** | **+31** |

- composition-4k（seq，计时窗 1.000 pread/op）：额外步数 **0/op**；
  静态超额 E = **+64**（B1）/ **+32**（候选）每 op。
- eofmix（tail 轮转）：额外步数 **1/3 op**、步数 4/3 op；旋转均值
  E_B1_eof = (64+78+57)/3 = **+66.33**/op；E_cand_eof = (32+33+31)/3 = **+32.0**/op。
  候选侧 arc 份额仅 8.33/32.0 = 26%；其余 74% 是 1-步/出口机器（E_cand(full)=32、
  E_cand(atEOF)=31），非每额外步机器。

## 3. 定量归因（+10.228 ns 残差）

静态→墙换算 λ 取自两条同二进制、CI 排零、互为对照的 B1 基线 cell（点估计=中位数）：
- S（seq）= +15.439 ns ↔ +64 instr/op ⇒ **λ₁ = 0.241 ns/instr**
- S_eof（tail）= +16.577 ns ↔ +66.33 instr/op ⇒ **λ₂ = 0.250 ns/instr**
两锚相差 4% —— 静态模型+单一 λ 对 B1 侧两个 pattern 同时成立
（尾混合比 seq 只多 2.33 instr/op 静态，实测只多 1.14 ns）。

候选 eofmix 预测：E_cand_eof × λ = 32.0 × {0.241, 0.250} = **+7.7…+8.0 ns**。
实测 D_cand_eof = **+10.228**（中位数；均值 +8.567；EXCESS 号均值 CI
[+2.115, +15.019]）。

- 按中位数：缺口 2.2–2.5 ns（占 21.6–24.6%）——预测落在实测 CI 内但未覆盖点值。
- 按均值：缺口 0.6–0.9 ns（7–10%）——近似覆盖。
- 拆分：arc 份额 8.33 instr × λ ≈ 2.0–2.1 ns；1-步/出口份额 23.67 × λ ≈ 5.7–5.9 ns；
  静态模型未归因余量 ≈ 2.2–2.5 ns。

不确定度与模型局限（为何不能宣布"定量 account for"）：
1. 跨二进制同臂锚点互斥：Δ_own(seq) −16.565 ↔ 静态 −32 ⇒ λ′=0.518；
   Δ_own_eof −9.813 ↔ −34.33 ⇒ λ′=0.286 —— 两 λ′ 相差 1.8×，说明静态→墙映射
   本身带 pattern/二进制级（布局、PLT、icache）系统差 ±30–80%。
2. 候选自身矛盾（关键）：候选 seq cell 与 tail cell 携带**相同**的 +32.0 instr/op
   静态超额，实测却分别为 D_cand(seq) = −3.563（CI 含 0，PARITY）与
   D_cand_eof = +10.228（CI 排零，EXCESS）——同一静态超额在两种 pattern 下墙值
   相差 ~13.8 ns。静态×步数模型无法产生该结构；EOF 边界 pattern（短返回+0 返回、
   2/3 op 走 eof 出口、每 3 op 一次回边分支）对 AX−A1x 差有静态计数之外的动态放大。
3. 会话噪声地板 sd_AA ≈ 14.6–15.6 ns（MDE′ 7.8–8.3 ns）与 CI 半宽 ~6.5 ns：
   2.2–2.5 ns 缺口在噪声内，但同样无法用静态模型证明为零。

PMU 旁证（§2.4，非门禁）：AX−A1x +128.0 → +66.1 instr/op，与静态 E 64 → 32
同步减半（两侧 PMU/静态 ≈ 2.0–2.1×），支持旋转/静态结构，但不约束 ns 级 λ。

结论：复算定量解释了残差的量级与构成（≈75–78% 中位数口径 / 90–93% 均值口径；
arc 份额仅 ~2 ns），但**未**定量 account for 残差点值（中位数缺口 2.2–2.5 ns），
且候选 seq/tail 同静态-异墙的矛盾表明剩余部分是 EOF 边界动态放大，非每步静态成本。

## 4. §3.5 claim cap 判定

**维持（不解除）**。理由：
- ask 判据"仅当复算定量 account for 残差时才可解除"未满足（§3 的缺口与矛盾）。
- plan §3.5 G4 的判据是被测墙判据（D_cand_eof 不再 EXCESS）；静态复算不能推翻
  +10.228 ≥ max(5.535, 7.010) 的实测 EXCESS。尾混合 wall-parity 主张仍被 cap。
- §9 条件 2 的"执行 re-attribution"义务就此履行完毕（拆分已定量并落盘），其结果
  是**部分归因**：+32.0 instr/op 候选侧旋转均值静态超额 × pattern 依赖
  λ∈[≈0, 0.320] + 未归因 EOF 边界动态放大（~2.2–2.5 ns 中位数口径）。claim 标签
  仍为 "measurable improvement with re-attributed residual"，其中
  re-attributed 现已定量落地。
- 若要解除：需消除残差（如出口/1-步机器进一步收缩 E_cand_eof 的 74% 份额）或
  按 §2.4 纪律重测该 cell 并以合规记录改判 D_cand_eof。

## 5. 未复算项（NOT_RUN_WITH_REASON）

- 候选 write_all_at/write_all 形状级复算：属 P2C2 范围，本 ask 未要求。
- ac/a1c（read_exact cursor）cell 的 A1c 臂计数：cursor cells 与 +10.228 残差
  （ax/a1x cell）无关，未展开。
- eofmix 的 A1x 侧访存明细：只数了指令数（54/23/56/79），访存未逐条数——不影响
  归因结论（差值由 AX 侧驱动）。
