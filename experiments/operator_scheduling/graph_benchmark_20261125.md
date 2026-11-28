# 收集图集上的调度复测与上下界

2026 年 11 月 25 日。在固定来源的两组输入上分别执行原问题定义的检验。HeuriGym 给定功能单元数量，优化完成周期。DiffSched 给定阶段数，允许依赖边两端处在同一阶段，优化峰值资源和跨阶段通信。[HeuriGym](https://arxiv.org/abs/2506.07972) · [Differentiable Combinatorial Scheduling at Scale](https://proceedings.mlr.press/v235/liu24al.html)

## HeuriGym 的 24 张图

| 策略 | 通过官方检查 | 达到作者 ILP 参考 | 相对手写基线的周期几何平均比 | 24 图合计求解秒数 |
| --- | ---: | ---: | ---: | ---: |
| 手写关键路径列表调度 | 24/24 | 13/24 | 1.0000 | 0.92 |
| 既有独立生成第 2 份规则 | 24/24 | 24/24 | 0.9711 | 10.91 |
| 既有反馈轨迹第 1 份规则 | 24/24 | 24/24 | 0.9711 | 10.65 |

这两份 Agent 程序均在 11 张图上减少了周期，其余 13 张打平。只看上游 19 张 eval 图，几何平均周期比为 0.9636。24 张包含上一轮用过的 5 张 demo、12 张验证和 7 张预留。本轮重新执行了全部 72 个排程，逐一通过严格解析、作者 verifier 和作者 evaluator。同一逻辑 CPU 串行执行，每份候选每图最多 10 秒，计时含隔离进程启动。本轮未对这些已知输入重新生成 Agent 算法。[求解器及独立核查入口](graph_benchmark_heurigym.py) · [冻结候选来源](round1_20261120/README.md)

最优性另外检查。4 个参考值达到关键路径或同类资源负载下界。对剩余图设定时限 H = 作者参考 − 1，并由依赖计算每项操作最早和最晚开始周期。13 个图在某个时间窗内必须占用的同类功能单元周期已经超过容量。6 个图由精确布尔模型交给 Z3 4.12.3，返回 UNSAT。对这 **23/24 张图**，已有合法排程与不可更优的独立证据。在 `invert` 上，两份规则均得到与作者相同的 23 周期，Z3 的 45 秒和固定区间 CP-SAT 的 90 秒复核均未裁定 22 周期可行性。额外采用布尔时间索引和区间约束的组合模型，给定已知解提示和 130 秒，再次返回 UNKNOWN。分别保留“达到作者参考”和“证明达到下界”的判断。[区间 CP 复核](graph_benchmark_exact.py) · [上游 ILP 参考](../../benchmarks/hls_scheduling_2024_2026/sources/heurigym_tools/operator_scheduling/baseline/baseline.json)

## DiffSched 的 18 张随机图

论文 §4 对合成图设置 10 个阶段、权重 R=10。节点 i 的阶段 sᵢ 在 0 至 9 内，边 u→v 须满足 sᵤ ≤ sᵥ。同阶段无通信成本。每个阶段累加所属节点的原始内存权重，每条原始正权重边按作者代码转换为 1。目标为 **10 × 最大阶段内存 + 各条边跨越的阶段边界数之和**。用自行实现的整数评分器计算总目标，并实际从上游生成器构造原版 `legal_check`，逐一核查合法性及跨阶段通信成本。论文 Table 1 的 RW1–RW12 的节点数和深度与 12 个归档输入吻合。另有 6 个较大的 dense 随机图也保留在本次归档扫描。上游没有提供论文使用的 EPFL 文件。[上游模型](../../benchmarks/hls_scheduling_2024_2026/sources/diffsched_icml24/parse_graph_w_attr.py)

预先在每个“密度 × 名义规模”组合中取编号 1 开发，共 6 张。编号 2 和 3 共 12 张预留，同一图分布和规模可跨集合。先按拓扑顺序把累计内存权重切成近似均衡的连续段，得到全部合法的手写起始排程。两次 `gpt-6.1-sol`、`medium`、`--ephemeral` 模型调用生成两个完整的阶段分配改进函数，第二次收到第一版代码和六个开发结果。候选在单 CPU、无网络的 bwrap 隔离进程内运行，每图 10 秒墙钟。按开发覆盖数、相对初始目标的几何平均比、提交顺序选拔，记录代码哈希后再运行预留图。模型仅看开发图汇总。本任务改变了候选接口，属于阶段分配赛道的扩展。与 Light-HLS 的单端口列表优先级接口分别评测。[实验入口](graph_benchmark_diffsched.py)

第一版在保持依赖关系的前提下尝试移动一个节点或交换两个节点，增量计算内存峰值与通信成本。第二版按阶段维护依权重排序的节点，优先尝试权重相近的可交换节点，改善内存平衡的同时缩短跨阶段距离。它在 6 张开发图上逐图优于第一版，获选代码 SHA-256 为 `a42194f956473544594793c9988b383b72ecb2a8a096473c3f9b87d9c3f745ec`。

另以 OR-Tools 9.14.6206 实测数学等价的紧凑 CP-SAT 模型。同一初始排程作为 hint 和可行上界，单图 1 个搜索 worker、10 秒求解时间，最多同时执行 3 图。模型搭建耗时另记。目标统计预留合法解。若求解器未返回新可行解，保留已验证的初始排程。与 Agent 的进程墙钟上限口径略有不同。作者在 Xeon Gold 6418H 和 RTX 4090 上给各方法 3600 秒并报告相对各自起点的归一化曲线，本机是 i9-13900，缺少 NVIDIA CUDA 设备。当前短时 CP-SAT 对照不提供对论文 GPU 方法的胜负判断。[紧凑 CP 模型](graph_benchmark_stage_cp.py) · [论文实验条件](https://proceedings.mlr.press/v235/liu24al.html)

### 实测成绩

| 口径 | 分段基线 | 第一版 Agent | 第二版 Agent |
| --- | ---: | ---: | ---: |
| 6 个开发副本，目标／分段基线的几何平均 | 1.00000 | 0.99116 | 0.99094 |
| 12 个预留副本，目标／分段基线的几何平均 | 1.00000 | 未进入预留评测 | 0.99157 |
| 12 个预留副本，相对本机限时 CP-SAT 合法结果 | 见逐图记录 | 未进入预留评测 | 0.99237 |

冻结的第二版在 12/12 个预留副本上严格优于起始排程，几何平均目标下降 **0.843%**。相对相同输入上的本机短时 CP-SAT 对照，12/12 提升，几何平均目标下降 **0.763%**。其中 CP-SAT 在 6 个副本返回 FEASIBLE，在另 6 个副本返回 UNKNOWN，后者使用共同的起始排程。Agent 候选求解墙钟合计 104.52 秒，训练期两次模型调用另计。单看绝对优化空间，合法下界为 `10 × max(最大节点权重, ceil(节点权重总和 / 10))`，它利用通信非负性与内存峰值约束。第二版和此下界的几何平均比为 1.001459，各图的下界差距在 **0.058% 至 0.249%**。剩余空间受下界约束，但最优值尚未确定。[逐图目标、阶段分配、CP 状态与模型实录摘要](graph_benchmark_diffsched_20261125.json) · [HeuriGym 逐图结果与下界](graph_benchmark_heurigym_20261125.json)

## 复现

原始逐图排程、Z3 输入、模型提示、模型回复、逐图阶段分配和调用记录位于被忽略的 `runs/graph_benchmark_20261125_*`。使用新的输出目录运行。NetworkX 3.1 供上游 GML 与合法性代码使用，Z3 负责 HeuriGym 的较小精确布尔模型，额外 CP 比较使用 OR-Tools 9.14.6206。调度候选需支持 bwrap 隔离。

```bash
python3 experiments/operator_scheduling/graph_benchmark_heurigym.py --output runs/new_heurigym
python3 experiments/operator_scheduling/graph_benchmark_exact.py runs/new_heurigym
python3 experiments/operator_scheduling/graph_benchmark_diffsched.py --output runs/new_diffsched
python3 experiments/operator_scheduling/graph_benchmark_stage_cp.py runs/new_diffsched
```

若只重跑已冻结的候选，在 DiffSched 命令加上 `--replay experiments/operator_scheduling/graph_benchmark_diffsched_20261125.json` 可跳过模型调用。既有候选含限时搜索，机器负载可影响完成的搜索步数。归档中保留原次运行的实测值和选定的阶段分配，供独立核对。
