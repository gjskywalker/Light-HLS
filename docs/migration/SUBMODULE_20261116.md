# DAPO-II 固定版本子模块改造（2026-11-16）

本次先比较两个工作区，再把 DAPO-II 中的 Light-HLS 源码副本改为独立主仓的固定版本子模块，保留原有 `Light_HLS/...` 调用入口。

## 差异检查结论

输入状态：独立仓库为 `8c40f30`，DAPO-II 为 `40eb950d1af686e30e9c30fc1cd335949256d3b3`；两个工作区均无 Git 报告的未提交修改。

DAPO-II 仓库比上次多出一个提交，但 **Light-HLS 编译器源码并没有领先于 11 月 13 日同步时的实际输入**。之前源工作区的浮点等修改现在被正式提交到了 DAPO-II，并不代表出现了新的编译器差异。

| 对比范围 | 结果与处理 |
| --- | --- |
| 双方 `Light_HLS/` 的 805 个文件／符号链接 | 796 个一致，9 个差异全部是独立仓库上次保留的构建适配和集成修复；继续保留 |
| 通用验证依赖 | `Dataset/Testbenches/SOURCE_FIXES.md`、`source_fixes.cfg` 在 DAPO-II 中更新；同步到主仓，修复源码的注册哈希保持一致 |
| 其余已共享依赖 | 源码夹具、独立验证器、浮点资源表生成工具与主仓一致 |
| RL/GNN 与训练控制器 | 由 DAPO-II 继续管理，不迁入编译器主仓 |

本次新增两项主仓适配：验证器显式接受 `--project-root`；为 `App/backend_types` 空占位目录加入受 Git 跟踪的 README。后者解决了首次全新子模块检出暴露的能力矩阵目录缺失问题，没有将其改为功能验证通过项。

[逐文件清单](sync_20261116_manifest.csv)记录 891 个既有输入条目及新增占位说明，共 892 项；其中 881 项与 DAPO-II 输入相同，其余为保留适配、显式数据集路径和占位目录修复。原 11 月 13 日清单继续保留为历史基线。

## 最终目录与代码归属

```text
DAPO_II/
  .gitmodules
  third_party/Light-HLS/              # 固定提交的 submodule
    Light_HLS/                       # 主仓中的编译器源码
  Light_HLS -> third_party/Light-HLS/Light_HLS
  HLS_Data_Lib -> Light_HLS/HLS_Data_Lib
  scripts/check_light_hls.py
  Models/                            # DAPO 的模型与搜索代码
  Dataset/                           # DAPO 的实验数据
  experiments/                       # DAPO 的实验控制器
```

父仓库删除原源码副本的索引记录，使用 `160000` gitlink 固定子模块版本。兼容入口是受 Git 跟踪的相对符号链接；旧板卡库入口继续指向同一套子模块数据。

旧 `Light_HLS` 目录连同构建产物、未跟踪验证脚本整体保存在本机：

```text
/home/eeuser/Desktop/DAPO_II/.local_backups/light_hls_before_submodule_20261116/
```

备份目录被父仓库忽略，不参与新的构建。本次在新子模块中重新构建，没有跨目录复用旧 CMake 缓存。

## DAPO 接口调整

- `verify_rtl.py --project-root <DAPO_II_ROOT>` 显式查找 DAPO 专用基准数据。其默认值继续支持独立项目自己的布局；数据不存在时给出明确参数错误。
- `experiments/dapo_ii_pass_order/run_experiment.py` 调用验证器时传入父项目根目录。
- `scripts/check_light_hls.py` 检查父仓库索引记录的提交、子模块实际 HEAD、源码脏状态，以及两个兼容链接。可输出 JSON 作为实验版本记录。
- `experiments/dapo_ii_cross_validation/run_batch.py` 自动将检查器结果写入 `protocol.json` 的 `light_hls_revision` 字段。开发中的脏状态会被记录，不能被误认为是干净固定版本。
- 父仓库中针对原源码副本的过时忽略规则已移除；编译器构建产物由子模块自己的 `.gitignore` 管理。

已有实验结果不重写。切换编译器版本后应新建输出目录；旧流程对二进制／验证器哈希不一致的拒绝检查继续有效。

## 实际验证

| 检查 | 结果 |
| --- | --- |
| 从 DAPO-II 的旧入口构建 | 新子模块中的 `Light_HLS_Top`、`Light_HLS_DAPO` 均成功 |
| 核心回归 | 27／27 组最终通过：完整运行通过 26 组，补齐占位目录后 AppCapability 定向复验通过 |
| 能力矩阵 | 39 个目录：28 个生成／语法检查通过，10 个预期拒绝，1 个不适用 |
| 浮点 FIFO | FP16／FP32／FP64 均通过，包括停顿与复位检查，验证第三方 RTL 的相对定位 |
| 固定版本检查器 | 6 项真实临时 Git 仓库测试通过：干净版本、忽略构建产物、源码脏状态、未暂存版本变化、已暂存新 gitlink、断裂链接／未初始化场景 |
| 交叉验证接口 | 28 项通过 |
| RL 适配器 | 15 项通过，使用现有 GRL Python 环境 |
| DAPO 专用验证器 | 5 项测试通过；另通过旧链接调用，显式指定 DAPO 根目录，完成 substring 的两组 UBSan 源码检查 |
| 实际批量控制器 | `Testbenches_random24` 的 baseline-only 流程完成；两组源码／native IR／RTL 比对通过，分类为 `scheduled_exact`，工具哈希保持不变 |

核心汇总明确标记为多次运行合并，首次空目录失败记录单独保存。该修复只补充目录说明和能力矩阵描述，编译器源代码及构建二进制未改变。

构建与主要功能检查使用代码提交 `665ed92`；能力矩阵复验使用补齐占位目录后的 `43a11bf`。随后主仓只增加本报告及验证证据，DAPO-II 最终 gitlink 固定到包含这些文档的主仓提交。**最终版本以 DAPO-II 提交内的 gitlink 为准**，不追随远端分支自动漂移。

验证期间父仓库改造尚未提交，因此本次 batch protocol 如实保留原父提交和 `parent_index_pin_differs_from_head: true`。这表示待提交的 gitlink 已进入索引，不是最终父提交号。

本次没有进行全量 RL 搜索或重新做 Vivado 综合；验证目标是依赖结构、路径兼容和现有功能保持正确。证据见 [validation_20261116](validation_20261116/README.md)，完整运行目录保留在 DAPO-II 的 `runs/submodule_20261116/`。

## 日常流程与复现

完整命令见 [SUBMODULE_WORKFLOW.md](SUBMODULE_WORKFLOW.md)。基本规则是：编译器改动先在 Light-HLS 分支提交、验证并推送，再让 DAPO-II 更新到明确提交。

已有 DAPO-II 检出更新后执行：

```bash
git submodule update --init --recursive
python3 scripts/check_light_hls.py
```

新检出使用 `git clone --recurse-submodules`。不要使用 `git submodule update --remote` 恢复实验版本，也不要重新建立可编辑的编译器副本。
