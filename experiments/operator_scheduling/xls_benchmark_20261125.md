# 5 个 ISDC 公开对应物的 XLS 流水调度实测

2026 年 11 月 25 日。使用 Google 的 XLS Linux 官方发行版，从已收集的全部 5 个 DSLX 入口生成优化 IR，再分别执行 SDC 和 ASAP 调度。10 个流水 RTL 都通过 Icarus Verilog 仿真，逐位与原始 DSLX 函数的解释执行结果核对。同一设计上两种策略得到相同的阶段数，其中 4 个设计的 SDC 排程使用更少的编译器寄存器位。[逐例结果和工具哈希](xls_benchmark_20261125.json)

| DSLX 设计 | 时钟目标 ps | SDC／ASAP 调度阶段数 | 含接口寄存器的签名延时（周期） | SDC 寄存器位 | ASAP 寄存器位 | 实测功能向量（SDC／ASAP） |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `crc32` | 2500 | 1／1 | 2 | 40 | 40 | 16／16 |
| `hsv2rgb` | 5000 | 2／2 | 3 | 103 | 212 | 16／16 |
| `float32_fast_rsqrt` | 5000 | 7／7 | 8 | 646 | 2498 | 16／16 |
| `sha256` | 2500 | 96／96 | 97 | 75204 | 188065 | 16／16 |
| `fpexp_32` | 5000 | 48／48 | 49 | 9552 | 65274 | 16／16 |

使用 2026-11-19 发布的 `v0.0.0-11174-g32dc47058`，归档大小 143,017,625 字节，SHA-256 为 `d62488b1c8270ac626aab13a03dfddb238458270f64164b8bf3ccb5d782d0fe7`。5 个输入及 DSLX 标准库采用此前收集的 `f0ad0e58eafcb062cbb08b4e7751c7442ac8346c` 修订。优化和调度均使用内置 `sky130` 延时模型。时钟选自 DATE 2024 论文 Table I 的对应行。两组共用同一份优化 IR，显式启用输入和输出寄存器，并生成 Verilog。阶段数取自 `PipelineScheduleProto.length`，接口延时取自 `ModuleSignatureProto.pipeline.latency`，寄存器位数直接汇总 XLS block IR 的每个寄存器类型位宽，含输入输出寄存器。[官方发行版](https://github.com/google/xls/releases/tag/v0.0.0-11174-g32dc47058) · [ISDC 论文](https://doi.org/10.23919/DATE58400.2024.10546877)

每个设计先运行原始 DSLX 单元测试，五个模块合计 12 个测试通过。DSLX bytecode 默认跳过 `float32` 模块的三个 quickcheck。每个流水 RTL 另连续送入 16 个输入（含全零、全一和固定种子，浮点另含正常数、负数、无穷和次正规等位型），按签名延时采集 RTL 原始输出。额外的 DSLX 断言直接调用同一顶层函数，对对应输入与输出逐位核对。每个策略验证相同的 80 份输入输出对，总计 160 次。所有差分日志和生成 RTL 保留在 `runs/xls_benchmark_20261125/formal/`。

### 能与论文比较到什么程度

收集标记为 `broader_suite` 的这五个输入与论文设计名称或函数用途对应。论文在 17 个设计上比较自己的反馈导向 ISDC 和当时的 XLS SDC，并用 Yosys/OpenSTA 对 SKY130 综合时序做评价。本次跑的是当前公开对应物、近同期发行的标准 XLS SDC 和开发用 ASAP。即使时钟数值相同，精确输入修订、原 ISDC 实现、下游综合库和 STA 配置仍有差异。阶段数、寄存器位和逐位功能构成可执行的 XLS 组内基线。它们不能归为 Light-HLS Agent 的结果，不能用来宣称已超过论文物理面积或时序成绩。

### 复现

先下载上述发行版并核对 SHA-256，将工具解包到独立目录。脚本需要 Python 3.10 以上、`iverilog` 和 `vvp`。从仓库根目录使用新的输出目录运行。完整命令及各步时长保存在每个用例的 `commands.jsonl`。[复现脚本](xls_benchmark.py)

```bash
python3 experiments/operator_scheduling/xls_benchmark.py \
  --tools runs/xls_benchmark_20261125/tools/xls-v0.0.0-11174-g32dc47058-linux-x64 \
  --output runs/xls_reproduction
```
