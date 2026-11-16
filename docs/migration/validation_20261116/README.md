# 2026-11-16 子模块改造验证证据

[改造报告](../SUBMODULE_20261116.md)解释输入差异、固定版本结构和验证范围。

| 文件 | 内容 |
| --- | --- |
| [difference_review.json](difference_review.json) | 两个工作区的初始提交、805 个组件条目对比、9 处保留适配及两份上游元数据更新 |
| [environment.json](environment.json) | 构建命令、工作目录、二进制哈希、测试时的代码／元数据版本和备份路径 |
| [build_tail.txt](build_tail.txt) | 两个入口在新子模块中构建成功的日志末尾 |
| [core_full_run.json](core_full_run.json) | 完整 27 组运行；26 组通过，AppCapability 因 Git 不保留空目录而失败 |
| [core_regressions_combined.json](core_regressions_combined.json) | 完整运行与能力矩阵复验的合并汇总，27／27 最终通过 |
| [app_capability_matrix.json](app_capability_matrix.json) | 占位目录修复后的全部 39 个目录检查结果 |
| [fp_fifo.json](fp_fifo.json) | 三种浮点精度的 DATAFLOW FIFO 停顿／复位测试 |
| [pin_tests.txt](pin_tests.txt) | 6 项版本检查器回归测试 |
| [cross_validation_tests.txt](cross_validation_tests.txt) | 28 项交叉验证接口测试 |
| [rl_adapter_tests.txt](rl_adapter_tests.txt) | 15 项 RL 适配器测试 |
| [verifier_tests.txt](verifier_tests.txt) | 5 项 DAPO 专用验证器测试 |
| [verifier_reference.json](verifier_reference.json) | 显式父项目根目录下 substring 的两组源码检查；本项不包含 RTL 比对 |
| [batch_protocol.json](batch_protocol.json) | baseline-only 集成检查的实际命令、输入和工具哈希，以及子模块版本记录 |
| [batch_smoke.json](batch_smoke.json)、[batch_completion.json](batch_completion.json) | random24 的源码／native IR／RTL 验证、周期分类和工具未变检查 |

全量日志与构建／仿真产物保留在 `/home/eeuser/Desktop/DAPO_II/runs/submodule_20261116/`，不加入源码提交。
