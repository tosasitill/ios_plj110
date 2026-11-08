# X9 工作约定

当前状态、构建/上机命令、启动参数见 `../NOTES.md`;构建细节见 `BUILD.md`。本文件只保留长期规则。
(2026-11-08 精简;旧 pan 时代的逐版流水账见 `git show 2406f8d:x9/AGENTS.md`。)

## 参考与兼容

- 遇到 XNU/iOS 用户态兼容问题,**先查**本地 UniXNU 与 hackios-xnu-kernel 的现成修复、构建输入和验证脚本(Mac:`work/references/UniXNU`、`work/references/hackios-xnu-kernel`),
  结合实机日志选择移植;确认参考没有覆盖后再自行设计。dali 补丁只迁与 MT6993/16 KiB 页不冲突的部分,其 4 KiB/全局 vtable/prelink 变更和 CPU 采样器不混入。
- 已落地的兼容手段(别重复做):`-apfs_shared_datavolume`(keybagd 走 shared-Data 路径)、`launchd_unsecure_cache=1`(launchd cache loader 未签名缓存模式)、
  cryptex 别名(libobjc-trampolines)、X9 AMFI/MAC privilege 授权、软件 AKS selector31、8 KiB RAM-only CHRP NVRAM proxy、decmpfs 全 codec(type 3/4,7/8,9/10,11/12,13/14)。

## 设备与流程

- 设备由用户手动刷入/`fastboot boot` 候选并测试;**不要自行 flash、reboot 或写分区**。每个破坏性步骤先向用户确认。
- 用户报告测试后,先读该次运行的日志(现在是 Mac `~/N.txt`;pstore / 磁盘 sidecar 为辅)。设备已恢复 Android 不能说明候选没刷入;没有日志也不等于没启动。
- 原始日志与可读副本分开保存,不覆盖前一版本;ECC 损坏字段不得猜成完整读数。日志、设备转储、序列号、Apple 产物不入 git。
- 2026-10-25 起:构建成功后**不再二次检查**产物(不额外解包、重算镜像 hash、反汇编验证、重建对比),除非用户明确要求;内置检查保留。
- 2026-10-28 起:直接编译、打包,不额外运行测试脚本/测试套件;仍可读源码和日志定位问题。
- 默认不设诊断检查点复位:保留屏幕和持久日志,持续运行到实际等待或故障。
- 不新增过程报告、timeline 或回读 Markdown;只保留必要的原始日志、解码结果和构建输入。交接写进 `../NOTES.md`。

## 硬件与根盘

- X9 实机 12 GiB RAM,面板 1256x2760。不要把任何 XNU 默认窗口说成硬件容量限制。
- 根盘路线 data-a-live:专用 F2FS pinned+immutable 文件(`/data/local/tmp/x9-ios*/…`)在 `userdata` 的 extent 内保存 16 GiB HFSX 根盘,内核原生 `ufsroot0` 挂载(`rd=ufsroot0 rootrw=1 ufsrw=1`);
  块表与 SHA-256 在私有 `root-manifest.json`(含序列号,不入 git)。不得正常覆盖、删除或解除 pin/immutable;访问内容须按 extent 读取。
  只写 manifest 列出的专用 extent;Android 的其他数据与 boot 分区不动。另有 8 MiB 专用磁盘日志文件(`x9-bootlog-*/console.bin`)。
- 刷机/staging 前先跑 root preflight(`x9/tools/check_plaintext_root.py`,mailbox 的 `update.py` 默认运行,不可跳过)。
