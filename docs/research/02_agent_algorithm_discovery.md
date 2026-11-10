# 用 AI agent 反复改进传统算法：三篇优先阅读论文与 Light-HLS 实验建议

检索截止：2026-11-10（Asia/Hong_Kong）。供 2026-11-11 review。本文是有选择的文献简报，不是系统综述；“优先”依据代表性、原始实验证据以及与调度启发式发现的相关性，没有把引用数作为排名。

**判断：这个方向有很强的先例。真正值得复制的是“模型提出代码 → 执行器评价 → 保留并组合有效候选 → 带着失败证据继续搜索”，而不是只在同一段对话中不断说“再好一点”。** 最建议读 AlphaEvolve、FunSearch、ReEvo 三篇：分别回答工程规模闭环、最小可行搜索框架、如何利用反思提高有限评估预算的效率。

## 1. 三篇论文怎么选

| 优先阅读 | 论文及已核验发表信息 | 为什么与本项目相关 | 应借鉴的部件 |
|---|---|---|---|
| 1 | Novikov et al., **AlphaEvolve: A coding agent for scientific and algorithmic discovery**，2025；作者标注为 white paper，arXiv:2506.13131 | 确实有部署中的调度启发式案例，并支持复杂代码和昂贵评估 | 分级评估、多指标、程序数据库、演化式代码修改 |
| 2 | Romera-Paredes et al., **Mathematical discoveries from program search with large language models**，*Nature* 625, 468–475 (2024)；在线首次发表 2023-12-14 | 把大模型变成小型启发式的候选生成器，容易映射到调度优先级函数 | 固定正确性框架、局部函数搜索、多个种群保留多样性 |
| 3 | Ye et al., **ReEvo: Large Language Models as Hyper-Heuristics with Reflective Evolution**，NeurIPS 2024 主会 | 将传统组合优化器中的启发式替换为演化得到的代码，适合昂贵的 HLS 评价 | 成对比较、短期反思、长期经验、交叉与精英变异 |

发表信息来源：[AlphaEvolve 原文](https://arxiv.org/abs/2506.13131)、[FunSearch 出版社页面及 DOI](https://www.nature.com/articles/s41586-023-06924-6)、[ReEvo 正式会议页面](https://papers.nips.cc/paper_files/paper/2024/hash/4ced59d480e07d290b6f29fc8798f195-Abstract-Conference.html)。AlphaEvolve 在本文中按可核验的技术报告身份引用；不把它标成已确认的同行评审会议论文。

## 2. 每篇读什么，以及证据到哪里为止

### 2.1 AlphaEvolve：与你的设想最接近

**论文证据。** 用户提供初始程序、允许演化的代码及评价器；系统从程序库采样上下文，让 Gemini 2.0 Flash/Pro 生成修改，执行后按结果保留候选。它支持逐级淘汰、多指标和并行评价。最贴近本项目的是 Google Borg 数据中心调度：作者报告发现的启发式部署后，平均回收原本闲置的资源，规模约为 **Google 全球集群总算力的 0.7%**。[方法与结果原文 §2、§3.3.1](https://arxiv.org/html/2506.13131v1)、[作者公布的调度应用结果](https://deepmind.google/blog/alphaevolve-a-gemini-powered-coding-agent-for-designing-advanced-algorithms/)

**边界。** 这是数据中心资源放置，不是 HLS 操作调度；0.7% 是作者报告的该场景结果，不能作为 Light-HLS 的预期收益。我们没有独立重做 Google 的生产部署实验。

**对 Light-HLS 的推断。** 最有价值的借鉴是把昂贵的 RTL/物理测量放在后级，用便宜的合法性与仿真筛选候选；让 agent 看到逐 benchmark 的失败和收益，而不只看到一个总分。能否迁移仍需实证。

### 2.2 FunSearch：最适合做第一版原型

**论文证据。** FunSearch 使用基于 PaLM 2 的 Codey 生成程序，由确定性评价器打分，并通过种群式搜索维持多样性。论文既有 cap set 新构造，也有在线 bin packing 启发式优于 first-fit/best-fit 的实验；后者与资源调度更接近。关键是搜索产生解的程序，而不只是某一个实例的解。[论文及方法](https://www.nature.com/articles/s41586-023-06924-6)

**边界。** 在所测实例分布上更好，不能推出对所有输入或最坏情况更好。官方仓库给出发现的启发式和演化实现，但没有提供原实验的语言模型、执行沙箱和分布式基础设施。[官方复现资源与范围](https://github.com/google-deepmind/funsearch)

**对 Light-HLS 的推断。** 第一轮可以只让 agent 改“合法 ready operations 的优先级函数”，保留依赖、端口、递归和协议检查器。这样更容易判断收益来自调度规则，而不是误删约束或偶然改变前端代码。

### 2.3 ReEvo：把“再想想”变成有结构的反思

**论文证据。** ReEvo 让生成器产生启发式代码；反思器比较两个候选的实际表现，形成短期建议，再累积为长期反思，用于交叉和精英变异。论文覆盖六种组合优化问题与五类算法，包括电子设计中的 decap placement。主要配置使用 GPT-3.5 Turbo，并比较了其他模型。[原文 §4–6 与附录 C](https://arxiv.org/html/2402.01145v3)

**边界。** 作者明确指出实验限制在 **100 次启发式评价**，效果不一定随预算扩大而保持。反思是引导搜索的文本，不是可证明正确的梯度；decap placement 也不是 RTL scheduling。[正式论文](https://proceedings.neurips.cc/paper_files/paper/2024/file/4ced59d480e07d290b6f29fc8798f195-Paper-Conference.pdf)

**对 Light-HLS 的推断。** 给反思器一对候选的逐例差异，例如“II 降低但 FF 增加”“RAM 冲突减少但组合路径变长”，让它给出下一轮可检验的假设。只有后续执行结果可以决定该建议是否有效。

若扩展阅读，只补一篇 **Evolution of Heuristics (EoH)**，ICML 2024：它共同演化自然语言思路和可执行代码，与 ReEvo 是很合适的同预算对照。前三篇已足够支撑本轮讨论。[正式论文](https://proceedings.mlr.press/v235/liu24bs.html)

## 3. 对“不断 push Codex，直到得到更好算法”的具体判断

下面是本项目的研究建议，不是上述论文已经验证的 HLS 结论。

把目标写成：**在相同正确性与硬件契约下，发现能够跨 kernel 泛化、带来实际 QoR 改善的可解释调度策略。** 要区分三种产物：调一个权重是在调参；写出跨实例更好的优先级规则是在发现启发式；改变合法调度空间、并提出可复现机制，才可能支撑更强的 scheduling algorithm 贡献。不能只凭收益大小命名“新算法”。

“持续迭代”不能保证突破。若评价器只奖励一个近似 latency，agent 很可能学会利用估计器偏差；若始终只看一个 benchmark，找到的可能是特化补丁；若不保留其他候选，当前最好分支也可能让搜索过早收敛。

### 3.1 从现有代码开始，先缩小搜索面

当前 [HI_OperationScheduling.cc](../../Light_HLS/Implementations/HI_OperationScheduling/HI_OperationScheduling.cc) 包含普通块和流水循环的排程逻辑；[HI_LoopPipelineAnalysis](../../Light_HLS/Implementations/HI_LoopPipelineAnalysis) 与 [HI_ResourceBinding](../../Light_HLS/Implementations/HI_ResourceBinding) 分别承担其他相关职责。不能把现有 DATAFLOW/FIFO 支持直接认作已经具备任意动态操作调度硬件。

建议先把“从合法 ready set 选哪个操作”抽成受限接口，特征可包括关键路径剩余长度、slack、扇出、bank/port 压力、递归边和中间值寿命。agent 可生成该接口的实现；合法性判定、评价代码和隐藏测试保持固定。若当前 scheduler 尚无这样的接口，抽取它应作为独立工程任务，不能假装已可直接替换。

第二阶段再探索 modulo scheduling 的候选顺序、局部回退/修复策略及 II 搜索；更大范围的 static/dynamic region 划分需要控制器、握手和死锁验证支撑，应单独立项。第一份[调度文献简报](01_hls_scheduling_2024_2026.md)讨论了相应方向。

### 3.2 一个可执行的闭环设计

```text
冻结 baseline、benchmark 划分、评价器、时钟和资源约束
                       ↓
agent 从历史候选中提出“假设 + 调度代码修改”
                       ↓
编译 → 调度合法性 → C/IR/RTL 差分及协议测试
                       ↓
通过者进入实测 QoR：仿真周期/吞吐 → 综合 → 必要的布局布线
                       ↓
保留 Pareto 候选、记录反例与代码哈希 → 成对反思 → 下一代
```

本仓库已有可用起点：[统一回归入口](../../Light_HLS/Tests/run_regressions.py)、[公平比较说明](../../Light_HLS/Tests/FairComparison/README.md)、[评价契约测试](../../Light_HLS/Tests/FairComparison/test_evaluation_contract.py)。这些资产减少搭建成本；仍需要另行实现候选隔离、搜索编排和数据集管理。

| 层级 | 检查内容 | 是否能够宣称“更好” |
|---|---|---|
| 快速筛选 | 可编译；依赖、资源、bank、loop-carried constraints 合法 | 只能说明进入后续验证 |
| 功能筛选 | C/IR/RTL 差分；连续事务、stall、reset、最终写回；随机与边界输入 | 在测试覆盖内正确；不自动等于形式等价 |
| 性能筛选 | 实测周期和持续吞吐，固定资源与频率要求 | 只能说明该运行契约下的局部收益 |
| 最终验证 | 同一物理流程，综合/布局布线；setup、hold、约束覆盖；留出集与重复运行 | 可支撑有边界的 QoR 改善结论 |

功能失败直接淘汰，不能让更低 latency 抵消错误。固定时钟下用最终提交间隔衡量持续吞吐，单次 latency 另报；LUT、FF、DSP、BRAM、编译成本同时保留。若每个候选频率不同，需统一到实际时间/吞吐后再比较。估计值、综合值与布线后值分列，不能混成同一个 reward。

### 3.3 防止看起来改进、实际不可用

- **按算法家族划分开发集、验证集和最终隐藏测试集。** 同一 kernel 的改名、尺寸变化和编译变体不要跨集合泄漏。开发集允许反馈；最终测试集不参与候选选择。
- **冻结评价契约。** 保持源程序、接口、memory latency、时钟、part、工具版本及 Tcl/XDC 一致；不允许候选通过修改计分、放松约束或删测试获胜。
- **保留公平基线。** 至少比较现有 scheduler、固定人工启发式、相同评估预算的随机/演化搜索，以及无反思 agent。小规模实例用 ILP/约束求解给出最优值或下界，判断还有多少调度空间。
- **计算全部成本。** 报告有效与无效候选数、LLM tokens、编译与 EDA 运行时、失败率和总体搜索时间；比较使用相同候选评价次数及实际资源预算。只报获胜候选隐藏了搜索代价。
- **重复并审查机制。** 使用多个独立搜索种子与物理实现种子；逐 kernel 报告退化和失败，用消融检验“收益来自哪个规则”。审查新规则是否只是识别固定 benchmark。
- **对泛化主张设边界。** 若只改善某类 memory-bound kernel，就按该类报告；若要宣称超越 static/dynamic/hybrid 的新机制，必须解释新增能力及其正确性条件，不能仅换名字。

### 3.4 建议的第一轮停止条件

作为可供明天修改的起始方案：固定 **100 个候选评价名额、至多 10 次完整物理验证、24 小时墙钟预算**，三者任一耗尽则结束该轮；这只是实验预算建议，不是本次已经启动的任务。下一轮是否扩大取决于收益曲线和失败原因。

候选进入最终审查的门槛可设为：所有必需正确性与时序检查通过；留出验证集在预先指定主指标上有稳定收益；最差退化与资源上限在事前确定范围内。**性能阈值要在观察搜索结果前确定**，并超过工具/种子噪声。没有达到门槛也保存失败分布和 Pareto 前沿，不能用“再跑久一点”替代结果。

## 4. 明天最值得 review 的三个决定

1. 首轮只搜索静态/流水调度优先级，还是愿意同时承担动态 region 的硬件支持成本？建议从前者开始。
2. 首要目标是固定资源下吞吐、单次 latency，还是面积—性能 Pareto 前沿？先选一个主目标，其余作为约束和辅助指标。
3. 哪些 kernel 家族用于开发，哪些只用于最终测试？在启动自动搜索之前冻结划分与评价脚本。

**证据状态：** 本文已通过论文原文、出版社/会议页面和作者项目核验文献身份及上述核心机制；没有在本地复现这些论文的完整搜索，也没有在本次迁移工作中运行 agent 调度发现实验。文中的 HLS 闭环、预算和研究判断均是待验证建议。
