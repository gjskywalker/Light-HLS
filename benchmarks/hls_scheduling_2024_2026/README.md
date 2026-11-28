# HLS Scheduling Benchmark 收集说明：2024–2026

收集版本：**v0.1**。检索截止与整理日期：**2026-11-19，Asia/Hong_Kong**。

本集合服务于真实 HLS 场景下的调度算法开发：从近期论文追溯作者实际使用的计算程序、输入驱动和约束，保留原始材料，为后续 Light-HLS 适配和调度器对比建立基础。它目前是**有来源、有固定版本、有文件完整性检查的收集快照**，尚不是全部可以一键运行的 Light-HLS 端到端测试套件。

## 1. 收集结果与统计口径

| 项目 | 本次结果 |
| --- | ---: |
| 登记论文 | 14 篇：2024 年 5 篇、2025 年 5 篇、2026 年 4 篇 |
| 已取得源码或图材料的论文 | 11 篇；另外 3 篇记录缺失情况 |
| HLS 源码配置记录 | **115 条** |
| 辅助图输入 | **42 条**：DiffSched 18、HeuriGym 24 |
| 合计配置／输入记录 | 157 条 |
| 不同主输入文件 SHA-256 | 155 个；不等于独立算法数 |
| 收集包中的原始文件 | **512 个，35,366,354 字节，约 33.7 MiB** |
| 原生 ASan／UBSan 执行检查 | Vericert 的 27 项执行完成，无 sanitizer 诊断 |
| 收集快照建立时的验证范围 | 当时只核对输入与来源。后续的 Light-HLS 编译、RTL 功能比对、同器件布局布线和逐篇比较条件见[实验报告](../../experiments/operator_scheduling/paper_sweep_20261125.md)。 |

**115 条源码配置不能解释为 115 个独立 kernel。** 例如 CRUSH 包含同一程序的两类实现及展开配置；不同论文还会重复使用 PolyBench 程序。`duplicates.json` 记录主文件哈希重复和初步家族关联。81 个初始分组只是后续人工审查的起点，不是已经确认的独立应用数量。

本次执行定向文献检索、作者页面／仓库追踪及归档核查，覆盖静态、动态、混合和任务／数据流相关调度。它不是穷尽 2024–2026 全部论文的系统性综述，也不是按引用量排列的清单。没有因为程序暂不适配 Light-HLS 而把它从来源清单中删除。

## 2. 论文、材料和收集数量

表中的“收集”指实际取得对应文件；材料关联不代表已经复现作者工具链、变换结果或性能。

| 年份 | 论文及范围 | 本次实际取得 | 版本／映射界线 |
| --- | --- | --- | --- |
| 2024 | [Hyperblock Scheduling for Verified High-Level Synthesis](https://doi.org/10.1145/3656455)，PLDI；静态 hyperblock、chaining | **27** 个 `polybench-syn` 整数 C 程序，含初始化、checksum、`misc.h`、构建与仿真脚本 | 来自 [Zenodo 10808233](https://zenodo.org/records/10808233) 的固定源码归档；没有下载 9.7 GB VM |
| 2024 | [Subgraph Extraction-based Feedback-guided Iterative Scheduling for HLS](https://doi.org/10.23919/DATE58400.2024.10546877)，DATE；ISDC、下游反馈 | **5** 个当前公开 XLS DSLX 对应物及其递归 import 依赖 | 论文 Table I 共 17 项；5 项仅标 `broader_suite`，未恢复论文实验时的精确修订；其余 12 项逐项记录不可获得或关联待核实 |
| 2024 | [Suppressing Spurious Dynamism of Dataflow Circuits via Latency and Occupancy Balancing](https://doi.org/10.1145/3626202.3637570)，FPGA；动态电路简化 | **8** 个源码配置及头文件、实验选择脚本 | [固定归档 10440335](https://zenodo.org/records/10440335)，按实验脚本的 8 项收录，未把整个 regression suite 计入 |
| 2024 | [R-HLS: An IR for Dynamic High-Level Synthesis and Memory Disambiguation based on Regions and State Edges](https://doi.org/10.1145/3676536.3676671)，ICCAD；动态 IR、内存消歧 | **8** 个论文引用的 StoQ 基线输入 | 论文 §5/Table 1 明确程序名；文件取自其引用的 [StoQ 仓库](https://github.com/EPFL-LAP/fpga23-straight-lsq-interface)，标 `paper_cited_baseline_variant`，不等同 R-HLS 变换后的精确版本 |
| 2024 | [Differentiable Combinatorial Scheduling at Scale](https://proceedings.mlr.press/v235/liu24al.html)，ICML；图级约束优化 | **18** 个公开 GML 随机图，以及 LP／CP 模型生成脚本 | [官方仓库](https://github.com/Yu-Maryland/Differentiable_Scheduler_ICML24) 的可获得子集；README 中 `el`／`el_mapped` 仍为 TBD，未取得论文 EPFL 图输入；只放辅助轨道 |
| 2025 | [Stream-HLS: Towards Automatic Dataflow Acceleration](https://doi.org/10.1145/3706628.3708878)，FPGA；任务／数据流架构优化 | **15** 个应用配置：7 PolyBench、2 MLP、2 CNN、2 Transformer、2 synthetic；含生成的 C++、PyTorch 模型、testbench、原始输入／golden 二进制和 Tcl | [官方 FPGA2025_Artificats](https://github.com/UCLA-VAST/Stream-HLS/tree/main/FPGA2025_Artificats)；程序身份与论文集合匹配，不代表每项资源配置一致 |
| 2025 | [CRUSH: A Credit-Based Approach for Functional Unit Sharing in Dynamically Scheduled HLS](https://doi.org/10.1145/3669940.3707273)，ASPLOS；动态资源共享 | **23** 条配置：Table 2 的 11 项、Table 3 的 11 项，以及 Table 1 的展开例子 | [固定归档 14046724](https://zenodo.org/records/14046724)，原始脚本和手工处理 `gsumif` 的步骤均保留；23 条不是 23 个独立家族 |
| 2025 | [Dynamic Loop Fusion in High-Level Synthesis](https://doi.org/10.1145/3706628.3708871)，FPGA；循环融合、动态依赖控制 | **9** 个 Intel SYCL host＋kernel 程序，含原始脚本及 `include_sycl` 依赖 | 作者 [Zenodo 14383450](https://zenodo.org/records/14383450) 归档；按实际启用脚本与论文表格计 9 项 |
| 2025 | [A Source-to-Source Compiler to Enable Hybrid Scheduling for High-Level Synthesis](https://doi.org/10.3390/electronics14234578)，Electronics；PCA 混合调度 | **9** 条相关输入：引用的原始套件中 5 个同名输入，另有 4 个别名／后续上游候选 | 引用 [HLS-benchmarks v1.0](https://zenodo.org/records/3561115)；PCA 转换器、转换后版本及作者输入向量未恢复，不能算作 9 项精确论文版本 |
| 2025 | [Resource and Phase Awareness for Dynamically Scheduled High-Level Synthesis](https://doi.org/10.1145/3728179.3728194)，HEART；资源／phase-aware | **0**；登记 Comb1、Fdtd、Gemver、Comb2 | 未取得精确公开源码，保留缺失记录，不用同名程序替代 |
| 2026 | [Out with LSQs: Custom Circuits for Memory Access Reordering in Dynamic HLS](https://doi.org/10.1145/3748173.3779204)，FPGA；内存执行控制 | **11** 个 `skip/all-inputs.txt` 指定的输入，含原始头文件、`Integration.h` 和实验模板 | 作者归档 README 指向 [replace-lsq 分支](https://github.com/EPFL-LAP/dynamatic/tree/replace-lsq)，固定 Git 提交；没有下载约 759 MB 的完整归档，不能宣称与 ZIP 字节相同 |
| 2026 | [HeuriGym](https://arxiv.org/abs/2506.07972)，ICLR，预印本首发 2025；agent 图调度 | **24** 个 JSON DAG：5 demo＋19 eval，另含官方 verifier／evaluator 和专家基线说明 | [作者数据集](https://huggingface.co/datasets/heurigen/heurigen-data) 固定修订；没有完整 C／RTL 执行接口，只作辅助轨道 |
| 2026 | [RAG-Enhanced Kernel-Based Heuristic Synthesis (RKHS)](https://arxiv.org/abs/2604.26153)，预印本／workshop 报告 | **0** | 论文报告 200 个训练图、50 个验证图，代表项使用 G1–G4；未发现可复现的图文件、生成器／种子和公开 artifact，未自行生成替代图 |
| 2026 | [NeuroSchedule2.0](https://doi.org/10.1109/TCAD.2026.3661431)，TCAD；GNN／RL 调度 | **0** | 本次仅核验出版信息和摘要，未获得可核查的完整实验章节／公开 benchmark 链接；状态为 `metadata_only` |

完整证据定位、章节／表号、固定版本和缺失清单分别保存在 [papers.json](papers.json)、[artifacts.json](artifacts.json) 和 [missing_materials.json](missing_materials.json)。

AlphaEvolve-XLS 是相关工程项目，但未核验为上述三年正式调度论文，因此未计入论文或案例数量。检索中出现的 2027 年条目也未纳入。

## 3. 源码配置覆盖什么问题

| 材料组 | 主要用途 | 当前使用边界 |
| --- | --- | --- |
| Vericert：27 项整数 PolyBench | 固定尺寸循环、数组、跨基本块操作、chaining；适合建立静态后端对照 | 程序包含 `main` 和自定义算术 helper，仍需确定 Light-HLS 顶层接口；27 项 native 检查不代表 RTL 通过 |
| Dynamatic／CRUSH／Out with LSQs | 条件控制、稀疏／间接访存、浮点操作、资源共享、内存重排 | 原协议、初始化和工具参数保留；不能把原 `main` 正常返回当作独立 oracle，动态协议／背压仍需专门验证 |
| R-HLS 引用的 StoQ 输入 | 2mm、3mm、atax、covariance、getTanh、histogram、jacobi_1d、triangular | 输入关联明确，但精确 R-HLS 转换版本不明；`kernel_2mm` 另有初始化／整数溢出风险记录 |
| Stream-HLS | 多 kernel 数据流、循环／任务层优化；从原模型到生成 C++ 的对应关系 | 需要 Vitis 等外部环境；15 项原始输入／golden 文件已打包，但未重新执行 PyTorch 或 C／RTL 对比 |
| Dynamic Loop Fusion | RAW／WAR／WAW 示例、FFT、BNN、矩阵幂、PageRank、histogram、tanh＋SpMV | 完整 Intel SYCL host＋kernel，不是可直接传入 Light-HLS 的普通 C；适配需要另存补丁 |
| XLS 对应物 | 真实编译器 IR／流水级调度的补充材料 | DSLX 格式；5 个入口递归 import 源码已补齐，但没有构建 XLS，也未声称恢复 ISDC 实验版本 |
| DiffSched／HeuriGym | 快速测试调度策略、资源模型和规模行为 | 图级辅助集合，不能与端到端 HLS 的 RTL／物理结果混为一组成绩 |

静态、多级流水、动态／混合的指标应分别定义。本次不把它们压缩成一个总分，也没有统一重写所有程序的 pragma、pass sequence、时钟或资源预算。

## 4. 目录结构和清单字段

```text
hls_scheduling_2024_2026/
  README.md                    本说明
  papers.json                  论文及原文证据定位
  artifacts.json               固定提交／归档校验值、URL、许可
  cases.json / cases.csv       157 条配置／输入的逐项清单
  files.json                   原始文件路径、SHA-256、大小、用途
  duplicates.json              主文件哈希重复及初始家族分组
  missing_materials.json       文件依赖缺口与论文材料缺口
  summary.json                 机器可读统计
  sources/<artifact_id>/       按原目录结构保留的源码／驱动／配置／数据
  reports/                     原生检查、文件／图检查及依赖闭包的归档摘要
  tools/verify_collection.py
  tools/check_native_vericert.py
```

`cases.json` 中建议优先查看：

- `paper_ids`、`artifact_id`：论文与来源的对应关系。
- `source_path`、`sha256`、`bundle_files`：实际输入及已收集依赖。
- `mapping_confidence`：属于论文集合、引用基线或关联候选的哪一类。
- `harness`、`input_configuration`／`parameters`、`scheduling_configuration`：可获得的驱动、尺寸及约束。不同原始工具的字段不强行伪装成相同含义。
- `eligibility`、`known_issues`／`notes`：待审查或隔离原因。
- `native_check_status`、`light_hls_status`、`rtl_functional_status`、`paper_qor_reproduction`：互不替代的验证状态。
- `family_group`、`split_status`：后续去重与划分依据；本次不将其自动变成最终测试集。

以 `cases.json`、`files.json` 与 `reports/` 查看收录项及验证结果，以 `artifacts.json` 核对固定版本和许可。收集阶段的 `research_*.json` 与重复的 `COLLECTION_ORIGIN.json` 保留为本地记录。日常实验输出放在仓库根目录的 `runs/`。`sources/` 中登记为输入或 golden 的 `.bin` 文件是原始测试数据，随对应驱动一起保留。

## 5. 如何理解版本和映射可信度

| `mapping_confidence` | 数量 | 含义 |
| --- | ---: | --- |
| `exact` | 87 | 原文／作者归档确认属于相应程序集合；不自动保证变换后版本、参数和 QoR 全部一致 |
| `exact_names_artifact_linked_git_snapshot` | 11 | 作者指定分支和名单确认成员身份，使用固定 Git 快照；未声称与完整归档字节相同 |
| `paper_cited_baseline_variant` | 8 | 取自论文明确引用的基线输入来源，未恢复实验专用变换版本 |
| `broader_suite` | 9 | 公开上游对应物、别名或后续候选；不能用于宣称精确论文复现 |
| `exact_available_suite` | 42 | 已获得的辅助图集合，仍不属于 C→RTL 功能测试 |

需要特别保留以下差异：

1. **Stream-HLS synthetic 配置**：两项归档配置的 DSP 上限为 512，而论文 Table 10 表述为 2560。程序身份匹配，不代表配置匹配；差异已写入逐 case 的 `configuration_warning`。
2. **Dynamic Loop Fusion 数量**：论文 §7.1 文字提到 10 项，但 Table 1、§7.2 和启用脚本对应 9 项，归档 changelog 说明删除 gemver。本集合按实际 9 项记录，没有补造第十项。
3. **PCA hybrid**：取得的是引用套件及候选输入，不是作者的转换后 PCA 程序。`vecNormTrans`、`gesummv`、`covariance` 的精确论文版本等仍在缺失表中。
4. **R-HLS**：StoQ 输入的可取得性不能替代 R-HLS 自身编译流程和预处理版本的核查。
5. **DiffSched**：18 个 GML 文件不能覆盖论文中缺失的 EPFL 输入，也不应被改写成普通 operation scheduling 后冒称原问题不变。

## 6. 许可证、依赖和缺失材料

本目录是多来源集合，**不存在一个可以覆盖所有内容的统一上游许可证**。每个来源的许可文件、归档元数据、源码作者注释和来源链接均应保留；仓库根目录许可证不会取代这些条款。

- Vericert 仓库保留 GPL-3.0 及逐文件 PolyBench 来源说明；没有把 CompCert 实现复制成 benchmark。
- Stream-HLS、StoQ、DiffSched 等保留各自 MIT 许可。
- XLS／Dynamatic 保留 Apache-2.0、LLVM exception 和相应第三方 MIT／BSD notices。
- HeuriGym 图数据集为 CC-BY-4.0；验证工具代码另为 Apache-2.0。
- Dynamic Loop Fusion 的归档元数据为 CC-BY-4.0，代码与 `include_sycl` 的 MIT notice 分别保留。
- Spurious Dynamism 和 CRUSH 的许可声明来自作者归档元数据／论文说明；归档顶层没有发现完整 LICENSE 文件，未拿 vendor 子目录许可证冒充整个归档许可。
- HLS-benchmarks v1.0 的 Zenodo 元数据标 GPL-2.0-or-later，归档无 LICENSE 文件；没有用当前上游 GPL-3.0 替换旧版声明。
- 没有建立根许可的 JLM 辅助 test-suite 只登记元数据，没有作为源码 bundle 导出。

**目前明确记录的源码 header 缺口**是：CRUSH 展开示例缺少其精确 2025 版 `dynamatic/Integration.h`。2026 分支虽然有同名 header，但没有跨版本静默补入；因此该例子的依赖仍未闭合。

这不表示“总体只缺一个文件”。**整套论文材料的缺口**还包括 HEART 的精确程序、RKHS 图数据、NeuroSchedule2.0 实验说明／artifact、ISDC 未公开案例、PCA 变换结果，以及 DiffSched 的 EPFL 文件等。

DLF 的 PageRank 原始归档含硬件用 `web-NotreDame.txt`，但脚本引用的 `test2-web-NotreDame.txt` 不在归档中；图数据未作为许可已明确的普通源码文件打包，需要单独核查。本地完整下载可供进一步审查。

原始压缩包、工具仓库、论文证据及较大的历史输出保存在当前项目被忽略的 `runs/benchmark_collection_20261119/`。当前 `sources/` 是有选择的输入／依赖快照，没有复制整个工具源码、VM 或编译产物。

## 7. 本次做了哪些验证

### 7.1 文件和图输入检查

[collection_validation.json](reports/collection_validation.json) 记录：512 个原始文件的哈希和大小检查通过，157 条记录的主输入和已收集依赖均已登记；已知缺失项另列于缺失清单。42 个辅助图通过节点／边引用及无环性检查，HeuriGym 图额外检查操作延迟和资源容量为正。

这些检查**没有求解图调度，也没有验证某个调度结果**。XLS 的 5 个入口另完成递归 import 源文件闭包检查，见 [xls_dependency_closure.json](reports/xls_dependency_closure.json)。

### 7.2 原生程序检查

[native_vericert.json](reports/native_vericert.json) 记录原始 27 个 Vericert 程序在自带初始化输入上的执行，使用 ASan／UBSan、禁止 sanitizer 错误后继续运行，未出现诊断。LeakSanitizer 关闭，地址与未定义行为检查保留。另用打包后的 `2mm` 路径验证了检查脚本和相对 header 定位。

这些程序的 `main` 可能将 checksum 作为返回值，因此非零正常退出不能直接解释为测试失败。自带 checksum 也不是独立数学 oracle；本次没有据此宣布原程序在所有输入上正确或与 RTL 等价。

StoQ 的 `kernel_2mm` 有未初始化 `tmp`／随机有符号算术风险，保留原样并登记隔离原因，没有为了“跑通”而修改来源程序。

### 7.3 可复现命令

从 Light-HLS 仓库根目录检查完整性；只需要 Python 3.9+，不调用网络或上游求解器：

```bash
python3 benchmarks/hls_scheduling_2024_2026/tools/verify_collection.py \
  --output /tmp/hls_scheduling_collection_check.json
```

运行一个打包后的原生检查，要求 Clang 和 sanitizer runtime，输出目录应为空或不存在：

```bash
python3 benchmarks/hls_scheduling_2024_2026/tools/check_native_vericert.py \
  --case vericert24_2mm --output runs/benchmark_native_2mm_new
```

去掉 `--case` 可检查全部 27 个程序。脚本不启动 HLS、Vivado 或 agent 搜索；Vitis、Intel SYCL、XLS 等原始工具链仍是相应论文流程的外部依赖。

## 8. 用于调度算法研究前的准入规则

这一版没有冻结开发／验证／最终测试划分。`duplicates.json` 中的家族关系需结合算法语义人工复核，同一程序的尺寸、浮点／整数和 pragma 变体应成组处理。HeuriGym 原有 demo／eval 标记保留，不把本次查看过的材料宣称为新的盲测集合；当前 DAPO／Light-HLS 已经用过的程序家族也需要记录使用历史。

后续针对某个研究赛道固定以下内容后，才应比较调度器：合法输入域、独立参考或可审计 oracle、顶层接口、IR 预处理、pragma、时钟、操作延迟与启动间隔、功能单元数量、bank／port／访存延迟，以及动态协议／背压条件。

适配版应另存，并记录相对当前原始 SHA-256 的补丁。原始输入、参考模型和评价脚本不应被调度搜索 agent 一并修改。Light-HLS 能否编译、RTL 是否正确、目标时钟是否达成以及论文 QoR 是否复现，应继续独立报告。
