# X9 AppSync / 越狱用户态接入

这是 **20H392、arm64 的离线候选**，不是已验收的完整越狱或 Mac 配对镜像。
当前私有 runtime 为 **v6**，包含 storage repair 工具与注入/storage 链；历史 `~/428.txt`
有 v6 注入/storage repair 运行证据，但没有 Mac 传输后的真实 IPA 安装、应用启动或持久性验收。
kernel 只验证并注入精确 runtime 环境，不安装这些文件。当前 `.local/` v6 路径与候选 manifest
是私有构建输入，不提交到 Git。实际状态以 [`../../NOTES.md`](../../NOTES.md) 为准。
不改 Apple executable、launchd plist、dyld cache，也不移植 sheng 的
arm64e/PAC、SPTM、trust-cache-v2、DWC3 或全局 vtable 补丁。

## 已接入的路径

- `vendor/AppSync.m`：installd 和 MobileInstallationHelperService 的
  `MICodeSigningVerifier performValidationWithError:`；SpringBoard 的
  `FBSSignatureValidationService trustStateForApplication:`。
  核对 Objective-C 返回值和每个参数/属性的 type encoding 后才换 IMP；
  不兼容时明确保持原实现。仅影响 Data 上 `.app`，原始 verifier 的选项在
  `@finally` 中恢复。20H392 的本地 framework 中已找到这些 selector 字符串；
  字符串不是 runtime ABI 或 hook 成功证明。
- `vendor/Signing.m` 和 in-process ldid：原校验先执行；失败时才锁定安装副本、
  拒绝加密/畸形 code 和逃逸 symlink，递归生成 ad-hoc 签名并重新走原校验。
  原样保留 entitlements。代码页和资源签名校验没有被这次改动关闭。
- `kern_exec.c`：显式 `appsync=1` 时，Data 用户 `.app` 的主程序不再被
  SWEET 的系统兼容代码一律升成 platform。系统服务仍沿用原规则；不修改
  当前 AMFI/Sandbox 策略。避免把 sheng 的工程 trust cache 方案叠到现有策略上。
- exec 环境只注入三个目标 executable；两个运行库必须与构建 manifest
  的大小/SHA256 完全一致。不存在、改变或已有 DYLD 注入链时不追加。
  **内核不安装任何文件**。
- `x9-appinst` 使用原生 LSApplicationWorkspace，先复制 IPA，避免原安装器
  消耗调用者原件。`x9-jbctl --status` 只报告 AMFI 和工具文件是否存在，
  不伪造越狱成功、hook 加载、USB/Trust 或 root 权限。

## 本地存储（文件 App「我的 iPhone」/ Safari 下载）

症状同根：LocalStorage 文件提供者的默认域根目录是 AppGroup 容器
`group.com.apple.FileProvider.LocalStorage` 内的 `File Provider Storage`
（Safari 默认下载目录为其下 `Downloads`）。种子根盘里该容器只有元数据、
没有这个文件夹，stock 流程在本机不生成它；MobileSafari 的 LS 记录同时缺
dataContainerURL（容器在盘上存在）。v5/v6 的 jbctl 修复
（`--repair-container`、`--ensure-local-storage`）经 AppSync 从 SpringBoard
派生，在 r286/r287 日志中均 exit=0，但 fileproviderd 在开机约 13 s 已启动、
修复约 18 s 才落地，且修复从未校验 FP 域是否真被提供——用户侧症状依旧。

2026-11-27 起内核在 PID1（launchd 之前）做同一修复
`sweet_x9_prepare_local_storage`（`kern_exec.c`，门控 `x9_local_storage`，
默认开、`=0` 关闭，失败不阻塞启动）：按与 jbctl 相同的规则扫描 AppGroup
元数据标识定位容器，预建 `File Provider Storage` 与 `Downloads`（0755）
并把属主规范为 mobile（501:501）。jbctl 保留为二次修复，并新增只读的
FP 域普查日志（`fp domains count=… localStorage=… userEnabled=…`），供下轮
上机判定域是否被 fileproviderd 提供。主机测试
`x9/tools/test_x9_local_storage_bootstrap.py` 从真实 kern_exec.c 提取函数
验证发现/建夹/属主/门控行为。仍待上机验证：文件 App 是否出现「我的
iPhone」、Safari 是否能下载、FP 普查日志的实际值。

## 构建（只在 Mac）

```sh
python3 -I x9/appsync/build.py --out x9/out/appsync-runtime-<unique-name>
```

输出目录必须是新的 `x9/out/` 子目录；不在 `.local/` 新建实验目录。生成：

- `root/private/var/x9/jailbreak/20H392/{lib,bin,licenses}`；
- `runtime.json`：签名后的库/工具 metadata；
- `signer.json`：固定源依赖、target、cache 和 LGPL 可重链接对象的位置。

首次下载 ldid/OpenSSL/libplist，核对 pinned archive SHA256；后续命中
content-addressed cache。用系统 CA 校验 HTTPS，不关闭证书验证。
AppSync 是独立 GPL-3.0-or-later 用户库，归属见 `vendor/PROVENANCE.md`。
签名器依赖许可一起交付，不把它们改成仓库 MIT 许可。

正常内核构建使用私有 local config 的：

```json
{"appsync_runtime": "x9/out/appsync-runtime-<unique-name>/runtime.json"}
```

`build.py` 只生成 gitignored `x9_appsync_runtime.inc` **哈希 metadata**。
没有 manifest 的 `appsync=1` kernel build 拒绝，不暗中打包成功。
默认不开启；不要直接把参数追加到当前 603B boot-args，607B 限制仍有效。

## 部署和验收门槛

1. **先确认新增根盘文件的 staging，再操作设备。** 只能按现有 pinned root
   manifest 的专用 extent 流程、preflight、备份和回读执行；本目录没有绕过
   consent 的自动部署器。不要普通文件写入 Android 上 pinned HFS 容器，
   不挂载/覆盖 live root 或旧快照，不修改 GPT/boot/modem/persist。
2. 仅库文件就位还不算成功：必须看见 `X9 APPSYNC: verified runtime environment
   appended`，随后真实 installd/Helper/SpringBoard 的 adapter active 日志。
   ABI mismatch 或 loader error 时返回该层，不虚报“可以安装”。
3. 只有 runtime 已 staging 且注入链确认活跃后，才在设备 iOS 的已授权控制会话执行：

   ```sh
   /private/var/x9/jailbreak/20H392/bin/x9-jbctl --status
   /private/var/x9/jailbreak/20H392/bin/x9-appinst /path/to/Example.ipa
   ```

4. Mac 的真实配对和安装入口见 `../usb/README.md`；先接通原生 Apple USB，
   再验收 Trust/Pair/TLS、AFC/Install/Lookup、应用实际启动、重启后的持久性。
   仅 Install Complete 或 appinst exit0 不代替这组验收。

## 越狱环境边界

当前 SWEET 已有非强制 AMFI/Sandbox 和 root/system compatibility，但这不是
完整 bootstrap。这次 runtime **不包含** dpkg/APT、Sileo/Cydia、通用 tweak
注入器或 OpenSSH，也不设置默认 root 密码、不启动监听服务。不能因此声称
“完整越狱环境已经安装”。下一步需要选择兼容 iOS16/D22 的 bootstrap，
核对 source、依赖和原文件冲突，并在单独确认后 staging。不要直接执行
palera1n 的 loader/postinst：它的 APFS、mount、launchd 和 kernel 假设不适用本机
HFSX/MT6993。Finder/原生 USB backend 仍是独立未完成项。
