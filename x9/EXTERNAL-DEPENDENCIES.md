# X9 外部输入(不在 git 里的东西)

(2026-11-08 重写;迁移前的逐脚本盘点见 `git show 2406f8d:x9/EXTERNAL-DEPENDENCIES.md`。)

构建在 Mac 上进行,仓库内被 gitignore 的 `.local/` 保存全部外部输入;`python3 x9/build/prepare-local.py --check` 校验布局,
去掉 `--check` 会把维护中的脚本与 `x9/metal/General` 用相对符号链接接入 `.local`。克隆 git 本身**不会**带来这些输入。

| 输入 | 位置 | 用途 |
| --- | --- | --- |
| 冻结的 XNU 对象树 | `.local/dxb/`(`local.json` 的 `dxb`)| kernel 链接用的 1000+ 个外部对象、`EXPORT_HDRS`、`fakeroot`、头文件;由 `xnu-8796.141.3` + sweet/topaz 补丁链(至 0185)构建 |
| 根盘清单 | `local.json` 的 `root_manifest` | extent 表、设备序列号;mailbox preflight 与 boot 打包都读取,**绝不提交** |
| Metal/PanVK 构建树 | `.local/x9/work/x9-metal-render/` | Mesa `300b5fd7…`+补丁、AIR frontend、metal2vulkan **963f0e64**(UniXNU 642c4fef 系列)+补丁、libdrm、Rust/venv(`build/requirements-metal.txt`)|
| Apple 输入 | `.local/…/work/ios16-20H392/`、`.local/x9/work/dsc-20H392/` | 重建根盘的 IPSW 内容;设备实跑的 dyld cache 拷贝(反汇编用)|
| 工具链 | `local.json` 的 `developer_dir`、`llvm_clang`、`lld` | Xcode 15(clang 15, MacOSX14.0 SDK)、LLVM/lld 22、ccache |
| 厂商参考 | Mac `~/HelioDarwin/oppo-source/` | OPPO kernel_device_modules-6.12(DVFSRC、显示 mutex/DSI、Wi-Fi gen4m 等)|
| Android 抓取 | `.local/x9/android-join-*`、`android-pull-*`、`wifi-nvram/` | Wi-Fi 联网抓包、NVRAM/校准字来源(含 `x9_wifi_xo*`)|
| 参考工程 | Mac `work/references/{UniXNU,hackios-xnu-kernel}` | 兼容修复的首选来源 |
| 逆向工具 | Mac `~/x9-kbd-dis/` | dsc 反汇编脚本,不在 git |

维护者须知:
- 第三方树的补丁见 `PANVK-PATCHES.md` 和 `build/third_party/`;Meson 缓存不可跨目录拷贝,换位置需重新生成构建配置与 venv。
- `local-source-lock.json` 记录第三方源码快照哈希。
- 设备转储、序列号、Apple 镜像始终留在 git 外。
