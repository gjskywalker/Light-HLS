# Light-HLS

Light-HLS is an experimental LLVM/Clang 18 based compiler that translates a supported subset of C/C++ into synthesizable Verilog. It includes dependence analysis, operation scheduling, resource binding, loop pipelining, task DATAFLOW, RTL generation, and optional Vivado synthesis.

This standalone repository was extracted from the `Light_HLS/` component of DAPO-II on 2026-11-10. The original component layout is retained so existing scripts and documentation keep their relative paths. Source, regression helpers, examples, board libraries, and engineering notes are included; build directories are regenerated locally.

## Layout

```text
Light_HLS/
  App/                    C/C++ examples and reference testbenches
  Implementations/        LLVM passes and RTL backend
  Tests/                  Compiler entry point, builds, and regressions
  HLS_Data_Lib/           Operator timing/resource tables and board metadata
  BuildLibs.sh            Learner-library build entry point
HLS_Data_Lib -> Light_HLS/HLS_Data_Lib
docs/
  RTL_BACKEND_DESIGN.md    Supported subset and backend contracts
  migration/              Extraction provenance and fresh validation evidence
  research/               Scheduling and agent algorithm-discovery reading notes
```

## Build

Required: LLVM **18** with Clang and Polly development libraries, a C++17 compiler, CMake, and Make. Put the matching `clang`, `clang++`, `opt`, and `llvm-config` on `PATH`. The current build links LLVM/Clang libraries statically and needs several GB of disk space. `lld` is optional.

```bash
Light_HLS/Tests/Light_HLS_Top/Build.sh \
  -DLLVM_DIR=/usr/local/lib/cmake/llvm \
  -DPolly_DIR=/usr/local/lib/cmake/polly \
  -DHI_USE_LLD=ON
```

The executable is `Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top`. Adjust the two package paths for your LLVM installation. Omit `-DHI_USE_LLD=ON` if `lld` is unavailable.

## Generate RTL

From the repository root:

```bash
export HI_HLS_LIB_PATH="$PWD/Light_HLS/HLS_Data_Lib"
Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  Light_HLS/App/backend_scalar/backend_scalar.c backend_scalar \
  --board u250 --freq 100MHz --synthesis disable \
  --output "$PWD/runs/scalar"
```

The explicit library path also supports input files outside the repository. Frequency requires a unit, such as `100MHz`. Open `runs/scalar/reports/report.rpt` for the report index and `runs/scalar/reports/rtl/top.v` for generated RTL. Scheduling, binding, and summary reports are written alongside RTL. Vivado measurement is enabled with `--synthesis enable` and requires a working Vivado installation.

## Verify

The core regression runner requires Python 3.10+, Icarus Verilog (`iverilog` and `vvp`), `jq`, and the LLVM tools above. It runs **27 suites** covering source/IR/RTL comparisons, scalar and memory interfaces, pipelining, partitioning, storage, FIFO/PIPO events, DATAFLOW, resets, transaction acceptance, and evaluation contracts.

```bash
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output "$PWD/runs/regressions" --keep-going
```

Use a new or empty output directory. The runner emits JSON, CSV, RPT, and per-suite logs. Its capability matrix includes supported generation and expected rejection; generation success alone does not establish functional equivalence. Fresh migration results are recorded in [the migration report](docs/migration/MIGRATION_20261110.md).

## Supported scope

The backend supports integer scalar computations, branches and sequential loops, bounded memory interfaces and local RAM, supported canonical fixed-trip loop pipelines, array partitioning, storage binding, and direct-call DATAFLOW graphs with bounded FIFO/PIPO channels. U250 is the primary characterized target; Artix-7 data is also retained.

Floating-point RTL and general C++/AXI coverage are incomplete. Unsupported cases must be checked against the [backend design and rejection rules](docs/RTL_BACKEND_DESIGN.md). The timing/resource estimator is a heuristic; its predictions are distinct from RTL simulation cycles and measured synthesis results.

The optional `Light_HLS_DAPO` target and `Tests/DAPOII` preserve the DAPO integration interface. Some historical DAPO integration tests require the original experiment archives and are outside the standalone 27-suite runner. The RL/GNN training stack is maintained in the original DAPO-II repository.

## Research notes

Two Chinese reading notes were prepared for review on 2026-11-11:

- [HLS scheduling, 2024–2026](docs/research/01_hls_scheduling_2024_2026.md): static, dynamic, and hybrid approaches, representative papers, and testable directions for Light-HLS.
- [Agent-driven algorithm discovery](docs/research/02_agent_algorithm_discovery.md): AlphaEvolve, FunSearch, ReEvo, and a proposed evaluation-driven scheduling search loop.

These notes distinguish published results from proposed experiments. An automated algorithm-discovery run has not been started as part of the migration.

## Provenance and license

See [source provenance](docs/SOURCE_PROVENANCE.md), the [migration report](docs/migration/MIGRATION_20261110.md), and the preserved [upstream README](Light_HLS/README_UPSTREAM.md). Source remains under the inherited [GPL-3.0 license](LICENSE). Dated optimization notes and baseline archives describe historical experiments; they are separate from fresh migration validation.
