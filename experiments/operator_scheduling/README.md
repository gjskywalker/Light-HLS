# Codex 算子调度实验

[近期论文 benchmark 的端到端实验、配置与跨工具对照](paper_sweep_20261125.md) · [真实 HLS 调度的研究动机](../../docs/research/03_hls_scheduling_motivation.md)

本实验比较固定候选数下的两种 Codex 调度算法生成方式。输入是 HeuriGym 的算子 DAG。给定操作延时和各类功能单元数量，求每个节点的整数起始周期，使所有依赖和资源约束成立，并最小化最后一个节点的完成周期（makespan）。操作从开始到结束持续占用一个功能单元。

[第一轮实验结果及选中的调度器](round1_20261120/README.md)

## 第一轮设置

| 项目 | 设置 |
| --- | --- |
| 手写基线 | 事件驱动的列表调度。优先运行就绪操作中到图出口的最长路径较长者，路径长度包含操作延时。 |
| 独立生成 | 10 次新会话，每次使用相同的任务描述和 5 张示例图。 |
| 反馈迭代 | 10 次新会话。第一轮的提示与独立组相同，后续每轮附加上一轮代码及其在 5 张示例图上的执行结果。 |
| 验证集 | 12 张图。每组先按合法调度的图数排序，再比较相对基线 makespan 比值的几何平均值，完全相同则取较早的候选。 |
| 预留集 | 7 张图。`dag_500`、`dag_1000`、`dag_1500`、`fir1`、`fir2`、`jpeg_fdct`、`jpeg_idct`。每组只评测其选中的一份算法。 |
| 运行条件 | 两组使用同一 Codex 模型和推理强度，每次生成限时 240 秒，每张图的求解限时为 10 秒。预留集在同一逻辑 CPU 上串行评测，记录包含进程启动开销的耗时。 |

5 张示例来自 HeuriGym 的 `demo` 目录。其余 19 张公开 `eval` 图在生成候选前划分，三个规模图、两个 FIR 图和两个 JPEG 图整组预留。验证及预留图的内容都未放入模型提示。公开图可能出现在模型训练材料中。一次运行主要用于验证流程和观察差异，后续评估需要更多图族和重复运行。

实验覆盖静态图级调度。迁移到 Light-HLS 时，还需要定义目标后端的操作延时、流水化资源占用方式及额外依赖，并通过端到端验证检查生成 RTL。

## 运行与查看结果

需要 Linux、Python 3.10 或更新版本、`bwrap`、`taskset`，以及已登录且可发出模型请求的 Codex CLI。运行环境应支持 `bwrap` 创建网络等命名空间。候选代码在无网络的独立进程里执行。除系统运行时，进程只挂载当前输入图、候选代码、执行器和专用输出目录，同时限制 CPU 时间、内存和单文件大小。

在仓库根目录执行以下命令。每次运行使用一个新目录。

```bash
python3 experiments/operator_scheduling/run_round.py \
  --output runs/operator_scheduling_round1_20261120 \
  --model gpt-6.1-sol --effort medium --rounds 10 --parallel 3
```

- `protocol.json` 记录模型、输入划分、文件哈希、排序规则和运行约束。
- 各候选目录保存实际提示、模型事件、候选代码和逐图评测记录。
- `selected.json` 保存预留图评测前选定的候选及代码哈希。
- `results.json` 保存逐图结果、各轮验证分数和 HeuriGym 作者公布的 ILP 参考值。参考值直接读取收集包。

数据来源见仓库内的 [HLS Scheduling Benchmark 收集说明](../../benchmarks/hls_scheduling_2024_2026/README.md)。

## Light-HLS 工作流试点

[2026-11-24 试点结果](flow_pilot_20261124.md)

`flow_pilot.py` 对比固定模型与请求数下的独立生成、执行反馈和执行反馈加独立审阅。三组各跑两条轨迹，每条 3 次请求。独立组和反馈组各发起 3 次候选生成，审阅组用 1 次请求审阅首份代码，再生成第二份候选。报告分别记录实际 token、生成状态和候选成绩。所有 Codex 请求使用 `--ephemeral`。

先按仓库根目录的构建说明更新 `Light_HLS_Top`，再执行以下命令。`--calibrate-only` 可单独检查编译器、图的精确参考和 RTL 仿真。

```bash
python3 experiments/operator_scheduling/flow_pilot.py \
  --output runs/operator_scheduling_flow_pilot_v2_20261124 \
  --repeats 2 --model gpt-6.1-sol --effort low
```

`flow_pilot.c` 提供四个直线 C 用例，`lookup_xor` 和 `mix_words` 用于开发，`weighted_sum` 和 `pack_channels` 用于预留评测。目标配置为 U250、250 MHz 调度约束。编译器以 `HI_EXPERIMENT_SCHEDULE_GRAPH` 指定输出图，记录操作延时、数据依赖和发请求时的 bank／端口容量。以 `HI_EXPERIMENT_SCHEDULE_REPLAY` 指定 JSON 起始周期数组时，原调度器检查并回放候选。实验入口要求单基本块、只读或无内存访问，适用于这轮非流水试点。

`flow_schedule.py` 在隔离进程中加载 Agent 提交的优先级函数，使用固定列表调度器生成起始周期。小图通过枚举端口请求顺序得到给定图模型下的精确参考。候选先过图检查，再由 Light-HLS 回放，并用 Icarus Verilog 对每个生成的 RTL 执行 32 个原生 C 差分测试。每条轨迹按开发集 RTL 通过数、RTL 周期比值和提交顺序选出候选，保存选拔后再对预留图运行选中的代码。

## KDA 启发的调度诊断对照

[三组实验结果、用例划分与复现说明](diagnostic_flow_20261125.md)

`diagnostic_flow.py` 在同一初始策略上比较结果反馈、固定诊断和 Agent 自选诊断。三组都限额调用临时 Codex 会话，候选只修改 `priority(op)`。调度器记录每次选择时的就绪操作、满足资源约束的操作和实际选择。实验脚本管理父候选、诊断查询、实测 RTL 周期和开发集选拔。冻结后在八个预留设计上回放，独立核查工具可重新编译并用另一批输入做 C/RTL 差分验证。

## Bambu 调度机制对照

[Bambu 与 Light-HLS 的 27 项对照实验](bambu_attribution_20261125.md)记录了论文版 Bambu 的调度源码、432 组 RTL 仿真、运算链与前端的独立消融，以及 Jacobi-1D 在两套工具中的端口实验。
