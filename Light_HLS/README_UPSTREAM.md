# LLVM-18-Tutorials

This repository is an updated version of [LLVM-11-Tutorials](https://github.com/zslwyuan/LLVM-11-Tutorials), where you can find a more detailed overview of the project.

Here I mainly record the differences between **LLVM-11** and **LLVM-18**. The concrete changes are documented inside each experiment folder.

I’m happy to discuss—suggestions are welcome (jgeab@connect.ust.hk). If you find this repository useful, a star would be appreciated.

## Before Running the Experiments

> **Auxiliary File Settings**: _To avoid potential warnings from VS Code, it is recommended to set the configurations in `c_cpp_properties.json` as follows:_

- `includePath`:
  - `${workspaceFolder}/\*\*`
  - `/usr/local/include/`
- `cStandard`: `c17`
- `cppStandard`: `c++17`

> **CMake Path**: _The compilation logic can be complex within these folders. Here is an explanation:_

- The pass source codes can be found within the `Implementations` folder.
- The specific experiments testing each pass can be found in the `Tests` folder.
- The compiled libraries can be found in `Tests/LLVM_Learner_Libs`. Remember to include the corresponding passes in the relevant CMake files.
- The `App` folder contains the main pass-debugging benchmarks.
- The `HLS_Data_Lib` provides the timing and resource data needed for Light-HLS’s early-stage QoR estimation.
