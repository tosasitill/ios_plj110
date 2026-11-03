> 2026-11-01 脚本整合：`build.py plugins|all --revision N` 统一编译 PanVK/GeneralMetal、打包插件（`package-plugins.py`，从 git 历史恢复的 pan171-174 打包逻辑，已用 207 插件包逐字节核对）并更新内核白名单；用户态脚本的 Xcode/Homebrew 路径改读 `local.json` 的 `developer_dir`/`homebrew`；一次性 prepare 与分阶段局部编译脚本移入 `legacy/work/x9-metal-render/`；mailbox 根盘预检默认使用 `tools/check_plaintext_root.py` + `root_manifest`。下文的“mailbox ROOT/check-root.sh 缺口”“旧布局路径”已按此处理。

> 2026-11-01 迁移更新：以下是迁移前盘点。当前依赖已集中到仓库内 `.local/`，路径和验证结果见 `BUILD.md`；脚本见 `build/scripts/`（历史脚本 `x9/legacy/` 已于 2026-11-03 删除，可从提交 ca5b8d5 恢复）。新工作区仅保留一份最新的历史安装 root 和当前运行时源码，已删除旧镜像副本及 Rust 缓存。后经用户授权，已删除旧目录中对应的过期镜像、运行时副本和缓存；设备分区备份未改动。

# X9 外部依赖盘点（2026-11-01）

范围：当前配置、构建代码、文件存在性与脚本内容对比；没有构建、运行迁移脚本或操作设备。无同名文件不等于功能未迁移，历史 prepare 脚本尤其如此。

## 当前 kernel / boot 必需

- `/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb/`：`x9/build/local.json` 的 dxb。链接清单使用 **1069 个外部 XNU 对象，全部存在**；另依赖 `xnu/` 头文件/源码、`build/xnu.obj/EXPORT_HDRS/`、`fakeroot/` 和编译工作目录。只迁移仓库的 overlay 不能独立重建内核。
- `/Users/tosasitill/HelioDarwin-x9-lk-preserve/x9/out/pan220-batched-cache-ranges/root-manifest.json`：当前 boot 打包直接读取，文件存在；设备专属数据，保持 git 外存储。

## Metal / PanVK 未完成迁移

旧位置基准：`/Users/tosasitill/HelioDarwin-x9-lk-preserve/work/x9-metal-render/`。

- 第三方源码：`mesa-render-source/`、`mesa-source/`（AIR frontend）、`metal2vulkan-source/` 及多个 `metal2vulkan-pan*-source/`、`libdrm-2.4.133/`。版本与补丁部分已入库，不应简单把所有历史副本一起搬入。
- 构建环境/生成输入：`build-env/`、`build-tools/`、`build-host/`、`build-panvk-host/`、`render-link-inputs/optional-vulkan-entrypoints.rsp` 等。建议重建虚拟环境并固定依赖；不直接搬含绝对路径的构建缓存。
- `x9/build/third_party/00-mesa-panvk-x9.patch` 仍硬编码旧工作区的 libdrm、`MaliMetal/X9/General/` 和 rsp 路径。
- 已入库脚本仍使用 `w=脚本所在目录`、`root=w.parents[1]` 的旧布局。现在位于 `x9/build/scripts/`，会找这里的源码/venv，并把桥接路径解析为 `x9/MaliMetal/X9/General`，实际仓库代码在 `x9/metal/General`。不能因脚本已复制就认定可运行。
- 旧 `build-general-metal.py` 比入库副本多 `--runtime-source`、`x9_runtime_latest` Rust cfg、`X9_RUNTIME_LATEST` C 宏和 `-lcompression`。
- 旧 `package-panvk-native.py` 比入库副本多 `--source-library`。
- `build-pan206-panvk.py`、`build-pan207-panvk.py`、`prepare-pan208.py` 等没有同名入库文件，需结合已有补丁判断应迁移的逻辑。

## root / mailbox / 调试链

- `x9/tools/build_full_ios_root.py:94` 硬编码 `/Users/tosasitill/HelioDarwin/work/ios16-20H392`；这是重建 root 的 Apple 输入，不是普通源码依赖。
- `x9/tools/test_x9_nvram_proxy.py:5` 硬编码旧 dxb/xnu。
- `x9/mailbox/update.py` 仍调用 `ROOT/x9/out/pan97-amfi-aks-only/check-root.sh`。当前 `ROOT=P.parents[2]` 解析到 `/Users/tosasitill`，不是本仓库：路径迁移有缺口。
- 旧工作区 `work/x9-metal-update/`、`x9/out/*/plugin-package/` 保存交付状态；有些 prepare 脚本仅从上一候选复制 plugin-package，不能都当作独立 packager。
- 旧 `work/x9-incremental-root/`、`x9-hfs-recovery/`、`x9-metal-install-*`、`x9-usb-live/`、`x9-thread-symbols/` 仍有恢复、安装、USB 日志与符号化辅助脚本。下表是浅层 Python 脚本对比，不覆盖所有深层历史产物。

## /Users/tosasitill/work

未发现当前 X9 源码引用这个目录。顶层主要是无关 ruc 项目；`entire-trial/` 只有 README.md（hello）。不要与两个 HelioDarwin 目录各自的 work/ 混淆；无关项目内容未扫描。

## 后续优先级

1. 保留并配置独立的 dxb 输入与私有 root manifest。
2. 修正用户态构建的目录解析，合并两个脚本的缺失选项，固定第三方版本及环境生成步骤。
3. 整理 plugin 打包/更新链，修复 mailbox 根目录及 check-root.sh 来源。
4. 按需迁移诊断/恢复工具；设备 dump、序列号、Apple 镜像保持 git 外。

## 旧 work 下浅层 Python 脚本对比

| 旧 work 相对路径 | 与当前 git 已跟踪文件比较 |
| --- | --- |
| `usb-lk-analysis/replay_shim_ep0.py` | 无同名已跟踪文件 |
| `usb-lk-analysis/trace_lk_usb.py` | 无同名已跟踪文件 |
| `usb-lk-analysis/trace_start_unicorn.py` | 无同名已跟踪文件 |
| `usb-lk-analysis/trace_vendor_force_vbus.py` | 无同名已跟踪文件 |
| `x9-about/read_diagnostic_dirs.py` | 无同名已跟踪文件 |
| `x9-amfi-cache-loader/replay_cache_loader.py` | 无同名已跟踪文件 |
| `x9-battery/prepare.py` | 无同名已跟踪文件 |
| `x9-battery/prepare211.py` | 无同名已跟踪文件 |
| `x9-battery/prepare212.py` | 无同名已跟踪文件 |
| `x9-battery/prepare213.py` | 无同名已跟踪文件 |
| `x9-battery/prepare214.py` | 无同名已跟踪文件 |
| `x9-battery/prepare215.py` | 无同名已跟踪文件 |
| `x9-cache-batch/prepare.py` | 无同名已跟踪文件 |
| `x9-cache-manifest/prepare.py` | 无同名已跟踪文件 |
| `x9-cache-publish/prepare.py` | 无同名已跟踪文件 |
| `x9-cache-reuse/prepare.py` | 无同名已跟踪文件 |
| `x9-cryptex/benchmark_ufs.py` | 无同名已跟踪文件 |
| `x9-cryptex/replay_dyld.py` | 无同名已跟踪文件 |
| `x9-dali-compat/review_dali.py` | 无同名已跟踪文件 |
| `x9-decmpfs/extract_all_fixtures.py` | 无同名已跟踪文件 |
| `x9-decmpfs/inventory_compression.py` | 无同名已跟踪文件 |
| `x9-decmpfs/validate_corpus.py` | 无同名已跟踪文件 |
| `x9-gpu-coherent/prepare-pan209.py` | 无同名已跟踪文件 |
| `x9-hfs-recovery/verify_repaired_sources.py` | 无同名已跟踪文件 |
| `x9-incremental-root/compare_journal.py` | 无同名已跟踪文件 |
| `x9-incremental-root/make_digests.py` | 无同名已跟踪文件 |
| `x9-incremental-root/plan_from_scan.py` | 无同名已跟踪文件 |
| `x9-incremental-root/plan_journal_diff.py` | 无同名已跟踪文件 |
| `x9-incremental-root/refine_diff.py` | 无同名已跟踪文件 |
| `x9-incremental-root/restore_journal.py` | 无同名已跟踪文件 |
| `x9-incremental-root/restore_parallel.py` | 无同名已跟踪文件 |
| `x9-incremental-root/restore_scan_direct.py` | 无同名已跟踪文件 |
| `x9-incremental-root/scan_journal.py` | 无同名已跟踪文件 |
| `x9-incremental-root/test_restore_journal.py` | 无同名已跟踪文件 |
| `x9-keybag-shared-data/replay_keybagd.py` | 无同名已跟踪文件 |
| `x9-metal-install-20261029/apply-install.py` | 无同名已跟踪文件 |
| `x9-metal-install-20261029/plan-install.py` | 无同名已跟踪文件 |
| `x9-metal-install-20261029/reconstruct-fast.py` | 无同名已跟踪文件 |
| `x9-metal-install-20261029/reconstruct.py` | 无同名已跟踪文件 |
| `x9-metal-install-pan172/apply-install.py` | 无同名已跟踪文件 |
| `x9-metal-install-pan172/plan-install.py` | 无同名已跟踪文件 |
| `x9-metal-install-pan172/preflight.py` | 无同名已跟踪文件 |
| `x9-metal-install-pan172/reconstruct-fast.py` | 无同名已跟踪文件 |
| `x9-metal-render/build-compiler.py` | 相同：x9/build/scripts/build-compiler.py |
| `x9-metal-render/build-general-metal.py` | 不同：x9/build/scripts/build-general-metal.py |
| `x9-metal-render/build-native-kernel-link.py` | 无同名已跟踪文件 |
| `x9-metal-render/build-pan206-panvk.py` | 无同名已跟踪文件 |
| `x9-metal-render/build-pan207-panvk.py` | 无同名已跟踪文件 |
| `x9-metal-render/build-panvk-ios.py` | 相同：x9/build/scripts/build-panvk-ios.py |
| `x9-metal-render/build-panvk-native-device.py` | 相同：x9/build/scripts/build-panvk-native-device.py |
| `x9-metal-render/build-panvk-native-ios.py` | 不同：x9/build/scripts/build-panvk-native-ios.py |
| `x9-metal-render/build-panvk-native-physical.py` | 相同：x9/build/scripts/build-panvk-native-physical.py |
| `x9-metal-render/build-panvk-native-queue.py` | 相同：x9/build/scripts/build-panvk-native-queue.py |
| `x9-metal-render/build-panvk-native.py` | 相同：x9/build/scripts/build-panvk-native.py |
| `x9-metal-render/build-panvk.py` | 相同：x9/build/scripts/build-panvk.py |
| `x9-metal-render/build-render-cs.py` | 相同：x9/build/scripts/build-render-cs.py |
| `x9-metal-render/build-render-deploy.py` | 相同：x9/build/scripts/build-render-deploy.py |
| `x9-metal-render/build-render-display.py` | 无同名已跟踪文件 |
| `x9-metal-render/build-render-kernel.py` | 无同名已跟踪文件 |
| `x9-metal-render/build-render-user.py` | 相同：x9/build/scripts/build-render-user.py |
| `x9-metal-render/build-resident-kernel.py` | 无同名已跟踪文件 |
| `x9-metal-render/build-v14.py` | 相同：x9/build/scripts/build-v14.py |
| `x9-metal-render/package-panvk-native.py` | 不同：x9/build/scripts/package-panvk-native.py |
| `x9-metal-render/pan200-integrate-render.py` | 无同名已跟踪文件 |
| `x9-metal-render/pan200-integrate-samples.py` | 无同名已跟踪文件 |
| `x9-metal-render/prepare-build.py` | 相同：x9/build/scripts/prepare-build.py |
| `x9-metal-render/prepare-general-runtime.py` | 相同：x9/build/scripts/prepare-general-runtime.py |
| `x9-metal-render/prepare-pan208.py` | 无同名已跟踪文件 |
| `x9-metal-render/prepare-panvk-native-budget.py` | 无同名已跟踪文件 |
| `x9-metal-render/prepare-panvk-native-device.py` | 相同：x9/build/scripts/prepare-panvk-native-device.py |
| `x9-metal-render/prepare-panvk-native-groups.py` | 相同：x9/build/scripts/prepare-panvk-native-groups.py |
| `x9-metal-render/prepare-panvk-native-host-import.py` | 相同：x9/build/scripts/prepare-panvk-native-host-import.py |
| `x9-metal-render/prepare-panvk-native-link.py` | 相同：x9/build/scripts/prepare-panvk-native-link.py |
| `x9-metal-render/prepare-panvk-native-no-drm.py` | 相同：x9/build/scripts/prepare-panvk-native-no-drm.py |
| `x9-metal-render/prepare-panvk-native-physical.py` | 相同：x9/build/scripts/prepare-panvk-native-physical.py |
| `x9-metal-render/prepare-panvk-native-queue.py` | 相同：x9/build/scripts/prepare-panvk-native-queue.py |
| `x9-metal-render/prepare-panvk-native-status.py` | 相同：x9/build/scripts/prepare-panvk-native-status.py |
| `x9-metal-render/prepare-panvk-native-tiler.py` | 相同：x9/build/scripts/prepare-panvk-native-tiler.py |
| `x9-metal-render/prepare-panvk.py` | 相同：x9/build/scripts/prepare-panvk.py |
| `x9-metal-render/prepare-shared-cache.py` | 相同：x9/build/scripts/prepare-shared-cache.py |
| `x9-metal-render/save-native-engine-patch.py` | 无同名已跟踪文件 |
| `x9-metal-render/save-shared-cache-patch.py` | 无同名已跟踪文件 |
| `x9-metal-update/update.py` | 不同：x9/mailbox/update.py |
| `x9-short-cache-waits/prepare.py` | 无同名已跟踪文件 |
| `x9-thread-symbols/backboard-methods.py` | 无同名已跟踪文件 |
| `x9-thread-symbols/cache_inspect.py` | 无同名已跟踪文件 |
| `x9-thread-symbols/pan120-symbolicate.py` | 无同名已跟踪文件 |
| `x9-thread-symbols/pan122-symbolicate.py` | 无同名已跟踪文件 |
| `x9-thread-symbols/pan125-symbolicate.py` | 无同名已跟踪文件 |
| `x9-thread-symbols/pan128-abort-symbolicate.py` | 无同名已跟踪文件 |
| `x9-thread-symbols/symbolicate.py` | 无同名已跟踪文件 |
| `x9-usb-live/test_usb_log.py` | 无同名已跟踪文件 |
| `x9-usb-live/usb_log.py` | 无同名已跟踪文件 |
