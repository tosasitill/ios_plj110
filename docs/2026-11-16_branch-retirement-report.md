# 24 小时无新提交分支清理

## 筛选与结果

按 **2026-11-16 18:47:04 +0800** 的固定快照筛选，截止时间为 **11-15 18:47:04**，排除 `main` 和本轮临时分支。

本地与真实远端分支按名称合并；同名但不同 head 时检查两者。判断依据为所有可达 commit 的最新 committer 时间，而非只看最后一个文件改动、作者时间或同名远端是否存在。执行期间不滚动扩大删除范围。

| 符合条件的分支 | 最新提交时间（+0800） | 原始 head | 本地处理 | 远端处理 |
| --- | --- | --- | --- | --- |
| `feat/audio-dsp-boot` | 11-13 18:04:09 | `1a76d62c` | 工作树归档、分支删除 | 已删除 |
| `feat/audio-ipi` | 11-14 23:18:18 | `6717e8af` | 原无本地分支 | 已删除 |

两条分支都已是初始 published main `e0e7815c` 的祖先，未合入提交数为 0。实际 `git merge --ff-only <head>` 均返回 `Already up to date`，前后 tree hash 相同；main push 返回 `Everything up-to-date`。不需要补造 history-only merge，也没有新增生产代码。

相机分支的最新提交在 11-15 22 点左右，筛选时不足 24 小时；HEVC 和 SMS 也有近期提交，本轮不合并/删除它们。远端 CALL/SMS 分支同样按快照筛选，不能只因已合 main 就忽略用户的 24 小时条件。

## 保存与删除顺序

1. `fetch --prune`、真实 `ls-remote --heads`、可达时间与祖先检查。
2. 确认选中的原始 head 已包含在已推送 main，执行无变化的 ff merge/push 复核。
3. 确认音频 worktree tracked clean，保存全部 ignored 文件后清理 worktree/local branch。
4. 两条远端删除使用 **atomic push + 每个 ref 的原始 hash lease**；若分支期间新增提交则拒绝删除。实际 push exit0，两条远端均已删除。
5. 再次 fetch/prune、ls-remote 复核；记录其它任务的同期变化，不重置或代为删除它们。

原音频树共有 1166 项 ignored 文件记录，其中 `.local` 是共享输入 symlink，仅移除该树的链接、保留目标。其余 **1165 个文件**全部保存：

- 16 项整个输出目录/日志无覆盖迁至主 checkout 的同名 `x9/out/<name>`，例如 `x9/out/x9-dspboot2-20261113/boot.img`。
- 29 项额外生成输入、配置及 pycache 保留到 `x9/out/stale-branch-archive-20261116/audio-dsp-boot/<原相对路径>`。

没有更改共享 `.local`、旧镜像内容、设备分区或备份；没有新 kernel/boot/CI 或实机测试。

## 库存与并行状态

删除后 r2 快照：本地 **7 条（排除本轮临时分支，含 main）**、远端 **26 条（含 main）**，同名去重 **29 条**。独立未 push 提交 **45 个**：camera-ios-preview 33、camera-source 12。

期间三条 HEIF 辅助本地分支消失、HEVC/SMS 新增并发布提交、根 checkout 状态变化，均不是本轮删除动作。原 retirement wrapper 在删除成功后因保守的“其它 ref 不变”断言 exit1；库存 r1 因同期提交让预期数量暂时变化而 exit1。失败保留，r2 改为记录真实库存并 exit0，没有重复删除或恢复其它工作。

后续 main 的同期 SMS `2c34c540` 由本轮工作树正常 ff 吸收，原报告/NOTES 均保留。最终收尾库存可能受其它任务继续影响，以最后的 `finalization.json` 为准。

## 复核与恢复

```sh
git ls-remote --heads origin refs/heads/feat/audio-dsp-boot refs/heads/feat/audio-ipi
git branch --list feat/audio-dsp-boot feat/audio-ipi
git merge-base --is-ancestor 1a76d62c origin/main
git merge-base --is-ancestor 6717e8af origin/main
```

前两条应没有输出，后两条应 exit0。若需要重新检查原分支，可用其已保存 hash 新建分支，例如 `git branch feat/audio-dsp-boot 1a76d62c`；无需从归档恢复 git 历史。

产物：`x9/out/stale-branch-cleanup-20261116/`，包括 `inventory-initial.json`、`inventory-after-retirement.json`、`retirement.json`、实际脚本/失败记录和 `finalization.json`。副本 `report.md` 可在尚未切换到最新 main 的 checkout 直接打开。
