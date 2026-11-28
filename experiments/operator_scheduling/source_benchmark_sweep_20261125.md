# 论文源码集合的 Light-HLS 编译、功能与调度对照

**修复版复测。** 同一 115 条输入使用修复读提升与字节寻址的编译器 `4f7f72da…` 重新运行，其中 101 条实际进入 C/C++ 编译。U250、250 MHz 下有 74 条生成 RTL，74 条通过对应的 C/RTL 校验，含 18 条 Vericert 和 56 条其他论文配置。此前失败的两条 CRUSH `symm_float` 配置均通过。对 56 条非 Vericert 配置，用相同二进制和四组输入重新比较普通块策略。Agent 与手写关键路径逐项打平，两者相对原生均为 1 胜、55 平、0 负，配置等权的周期几何平均降低 0.49%。唯一收益仍是 `gaussian` 的 12,022 → 9,115。以下各节保留首次扫描和当时发现的反例。[修复后 115 条覆盖](source_repaired_results_20261125.json) · [修复后 Agent](source_repaired_agent_20261125.json) · [修复后手写](source_repaired_manual_20261125.json)

2026 年 11 月 25 日。实验使用已收集的 115 条 HLS 源码配置。逐项验证源文件 SHA-256 后，101 条 C/C++ 配置实际进入 Light-HLS，66 条生成 RTL，64 条通过相应的 C/RTL 检查。论文输入之间有家族重复，尤其 CRUSH 的两张表多次使用同一算法。原始归档与变体的关系保存在逐项结果的 `mapping_confidence` 中。[收集清单](../../benchmarks/hls_scheduling_2024_2026/README.md) · [逐项源码结果](source_benchmark_results_20261125.json)

## 覆盖与输入

| 来源 | 配置 | 生成 RTL | C/RTL 通过 | 条件 |
| --- | ---: | ---: | ---: | --- |
| Vericert | 27 | 10 | 10 | 使用作者 Makefile 的 `SYNTHESIS` 宏，顶层是含初始化、运算、校验的完整 `main`，比较 32 位返回值。本表使用 U250 配置。 |
| Stream-HLS | 15 | 0 | 0 | 实际调用了 Light-HLS 和安装的 Vitis 2025.2 头文件，35 秒单次编译限时均超时。用本地 stream 头另做语法适配后，观察到分区指令格式、PHI 分析断言或数学声明等障碍。 |
| ISDC 公开对应物 | 5 | — | — | DSLX 需要 XLS 前端。 |
| Spurious Dynamism | 8 | 8 | 8 | 全部进行四输入集端到端差分。 |
| CRUSH | 23 | 22 | 20 | 两个表中的 `symm_float` 在原生 Light-HLS 上均产生真实输出差异。展开例子缺匹配版 Integration.h。 |
| Out with LSQs | 11 | 11 | 11 | 均通过独立生成的四输入集，另有原始驱动验证见下表。 |
| R-HLS 引用的 StoQ 输入 | 8 | 8 | 8 | 使用初始化完整的独立缓冲区。原始 `kernel_2mm` 驱动的初始化、溢出问题仍保留在上游记录中。 |
| Dynamic Loop Fusion | 9 | — | — | 完整 Intel SYCL host/device 程序，需要原硬件流程。 |
| PCA 的归档版本 | 6 | 5 | 5 | 按入口将原版 g/g1/g2 helper 合并成一个翻译单元，移除重复的未保护头文件包含。`getTanh` 编译触发编译器断言。 |
| PCA 的上游候选 | 3 | 2 | 2 | 与论文版本的映射置信度有限。 |

非 Vericert 输入使用 U250、250 MHz 编译约束和默认板级库。基线二进制 SHA-256 为 `3c3514c5e45f4a15034a1cb9b452a1b4bea556a1ad88a2ba37579ff19a7a9a27`。每个候选经过相同编译入口，用 Icarus Verilog 模拟同步 ap_memory、start/done 协议。独立 C/C++ oracle 用 Clang、`-O1 -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all` 编译。四组确定性输入覆盖小整数或浮点数据、下标与标量参数；条件归约包括正负输入，归一化有非零分母，递归矩阵更新使用限制溢出的输入。逐次比较全部外部数组、标量返回值及每次启动到完成的周期。测试只覆盖这些输入。单次编译限时 35 秒，主扫测每次 RTL 仿真限时 55 秒，对 (N=256) 的 `pca_gesummv` 延长至 350 秒，四组均复现 263,168 周期。未测试布局布线或面积。

CRUSH 的两个 `symm_float` 配置在可定义输入上均遇到 `C[34]` 的 RTL 字样 `0x421c0000` 与原程序 `0x42280000` 不同。此处统计了两条配置、同一个算法家族。它们保留为正确性回归证据。

## Stream-HLS 作者端 C++ 功能验证

在归档提交 `00c6219a82fc710492f4eb44e4f34836dfb19e08` 的全部 15 个 opt5 配置中，生成的 C++ 内核均和作者未修改的 testbench 成功编译运行，退出码均为 0。主机使用 g++ 11.4.0、C++17、`-O1 -ffp-contract=off` 和 Vitis 2025.2 的 HLS 头文件。每个配置使用归档的一组输入、权重和 golden，文件均已按清单校验长度及 SHA-256。作者的判据是逐元素比较两个方向的浮点差值，任一超过 0.1 即触发断言。供审计的第二次运行仅在同一 testbench 的 `forward` 调用后增加二进制输出转储，保留原有循环和断言。[作者公开材料](https://github.com/UCLA-VAST/Stream-HLS/tree/00c6219a82fc710492f4eb44e4f34836dfb19e08/FPGA2025_Artificats) · [逐例输出误差与负对照](stream_hls_csim_20261125.json)

`bicg` 和 `mvt` 各有两个输出张量，其他 13 个配置各有一个。总计 17 个 float32 张量、378,204 个输出元素全部有限，最大绝对误差为 0.064453125，出现在 `k7mmseq_unbalanced`。在各自独立的 golden 副本中逐张量更改首元素，原始 testbench 在 17/17 次负对照中均按预期触发断言。这部分只验证了 Stream-HLS 原生主机 C++ 的功能，表中它在 Light-HLS 的已过 RTL 计数仍为 0。[复现实验入口](stream_hls_csim.py)

## 冻结策略的外部迁移

先在与此处外部配置分开的 Vericert 开发集冻结四个特征的系数，再在全部 54 个非 Vericert 且原生 C/RTL 通过的论文配置上评测。四个系数顺序为加权下游路径长、下游访存链长、直接后继数、当前操作延时。对照的人工关键路径规则为 `[1,0,0,0]`，冻结 Agent 规则为 `[4,8,1,-2]`。库、前端、内存接口、测试输入及选用父候选的规则保持一致。开发与迁移套件有 PolyBench 等算法家族重叠，外部测试提供跨材料来源的证据。[选拔流程](paper_order_flow.py) · [人工规则逐例结果](source_critical_results_20261125.json) · [Agent 逐例结果](source_agent_results_20261125.json)

直接部署时，四个权重用于全部普通基本块。逐块门控时，新旧编译的控制流必须对应，编译器仅依靠静态状态数决定是否采用候选。没有基本块变短则沿用已经验证的原生 RTL；若有块变短但另有块变长，使用固定策略只重编选中的块并重新检查。末端 RTL 周期不参与该决定。

在 54 项配置上，人工规则与 Agent 规则的四组输入均通过 C/RTL 校验，两者的逐项周期全部相同。与原生调度比较，53 项持平，Out with LSQs 的 `gaussian` 从 12,022 降至 9,115 周期，缩短 24.18%。按配置等权计算，候选与原生周期比的几何平均为 0.994887，缩短 0.51%。门控在前 53 项保留原生结果，在 `gaussian` 只选中 `for_body8`，可直接复用已检查的候选 RTL。两个部署方式均为 1 胜、53 平、0 负。

## 作者原始输入和公开结果

Out with LSQs 的固定快照提供 `HLS_VERIFICATION`。本次用它直接运行归档中的每个 `main`，以原始 `CALL_KERNEL` 导出所有传参、结果和外部缓冲区。9 项原始驱动通过 sanitizer 且原生、人工与 Agent RTL 均匹配；另外 2 项被拒绝，其中 `triangular` 访问长度 10 数组的第 10 个下标，`kernel_2mm` 出现有符号乘法溢出。这两项没有计入论文周期并列。表中作者数据取自指定提交的 Ours/Best 和 Baseline/Best 行；Light-HLS 的周期是自身 start/done 的运算区间，作者的协议、存储架构、目标器件及计时端点尚未完全对齐。[原始驱动与三份 RTL 结果](source_author_outlsq_20261125.json) · [作者 Ours/Best](https://github.com/EPFL-LAP/dynamatic/tree/b74ffe6623c88a16704fc0b8dc7c7b718aa169d7/skip/results/Ours/Best) · [作者 Baseline/Best](https://github.com/EPFL-LAP/dynamatic/tree/b74ffe6623c88a16704fc0b8dc7c7b718aa169d7/skip/results/Baseline/Best)

| 原始 workload | Light-HLS 原生周期 | 手写及 Agent 周期 | 论文 Ours 周期 | 论文 Baseline 周期 |
| --- | ---: | ---: | ---: | ---: |
| bicg | 2,790 | 2,790 | 2,024 | 1,877 |
| gaussian | 12,022 | 9,115 | 12,336 | 17,476 |
| matrix_power | 1,558 | 1,558 | 1,057 | 797 |
| get_tanh | 12,000 | 12,000 | 3,197 | 5,010 |
| histogram | 5,002 | 5,002 | 1,054 | 1,058 |
| atax | 3,260 | 3,260 | 3,265 | 3,320 |
| jacobi_1d_imper | 1,770 | 1,770 | 1,345 | 1,182 |
| mvt_float | 7,380 | 7,380 | 10,444 | 10,446 |
| kernel_3mm | 12,660 | 12,660 | 6,463 | 5,809 |

按各自协议的周期数直接并列，三项低于作者 Ours，六项高于作者 Ours。其中 `gaussian` 能定位到调度选择，`atax` 和 `mvt_float` 在使用新策略前就已得到相同周期。只有补足接口、时序与实现条件后才能比较端到端硬件性能。

## 图导出与报告修复

旧的 `HI_EXPERIMENT_SCHEDULE_GRAPH` 接口限定单基本块。多基本块顶层返回作用域错误后，文本和 JSON 报告仍尝试按名字访问未完成的调度表，在一批论文程序上触发 `std::out_of_range`。改用已有的可处理缺失调度项的取名函数后，真实 FIR 用例以退出码 1 正常返回 `EXPERIMENT_SCHEDULE_SCOPE`，且 schedule.json 和 rtl/failure.json 均可读取。其正常编译生成的 RTL、调度 JSON 和文本报告与修复前逐字节一致。权重对照使用固定二进制的普通基本块入口，不受直线图接口的限制。

## 复现

当前构建默认重现修复后的扫描。本工作区保留了初次与最终扫描的独立可执行文件；可给以下 Light-HLS 测量命令传 `--compiler runs/source_paper_tools_20261125/Light_HLS_Top` 以复核初次结果，或传 `--compiler runs/paper_exception_checks_byte_gep_20261125/Light_HLS_Top` 以复核最终结果。每一轮的原生、人工和 Agent 必须共用二进制。最终版也重跑了 11 个 Out with LSQs 的作者驱动，9 个有效驱动的 Agent 与人工逐例打平，对论文 Ours 仍为 3 项周期较小、6 项较大。[最终版原始驱动与论文参照](source_repaired_author_20261125.json)

从仓库根目录运行，使用相同的 Light-HLS、板级库、Clang、Icarus 和可用的 Vitis include。两种权重需要不同的空输出目录。原始正式运行和附加检查分别保存在 `runs/source_paper_sweep_v2_20261125`、`runs/source_paper_vector_audit_20261125`、`runs/source_paper_pca_unity_20261125`、`runs/source_paper_{critical,agent}_order_20261125` 以及 `runs/source_paper_author_outlsq_20261125`。初次二进制被其他构建覆盖的尝试和向量适配试跑已剔除。

```bash
python3 experiments/operator_scheduling/source_benchmark_sweep.py \
  --output runs/rep_source --summary runs/rep_source/summary.json \
  --jobs 6 --timeout 35 --sim-timeout 350
python3 experiments/operator_scheduling/source_benchmark_sweep.py \
  --baseline-run runs/rep_source --baseline-summary runs/rep_source/summary.json \
  --weights '[4,8,1,-2]' --output runs/rep_agent --summary runs/rep_agent/summary.json \
  --jobs 6 --timeout 35 --sim-timeout 350
python3 experiments/operator_scheduling/source_benchmark_sweep.py \
  --baseline-run runs/rep_source --baseline-summary runs/rep_source/summary.json \
  --weights '[1,0,0,0]' --output runs/rep_critical --summary runs/rep_critical/summary.json \
  --jobs 6 --timeout 35 --sim-timeout 350
python3 experiments/operator_scheduling/stream_hls_csim.py \
  --output runs/rep_stream_host --summary runs/rep_stream_host/summary.json
python3 experiments/operator_scheduling/source_benchmark_sweep.py \
  --archived-outlsq runs/rep_source --author-policies runs/rep_critical runs/rep_agent \
  --author-artifact runs/benchmark_collection_20261119/dynamic/outlsq \
  --output runs/rep_author --summary runs/rep_author/summary.json
```
