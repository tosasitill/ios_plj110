# X9 外部输入

构建只依赖本仓库 + 编译工具（Xcode 15、KDK_13.5_22G74、Homebrew LLVM/lld 22、ccache、python3.13）。
所有不能进 git 的输入（Apple 原件、设备转储/固件、根盘 extent 表、第三方构建树、参考仓库）放在固定的 gitignored
`.local/` 布局里，完整列表与来源见 `../NOTES.md` §2.2，`python3 x9/build/prepare-local.py --check` 校验。

- XNU：源码改动全部在 `xnu/`（相对 apple-oss `xnu-8796.141.3` 的补丁），`xnu/build-xnu.sh` 生成 `.local/dxb`、`.local/dxb-iosabi`。
- 第三方树补丁：`PANVK-PATCHES.md`、`build/third_party/`；Meson 缓存不可跨目录拷贝，`.local/x9/work/x9-metal-render` 路径固定。
- `build/local-source-lock.json` 记录第三方源码快照哈希。
- 设备转储、序列号、Apple 镜像始终留在 git 外。新增输入时同时更新 NOTES §2.2 与 `prepare-local.py`。
