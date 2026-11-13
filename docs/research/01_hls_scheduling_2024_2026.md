# HLS 调度：2024–2026 代表工作与 Light-HLS 的研究切入点

整理日期与检索截止：**2026-11-10（香港时间）**。供 2026-11-11 review。本文将“近三年”解读为 2024、2025、2026 三个历年；2026 尚未结束。

**核心判断：还有改进空间，但不能仅以“把 static 与 dynamic 结合”作为新意。** 近年的进展更具体：找出哪些依赖必须在执行时处理、消除多余握手、让资源共享与动态执行共存，以及改变循环之间可重叠的范围。对目前 Light-HLS，先研究可验证的静态调度启发式及其资源／存储联合决策，比立即重建完整动态后端更容易得到可归因的结果。这是本文的研究建议，尚非实验结论。

## 1. 先厘清分类与比较层级

| 类型 | 哪些决策在何时作出 | 主要收益 | 必须计入的代价 |
|---|---|---|---|
| Static scheduling | 编译时确定相对启动时刻、控制状态或模调度；执行时仍可有条件分支与停顿 | 容易做时序控制、运算链接与资源共享 | 对不确定依赖和长短路径可能较保守 |
| Dynamic scheduling | 硬件依数据／控制 token、资源可用性等条件决定启动 | 可利用执行时才知道的独立性，适合部分不规则程序 | 握手、缓冲、仲裁、内存消歧与死锁处理成本 |
| Hybrid scheduling | 在不同区域、粒度或依赖上组合两者 | 仅为真正有用的动态性付费 | 分区、边界协议和成本模型会影响整体收益 |

上述分类描述的是**所生成硬件的调度方式**。用 GNN、RL 或 LLM 在编译时搜索调度，生成的仍可能是纯静态硬件；有 FIFO 和 DATAFLOW 也不能直接等同于逐运算动态调度。基本分类与代价可参照 [DASS 正式论文](https://doi.org/10.1109/TCAD.2021.3065902)。

另外，operation scheduling、loop transformation、task scheduling、resource binding 是相互作用但不同的问题。下面明确标出层级，避免把一个较完整的编译流程改善误认为单一 scheduler 的改善。

## 2. 近三年值得优先读的七篇

选取依据是问题相关性、PLDI／ASPLOS／FPGA／ICCAD 等主要研究社群的正式发表，以及可核验的方法或实验材料。这是**代表性阅读清单，不是引用量排名或系统性综述**；较新的 2026 工作尤其不能断言已有长期影响力。

| 论文、年份与状态 | 调度类型／层级 | 方法与价值 | 阅读时应保留的界线 |
|---|---|---|---|
| **[Hyperblock Scheduling for Verified High-Level Synthesis](https://yannherklotz.com/papers/pldi24_hsvhls.pdf)**；Yann Herklotz、John Wickerson；**PLDI 2024 / PACMPL 8(PLDI), Article 225**，已正式发表；[DOI](https://doi.org/10.1145/3656455) | Static；跨基本区块的运算调度 | 对带 predicate 的 hyperblock 调度并支持 operation chaining；以 Coq 验证的 validator 检查每份调度。PolyBench/C 上，相对原 Vericert，作者报告硬件平均加速 2.1×。 | 重点是让强化 scheduler 仍具正确性保证；不能把对原 Vericert 的提升理解成普遍胜过商用 HLS。对 agent 自动修改 scheduler 特别有方法论价值。 |
| **[Stream-HLS: Towards Automatic Dataflow Acceleration](https://arxiv.org/html/2501.09118v1)**；Suhail Basalama、Jason Cong；**FPGA 2025**，已正式发表；[DOI](https://doi.org/10.1145/3706628.3708878) | 编译时全局优化；多 kernel 的 loop／task 层级 | MLIR 框架与解析模型，使用 MINLP 协同考虑循环顺序、节点流水、图层流水和并行度，生成含 FIFO 的 dataflow 架构。说明单节点最优不代表整个应用最优。 | 不能只因有流式传输便归类为 Dynamatic 式动态调度；它主要是上层架构／调度搜索，并非替换任意 operation scheduler 的通用算法。 |
| **[R-HLS: An IR for Dynamic High-Level Synthesis and Memory Disambiguation based on Regions and State Edges](https://www.sjalander.com/research/pdf/sjalander-iccad2024-rhls.pdf)**；David Metz 等；**ICCAD 2024**，已正式发表；[DOI](https://doi.org/10.1145/3676536.3676671) | Dynamic；IR 与内存依赖表达 | 以 RVSDG 的 regions／state edges 表达全局数据流，并分散处理内存消歧，减少控制流 IR 重建跨区块并行性的负担。 | 这是 IR 与硬件生成方法的共同改变；报告的改善不能全部归因于一个调度优先级公式。适合思考“搜索空间是否被 IR 限制”。 |
| **[Suppressing Spurious Dynamism of Dataflow Circuits via Latency and Occupancy Balancing](https://dynamo.ethz.ch/wp-content/uploads/sites/22/2024/04/Xu_FPGA24_SuppressingSpuriousDynamism.pdf)**；Jiahui Xu、Lana Josipović；**FPGA 2024**，已正式发表；[DOI](https://doi.org/10.1145/3626202.3637570) | Dynamic 电路的静态化／简化；控制与缓冲层级 | 用线性规划平衡 latency 与 token occupancy，减少虚假停顿，进而暴露可移除握手逻辑的区域。它直接挑战“动态电路一定需要到处握手”的直觉。 | 与 DASS 的整块 static islands 路径不同；不能把所有动态控制都移除。收益取决于电路是否具有可利用的规律性与选定的面积／吞吐目标。 |
| **[CRUSH: A Credit-Based Approach for Functional Unit Sharing in Dynamically Scheduled HLS](https://dynamo.ethz.ch/wp-content/uploads/2025/06/Xu_ASPLOS25_CRUSH.pdf)**；Jiahui Xu、Lana Josipović；**ASPLOS 2025, Vol. 1, pp. 249–263**，已正式发表；[DOI](https://doi.org/10.1145/3669940.3707273) | Dynamic；资源共享与执行控制 | 用 credit 解耦共享运算器的资源依赖，避免共享引入的死锁，并保留乱序存取共享单元的机会。相对 prior in-order sharing，作者报告平均 DSP 减少 12%、FF 减少 15%。 | 这不意味“任意共享都不会死锁”；其保证有电路与协议假设。共享 wrapper 自身也有成本，应同时看执行时间与资源。 |
| **[Dynamic Loop Fusion in High-Level Synthesis](https://arxiv.org/html/2501.14631v1)**；Robert Szafarczyk、Syed Waqar Nabi、Wim Vanderbauwhede；**FPGA 2025, pp. 211–222**，已正式发表；[DOI](https://doi.org/10.1145/3706628.3708871) | 编译器／硬件协同；静态分析加动态内存消歧 | 利用地址单调性与 program-order schedule，让有不确定内存依赖的 sibling loops 重叠执行。证明研究空间不只在单个 loop 内的 II。 | 关键条件是内层循环地址单调不递减；不是任意 irregular loops 的通解。作者的 14×／4× 平均加速分别对其 static／dynamic baselines，不能直接外推至 Light-HLS。 |
| **[Out with LSQs: Custom Circuits for Memory Access Reordering in Dynamic HLS](https://www.epfl.ch/labs/lap/wp-content/uploads/2026/03/PirayadiFeb26-Out-with-LSQs-Custom-Circuits-for-Memory-Access-Reordering-in-Dynamic-HLS-FPGA26.pdf)**；Rouzbeh Pirayadi、Ayatallah Elakhras、Mirjana Stojilović、Paolo Ienne；**FPGA 2026, pp. 92–102**，已正式发表；[DOI](https://doi.org/10.1145/3748173.3779204) | Dynamic；依赖专用的内存执行控制 | 依静态分析后真正剩余的依赖生成定制控制电路，取代过于通用的 load-store queue。相对优化过的 LSQ，作者报告相近执行时间下平均资源减少 37%。 | 是内存消歧机制的改进，并不自动解决所有调度问题。地址追踪窗口、条件执行及前进性仍需处理；2026 工作的后续影响尚待观察。 |

**第八篇，直接对应 hybrid 的补读：** Yuhan She、Yanlong Huang、Jierui Liu、Ray C. C. Cheung、Hong Yan，**[A Source-to-Source Compiler to Enable Hybrid Scheduling for High-Level Synthesis](https://www.mdpi.com/2079-9292/14/23/4578)**，*Electronics* 14(23):4578，2025-11-22，已正式发表，[DOI](https://doi.org/10.3390/electronics14234578)。它将动态性暴露于 source 层，再使用现有 static HLS 工具，对“保留成熟后端，透过前端变换实现混合执行”很有参考价值。将其列为相关补充，不与前七篇的社群代表性等量看待；现有检索不足以支持其已具同等影响力。

## 3. 必须知道、但不应冒充近三年的奠基工作

1. Jianyi Cheng、Lana Josipović、George A. Constantinides、Paolo Ienne、John Wickerson，**[Combining Dynamic & Static Scheduling in High-level Synthesis](https://jianyicheng.github.io/assets/pdf/ChengFPGA20.pdf)**，FPGA **2020**；扩展版 **[DASS](https://doi.org/10.1109/TCAD.2021.3065902)** online **2021**、TCAD 卷期 **2022**。在动态外围中放入 static islands；因此“混合”本身早已不是新问题。
2. Robert Szafarczyk、Syed Waqar Nabi、Wim Vanderbauwhede，**[Compiler Discovered Dynamic Scheduling of Irregular Code in High-Level Synthesis](https://eprints.gla.ac.uk/303044/)**，FPL **2023**，[DOI](https://doi.org/10.1109/FPL60245.2023.00009)。反过来从 static HLS 出发，选择性引入动态基本区块、循环和内存操作；对 Light-HLS 的现有架构比全面动态化更接近。虽然学校页面在 2026 更新，它仍是 2023 论文。

## 4. 是否存在“更好的 scheduling algorithm”？

**在明确的目标与工作负载下可以更好；目前证据不支持一个同时全面支配 static、dynamic 和 hybrid 的单一方法。** 单纯减少 cycle count 可能拉长 critical path；增加并行性可能增加 DSP、RAM、仲裁与 routing 负担；对 profile 的期望收益也可能在数据分布改变时消失。上述论文的 baseline、平台、内存系统及优化范围并不一致，不能把各自 speedup 排成排行榜。

综合前述工作，值得研究的决策空间是：

> 对每一个区域／依赖，决定多少顺序可在编译时固定、哪些条件必须在执行时判定，并一并决定资源共享、缓冲、bank 与物理时序代价。

这是对文献的综合推论，并非已证实的新算法。可分成三条路：

若进一步考虑“执行阶段改变时换调度／配置”，也已有直接相关工作：Mathias Bouilloud、Lana Josipović、Wayne Luk 的 **[Resource and Phase Awareness for Dynamically Scheduled High-Level Synthesis](https://dynamo.ethz.ch/wp-content/uploads/2025/06/Bouilloud_HEART25_ResourceAndPhaseAwareness.pdf)**，**HEART 2025, pp. 14–24**，[DOI](https://doi.org/10.1145/3728179.3728194)。它对 buffer 优化加入资源上限，并为不同 phases 生成配置、探讨执行时重配置。其 phase 侦测与频率信息是给定假设；因此“phase-aware”这个方向本身也不能当作新颖性。

| 路径 | 可检验问题 | 已有工作的重叠与风险 |
|---|---|---|
| **A. 改善静态 heuristic，联合考虑 binding／storage／banks** | 对相同合法操作集合，更好的 ready-list 优先级、模调度顺序或有限回溯，能否在保留 II 的同时减少暂存、mux 与实际执行时间？ | 属于成熟研究方向；需提出可重复的决策规则与跨程序证据。只换一个权重或只调 pragma，贡献可能不足。 |
| **B. 静态主干＋依赖专用的局部动态控制** | 能否只对无法静态消除且确实限制吞吐的依赖，生成小型执行时检查，而不让整个 datapath 都握手化？ | 与 DASS、FPL23、FPGA26 LSQ 替代机制高度接近；必须比对分区粒度、生成方法、成本模型与正确性条件，不能直接宣称新。 |
| **C. 以 profile 选择有保护的特化调度** | 对常见分支／无 alias 情况使用快路径，条件不成立时安全切回保守调度，能否抵销 guard 与重复控制成本？ | Guard、speculation、profile-guided 优化都是既有概念。新意需来自可证明的切换协议、特定可利用结构或更好的搜索策略；profile 不得用来删除真依赖。 |

我建议 **先 A，再以 B 为中期方向**。A 可以较快回答“agent 是否真的找到泛化的调度规则”；B 要求新增协议与后端能力，若一开始同时改 scheduler、IR 和 RTL，较难判定收益来源。C 需要足够真实的输入分布，暂不作第一个实验。

## 5. 对目前 Light-HLS 的具体落点

本节根据当前本地程序与设计文件，不代表已实现下列研究提案。

- [后端设计说明](../logs/RTL_BACKEND_DESIGN.md)描述普通路径为基本区块内 ASAP 调度，跨区块值注册；[HI_OperationScheduling.cc](../../Light_HLS/Implementations/HI_OperationScheduling/HI_OperationScheduling.cc)还包含单区块 loop pipeline、依赖／内存限制及模端口保留。这是一个足以开始做静态 scheduler 对照的基础。
- [HI_ResourceBinding](../../Light_HLS/Implementations/HI_ResourceBinding/HI_ResourceBinding.cc)、[HI_BackendDependenceAnalysis](../../Light_HLS/Implementations/HI_BackendDependenceAnalysis/HI_BackendDependenceAnalysis.cc)与[HI_BackendValidation](../../Light_HLS/Implementations/HI_BackendValidation/HI_BackendValidation.cc)提供可分离的决策与检查位置。新增候选策略时，先维持同一套依赖、接口与合法性契约，让差异可归因于调度决策。
- 现有 DATAFLOW／FIFO 后端可提供任务层级实验材料，但在未检查其逐操作执行协议之前，不应把它当作已具备完整 Dynamatic 类 dynamic scheduling 的证据。

**第一个可被否证的研究假说：** 在保持现有 RTL 语义与支持范围的条件下，agent 产生的调度 heuristic 能利用 recurrence criticality、bank 冲突和 value lifetime，对未参与搜索的程序改善“实际执行时间—资源”Pareto 前沿，而不只改善估计 latency。

最小实验约定如下：

1. **冻结问题。** 先选普通静态区块或现有单区块 pipeline 其中一类；固定 LLVM 优化流程、board、时钟、运算器 latency、接口与存储配置。以现有 scheduler、常见 critical-path／mobility list scheduling 和小图上的精确求解器作对照。
2. **隔离搜索与评估。** 按程序家族切分 train／validation／test，保留不同 trip count、bank／port 数、分支条件与输入分布；用相同评估次数和时间预算比较 agent、随机搜索及人工 heuristic。
3. **先检查合法性再计分。** 检查 SSA／内存／跨迭代依赖、端口与资源冲突、可观察副作用顺序；做 C／IR／RTL 差分测试及适用范围内的形式化验证。若加入动态控制，另需检查前进性与死锁，单次模拟通过不够。
4. **保留可实测结果。** 同时记录 achieved II、transaction cycle count、综合／实现时序和 LUT／FF／DSP／BRAM；以明确接口事件定义开始与完成。执行时间应以实际工作频率计算，不能用目标频率替代未达成的时序。
5. **设置失败判准。** 如果收益只存在搜索用 benchmarks、只改善估计器，或等预算非 agent 搜索也有同样收益，就不能声称发现更好的通用 scheduling algorithm。留下失败例与消融结果，它们能指向下一轮需要改变的规则。

## 6. 明天 review 时建议先作的三个判断

1. **研究对象：** 第一阶段是否只做现有静态／模调度 heuristic，而把新增动态硬件留到下一阶段？
2. **成功标准：** 主要追求固定资源约束下的实际执行时间，还是固定吞吐下的资源减少？用明确约束及 Pareto 前沿，比任意加权总分更容易解释。
3. **论文贡献：** 要产出“agent 辅助搜索流程”，还是“可独立执行、可说明且跨程序有效的新调度规则”？后者对用户最初提出的算法研究问题更直接。

## 7. 检索与证据范围

采用 academic-research-suite 的 literature-review 路径，依序做候选检索、出版信息核对及跨工作综合；不是 PRISMA 系统性综述。查询包括 `HLS static dynamic hybrid scheduling 2024 2025 2026`、`Dynamatic functional unit sharing`、`dynamic loop fusion`、`HLS memory disambiguation` 以及各篇精确题名；再追查作者／实验室正式页面、出版社纪录与论文全文。纳入 2024–2026 正式研究论文，早期工作另列；排除视频流的同名 HLS、FPGA 云端作业调度，以及仅做 pragma 推荐却无调度方法关系的工作。

本清单在 dynamic 路径上较集中于 ETH／EPFL 的 Dynamatic 研究脉络，已补入 Glasgow、UCLA 与 R-HLS 路线，但仍有学派与工具偏向。数值均是作者在各自实验设定中的报告，本文未重现外部论文实验；新颖性只是本次有限检索下的研究判断。部分 ACM DOI 直接开启受存取限制，已以作者公开全文或大学典藏交叉核对，不将存取限制当作文献不存在。2026 的补充工作与后续引用仍值得在正式立题前再检索一次。
