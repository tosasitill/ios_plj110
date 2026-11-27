# X9 Sileo / storm 包管理前缀

机主决策（2026-11-27）：包管理前缀不用 Procursus 的 `/var/jb`，用自有前缀
**`storm`**。本文是该前缀的布局规格与落地阶段定义；实现状态逐项标注，
未上机验证的结论一律写「待上机判别」并给出判别方法，不把设计当验收。

## 1. 约束（本项目实测事实）

| 事实 | 出处 | 对布局的影响 |
| --- | --- | --- |
| iOS 根盘是 `userdata` 内 pinned/immutable 的 32 GiB HFSX extents，由 `ufsroot0` 挂载 | `NOTES.md:48` | 根盘新增文件只能走 mailbox pinned root-manifest 的专用 extent 流程（preflight + 回读校验）；落盘后不可变 |
| 根盘文件 staging 是唯一已确立的新增系统文件通道（AppSync runtime、蓝牙诊断 dylib、Metal 插件都走它） | `x9/appsync/README.md`、`x9/mailbox/` | bootstrap 的只读部分必须做成可 stage 的 root payload |
| dpkg/APT 运行时需要持续可写状态：`var/lib/dpkg/status`、lock、`var/cache/apt`、源列表 | dpkg/apt 通用语义 | 可变状态不能放进 immutable extents，必须落在可写卷 |
| Data 卷在线：SpringBoard、应用容器、`/private/var/mobile/...` 读写均在现行系统上工作 | `NOTES.md` §1 会话记录、`x9/appsync/jbctl.m`（对 `/private/var/mobile/Containers/...` 的实际操作） | 可变状态的候选落点是 Data 卷 |
| 本机启动链（LK → SWEET）与分区布局里没有 Procursus 式独立 preboot 卷的任何记录 | 全树检索无 preboot 挂载/卷证据 | Procursus 的 `/var/jb → /private/preboot/<hash>/procursus` 映射在本机无对应物，不采用 |
| 内核 AMFI/Sandbox 为非强制；exec 时按构建 manifest 的 size/SHA256 核对运行库后才注入 DYLD 环境 | `x9/appsync/README.md`、`x9/kernel/src/bsd/kern/x9_appsync_env.inc` | 未签名/自签二进制「能执行」的门槛低于正规越狱；新工具沿用哈希核对思路做调用方校验 |
| 现有 X9 越狱用户态前缀 `/private/var/x9/jailbreak/20H392/` 已被内核哈希合同（`x9_appsync_runtime.inc`）绑定 | `x9/appsync/manifest.py` | 不迁移、不改名；storm 与之并存（见 §4） |

## 2. 前缀布局

公开前缀名：**`storm`**。所有包、二进制的编译/配置路径只使用公开路径
`/var/storm/...`（规范形 `/private/var/storm/...`）；真实落点不进任何包元数据。

```
/private/var/storm                        公开前缀（符号链接，随 root payload 一起 stage）
  -> /private/var/mobile/Library/X9Storm/current
/private/var/mobile/Library/X9Storm/      真实树（Data 卷，可写、持久）
  <version>/                              版本化完整树：usr/ etc/ var/ Applications/ Library/
  current -> <version>                    prep/helper 原子切换
/private/var/x9/storm/payload/<version>/  只读源（根盘 staged payload，prep 的同步来源）
```

- **为什么是符号链接 + Data 真实树**：根盘 immutable 决定了 dpkg 状态必须外置；
  Data 卷是唯一已证可写且持久的卷。版本化目录 + `current` 链接使工具链升级
  等于「stage 新 payload → prep 同步到新版本目录 → 翻转链接」，旧版本保留可回滚。
- **为什么不学 Procursus 放 preboot**：本机无此卷（见 §1）。
- **包架构标识**：`iphoneos-arm64`（与 rootless 生态一致），包内路径前缀
  `/var/storm`。Sileo 的源配置、dpkg admindir 等全部按公开路径写。

### 2.1 待上机判别项（阻塞布局最终定版）

1. **Data 卷路径能否执行二进制**：本项目尚无任何工具从 Data 路径 exec 的记录。
   判别：已授权会话里把 `x9-jbctl` 复制到
   `/private/var/mobile/Library/X9Storm/probe/` 后直接执行，看退出码与日志。
   - 成功 → 按 §2 布局定版。
   - 失败 → 转 Plan B：`/private/var/storm` 改指根盘 payload（只读执行），
     dpkg/apt 的可变状态经配置重定向到 Data：
     `dpkg --admindir=/var/storm-state/admindir`、apt 的
     `Dir::State`/`Dir::Cache`/`Dir::Log` 指向 `/var/storm-state/...`
     （`/var/storm-state` 为 Data 上的真实目录，由 staged 符号链接暴露）。
     Plan B 下装包写 `.app` 到 Data 的 Applications 目录仍需 exec 判别通过，
     否则包内应用不可运行——该结论同样由判别 1 给出。
2. **根盘运行系统上 `/private/var` 是否可写**：不假设可写。`/private/var/storm`
   符号链接作为 staged payload 的条目落盘（payload 根树含 `private/var/storm`
   链接条目），不依赖运行时创建。
3. **root 卷上 setuid 位是否生效**（阶段 4 helper 的前置）：判别方法见
   `rootexec/README` 对应节；不生效则 helper 改走内核协助形态，接口不变。

## 3. 组成与阶段

| 阶段 | 内容 | 本轮产物 |
| --- | --- | --- |
| 0 | 本规格：前缀、布局、包架构、判别项 | 本文件 |
| 1 | 最小工具链子集（dpkg、apt、dash、核心文件工具、uicache）按 storm 前缀组装成 bootstrap 文件集 + manifest | `build.py`、`sources.json`、`bootstrap/` 输出 |
| 2 | payload 化（可 stage 的 root 树）+ 开机/首次 prep 脚本 + `x9-jbctl --status` 探针更新到 storm 路径 | `prep/`、`x9/appsync/jbctl.m` 修改 |
| 3 | 受控 root 执行通道（窄接口 helper）+ SileoRootWrapper 对接形态 | `rootexec/` |
| 4 | Sileo 本体构建/重打包（iphoneos-arm64、storm 前缀适配） | `sileo-pkg/` 或卡点记录 |

uicache 的取舍：优先 Procursus uikittools 的 `uicache`（通用：任意 bundle id、
前缀下 `Applications/` 注册）。`x9-jbctl --register` 只处理根目录
`/Applications/<Name>.app` 且 bundle id 限定 `org.x9.*`，只能注册 X9 自有应用，
不能替代通用 uicache；两者并存，jbctl 作为 X9 自有应用的既有通道不动。

## 4. 与现有 `/private/var/x9/jailbreak/20H392/` 的关系：并存

- AppSync runtime、内核哈希合同、X9Installer 都绑定旧前缀，迁移会同时动
  内核合同与已构建 runtime，收益为零、风险实在——不收编。
- storm 的 payload 放在同级新目录 `/private/var/x9/storm/`，状态放在 Data 的
  `/private/var/mobile/Library/X9Storm/`；`x9-jbctl --status` 同时报告两套。
- 长期若 storm 成为唯一包管理面，再单独决策 AppSync runtime 的去留。

## 5. 验收口径

- 每阶段产物必须有主机侧校验（架构、签名状态、manifest 哈希、脚本语法、
  白名单测试），校验脚本随产物提交。
- 「装上 Sileo」的验收不在本轮：需上机完成 §2.1 判别、payload staging、
  prep、helper 激活、Sileo 图标注册与启动、真实源 `apt-get update` 与
  小包安装全链，缺一环只能报到该环为止。
