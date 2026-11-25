# X9 工作约定

当前状态、候选、构建/上机命令与启动参数见 [`../NOTES.md`](../NOTES.md)；构建契约见
[`BUILD.md`](BUILD.md)。本文件只保留长期规则。仓库级规则见
[`../AGENTS.md`](../AGENTS.md)。

## 参考与兼容

- 遇到 XNU/iOS 用户态兼容问题，先查固定 `.local/references/` 中的 UniXNU、
  hackios-xnu-kernel 等现成修复、构建输入与验证脚本，再结合日志决定移植。
  不把其它 SoC 的寄存器、4 KiB 页、全局 vtable/prelink 或 CPU sampler 混入 MT6993/16 KiB。
- 已落地的兼容手段不要重复实现：`-apfs_shared_datavolume`、
  `launchd_unsecure_cache=1`、cryptex 别名、X9 AMFI/MAC 授权、软件 AKS selector31、
  RAM-only CHRP NVRAM proxy、decmpfs codec 链。某项“存在”不等于当前候选已验收。

## 构建与证据

- 私有输入保持 `NOTES.md` §2.3 的固定 `.local/` 布局，先运行
  `python3 -I x9/build/prepare-local.py --check`。不在 `.local/` 新建实验目录。
- 完整设备候选使用 `x9/build/boot-device.json`；`boot.json` 只有基础参数。
  每个候选使用新的 `--name`，不覆盖旧镜像。Bluetooth passive 默认只有 `x9_bt=1`，
  完整初始化才显式加 `x9_bt_init=1 x9_bt_fc=1`。
- XNU 本体改动提交为 `x9/xnu/` patch，并用 `x9/xnu/build-xnu.sh` 重建；
  overlay 改动在 `x9/kernel/src/`、`recipe.json`、`link-order.txt`。并行任务使用隔离对象树。
- 插件 revision/hash 变化必须同步 allowlist、mailbox 与 root 包；allowlist 不变时
  kernel-only 重建无需 staging。当前 allowlist 为 280，不据此声称设备已安装 280。
- 构建成功后不额外解包、重算 hash、反汇编、重建对比或运行测试套件，除非用户明确要求；
  内置检查保留。直接编译/打包，不把 host PASS 写成设备验收。
- 明确区分观察、推断、host-verified、candidate_ready、stageable、root_installed、
  device boot、runtime_verified 与 audio_verified。不伪造 sample、ACK、frame time 或性能提升。

## 设备与根盘安全

- 设备不支持临时 `fastboot boot`。不要自行 flash、reboot、reset、写 root/mailbox、
  改分区或探测 live sysfs；只有用户对 exact action 明确授权后才执行。
- 授权写入只针对 `boot_b`。不得改 GPT、bootloader、recovery、modem、persist、备份或
  pinned root extents 之外的 userdata。每个破坏性步骤单独确认，保留 Android 与分区备份。
- iOS 根盘是现有 `userdata` 内 pinned/immutable extent 上的 **32 GiB HFSX**（16 GiB base +
  16 GiB extension），由原生 `ufsroot0` 挂载。块表/SHA-256/序列号在私有
  `root-manifest.json`，不入 Git。root/mailbox 操作必须跑 preflight，不能把 Android 路径
  当作可写 HFS mount，也不能正常覆盖、删除或解除 pin/immutable。
- 用户报告测试后先读该次确切日志；`~/N.txt`、pstore、磁盘 sidecar 可互相印证。
  没有日志不等于没启动，桌面可见不等于 GPU 出帧，进程存活不等于蓝牙发现或音频成功。
- 原始日志与可读副本分开保存，不覆盖前版；ECC 损坏字段不得猜读。日志、设备转储、
  序列号、Apple 产物与私有 manifest 不入 Git。

## 研究、文档与并行工作

- 离线研究只使用已有源码、Android 日志与静态 Apple 证据；不访问设备、不造运行结果。
  避免盲目的全盘或全仓库搜索，沿 `NOTES.md` 与子系统文档到已知路径。
- 不新增过程报告、timeline 或回读 Markdown；必要原始日志、解码结果与构建输入保留在
  gitignored `x9/out/`。每里程碑和停止前更新 `NOTES.md`，持久契约写入对应 README。
- 不覆盖、提交或删除 `issues.jsonl`、其它窗口未提交工作、旧候选或备份。
- 不修改 `.local/`、`x9/out/` 或上游 README；`docs/sweet/` 保持历史存档。
