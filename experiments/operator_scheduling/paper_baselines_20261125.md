# 已收集 14 篇调度论文的比较基线

核对日期为 2026 年 11 月 25 日。表中给出的数字是作者发表或归档的参考结果，Light-HLS 本轮的实测成绩由相应实验报告给出。跨工具的同程序比较同时包含前端、绑定、存储体系和 RTL 生成的影响。在 Light-HLS 内归因给调度器时，应让这些部分共用同一实现，并逐项检查功能。

## 可以落到具体数字的原始基线

- **HeuriGym 有逐图的专家 ILP 周期。** 24 张图保留了原边、节点时延、各类算子上限和原始 demo／eval 身份。以原始 verifier 判合法、原始 evaluator 计 makespan，就能在相同数学问题上直接与作者保存的周期比较。例如 `fir1 = 16`、`jpeg_fdct = 23`、`dag_1500 = 92`。若同一目标的独立下界已等于该值，则任何合法调度皆不能取得更少周期。求解时间和解的质量可分别比较。[作者 ILP 结果](../../benchmarks/hls_scheduling_2024_2026/sources/heurigym_tools/operator_scheduling/baseline/baseline.json) · [作者论文](https://arxiv.org/abs/2506.07972)
- **Vericert 的归档包含 27 个完整程序、5 种编译配置、135 个绝对周期值。** 本次逐列从原作者 tar 中读取，核查五列程序集合一致，保留原始程序名称。以 `bicg` 为例，`main` 初始化 42×38 矩阵，执行计算再异或校验；Vericert-original、list、hyperblock 分别为 130,790、77,513、65,812 周期，Bambu 的默认优化和关闭多数优化分别为 18,404、63,024 周期。功能、输入/尺寸、完整执行区间、存储接口和计时边界对齐后才可按周期并列。比较物理耗时和面积还需对齐器件及时序约束。[135 项作者结果](vericert_published_cycles_20261125.csv) · [原 C 程序](../../benchmarks/hls_scheduling_2024_2026/sources/vericert_pldi2024_archive/benchmarks/polybench-syn/linear-algebra/kernels/bicg.c) · [作者归档与复现说明](https://zenodo.org/records/10808233)
- **动态工具的结果也可以定位到具体工作负载。** 例如 Out with LSQs 的固定快照给 `atax` 两个最优配置分别记录了 3,265 和 3,320 周期。二者的布局布线周期分别为 6.92 和 7.07 ns。前者的 `N=3` 是内存窗口参数，后者的 `6/6` 是 load/store 队列参数。复现时还需对齐仿真驱动、外部存储响应和动作边界；这些参数不能当成矩阵尺寸。[作者 Ours 记录](https://github.com/EPFL-LAP/dynamatic/blob/b74ffe6623c88a16704fc0b8dc7c7b718aa169d7/skip/results/Ours/Best/atax-3/results.csv) · [作者 Baseline 记录](https://github.com/EPFL-LAP/dynamatic/blob/b74ffe6623c88a16704fc0b8dc7c7b718aa169d7/skip/results/Baseline/Best/atax-6-6/results.csv)

## 逐篇比较条件

“条数”指本收集包记录数。同名的程序、不同实现或同一设计的多次种子不等于新增独立设计。通过一套 benchmark 的自动检查也不等于战胜提出它的工具。

| 论文／已收集条数 | 作者报告、实验对象与指标 | 对齐 Light-HLS 时要核对的条件 |
| --- | --- | --- |
| Hyperblock Scheduling for Verified High-Level Synthesis／27 | Fig. 10 评价 PolyBench 整数 C，仿真计整程序周期，Vivado 对 xc7z020clg484-1 布局布线，目标 100 MHz。相对 Vericert-original，hyperblock 的平均周期比为 0.46，执行时间比为 0.48。 | 对照 5 列，特别包含较强的 Bambu 默认配置。作者的仿真入口为 Verilator 的 `Vmain`，复位序列后计数器从 1 起，到 `finish` 为止；必须换算 Light-HLS 的启动／完成握手边界。保留原程序的初始化与 checksum，并区分 U250 与 Zynq 的硬件结果。[论文 §8](https://doi.org/10.1145/3656455) · [原工件](https://zenodo.org/records/10808233) |
| Subgraph Extraction-based Feedback-guided Iterative Scheduling for HLS／5 | Table I 的 17 个 XLS 设计用 SKY130、Yosys 和 OpenSTA。以 `crc32` 为例，在 2,500 ps 目标下，原 SDC／ISDC 分别为 3／1 级、75／38 个寄存器。作者报告总体寄存器下降 28.5%，调度用时约增加 40.8 倍。 | 收集的是 5 个公开 DSLX 对应物。精确 XLS 修订、工业设计和底层时序/寄存器口径不足。在 FPGA 上替换一个 C 基本块无法复现这项 ASIC 时序反馈结果。[论文 Table I](https://past.date-conference.com/proceedings-archive/2024/DATA/175_pdf_upload.pdf) |
| Suppressing Spurious Dynamism of Dataflow Circuits via Latency and Occupancy Balancing／8 | Table 4 在 Kintex-7、目标 6 ns、Vivado 2019.1 计入动态电路简化。FIR 的基准为 1,011 周期、453 LUT；All 简化为 1,010 周期、105 LUT。 | 优势主要体现在保持吞吐量时节省面积；需要完整循环、占用/背压行为、相同驱动和综合后资源。[作者论文 Table 4](https://dynamo.ethz.ch/wp-content/uploads/2024/04/Xu_FPGA24_SuppressingSpuriousDynamism.pdf) |
| R-HLS／8 | Table 1 的八个设计比较 StoQ、R-HLS、NoQ。以 `atax` 为例，三者分别为 840、787、1,585 周期，完成时间分别为 5.68、7.14、14.43 µs。R-HLS 的周期下降伴随实际时钟周期增大。 | 已收集论文引用的 StoQ 输入。比较需还原 R-HLS 输入和内存设置，并记录周期与布局布线时钟，否则该例的两种排名正好相反。[论文 Table 1](https://www.sjalander.com/research/pdf/sjalander-iccad2024-rhls.pdf) |
| Differentiable Combinatorial Scheduling at Scale／18 | Fig. 3 的 12 个随机工作负载在给定最大延迟下，比较目标函数随优化时间的变化，含 CPLEX、Gurobi、CP-SAT。随机实验使用延迟界 10、权重 10。 | 作者仓库公开 18 个 GML，成员数与 Fig. 3 的 12 项不同。保留其周期约束、目标和图到论文工作负载的映射，区分当前可行解与复现作者一小时收敛轨迹。[论文 §4](https://proceedings.mlr.press/v235/liu24al.html) · [官方模型与求解器](https://github.com/Yu-Maryland/Differentiable_Scheduler_ICML24) |
| Stream-HLS／15 | Table 7 的 `2mm` 在三个论文 DSP 预算（220/2,560/9,024）下，Opt5 的延迟依次为约 378,000／36,400／11,800 周期。归档 HLS 脚本使用 U280、3.33 ns；作者也分项对比 Allo 等工具。 | 需复现应用尺寸、分块、通道、DSP 限制、数据流和端到端计时。Table 7 官方 CSV 的汇总行标有 128/256/512，与正文 220/2,560/9,024 不同；Table 10 的 synthetic 例也存在预算不一致，需核对每次求解配置。每种预算逐列对照。[论文](https://arxiv.org/html/2501.09118v1) · [作者 Table 7](https://github.com/UCLA-VAST/Stream-HLS/blob/00c6219a82fc710492f4eb44e4f34836dfb19e08/FPGA2025_Artificats/results/tables/table7.csv) |
| CRUSH／23 | Table 2 的动态共享实验在 xc7k160tfbg484-1、6 ns、Vivado 2019.1 下评价。相对 In-order，作者报告平均 DSP 用量降低 12%、执行时间降低 4%。Table 1 的 `gesummv` 展开 75 后，无共享需要 790 DSP，CRUSH 使用 60 DSP。 | 需在相同展开、功能单元与背压协议下，比较功能、面积和时间。23 条含同程序的不同配置；一个展开例的精确版本头文件缺失。[作者论文 §6](https://dynamo.ethz.ch/wp-content/uploads/2025/06/Xu_ASPLOS25_CRUSH.pdf) |
| Dynamic Loop Fusion in High-Level Synthesis／9 | Table 1 使用 Arria 10 GX1150、双 DDR4，Quartus 19.2 给出布局布线报告。实际执行三次，取最短时间。FFT 的 STA／LSQ／FUS1／FUS2 用时 7.8／7.8／2.8／1.7 秒。 | 需要 Intel SYCL host/kernel、相同输入规模及 DRAM 路径；只测一个函数的 RTL 周期无法对应板上秒数。取作者实验脚本启用的九项。[论文 §7/Table 1](https://arxiv.org/html/2501.14631v1) · [工件](https://zenodo.org/records/14383450) |
| A Source-to-Source Compiler to Enable Hybrid Scheduling for High-Level Synthesis／9 | Table 2 在 xc7z020clg484-1、250 MHz、目标 II=1 上，比较多组输入的周期及墙钟时间几何均值。`histogram` 的 PCA 结果为 12,095 周期、45.4 µs。 | 归档中的九项含同名源程序和上游候选；缺作者转换的 PCA 版本及所用输入向量。动态设计的周期随输入而变。[论文 §6/Table 2](https://www.mdpi.com/2079-9292/14/23/4578) |
| Resource and Phase Awareness for Dynamically Scheduled High-Level Synthesis／0 | 作者用四项任务评估动态缓冲优化，目标为 xcvu19p-fsva3824-2-e、5 ns，最多节省 40% 缓冲。 | 本次按论文标题、作者和工件关键字复查，未定位这四项的可核对精确快照。通用 Dynamatic 安装不能恢复作者选择的相位和图配置。[作者论文](https://dynamo.ethz.ch/wp-content/uploads/sites/22/2025/06/Bouilloud_HEART25_ResourceAndPhaseAwareness.pdf) |
| Out with LSQs／11 | Vivado 2024.2、Kintex-7。固定分支有 `Ours/Best` 与 `Baseline/Best` 各 11 个最优配置的资源、实际时钟和周期，以及按窗口等参数的探索记录。 | 可用上面的 `atax` 数字做定点对齐；两边应各自按预先固定的同等搜索预算选配置。覆盖完整函数以及 memory ordering 的正确性。[论文](https://doi.org/10.1145/3748173.3779204) · [作者固定结果快照](https://github.com/EPFL-LAP/dynamatic/tree/b74ffe6623c88a16704fc0b8dc7c7b718aa169d7/skip/results) |
| HeuriGym／24 | 已取得 5 demo ＋ 19 eval 和同题的逐图专家数值。例如 `fir2` 17 周期。 | 共享原始验证器及求值器可以比较解的质量；已看过的 7 张预留图在新迭代中属于历史已知数据。若声称严格优于最优值，先复核约束、计时和最优性证书。[作者论文](https://arxiv.org/abs/2506.07972) · [作者参考数据](../../benchmarks/hls_scheduling_2024_2026/sources/heurigym_tools/operator_scheduling/baseline/baseline.json) |
| RAG-Enhanced Kernel-Based Heuristic Synthesis (RKHS)／0 | Table 2 报告 Full RKHS 在代表性 G1–G4 上分别为 52.0、98.8、95.2、71.8 周期，平均 79.45。 | 在作者预印本及按标题/工件关键词复查中仍未定位 200/50 张训练/验证图的文件、生成器与随机种子；无法在这些 G1–G4 上计算本方法分数。[论文和 Table 2](https://arxiv.org/html/2604.26153v1) |
| NeuroSchedule2.0／0 | IEEE 摘要报告，相对已有神经调度方法，大型真实 benchmark 的最大调度周期降幅为 8.41%。 | 核验到出版条目和摘要；本次未定位可对齐的逐例 CDFG、RTL、资源配置及开源工件。8.41% 是报告的最大降幅，不能作为所有图的统一胜负线。[出版条目](https://doi.org/10.1109/TCAD.2026.3661431) |

## 复核作者绝对周期

Vericert 的公共源归档内还有一层 `artefact/synthesis-results.tar.xz`，其 SHA-256 为 `f43b0201b64b23ac071ae54af317984a40fb067943f1e09ccd22bd17c0e100f5`。五列都直接来自 `synthesis-results/<配置>/exec.csv`，没有使用论文图像反推数字。下面的命令在已下载收集包上显示原始 hyperblock 周期。[下载来源](https://zenodo.org/records/10808233)

```bash
tar -xOJf runs/benchmark_collection_20261119/static/vericert-pldi2024/artefact/synthesis-results.tar.xz \
  synthesis-results/vericert-hyperblock/exec.csv
```

原作者的评估把周期与布局布线后的实际时钟、资源分别记录。达到目标时钟约束、领先某个基线配置、领先同一论文全部配置，分别需要独立的证据。源文件、测试向量或器件无法对齐时，应继续保留该项和具体原因，以便后续适配后补跑。[全部输入与来源](../../benchmarks/hls_scheduling_2024_2026/README.md)
