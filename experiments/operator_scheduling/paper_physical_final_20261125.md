# 最终多循环调度在七个完整程序上的物理结果

本次使用修复后的编译器 `4f7f72da2a3f70ded431f4281af82fc2c574c2396847f6768c0bc7befde6776a`，沿用原来的三个开发程序和四个按哈希抽出的预留程序。按布局布线估算的执行时间，最终策略在开发集 1/3、预留集 0/4 的程序上快于 PLDI 2024 作者工件中的 Bambu 默认配置。唯一领先的 GEMM 缩短 3.27%，其手工策略与 Agent 生成的 RTL 相同。Light-HLS 有 5/7 个程序达到 100 MHz，Bambu 有 7/7 个达到。各组分别保留了抽样方法及较早策略的测量记录。[开发集物理记录](paper_physical_20261125.md) · [预留集抽样与早期策略对照](paper_physical_holdout_20261125.md)

## 逐程序对比

下表每个数据格依次列出最终 Light-HLS 部署和 Bambu 默认配置。负 WNS 表示没有满足 100 MHz 的时钟要求。资源以及 setup/hold 最差路径端点保存在机器数据中。

| 样本 | 程序 | Agent 部署 | 实测 RTL 周期，Light-HLS／Bambu | WNS (ns)，Light-HLS／Bambu | 估算执行时间 (µs)，Light-HLS／Bambu |
| --- | --- | --- | ---: | ---: | ---: |
| 开发 | GEMM | 循环 4 | 69,860／79,422 | +0.880／+1.707 | **637.12**／658.65 |
| 开发 | BiCG | 循环 3 | 35,814／18,404 | +0.599／+2.957 | 336.69／**129.62** |
| 开发 | Jacobi-1D | 联合循环 2、3 | 3,601／2,442 | +1.158／+2.376 | 31.84／**18.62** |
| 预留 | FDTD-2D | 联合循环 4、6 | 121,980／100,760 | −6.121／+0.916 | 1,966.44／**915.30** |
| 预留 | SYR2K | 原生 | 79,762／67,962 | +0.693／+1.914 | 742.34／**549.54** |
| 预留 | TRMM | 原生 | 36,371／28,362 | +0.885／+1.842 | 331.52／**231.38** |
| 预留 | Cholesky | 原生 | 281,304／317,584 | −2.260／+0.552 | 3,448.79／**3,000.53** |

Jacobi-1D 从先前单循环的 4,121 拍减至 3,601 拍，估算执行时间从 36.12 µs 缩短至 31.84 µs。FDTD-2D 的变化相反：联合调度比单循环减少 10,400 拍，但 WNS 从 −4.429 ns 降到 −6.121 ns，估算时间从 1,910.11 µs 增至 1,966.44 µs（增加 2.95%）。两份网表的最差组合路径均经过 13 个 CARRY4，数据路径的布线延迟从 5.717 ns 增至 7.673 ns。该程序的固定单循环 RTL 也在最终编译器中验证了相同哈希。

七项中有六项的最终手工策略与 Agent 的网表逐字节相同，包括 GEMM。FDTD-2D 两者的网表不同，手工策略选择循环 4、6、8，冻结 RTL 实测为 101,080 拍；本次物理比较仅对该程序最终部署的 Agent 网表进行了布局布线。

Cholesky 的 Light-HLS 比 Bambu 少用 36,280 拍（11.42%），但估算执行时间比 Bambu 长 14.94%。Light-HLS 的 10 ns 约束未闭合，把周期直接乘以 10 ns 会倒置这项对比的胜负。

## 配置与复现

每份实现均使用 Vivado 2025.2，器件 `xc7z020clg484-1`，目标周期 10 ns，Vivado OOC 包含综合、优化、布局及布线。Light-HLS 顶层为 `benchmark_main`，Bambu 顶层为 `main_top`，两者均含程序初始化、算法和校验。本次对比使用作者规定的固定输入。RTL 周期由 Icarus 四态仿真测得，每项对照原 C 的返回值和冻结记录；Bambu 另核对作者的 `exec.csv`。作者归档缺失部分初始化文件，Bambu 各自的最终校验值和周期仍复现归档结果。[作者工件](https://zenodo.org/records/10808233)。

所有结果使用相同的布局布线 Tcl 和资源计量方式，每个实例最多使用两个线程，同时最多运行两个实例。物理结果只按 RTL 的完整 SHA-256 复用。GEMM、BiCG 和 SYR2K 的最终 RTL 与已有实现逐字节一致；Jacobi-1D、FDTD-2D、TRMM 和 Cholesky 均重新进行了完整布局布线。每个程序都对照最终冻结记录复核实际网表、Icarus 周期和校验值。对于联合循环，还比对了构建配置和调度报告中的循环标签，Jacobi-1D 的两个策略复用同一份 `manual` 目录中的 RTL。[机器数据、哈希与报告路径](paper_physical_final_20261125.json)

Cholesky 的首次 Vivado 运行在布线器内部崩溃，另一次重试中 Icarus 退出。最终将进程绑定到两个 CPU（`taskset -c 24,25`）后，使用原始 Tcl、相同线程上限和完全相同的 RTL 成功完成四态校验和布局布线。七份最终 RTL 的 hold slack 均为正，所有可路由连线均完成布线。

```bash
python3 experiments/operator_scheduling/paper_physical_final_route.py \
  --single runs/collection_repaired_20261125 \
  --joint runs/collection_repaired_joint_20261125 \
  --run runs/paper_physical_final_repeat \
  --output runs/paper_physical_final_repeat/physical.json
```

命令读取已归档的开发集和预留集物理报告，在指定的新目录重跑四份新增网表。`--collect-only` 可以从完整的新目录重新提取结果。布局布线后估算时间采用实测 RTL 周期数乘以 `10 ns − WNS`，属于对单次静态时序结果的估算。只有 setup 和 hold slack 都非负，才报告可在目标 100 MHz 运行的时间；此处未作多种子统计或上板计时。[实际测量脚本](paper_physical_final_route.py) · [Vivado Tcl](paper_physical.tcl)
