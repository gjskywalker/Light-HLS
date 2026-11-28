# PLDI 2024 三个完整程序的同器件布局布线

这三项是预先指定的开发集案例。Light-HLS 使用项目单独表征的 `xc7z020clg484-1` 10 ns 操作库，顶层为保留原程序初始化、计算和校验的 `benchmark_main`。普通块排序实验中的关键路径基线和三条 Agent 轨迹对这三个程序全部采用原生调度；加入循环流水选择后，三份实际部署 RTL 均发生变化。两套结果分别记录。下述数字比较整套工具输出，不能用来归因 Agent 的调度策略。[PLDI 原论文和 Fig. 10](https://doi.org/10.1145/3656455) · [作者工件](https://zenodo.org/records/10808233)

## 最终选中的循环流水 RTL

三份实际部署的 RTL 在同版本布局布线后，估算执行时间全部短于原生 Light-HLS 和 Vericert hyperblock，其中 GEMM 也短于 Bambu。预先设定的手工策略逐例选中了相同循环，生成的 RTL 文件与 Agent 策略逐字节相同。这三例说明循环流水可以带来硬件收益，两种循环选择策略在这组开发案例上打平。[最终部署记录、RTL 哈希和物理结果](paper_physical_deployed_20261125.json)

| 程序、选中循环 | 编译所得 II | 实测 RTL 周期 | WNS／WHS (ns) | LUT／FF／Slice | DSP／BRAM18 当量 | 固定 100 MHz 时间 (µs) | 布局布线估算时间，入选／Bambu (µs) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `gemm` · `Loop_kernel_gemm_4` | 2 | 69,860 | +0.880／+0.016 | 1,256／1,351／496 | 3／5 | 698.60 | **637.12**／658.65 |
| `bicg` · `Loop_kernel_bicg_3` | 2 | 35,814 | +0.599／+0.048 | 1,211／1,366／508 | 6／8 | 358.14 | 336.69／**129.62** |
| `jacobi-1d` · `Loop_kernel_jacobi_1d_2` | 3 | 4,121 | +1.235／+0.098 | 264／301／101 | 0／2 | 41.21 | 36.12／**18.62** |

GEMM 在固定 100 MHz 时较 Bambu 少用 9,562 周期（12.04%），按各自布局布线延迟计算，估算执行时间缩短 3.27%。BiCG 和 Jacobi-1D 的入选 RTL 仍分别需要 Bambu 的 2.60 和 1.94 倍估算执行时间。三次 RTL 仿真都核对了冻结记录中的周期与原 C 的固定输入返回值。三次实现都完成布线，setup 和 hold slack 均为正。

## 在同一版 Vivado 重跑原生 Light-HLS 与作者网表

下表三套结果都使用 Vivado 2025.2，同一器件、10 ns 约束和相同的 OOC 主流程。列出的运行时间分别采用各自 RTL 的本次实测周期，乘以本次布局布线的设计延迟。

| 程序 | 实现 | RTL 周期 | WNS／WHS (ns) | 估算执行时间 (µs) | LUT／FF／Slice | DSP／BRAM18 当量 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `gemm` | Light-HLS 原生 | 83,060 | +0.561／+0.032 | 784.00 | 1,261／1,338／478 | 3／5 |
| `gemm` | Vericert hyperblock | 235,813 | +0.453／+0.132 | 2,251.31 | 3,033／2,447／1,082 | 18／4 |
| `gemm` | Bambu 默认 | 79,422 | +1.707／+0.073 | 658.65 | 1,735／1,821／712 | 3／6 |
| `bicg` | Light-HLS 原生 | 37,284 | +0.852／+0.035 | 341.07 | 1,201／1,338／490 | 6／8 |
| `bicg` | Vericert hyperblock | 65,935 | +0.453／+0.109 | 629.48 | 2,808／2,386／1,053 | 9／4 |
| `bicg` | Bambu 默认 | 18,404 | +2.957／+0.096 | 129.62 | 748／655／294 | 0／2 |
| `jacobi-1d` | Light-HLS 原生 | 4,641 | +1.314／+0.144 | 40.31 | 265／290／107 | 0／2 |
| `jacobi-1d` | Vericert hyperblock | 14,649 | +0.512／+0.146 | 138.99 | 651／1,249／409 | 0／1 |
| `jacobi-1d` | Bambu 默认 | 2,442 | +2.376／+0.180 | 18.62 | 363／290／128 | 0／4 |

上述九个实现的 setup 与 hold slack 均为正，所有可路由连线均完成布线。按固定 100 MHz 的功能周期或按布局布线延迟估算的时间，三项均为 Bambu 默认最快、Light-HLS 原生居中、Vericert hyperblock 较慢。各工具的 LUT、DSP 和 BRAM 使用并不相等，尤其是 Bambu 在 BiCG 的资源用量也更低。三例属于同一 PolyBench 来源和固定输入，本结果不能外推到所有程序。[九份实测与十五份作者原始配置的比较数据](paper_physical_results_20261125.json)

若按固定 100 MHz 运行，Light-HLS 的三个完整程序分别需要 830.60、372.84、46.41 µs。表中的估算执行时间沿用论文 Fig. 10a 的方法，用硬件周期数乘以 10 ns 减 WNS 后的最小周期估算值。没有进行板上墙钟测量。作者的 OOC 约束仅对主时钟进行设置，Vivado 提示独立时钟端口缺少 `HD.CLK_SRC`，板级时钟树及 I/O 延迟的影响未包括在内。

## 同源程序的三个参照

| 程序 | 作者 hyperblock 归档周期／估算 µs | 作者 Bambu 默认归档周期／估算 µs | 两种仿真器重跑归档 hyperblock 周期 | 实际 C checksum（十六进制） |
| --- | ---: | ---: | ---: | --- |
| `gemm` | 234,643／2,236.62 | 79,422／648.72 | 235,813 | `00000000` |
| `bicg` | 65,812／634.03 | 18,404／129.58 | 65,935 | `00000000` |
| `jacobi-1d` | 14,568／137.84 | 2,442／18.50 | 14,649 | `00000001` |

三个 Light-HLS 原生 RTL 在作者发表的 hyperblock 一列上周期和估算时间均更小，在 Bambu 默认一列均更大。归档中的另外三个配置也保留在机器数据内。同一程序在两套工具中的前端优化、内存组织和执行控制均可不同。GEMM 和 BiCG 在作者输入上返回零；原生 C、作者归档 RTL 与 Light-HLS 均对上该值，它仅检验这一次作者输入的规定输出。

仿真重跑使用归档内的三份 hyperblock SystemVerilog。Verilator 4.038 使用作者原始 `scripts/verilator_main.cpp`，Icarus 使用独立编写的逐边沿驱动。两者逐例给出相同周期和校验值，但高于归档 `exec.csv` 的差额分别为 1,170、123、81 周期。三个差额不相同；当前资料没有确认 RTL 快照和表中周期差异的根因，本文分别保留两套数字。作者驱动释放复位后先运行一拍，计数器置 1，再运行到 `finish`。Light-HLS 在接受启动的边沿将计数器置零，到 `ap_done` 为止。若把启动边沿计入，Light-HLS 数值增加一拍。此边界选择不改变三项与上述两个归档基线的胜负。

## Bambu 的 RTL 与存储边界

Bambu 默认配置的归档 `main_top.v` 包含计算、控制和片上 RAM，顶层只有 `clock`、`reset`、`start_port`、`done_port` 和 `return_port`。物理资源涵盖这个顶层之下的完整网表，两侧的仿真 testbench 均不参与综合。使用作者生成的 Bambu testbench 计时，两个带作者主目录的文件读写路径改为相对路径。归档没有附测试台读取的 `values.txt`，故从原生 C 取得每个无参数程序的唯一预期返回值，按原格式重建。三项的返回值验证均成功，Bambu 仿真周期与 `exec.csv` 逐项相等。

归档 RTL 引用了绝对路径下的 RAM 起始内容文件，`gemm` 缺 3 份、`bicg` 缺 5 份、`jacobi-1d` 缺 2 份。作者旧版综合日志及本机综合和 Icarus 日志均记录相同文件不可读。此次没有补造这些内存内容。程序在这组固定输入下经自身初始化获得正确返回值，其他初始化条件的等价性没有验证。Bambu 使用其原测试台的起止事件和计数公式，Light-HLS 与 Vericert 使用前述各自的接口约定。

## 配置与复现

Vivado 2025.2 的实际执行步骤与作者工件 Tcl 的关键命令对齐。先执行 `synth_design -mode out_of_context -no_iobuf`，再建立 10 ns 时钟、`opt_design`、`place_design -directive Explore`，在 post-place setup slack 低于 0.5 ns 时执行 `phys_opt_design`，最后执行 `route_design -directive Explore`。每个 Vivado 使用最多 2 个线程，最多并行两个实例。资源以 post-route utilization 为准，BRAM 用 18 Kb 当量表达，两个 BRAM18 当量对应一个 Block RAM Tile。作者三例的原始工具日志标为 Vivado 2023.1。[实现脚本](paper_physical.tcl) · [仿真驱动](paper_physical_tb.v)

保存的 `native` 输入目录应来自相同器件和时钟的完整程序编译；下列命令在一个新目录重跑九次 OOC 实现及 Icarus 差分，并读入作者五个配置的原始报告。命令可追加 `--verilator verilator` 运行作者的 Verilator 驱动；本次三份 hyperblock HDL 都已作此交叉复核。

```bash
python3 experiments/operator_scheduling/paper_physical.py \
  --native runs/collection_order_final_20261125/native \
  --run runs/paper_physical_repeat \
  --output runs/paper_physical_repeat/physical.json
```

对最终加入循环流水选择后的策略，使用冻结记录定位具体的 `agent.deployed` 版本，独立比对原 C 的返回值及 RTL 周期，并用同一 Tcl 进行 OOC 实现。相同哈希可重用原生结果，有变化时重新布局布线。

```bash
python3 experiments/operator_scheduling/paper_physical_deployed.py \
  --frozen runs/collection_verified_20261125 \
  --run runs/paper_physical_deployed_repeat \
  --output runs/paper_physical_deployed_repeat/physical.json
```
