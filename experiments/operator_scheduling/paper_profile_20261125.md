# 用原生 RTL 热点选择流水循环的事后探索

2026 年 11 月 25 日。每个程序只能让编译器试三个循环时，加入实际执行频率，能否帮助 Agent 在完整程序上比人工热点规则更有效地分配预算？研究者在设计本轮实验前看过早先静态策略的开发集及预留集结果。本轮模型请求仅含原有 8 个开发程序，冻结后才提取本轮预留集热点。这里的预留集结果属于事后探索。[逐例测量](paper_profile_results_20261125.json) · [复现脚本](paper_profile_flow.py)

## 测量与策略

实验沿用已固定的 27 个 Vericert PolyBench 完整 C 程序、输入和 32 位校验值。目标是 xc7z020clg484-1、100 MHz，Light-HLS 二进制 SHA-256 为 `b678bdb2b037efcc23bbdc221e87408d78c0788fda75afd3dbf72e4400a965e5`。每个原生正确程序用 Icarus Verilog 再运行一次，测试平台每周期统计 `dut.state`。生成的 RTL 保持原样，并要求加计数器后的周期、校验值与原生运行完全相同。编译器的循环映射日志将作者的计算循环对应到原生 LLVM 循环头，再通过调度报告的 `first_state` 和 `state_count` 得到循环头各状态的占用周期。入口状态的计数给出执行次数。这组计数仅代表该程序归档输入上的执行路径。

人工规则优先考虑“循环头占用周期减去入口访问次数”最大的循环，即理想 II=1 时可能省下的周期上界。平局时优先叶循环，再看嵌套深度与源码顺序。这使得同一深度、不同执行次数的矩阵运算循环有机会获得不同优先级。Agent 与人工规则拥有相同的信息和每程序至多三次 II=1 编译探测。两者按相同方式决定部署：保留编译器实际实现的简单循环，将其单次静态节省乘以“原生入口访问次数 / 精确 trip count”；选择预测总节省最大的正值候选，没有正值则使用原生调度。完整程序从启动到完成的最终 RTL 周期不参与选取。

一次独立、临时、只读的 `codex exec` 请求向 `gpt-6.1-sol` 提供开发集热点、人工探测成败和早先开发集策略的整程序周期。7 个程序有有效参考，`nussinov` 仅报告原生校验失败。模型冻结的 11 项权重为 `[0,12,4,-48,32,4,-6,-12,4,-24,16]`。依次对应循环头周期的对数、可省周期上界的对数、访问次数的对数、状态数、是否叶循环、嵌套深度、循环头 load 数、store 数、mul 数、源码循环体条件数、循环头是否已映射。对数使用 `log2(1+x)`。该规则偏好高频、结构简单且访存压力较小的循环。首次 CLI 初始化因本地状态库只读而失败，没有返回模型输出；成功调用使用 13,666 个输入 token、686 个输出 token。

## 第一轮结果

27 个完整程序中，24 个原生 RTL 通过，8 个开发程序中可评 7 个，19 个预留程序中可评 17 个。24 项共有 113 个源码计算循环头，全部映射到可计数的原生调度基本块。两臂各使用 70 次逻辑编译探测，合并后涉及 76 个“程序和循环”组合，其中 7 项需要新编译。每个配置在每臂最多占三个名额。两臂最终部署的 RTL 在所有可配对程序上均通过原 C 程序校验。周期比按程序等权取几何平均，越小越好。

| 集合 | 可配对 / 全部 | 人工 / 原生，胜平负 | Agent / 原生，胜平负 | Agent / 人工，胜平负 |
| --- | ---: | --- | --- | --- |
| 开发 | 7 / 8 | 0.937029，5/2/0 | 0.937029，5/2/0 | 1.000000，0/7/0 |
| 预留 | 17 / 19 | 0.913752，9/8/0 | 0.920732，7/10/0 | 1.007639，0/15/2 |
| 全部 | 24 / 27 | 0.920481，14/10/0 | 0.925456，12/12/0 | 1.005405，0/22/2 |

两种方法在 4 个程序上选择了不同的探测集合。Agent 对基本块状态数设置了较大的负权重。`heat_3d` 和 `jacobi_2d` 中，人工热点规则各选中一个能流水化的内循环，分别将 51,081 降至 46,921 周期、83,761 降至 80,121 周期。Agent 没有探测这两个程序中能带来收益的内循环，均回到原生周期。开发集的全平局不足以识别这一失分模式。

与上一轮静态 Agent 的整套部署方式相比，新 Agent 在预留 `covariance` 上从 108,494 降至 85,814 周期，在 `fdtd_2d` 上从 132,380 降至 121,740 周期，在 `2mm` 上从 60,016 降至 59,248 周期；其余有 11 项持平，3 项回退。17 项的几何平均比为 0.994336。上一轮还在流水收益不足时使用普通基本块排序，这项跨轮数字同时包含部署方式的差异。针对本轮控制变量一致的强人工对照，Agent 在预留集平均慢 0.76%。

## 第二轮开发集反馈与默认策略

研究者已见过第一轮的预留结果，第二轮独立临时会话只接收 8 个原开发程序的输入信息、真实热点、人工探测及第一轮 Agent 的开发反馈。每个程序固定保留人工热点首选。模型只对剩余循环排序，从中挑两个；后端采用与第一轮相同的静态收益规则和原 C/RTL 校验。这使得流水探测总预算仍是三个。部署门槛设在模型请求之前：原生正确的全部开发程序均须通过，且候选的整程序周期积必须严格小于人工基线。在同一组程序上，这等价于周期几何平均严格更低。否则部署人工规则；同时单列原始候选，以便观察未被采纳的提案。[开发判据、候选与逐例部署](paper_profile_residual_results_20261125.json) · [反馈与重放脚本](paper_profile_residual.py)

独立 Codex 候选为 `[0,-4,0,-16,0,0,16,0,0,0,0]`，成功请求的输入为 16,894 个 token，输出为 725 个 token。其后备优先级用循环头 load 数加分，对状态数与可省周期上界的对数扣分。这实际改变了若干开发程序的探测，例如 `ludcmp` 保留循环 5 后改探测 6 和 8，两项均被后端拒绝。

开发集 7/7 通过，候选在每项上都与人工周期完全相同。依据事先写入的规则，重放预留集之前已确定部署人工规则。第二轮原始候选与人工相比，在 24 个可验证程序的 15 个上确实更换了探测集合；最后仍只有 1 项出现周期差别。

| 集合 | 可配对 / 全部 | 原始候选 / 原生，胜平负 | 原始候选 / 人工，胜平负 | 最终部署 / 原生，胜平负 | 最终部署 / 人工，胜平负 |
| --- | ---: | --- | --- | --- | --- |
| 开发 | 7 / 8 | 0.937029，5/2/0 | 1.000000，0/7/0 | 0.937029，5/2/0 | 1.000000，0/7/0 |
| 预留 | 17 / 19 | 0.915659，9/8/0 | 1.002087，0/16/1 | 0.913752，9/8/0 | 1.000000，0/17/0 |
| 全部 | 24 / 27 | 0.921841，14/10/0 | 1.001478，0/23/1 | 0.920481，14/10/0 | 1.000000，0/24/0 |

受保护的首选使原始候选在 `heat_3d` 和 `jacobi_2d` 上复现了人工周期。唯一实际差异出现在 `gemver`，原生 46,272 周期、人工 43,232 周期，原始候选 44,792 周期。该例表明即使保留人工第一名，人工后两次探测也可能发现更好的单循环。最终部署在预留集相对原生的 8.62% 几何平均周期降幅归于人工热点策略。本次运行只支持开发门槛能阻止无增益提案被替换上线，尚未发现 Agent 优于强人工策略的证据。

## 迁移到修复后的编译器

另将冻结的第一轮 Agent 排序权重、人工热点规则、第二轮实际选出的人工部署一并迁移到修复版 Light-HLS。源二进制为 `b678bdb2b037efcc23bbdc221e87408d78c0788fda75afd3dbf72e4400a965e5`，目标为 `4f7f72da2a3f70ded431f4281af82fc2c574c2396847f6768c0bc7befde6776a`。两次运行使用完全相同的 27 份源程序、输入校验、开发/预留划分和板级库。没有新的模型调用或开发集选拔。由于研究者已见过这些程序的早期测量，这里衡量已冻结方法的编译器版本迁移效果。[修复版逐项记录](paper_profile_portability_results_20261125.json) · [迁移程序](paper_profile_portability.py)

在新生成的原生 RTL 上重新采集了全部 27 项的热点，128/128 个源码计算循环头有对应的可计数 FSM 状态。每臂仍至多探测三个循环，仅复用目标编译器在相同源代码上的探测。独立重新仿真选中候选，在同一测试平台、逐字节相同 RTL 的两臂之间复用校验。原生回退沿用本次重新获取热点时已核对的 RTL 校验。原先失败的开发程序 `nussinov`、预留程序 `lu` 与 `adi` 均进入新版本分母。原版 24 项的指标保留在前两节。

| 修复版集合 | 可配对 / 全部 | 人工或最终部署 / 原生，胜平负 | 首轮原始 Agent / 原生，胜平负 | 首轮原始 Agent / 人工，胜平负 |
| --- | ---: | --- | --- | --- |
| 开发 | 8 / 8 | 0.944678，5/3/0 | 0.944678，5/3/0 | 1.000000，0/8/0 |
| 预留 | 19 / 19 | 0.920297，10/9/0 | 0.926585，8/11/0 | 1.006833，0/17/2 |
| 全部 | 27 / 27 | 0.927455，15/12/0 | 0.931910，13/14/0 | 1.004803，0/25/2 |

两臂各用了 79 次逻辑探测，全部部署通过目标版本上的原 C 功能校验。新增案例中，`adi` 的人工及首轮 Agent 均将 271,321 降至 259,441 周期；`lu` 和 `nussinov` 均保留原生周期。首轮原始 Agent 在 `heat_3d`、`jacobi_2d` 上仍落后于人工。冻结的第二轮部署沿用人工规则，在完整预留集相对原生几何平均缩短 7.97%。这项迁移没有给 Agent 新的开发反馈，部署数字仍归于源版本选定的人工规则。

## 复现

在完整的固定编译器基线运行目录中保留 `native`、`input`、`probes` 和 `protocol.json`。重新调用模型可能得到不同权重。复验已冻结规则时可从已发布 JSON 提取 `frozen`，直接运行 apply。运行命令如下。

```bash
python3 - <<'PYTHON'
import json
from pathlib import Path

for name in ("profile", "profile_residual"):
    output = Path("runs") / f"rep_{name}"
    output.mkdir(parents=True, exist_ok=True)
    archive = Path("experiments/operator_scheduling") / f"paper_{name}_results_20261125.json"
    frozen = json.loads(archive.read_text())["frozen"]
    (output / "frozen.json").write_text(json.dumps(frozen, indent=2, ensure_ascii=False) + "\n")
PYTHON
python3 experiments/operator_scheduling/paper_profile_flow.py \
  --compiler runs/collection_frozen_binaries_20261125/b678_Light_HLS_Top \
  --base runs/collection_verified_20261125 --run runs/rep_profile --stage apply \
  --summary runs/rep_profile/results.json --jobs 6
python3 experiments/operator_scheduling/paper_profile_residual.py \
  --compiler runs/collection_frozen_binaries_20261125/b678_Light_HLS_Top \
  --base runs/collection_verified_20261125 --first runs/rep_profile --run runs/rep_profile_residual \
  --stage apply --summary runs/rep_profile_residual/results.json --jobs 6
python3 experiments/operator_scheduling/paper_profile_portability.py \
  --source-base runs/collection_verified_20261125 --target-base runs/collection_repaired_20261125 \
  --first-result experiments/operator_scheduling/paper_profile_results_20261125.json \
  --residual-result experiments/operator_scheduling/paper_profile_residual_results_20261125.json \
  --compiler runs/paper_exception_checks_byte_gep_20261125/Light_HLS_Top \
  --run runs/rep_profile_transfer --summary runs/rep_profile_transfer/results.json --jobs 6
```
