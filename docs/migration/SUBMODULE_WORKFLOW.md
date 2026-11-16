# Light-HLS 主仓与 DAPO-II 的版本管理

Light-HLS 编译器、RTL 和通用回归由本仓库维护。DAPO-II 通过 `third_party/Light-HLS` 子模块固定到一个提交，保留 `Light_HLS -> third_party/Light-HLS/Light_HLS` 相对链接兼容已有调用路径。

## 获取与构建

```bash
git clone --recurse-submodules git@github.com:gjskywalker/DAPO_II.git
cd DAPO_II
# 已有检出在更新父仓库后执行：
git submodule update --init --recursive
python3 scripts/check_light_hls.py

Light_HLS/Tests/Light_HLS_Top/Build.sh \
  -DLLVM_DIR=/usr/local/lib/cmake/llvm \
  -DPolly_DIR=/usr/local/lib/cmake/polly \
  -DHI_USE_LLD=OFF
export HI_HLS_LIB_PATH="$PWD/Light_HLS/HLS_Data_Lib"
```

构建输出属于子模块，由其 `.gitignore` 管理。两个检出可以分别构建；不要跨检出移动 CMake 缓存。子模块默认 detached HEAD 是固定提交的正常状态。

## 修改编译器

可以在独立 Light-HLS 工作区开发，也可以在 DAPO-II 的子模块内开发。后者先创建功能分支：

```bash
git -C third_party/Light-HLS switch -c scheduling-change
# 修改、运行受影响测试，然后在子模块内提交并推送。
git -C third_party/Light-HLS add <files>
git -C third_party/Light-HLS commit -m "Describe compiler change"
git -C third_party/Light-HLS push -u origin scheduling-change
```

编译器修改合入 Light-HLS 主分支后，再更新 DAPO-II。不要在父仓库重新加入编译器源码副本。

## 更新 DAPO-II 固定版本

先确认两个工作区的修改已提交或妥善保存，再选择经过验证且已推送的提交：

```bash
git -C third_party/Light-HLS fetch origin
git -C third_party/Light-HLS checkout --detach <LIGHT_HLS_COMMIT>
# 构建并运行 DAPO 接口／小规模集成验证。
git add third_party/Light-HLS
python3 scripts/check_light_hls.py
git commit -m "Update pinned Light-HLS revision"
git push
```

`check_light_hls.py` 检查子模块 HEAD、父仓库索引记录的 gitlink、源码工作区状态及兼容链接。暂存新 gitlink 后检查可以验证准备提交的版本；尚未暂存的版本变化会被报告为不匹配。构建产物按忽略规则处理。

默认使用 `git submodule update --init --recursive` 恢复父仓库指定的版本。`git submodule update --remote` 会追随远端最新提交，不用于固定版本的日常恢复。

## 数据与验证器的边界

- 编译器和通用测试夹具属于 Light-HLS；RL/GNN、训练数据和实验控制器属于 DAPO-II。
- DAPO 调用 `Tests/DAPOII/verify_rtl.py` 时显式传入 `--project-root <DAPO_II_ROOT>`。验证器不能再从子模块物理路径推断父项目的数据集位置。
- 独立仓库中的 `Dataset/Testbenches` 是通用验证夹具；DAPO-II 的同名数据集继续由实验项目管理。需要调整共同夹具时明确提交到两个项目，并核对注册表哈希。
- 实验应记录父仓库提交、Light-HLS gitlink／HEAD，以及已有的二进制、IR 和配置哈希。父仓库 gitlink 是版本依据，机器上的绝对路径不是版本依据。

## 首次改造的保留措施

首次替换 DAPO-II 的源码目录前，将旧目录（含未跟踪的验证脚本及构建产物）整体移动到被忽略的 `.local_backups/light_hls_before_submodule_20261116/`。保留原父仓库提交 `40eb950`，并记录两个工作区的比较清单。不要将备份重新加入 Git。

如需返回旧结构，应先保存子模块中的新修改，在独立工作区检出改造前的 DAPO-II 提交；不要直接删除子模块或覆盖备份。
