# Source provenance

## 2026-11-16 子模块改造

核查 DAPO-II 的 `40eb950` 后，确认编译器源码与上次同步输入一致；保留独立仓库的构建及集成修复，同步两份更新后的 Testbench 元数据。DAPO-II 改为通过 `third_party/Light-HLS` 的固定 gitlink 引用本仓库，并保留相对链接兼容原有入口。差异清单和验证结果见 [改造报告](../migration/SUBMODULE_20261116.md)，开发规则见 [版本管理说明](../migration/SUBMODULE_WORKFLOW.md)。下文为此前同步快照的历史记录。

## 2026-11-13 增量同步

本次从 `/home/eeuser/Desktop/DAPO_II` 的实际工作区同步 Light-HLS 更新。源仓库 HEAD 仍为 `024e088d4f53f863e9cf1b4558d222927232963c`，新增功能主要存在于未提交修改及未跟踪文件中，因此以 [本次哈希清单](../migration/sync_20261113_manifest.csv)记录实际输入与最终文件，不能仅用源 Git 提交复现。

同步内容包括浮点后端、Berkeley HardFloat RTL 及 BSD-3-Clause 授权、浮点资源表、循环／内存合法化修复、新示例和验证器。补充了 `Dataset/Testbenches`、独立的 `experiments/dapo_ii_cross_validation/verify_design.py`，以及 `experiments/floating_point` 的资源表生成工具，确保新增回归不依赖旧工作区。RL/GNN 训练代码、特征提取器、实验控制器、旧运行结果和编译产物仍留在 DAPO-II。

保留独立仓库的两处构建适配，并在同步后修复 DATAFLOW 任务边界、固定循环越界证明和不支持的 `ap_int` 接口拒绝；同时适配能力矩阵及报告字段检查。详细差异和本地验证结果见 [同步报告](../migration/SYNC_20261113.md)。下文为 2026-11-10 及更早的历史来源记录。

## Standalone Light-HLS extraction — 2026-11-10

This repository contains the `Light_HLS/` component from `/home/eeuser/Desktop/DAPO_II`, whose source repository HEAD was `024e088d4f53f863e9cf1b4558d222927232963c`. The source directory uses an underscore; the standalone repository name uses a hyphen. The source workspace is preserved.

The extraction includes the compiler, examples, board/operator libraries, regression fixtures and runners (including helpers ignored by the original repository), relevant backend engineering notes, and historical baseline evidence. It does not include the DAPO RL/GNN training stack, experiment controller, presentations, old compiler binaries, build trees, or generated run directories. Therefore the source Git revision alone does not reproduce all copied validation helpers. The [file manifest](../migration/source_manifest.csv) records their actual source and migrated SHA-256 hashes.

Two build files in the migration snapshot differ from the source: `Light_HLS/BuildLibs.sh` and `Light_HLS/Tests/Light_HLS_Top/CMakeLists.txt`. They pass the selected LLVM/Polly/compiler/build settings to the nested learner-library build, support a configurable job count, and quote paths. Scheduling, binding, and RTL generation sources are unchanged. Historical CMake-generated Makefiles and a compiled example executable are kept locally but excluded from the standalone Git snapshot.

The canonical library remains `Light_HLS/HLS_Data_Lib/`, with a relative repository-root `HLS_Data_Lib` symlink. Existing relative board aliases are preserved. The inherited GPL-3.0 license is retained at the repository root and under `Light_HLS/`. See the [migration report](../migration/MIGRATION_20261110.md) for fresh validation and the research reading notes under `docs/research/` for separately prepared analysis.

## Historical DAPO-II provenance — 2026-08-08

The following sections describe the earlier integration into DAPO-II. Paths and included components in these historical sections refer to DAPO-II, rather than to this standalone repository. DAPO-II used copied source snapshots instead of Git submodules.

### DAPO

- Source directory: `/home/eeuser/Desktop/DAPO`
- Git revision: `be65d983c2933a70ca82fbd73a26fcca47aeaaaf` (`main`)
- Included: `Models/RLModels`, `Models/GNNModels`, `Analysis`, `Dataset`, `build_analysis.sh`, `config.txt`, `generate_ir.py`, `requirements.txt`, `framework.png`, and the upstream README.
- The GNN snapshot includes six local `.pth` checkpoints and the current graph/HGED intermediate assets. Upstream ignores these directories, so the Git revision alone cannot reproduce them.
- Excluded: `Analysis/build`, `Analysis_tools`, Python bytecode and caches, `Raw_IRs`, and historical run outputs. `build_analysis.sh` regenerates the analysis binaries.

### LLVM-18-Tutorials / Light-HLS

- Source directory: `/home/eeuser/Desktop/LLVM-18-Tutorials`
- Git revision: `70e755628a3d00c4de6bf0da88596ebd8801421c` (`main`)
- Included under `Light_HLS/`: `Implementations`, `HLS_Data_Lib`, `Tests/Light_HLS_Top`, the `Tests/LLVM_Learner_Libs` build description, `App`, the source-pragma test runner, `BuildLibs.sh`, LICENSE, and the upstream README.
- Excluded: all `build/` directories, `Tests/LLVM_Learner_Libs/libHI_*.a`, the existing 928 MB `Light_HLS_Top` executable, Python caches, and LLVM tutorial experiments unrelated to the Light-HLS main program.

### Historical local compatibility decision

The canonical FPGA/HLS library is `Light_HLS/HLS_Data_Lib/`. The repository-root `HLS_Data_Lib` is a symbolic link to that directory. This arrangement satisfies all of the following requirements:

- DAPO resolves `../../HLS_Data_Lib/` from `Models/RLModels`, as specified in `config.txt`.
- Light-HLS resolves `../../../HLS_Data_Lib/` from its test/build directory.
- Both estimators consume exactly the same instruction and board data, supporting a fair comparison.
