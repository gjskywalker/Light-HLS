# 2026-11-13 同步验证证据

同步范围、修复理由和复现命令见 [同步报告](../SYNC_20261113.md)。以下文件是本次在独立仓库运行取得的结果；其中路径可能包含验证机器的绝对目录，复现时请替换。

| 文件 | 含义 |
| --- | --- |
| [environment.json](environment.json) | 工具版本、最终构建命令、两个编译器二进制 SHA-256、输入快照说明 |
| [core_regressions_combined.json](core_regressions_combined.json) | 27 组最终通过状态，显式标记为合并运行并记录每组来源 |
| [core_full_run.json](core_full_run.json) | 最终二进制的完整运行，25 组通过、两组报告字段断言失败 |
| [core_metadata_rechecks.json](core_metadata_rechecks.json) | AppCorrectness 通过；RTL_Backend 首次断言更新后仍失败，记录保持原样 |
| [rtl_metadata_recheck.json](rtl_metadata_recheck.json) | 补全 local RAM 报告字段后的 RTL_Backend 通过记录 |
| [core_initial.json](core_initial.json)、[extended_initial.json](extended_initial.json) | 集成修复前的运行，包含真实失败 |
| [extended_final.json](extended_final.json) | 最终二进制的 8 组扩展验证命令、脚本哈希、耗时和状态 |
| `floating_ir.json`、`floating_classic.json`、`native_half.json` | 浮点算术与 APFloat oracle 比对结果 |
| `fp_conversions.json`、`fp_math.json`、`fp_scheduled.json`、`fp_fifo.json` | 格式转换、数学操作、调度周期、FIFO 停顿／复位结果 |
| [expression_grouping.json](expression_grouping.json) | 9 个表达式示例的源码／IR／RTL 比对与周期检查 |
| [predicated_cases.json](predicated_cases.json) | 7 个条件访存、指针 PHI、静态查表与相关循环示例的命令、输入哈希及验证结果 |
| [source_repairs.json](source_repairs.json) | 8 个修复源码的独立 oracle 与 ASan／UBSan 检查，不能等同于 RTL 验证 |
| [app_capability_matrix.json](app_capability_matrix.json)、[文本矩阵](app_capability_matrix.rpt) | 最终单独重跑的 39 目录能力矩阵；Functional 列与生成成功分别记录 |
| [fp_fifo_synthesis/vivado_report.json](fp_fifo_synthesis/vivado_report.json) | 新运行的综合测量；同目录保存 Tcl、XDC、utilization 和 timing 文本报告 |
| [copied_files.json](copied_files.json) | 91 个组件复制项、86 个依赖项及两处保留构建适配 |
| [document_link_updates.json](document_link_updates.json) | 文档移动后修复相对链接的记录 |
| [source_changes_during_validation.json](source_changes_during_validation.json) | 校验结束时两份源注册表／说明文件已消失；独立仓库保留复制快照 |
| `*_failure.txt`、`initial_failures.txt`、`lld_link_failure.txt` | 关键失败的日志摘录 |
| [successful_build_tail.txt](successful_build_tail.txt) | 最终 GNU ld 构建日志末尾 |

完整日志、生成的 RTL、仿真可执行文件和综合 DCP 留在本机 `runs/sync_20261113/`，未作为源码提交。旧迁移记录及历史基线与本次证据分开保存。
