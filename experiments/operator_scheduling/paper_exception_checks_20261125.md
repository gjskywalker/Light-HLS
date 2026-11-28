# Vericert 输入的三个 Light-HLS RTL 异常复核

2026-11-25。`lu` 和 `nussinov` 使用 `collection_order_final_20261125` 中冻结的原生调度 RTL。`adi` 使用 `collection_verified_20261125` 中能生成 RTL 的后端。三个程序都执行了同一份输入的原生 C 参考实现。本文沿用主实验四态 RTL 校验门槛。[逐项结果、哈希和探针记录](paper_exception_checks_20261125.json)

| 输入 | 原生 C 校验值（十六进制） | 原 Icarus | 独立 Verilator 4.038 | 结论 |
|---|---|---|---|---|
| `lu` | `00000001` | 仿真进程在 240 秒墙钟超时，完成拍数未知 | 到 2,000 万及 1 亿拍时均未拉高 `ap_done` | 在可达的状态 74、75 之间陷入死循环 |
| `nussinov` | `0000001c` | 第 168,679 拍完成，返回 `00000000` | 相同拍数完成，返回相同的错误值 | 完成时整个动态规划表仍为零 |
| `adi` | `00000001` | 定向跟踪至 219,490 拍仍未完成，退出比较为未知值 | 第 219,481 拍完成，返回 `00000001` | 二态与四态结果不同，未计入主实验有效周期 |

## `lu` 的退出条件

从第 20,000,000 拍起连续观测 16 拍，控制器交替处于 74 和 75，累加数 `v_v182`、累加器 `v_acc_06_i44_i_i13` 和结果 `v_add_i47_i_i16` 均为 0，界限 `v_v174` 为 3。冻结 RTL 的状态 74 计算累加器加上累加数，随后进入状态 75。状态 75 仅在结果大于界限时退出，否则将结果写回累加器并返回状态 74。这两个状态都保持累加数和界限。因此从该可达状态出发，只要不重新复位，判断将一直是 `0 > 3`，控制器无法退出。另一个商计数寄存器仍在更新，但它没有进入这个判断。第 100,000,000 拍的返回总线为零，此时没有有效返回值。

## `nussinov` 的内部数据

在完成拍后，观测器通过只读端口读出 `seq_storage` 和 `table_v_storage`，并逐位置对照相同 C 源文件的 `init_array` 和 `kernel_nussinov`。输入序列 60 项全部一致。RTL 的 60 × 60 个表项全部为零，而 C 有 1,624 项非零。例如，`table[0][4]` 的 C 值为 1，`table[0][5]` 的 C 值为 2，两者的 RTL 值均为 0。

按 C 的执行顺序，第一个非零表项为 `table[55][58]`，对应 RAM 地址 3,358。四态写端口探针观察到状态 27 把正确的对角线候选值 1 写入。内层 `k` 循环却以计算对角线之前的 0 为初值，状态 31 随后两次向同一地址写回 0。[只读探针](paper_exception_checks_tb.sv)

冻结编译的 `.lighthls/top.bc` 在存入对角线结果后，才从相同地址读取 `%arrayidx156.promoted.i` 作为内层循环初值。`HI_InstructionMoveBackward0` 日志第 8,760 和 9,305 行记录了两次跨块提升，第二次将这条读操作移入 `for.body3.i`，越过了原本在它之前的同地址写操作。读写顺序在 `top_output_PM0.bc` 已经颠倒。`HI_RemoveRedundantAccessPM2` 日志第 1,211–1,212 行记录了向后转发旧值 `%.337.i`。最终 LLVM IR 的写数据为 `%34`，进入循环的 PHI 初值却为 `%23`，生成的 RTL 与该 IR 一致。[提升实现](../../Light_HLS/Implementations/HI_InstructionMoveBackward/HI_InstructionMoveBackward.cc)

## `adi` 的四态与二态差异

此版本 `top.v` 的 SHA-256 为 `5eb1f4a16ff7f98ea8aedfa6cdec5316a71be65dc20a66ad26d028b00f517774`。第 219,481 拍，Verilator 的 `ap_done=1`，而 Icarus 处于状态 34，`ap_done=0`，在随后的状态 35，退出比较 `v_v169 > v_v124` 为 `x`，因为界限 `v_v124` 为 `32'hxxxxxxxx`。控制流因此一直返回 34。这两个状态都保持界限。此次观察不支持把 219,481 拍用于性能比较。

## 两次独立构建的正确性结果

第一次修正禁止 `HI_InstructionMoveBackward` 把读取内存的指令跨基本块提升。这个 pass 依据 SSA 的数据依赖选择指令，并未证明提升路径上没有别名写入，也未证明在原本不执行的路径上可以安全地读内存。该修正恢复了 `nussinov` 和 `lu`。

对该版本的 `adi`，四态探针跟踪所有四个数组的读写。把接受启动的上升沿记作 1，第 8,854 个上升沿首次读取未知值。此时状态 82 读取 `u_storage[476]`，数组只有 400 项。Clang 把一个访问表示成两个连续的单索引 `getelementptr i8`，其字节偏移合计为 `156 + 80*k`。对 `int[20][20]`，相同位置的字地址为 `39 + 20*k`。以 `k=4` 为例，实际应读下标 119。第一版修复保留的 GEP 降低过程把 476 直接接入按字编址的 RAM。未知值随后流入退出条件，原 testbench 等到 2,000 万拍仍未完成。

第二次构建额外修正了 `HI_SeparateConstOffsetFromGEP`。在原始指针链仍可追溯时，检查单索引字节 GEP 的底层对象是否为栈上标量数组，并由 LLVM 已知位分析证明偏移可被元素大小整除。对于元素长度为 2 的幂且满足条件的访问，将 `getelementptr i8` 恢复成等效的元素类型 GEP，再进行已有的按字地址降低。这同时保留了有符号负偏移的含义。此范围以外的指针形式仍走已有路径。[地址降低实现](../../Light_HLS/Implementations/HI_SeparateConstOffsetFromGEP/HI_SeparateConstOffsetFromGEP.cc)

| 输入 | 仅修正读提升，四态 Icarus | 再修正字节 GEP，四态 Icarus | 最终 RTL 与原生 C 逐字比较 |
|---|---|---|---|
| `lu` | 336,784 拍，`00000001` | 336,784 拍，`00000001` | 输出矩阵 `A` 的 1,600 字全部一致 |
| `nussinov` | 180,656 拍，`0000001c` | 180,656 拍，`0000001c` | 动态规划表的 3,600 字全部一致 |
| `adi` | 2,000 万拍仍未完成 | 271,321 拍，`00000001` | 输出矩阵 `u` 的 400 字全部一致 |

逐字比较使用独立 testbench 在 `ap_done` 时读出完整输出数组，逐项与同一 C 源文件的初始化与内核函数对照。未改动固定输入、数据规模和返回校验算法。这些修复提高了有效覆盖率，上述周期是各版本实际仿真值。[逐字验证脚本](paper_exception_checks_memory.py) · [独立构建结果](paper_exception_checks_repaired_20261125.json)

两个独立可执行文件分别完成仓库本地回归，均为 **27/27 组通过**。这 27 组包括 RTL 行为、流水线、存储端口、绑定、事务和数据流等测试。逐组状态、各独立编译器的 SHA-256、上述三个用例的 RTL 哈希和完整输出数组摘要均保存在结果文件中。[本地回归入口](../../Light_HLS/Tests/run_regressions.py) · [结果和版本](paper_exception_checks_repaired_20261125.json)

## 复现

从仓库根目录执行下列命令，以新目录接收原始日志、读数及状态快照。输入来自先前真实程序扫描中的原生 C、已生成的 `top.v` 和原 Icarus 结果。脚本对 DUT 仅增加外部层次只读探针。复位保持两拍，随后拉高 `ap_start` 一拍，从接受启动后的下一拍开始计数。[诊断脚本](paper_exception_checks.py) · [原生扫描入口](paper_order_flow.py)

```bash
python3 experiments/operator_scheduling/paper_exception_checks.py \
  --existing-run runs/collection_order_final_20261125 \
  --verilator-root runs/paper_physical_20261125/verilator_local \
  --output runs/paper_exception_checks_replay

python3 experiments/operator_scheduling/paper_exception_checks.py \
  --existing-run runs/collection_verified_20261125 \
  --verilator-root runs/paper_physical_20261125/verilator_local \
  --cases adi --output runs/paper_exception_checks_adi_replay

python3 experiments/operator_scheduling/paper_exception_checks_rebuild.py \
  --existing-run runs/collection_verified_20261125 \
  --output runs/paper_exception_hoist_replay

python3 experiments/operator_scheduling/paper_exception_checks_rebuild.py \
  --existing-run runs/collection_verified_20261125 --recover-byte-gep \
  --output runs/paper_exception_final_replay

python3 experiments/operator_scheduling/paper_exception_checks_memory.py \
  --existing-run runs/collection_verified_20261125 \
  --rtl-run runs/paper_exception_final_replay \
  --output runs/paper_exception_memory_replay

python3 Light_HLS/Tests/run_regressions.py --keep-going \
  --light-hls-top runs/paper_exception_final_replay/Light_HLS_Top \
  --output runs/paper_exception_regressions_replay
```
