# Standalone Light-HLS migration — 2026-11-10

The Light-HLS component from `/home/eeuser/Desktop/DAPO_II/Light_HLS` is now available as a standalone workspace at `/home/eeuser/Desktop/Light-HLS`. The original source workspace is retained. The destination repository is [gjskywalker/Light-HLS](https://github.com/gjskywalker/Light-HLS), branch `main`.

## Snapshot and scope

- Source DAPO-II HEAD: `024e088d4f53f863e9cf1b4558d222927232963c`.
- The [SHA-256 manifest](source_manifest.csv) covers **740 entries**: 701 regular files and 39 relative symlinks. **738 entries match exactly**; the two build-only differences are described below. No copied symlink is broken.
- Compiler passes, scheduling/binding/RTL sources, examples, board/operator data, test fixtures, and regression runners are included. Helpers ignored by the original DAPO-II Git configuration are explicitly included here, so the original Git revision alone is insufficient to reconstruct this snapshot.
- Backend design notes and historical baseline evidence are retained. These dated archives are separate from the fresh validation below.
- Build trees, static libraries, old compiler binaries, generated RTL/run outputs, and local agent/editor configuration are excluded from Git. Two historical CMake-generated Makefiles and the compiled `App/2dloop2darray_pl_apint/test` example are also excluded. They can be regenerated rather than depending on old absolute paths.
- DAPO's RL/GNN training stack and experiment controller remain in the original repository. Some historical DAPO integration tests depend on that external workspace; they are not part of the standalone regression claim.

The inherited GPL-3.0 license, upstream README, and root `HLS_Data_Lib -> Light_HLS/HLS_Data_Lib` alias are preserved. [Source provenance](../SOURCE_PROVENANCE.md) distinguishes this extraction from the earlier DAPO-II integration.

## Build portability

The migration snapshot contains changes to two build files, with no scheduling or RTL algorithm changes:

1. `Light_HLS/BuildLibs.sh` uses quoted paths, direct CMake source/build directories, a validated parallel-job option, and forwards package/compiler settings.
2. `Light_HLS/Tests/Light_HLS_Top/CMakeLists.txt` passes the chosen LLVM, Polly, C/C++ compiler, build type, and job count to the nested learner-library build and tracks its build-description dependencies.

All learner libraries and the compiler were rebuilt locally, rather than copied from DAPO-II:

```bash
Light_HLS/Tests/Light_HLS_Top/Build.sh \
  -DLLVM_DIR=/usr/local/lib/cmake/llvm \
  -DPolly_DIR=/usr/local/lib/cmake/polly \
  -DHI_USE_LLD=ON -DHI_LEARNER_BUILD_JOBS=8
```

Environment: LLVM/Clang 18, GCC 11.4, CMake 3.22.1, Python 3.10.12, Icarus Verilog 11.0. Exact versions, command, executable fingerprint, and inherited compiler warnings are in [environment.json](validation/environment.json).

## Fresh functional validation

The final full run passed **27/27 suites** with the freshly built compiler. It covers C/RTL comparisons (plus native lowered-IR comparisons where applicable), LLVM verification, scalar and memory interfaces, pipelining, array partitioning, storage, FIFO/PIPO events, DATAFLOW completion, transaction/reset/stall behavior, and evaluation contracts. The App capability matrix separately checks both supported generation and expected rejection; successful generation alone is not treated as functional equivalence.

```bash
python3 Light_HLS/Tests/run_regressions.py \
  --light-hls-top Light_HLS/Tests/Light_HLS_Top/build/Light_HLS_Top \
  --output "$PWD/runs/migration_20261110/regressions_final" --keep-going
```

The final suites took approximately 105 seconds in total. The capability matrix covers 37 App directories: 24 generation/syntax passes, 12 expected rejections, and one not-applicable directory. Additional existing RTL-identity and RTL-verifier unit tests passed, five tests each. Generated artifacts and full logs remain locally under `runs/migration_20261110/`; compact [regression results](validation/local_regressions.json), [capability results](validation/app_capability_matrix.json), and other evidence are retained in [validation/](validation/).

### Preserved initial simulator failure

The first full run was **26/27**, with `QoRPressure` failing because the external Icarus `vvp` process received **SIGSEGV** while running `dsp_dataflow_100MHz/queued`. It did not report an RTL assertion failure. Its root cause remains undetermined.

Without changing the compiler, generated RTL, testbench, or acceptance criteria, the exact queued simulation artifact passed **20 consecutive runs**. A separate full QoRPressure rerun passed **4/4 cases**, and the final full regression passed **27/27 suites**. The first failure, reproductions, and final result are preserved; no automatic retry or relaxed assertion was introduced. A green final run does not establish that the simulator crash has been fixed.

## Synthesis and limitations

A fresh `backend_scalar` synthesis smoke passed with **Vivado 2025.2**, U250 part `xcu250-figd2104-2L-e`, and a 100 MHz target. Measured out-of-context synthesis utilization was **138 LUTs, 135 FFs, 0 DSPs, and 0 BRAM tiles**. Synthesis WNS was +5.912 ns under the recorded clock and I/O budgets. The [machine-readable synthesis result](validation/scalar_synthesis.json), Tcl, XDC, log excerpt, and fingerprints are retained in `validation/`. This is RTL synthesis only; no routed Fmax, place-and-route, or board execution is claimed.

The result validates the supported subset and exercised tests, rather than arbitrary C/C++ inputs. General floating point, AXI, and unrestricted dynamic operation scheduling remain outside the established backend coverage. Three inherited missing-return compiler warnings were observed and recorded; this extraction does not silently change those unrelated passes. Exact equivalence for all inputs and physical timing closure are separate future validation tasks.

## Review documents

- [HLS scheduling, 2024–2026](../research/01_hls_scheduling_2024_2026.md).
- [Agent-driven algorithm discovery](../research/02_agent_algorithm_discovery.md).

These are reading and experiment-design notes for 2026-11-11 review. No automated scheduling-discovery search was started during migration.
