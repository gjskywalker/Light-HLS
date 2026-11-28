# Bambu 比 Light-HLS 快在哪里

在 PLDI 2024 Vericert 工件的 27 个完整程序上，Bambu 默认配置有 19 项比冻结的 Light-HLS 多循环 Agent 少用周期。**最影响排名的调度机制是同拍运算链**，即允许有数据依赖的操作在同一拍连续完成。单独关掉它，Bambu 只在 `trisolv` 上仍然领先。换掉 Bambu 的操作优先级规则，27 项的周期全部持平。**前端和存储资源也能独立改变结果**：保留 Bambu 默认调度器、只将前端改为 `-O0`，它领先的项目就从 19 项降到 7 项。所有整程序周期都含作者的初始化、计算和输出校验。[原始论文 §8](https://johnwickerson.github.io/papers/vericert_pldi24.pdf) · [作者工件](https://zenodo.org/records/10808233) · [归档网表及逐项周期核对](bambu_archive_audit.json)

## 默认调度器实际做了什么

使用的版本是 PandA 2023.1，提交 `04336c437a53bc96ae90b74052c455629946ec8b`。从作者 VM 取出的 Bambu 主程序与官方 2023.01 发行镜像的 ELF 逐字节一致，SHA-256 为 `cbf1ff036d9337cefc7e339cdad463f7a406a18ac19d04311b19999ac1b641c1`。这一哈希核对覆盖主程序。镜像内编译器与插件采用官方发行包，另以归档的实测周期核对端到端口径。[VM 提取方法与哈希](bambu_paper_binary_20261125.json) · [作者复现说明](https://zenodo.org/records/10808233/files/README.pdf) · [Bambu 官方发行目录](https://release.bambuhls.eu/)

| 决策 | 论文所用 Bambu 版本 | 当前 Light-HLS Agent 可以改变什么 |
| --- | --- | --- |
| 下一个就绪操作 | 按基本块做资源约束列表调度。就绪操作初始优先级为 ASAP − ALAP，可安排时间窗口越窄，优先级越高。每过一个编译期控制步，仍在等待的就绪操作获得额外优先级。该动态更新发生在编译期间，硬件实现采用静态排程。[优先级源码](https://github.com/ferrandi/PandA-bambu/blob/04336c437a53bc96ae90b74052c455629946ec8b/src/HLS/scheduling/priority.cpp#L72-L104) | 普通块中根据剩余关键路径、剩余访存链、扇出、操作延时计算一次固定分数，再由后端分配周期。循环层另有一个选择是否流水化的过程。[优先级实现](../../Light_HLS/Implementations/HI_OperationScheduling/HI_OperationScheduling.cc) |
| 同拍能容纳哪些操作 | 就绪操作进入可实现它的功能单元队列。调度时考虑单元余量、多周期占用和启动间隔。对同拍依赖，检查操作完成时间、连接/多路器延时、建立余量、访存限制及能否串接。[功能单元队列](https://github.com/ferrandi/PandA-bambu/blob/04336c437a53bc96ae90b74052c455629946ec8b/src/HLS/scheduling/parametric_list_based.cpp#L1894-L1943) · [链接可行性](https://github.com/ferrandi/PandA-bambu/blob/04336c437a53bc96ae90b74052c455629946ec8b/src/HLS/scheduling/parametric_list_based.cpp#L1652-L1685) | Light-HLS 已能做端口检查、转发和组合链选择。普通块后端先按目标周期的 70% 等规则选择组合表达式。模型权重改变操作顺序，无法单独选择功能单元实现、改变端口数或重定组合链边界。[普通块后端](../../Light_HLS/Implementations/HI_OperationScheduling/HI_OperationScheduling.cc) |
| 调度面对的输入与资源 | 默认配置 BALANCED-MP 使用前端 O2，允许异步存储，默认配置两个存储通道，器件目标为 Zynq、10 ns。具体每个数组综合出的存储器及可并行访问次数需要看网表。默认不启用推测式 SDC；源码另有该调度器，不能以它解释这批默认结果。[默认参数](https://github.com/ferrandi/PandA-bambu/blob/04336c437a53bc96ae90b74052c455629946ec8b/src/BambuParameter.cpp#L3623-L3721) · [调度默认值](https://github.com/ferrandi/PandA-bambu/blob/04336c437a53bc96ae90b74052c455629946ec8b/src/BambuParameter.cpp#L4240-L4261) | 当前模型最多选三个候选循环，编译器证明可流水化后计算收益。端口和前端配置固定，模型只能在给定的操作图及资源预算内选择。[冻结循环策略](paper_loop_flow.py) |

这版 Bambu 的帮助文本与实际枚举把两个非默认模式的数字写反。实验按源码将 `--parametric-list-based=2` 用作静态 mobility，把 `=1` 用作固定优先级；后者未提供外部优先级表。两种模式都保留调度器的依赖、时序和资源检查。[枚举定义](https://github.com/ferrandi/PandA-bambu/blob/04336c437a53bc96ae90b74052c455629946ec8b/src/HLS/scheduling/parametric_list_based.hpp#L103-L116) · [实现分支](https://github.com/ferrandi/PandA-bambu/blob/04336c437a53bc96ae90b74052c455629946ec8b/src/HLS/scheduling/parametric_list_based.cpp#L665-L692)

## 如何检验归因

作者的默认和 `noopt` 都保留列表调度。`noopt` 同时指定 `-fno-if-conversion -O0 --do-not-chain-memories --do-not-use-asynchronous-memories --no-chaining`，并显式给出默认的 10 ns 时钟。这里分别改变就绪优先级、全局 chaining、访存 chaining、是否允许异步存储、前端优化等选项；每次都从原 C 重新生成 RTL，使用 Icarus、同一份作者 testbench 和原生 C 返回值核对。另将三类开关交叉，观察前端、链和存储选项的组合效应。各程序等权，周期比用几何平均。[重编及验证脚本](bambu_ablations.py) · [组合效应计算](bambu_effects.py)

共完成 **27 个程序 × 16 个配置 = 432 组**重新综合和 RTL 仿真，全部通过 C 返回值校验。新生成的 27 个默认结果和 27 个论文 `noopt` 结果逐项精确复现作者周期。另行回放作者归档的 54 份网表，也全部复现。作者归档缺少部分 RAM 初始化文件，本次用于消融实验的网表均从 C 重新生成。固定输入返回值相同并不保证所有中间数组或其他输入等价。Jacobi-1D 的四个 Light-HLS 端口配置另外核对了完整输出数组。[主消融数据](bambu_ablations_20261125.json) · [组合配置数据](bambu_factorial_arms_20261125.json) · [通道与优先级数据](bambu_priority_memory_20261125.json)

| Bambu 配置变化 | 相对默认周期减少／持平／增加的程序数 | 周期比几何平均（配置／默认） | 周期少于冻结 Light-HLS Agent |
| --- | ---: | ---: | ---: |
| 默认配置 | 0／27／0 | 1.000 | 19／27 |
| 动态 mobility 换成静态 mobility | 0／27／0 | 1.000 | 19／27 |
| 所有操作固定相同优先级 | 0／27／0 | 1.000 | 19／27 |
| 关闭同拍运算链 | 0／0／27 | **2.569** | **1／27** |
| 仅关闭与访存有关的同拍串接 | 0／0／27 | **1.367** | **10／27** |
| 存储限制为一个通道 | 0／14／13 | 1.096 | 17／27 |
| 排除异步存储 | 1／26／0 | 0.999996 | 19／27 |
| 前端从 `-O2` 改为 `-O0` | 0／0／27 | **1.421** | **7／27** |
| 仅关闭 if-conversion | 0／27／0 | 1.000 | 19／27 |
| `-O0` 并关闭同拍运算链 | 0／0／27 | 2.963 | 1／27 |
| 论文 `noopt` 完整配置 | 0／0／27 | 2.963 | 1／27 |

固定优先级和静态 mobility 都保留了 Bambu 的功能单元、依赖和时序约束。相对于默认配置，去除生成时间与命令行、将 RAM 路径替换为初始内容哈希后，静态 mobility 有 24/27 份网表完全一致，固定优先级有 23/27 份完全一致。其余网表虽有文本差异，整程序周期仍然一致。表中端口、运算链和前端各行都是单独干预，领先项目的减少数不能相加。[归一化网表与跨工具对比](bambu_effects_20261125.json)

### 在 Bambu 内分解默认配置到论文 noopt 的差距

以 F 表示关闭前端优化（`-O0 -fno-if-conversion`），S 表示关闭普通及访存串接，M 表示排除异步存储。每个程序覆盖全部八种组合。默认到 `noopt` 的周期几何平均扩大 **2.963 倍**。按六种关闭顺序，分别对每个开关的对数周期增量取平均（Shapley 分解），再对 27 个程序等权平均，得到下表。[逐项八种组合与分解结果](bambu_effects_20261125.json)

| 关闭的机制 | 对默认到 noopt 的对数周期差的份额 | 随另外两个开关状态变化，关闭此项的周期倍数范围 |
| --- | ---: | ---: |
| 同拍运算链与访存串接 S | 77.25% | 2.085–2.569× |
| 前端优化 F | 22.75% | 1.153–1.421× |
| 异步存储选择 M | 约 0% | 0.999996–1.000002× |

**77.25% 的分母仅是同一 Bambu 的默认到 `noopt` 的对数周期差。** 两端都使用相同的默认存储通道数，M 只控制异步存储，因此 M 接近零与 Jacobi 的端口效应可以同时成立。对于 Bambu 相对 Light-HLS 的差距，目前可以验证哪些条件足以改变胜负，尚不能给出调度器独自贡献的统一百分比。

## 三个能定位差距的程序

**Jacobi-1D 的主要周期差可以在 Light-HLS 内通过改变端口预算复现。** 保留同一个编译器、同一对已选流水循环、原输入、10 ns 和请求 II=1，仅改变数组 A、B 各自能用几个端口。两个方向的 sweep 分别从 A 或 B 读邻点。双端口让两个 sweep 的实际 II 均从 3 降到 2；四个点都逐项匹配 C 的 30 个输出。[端口干预的命令、全部数组和布局布线数据](bambu_jacobi_ports.json)

| A 端口／B 端口 | 两个 sweep 的实际 II | Light-HLS 整程序周期 |
| --- | --- | ---: |
| 1／1 | 3／3 | 3,601 |
| 2／1 或 1／2 | 一侧为 2、另一侧为 3 | 3,041 |
| 2／2 | 2／2 | 2,481 |

Bambu 默认用 2,442 周期。Light-HLS 原差距为 1,159 拍，增加两个端口省下 1,120 拍，补回 **96.64%**，仍相差 39 拍。Bambu 网表实测 A、B 各有 560 拍发生双读。按 100 MHz 两边的周期数仍是 Bambu 更少；按同版 Vivado 布线后估算，双端口 Light-HLS 为 17.56 µs，Bambu 为 18.62 µs。两份网表的存储成本不同，Light-HLS 为 410 LUT、0 BRAM18，Bambu 为 363 LUT、4 BRAM18。此例揭示了固定端口下单靠改排序无法触及的空间。96.64% 仅描述这个程序的这个干预；额外端口并非原 Agent 自动选择。[实际访存与作者网表](bambu_archive_audit.json)

反向实验也成立。在 Bambu 内保持原调度器和前端，只将通道配置改为 `MEM_ACC_11`、数量为 1，Jacobi-1D 从 2,442 升至 **3,562** 拍，同样增加 **1,120** 拍。于是两边均采用单通道／单端口设置时为 3,601 对 3,562，允许每数组双访问时为 2,481 对 2,442，两次都是 39 拍差距。两套工具的内部硬件实现依然各自独立。在全部 27 项中，这个 Bambu 通道设置有 13 项周期增加、14 项持平，周期几何平均增加 9.65%。[Bambu 通道及固定优先级干预](bambu_priority_memory_20261125.json)

**BiCG 的大部分整程序周期差在初始化结束前已经出现。** 取两份实际部署网表的同一观测事件，即只由初始化写入的矩阵 A 的最后一次写。两边都写了 1,596 次；事件在 Light-HLS 第 32,092 拍、Bambu 默认第 14,929 拍，差 17,163 拍。整程序分别为 35,814 和 18,404 拍，差 17,410 拍。默认 Bambu 在这一程序里此后没有读取该矩阵，Light-HLS 读 1,596 次，Bambu `noopt` 读 3,192 次。作者的整数初始化使 A、r、p 全零，两个工具对同一输入最终执行的内存工作量已经发生变化。时间差中的 98.58% 落在这个观测边界之前；这是执行区间定位，尚不能指认某一个编译 pass 或调度器的成本占比。[BiCG 源码](../../benchmarks/hls_scheduling_2024_2026/sources/vericert_pldi2024_archive/benchmarks/polybench-syn/linear-algebra/kernels/bicg.c) · [只读硬件探针和结果](bambu_archive_audit.py)

**Trisolv 也受完整程序计时边界影响。** 输入 L 的最后一次初始化写在 Light-HLS 第 30,460 拍、Bambu 默认第 4,373 拍，两者均写入 820 次。整程序为 34,008 对 7,802 拍，26,206 拍差距中已有 26,087 拍出现在此事件之前。事件之后的区间约为 3,548 对 3,429 拍，仍包含其余初始化、计算和校验。将整程序的四倍以上差距当作三角求解计算循环的单独调度加速，会误判改进方向。[逐例事件与时钟计数](bambu_archive_audit.json)

## 把一种优先级移植回同一 Light-HLS 后端

在独立构建中，只将普通块排序改为静态 mobility，复用原依赖、整数操作延时、访存约束、分配器、IR 平局规则和逐块接纳条件。27 个程序的原生控制网表与旧记录逐字节相同。启用逐块接纳时，新规则与冻结 Agent **27/27 周期相同**，两者仅将 Durbin 的 7,808 拍降到 7,028。强制使用排序时，两组 54 次 C/RTL 均通过，10 个设计的 RTL 不同，仅 Gemver 有周期差异，静态 mobility 为 46,272，Agent 为 47,872，原生也是 46,272。这项移植考察排序，没有移植 Bambu 的动态等待补偿、功能单元选择或链的时序可行性判断。[同后端实验与原生网表哈希](bambu_light_mobility_results_20261125.json)

## 运算链对布线后时间的影响

同拍运算链（operation chaining）允许一个操作的组合输出在同一时钟周期驱动下一个操作，减少中间等待周期，也可能拉长关键路径。选择已有双方布局布线记录的 BiCG 和 Jacobi-1D，在 Bambu 内只改变 `--no-chaining`，从 C 重新综合，每个程序两组使用完全相同的存储器初值。以下为 Vivado 2025.2、`xc7z020clg484-1`、10 ns、同一套 OOC 综合和布局布线流程的结果。四份设计均满足目标频率的 setup 和 hold。按布线后路径估算的执行时间取实测周期乘以 `10 ns − WNS`，每份网表各做一次布局布线。[物理结果、存储器哈希与资源](bambu_chaining_physical_20261125.json) · [布局布线脚本](bambu_chaining_physical.py)

| 程序 | 默认／关闭运算链，周期 | 默认／关闭，WNS (ns) | 默认／关闭，估算时间 (µs) | 默认开启链缩短的估算时间 |
| --- | ---: | ---: | ---: | ---: |
| BiCG | 18,404／50,453 | +2.957／+2.321 | 129.62／387.43 | 66.54% |
| Jacobi-1D | 2,442／12,825 | +2.376／+2.092 | 18.62／101.42 | 81.64% |

两项中，开启链既减少周期，也达到更好的布线后时序。Light-HLS 已经能做组合串接。要测量两种串接算法本身的差距，仍需在同一操作图、同一端口和功能单元预算、同一后端上替换链选择策略。

## 对下一轮 Agent 实验的含义

1. 先单独评估链的选择。固定前端 IR、存储配置和运算单元预算，让 Agent 在合法边界内选择同拍依赖链，用 RTL 周期筛选，再用布线后的执行时间和资源验证。这样能直接测到调度算法带来的收益。
2. 将端口数作为独立实验维度。向 Agent 提供每个数组每拍的读写需求、端口冲突以及实际 II。允许改变端口的结果应同时披露 LUT、BRAM 和时序，并与固定端口组分别报告。
3. 分开记录初始化、计算、校验的起止周期，以及各数组的实际读写次数。保留作者的原始输入做复现，另外以非退化输入和输出数组逐项比对检验泛化。

## 复现

从仓库根目录运行。下例使用本机已经解包的官方 AppImage、已收集的 PLDI 工件和一个全新的输出目录。若需重建工具目录，可对上述 SHA-256 的官方 AppImage 运行 `unsquashfs -o 189632 -d <工具目录> bambu-2023.01.AppImage`。本次采用 32 位目标所需的系统 multilib 头文件、Icarus Verilog 和原程序的固定输入。命令行最后一步只处理测量结果，无需再编译。

```bash
src=experiments/operator_scheduling
app=runs/bambu_causal_20261125/tools/march
out=runs/bambu_attribution_repeat
mkdir -p "$out"
python3 "$src/bambu_ablations.py" --appdir "$app" --run "$out/matrix" \
  --arms default static_mobility no_chaining no_memory_chaining no_async_memory \
         frontend_O0 no_if_conversion O0_no_chaining paper_noopt \
  --sim-timeout 900 --output "$out/matrix.json"
python3 "$src/bambu_ablations.py" --appdir "$app" --run "$out/factorial" \
  --arms F S FS FM SM --sim-timeout 900 --output "$out/factorial.json"
python3 "$src/bambu_ablations.py" --appdir "$app" --run "$out/priority" \
  --arms fixed_priority single_channel --sim-timeout 900 --output "$out/priority.json"
python3 "$src/bambu_effects.py" --matrix "$out/matrix.json" --factorial "$out/factorial.json" \
  --matrix-run "$out/matrix" --priority-run "$out/priority" --priority-results "$out/priority.json" \
  --archive-audit "$src/bambu_archive_audit.json" --output "$out/effects.json"
```
