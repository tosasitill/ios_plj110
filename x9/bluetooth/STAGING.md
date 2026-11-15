# Native Bluetooth root payload 安装器

`stage.py` 默认**纯离线 plan**；不会枚举 adb、访问设备或安装文件。
许可仅针对 root 变更：安装成功不提升 `runtime_verified` 或
`audio_verified`，二者始终为 `false`。启动、扫描、配对、BLE、A2DP、
HFP/SCO 仍需真实 iOS 验收。

## 输入与离线 plan

以下路径均须为仓库内输入（worktree 的固定共享 `.local` 链接可用）：

- `CANDIDATE`：一次完整 `candidate.py` 构建输出目录；`manifest.json`
  必须记录四个组件 exit 0、完整 candidate 和该 root payload 路径。
- `ROOT_MANIFEST`：现有私有 native mutable-root manifest，包含当前 base、
  extension、保护范围和 pin/FIEMAP 合同；不得使用旧生成镜像作为 live baseline。
- `APPLE_ORIGINALS`：私有 **20H392 原件目录**，同时包含 `bluetoothd`
  和 `BlueTool`。前者须匹配桥锁定的原始 SHA-256；二者在挂载后的 live
  snapshot 内逐字节比较，不修改 Apple 可执行原件。

```sh
python3 -I x9/bluetooth/stage.py \
  --candidate "$CANDIDATE" --payload "$CANDIDATE/root-payload" \
  --root-manifest "$ROOT_MANIFEST" --apple-originals "$APPLE_ORIGINALS"
```

plan 检查包内四个二进制、两个 jobs、权威 launchd cache，以及两个精确原始
jobs/完整原 cache 的 byte hashes、权限、安装顺序、Program-only 差异及完整
候选绑定；不依据 mtime 做身份判断。旧的六文件 payload 不再允许安装。

`candidate.py` / `build.py` 必须同时显式给 `--launchd-source` 和
`--launchd-cache-source`。后者是仓库内私有原件，不从挂载目录自动挑选或
把历史根盘克隆当成 live baseline；候选构建冻结完整原 cache，安装前必须
与 live snapshot 内 `/System/Library/xpc/launchd.plist` 逐字节相等。

20H392 stock launchd 的正常 `rd=ufsroot0` 环境要求系统 jobs 在 cache 中。
其 definition 使用缓存字典，独立 plist 的新 `Program` 不会覆盖它。
`launch_cache.py` 复用 Python `plistlib` binary parser/writer，仅追加两
Program 值、两个 job 字典及两个父容器；其它所有字段/records 保持相同。
原 707339 B object region、22269 个 object IDs/offsets 均逐字节保留，
ref width 仍为2。原 cache 固定796447 B，trailer `4/2/22269/0/707339`；
候选固定798777 B，trailer `4/2/22275/22269/709645`（依次为 offset width、
ref width、object count、top object、offset-table offset）。拒绝未知原几何、
shape、更多新对象或全量重排；内核 Siri gate 必须接受这个精确 alternate
geometry，并继续执行全部原 string-object guards，不能宽放 size 范围。

## 显式授权后执行

**必须先取得用户对此次 root 写操作的明确确认**。以下两个参数不是替代
用户确认，只将已获得的许可传给工具。用户需保持 Android 运行，直到安装
成功或回滚验证成功；工具从不重启、fastboot 或 flash。

```sh
python3 -I x9/bluetooth/stage.py \
  --candidate "$CANDIDATE" --payload "$CANDIDATE/root-payload" \
  --root-manifest "$ROOT_MANIFEST" --apple-originals "$APPLE_ORIGINALS" \
  --apply --serial "$X9_SERIAL" --state-dir x9/out/bt-root-install-UNIQUE \
  --confirm-root-modification install-native-bluetooth-root \
  --confirm-android-stays-running
```

执行条件与路径：

1. 本机 macOS，`adb`、`hdiutil` 可用；本机 root 或已授权的 `sudo -n`
   用于**离线挂载图像内**的 uid/gid 设置。须至少保留总 root 大小
   **+3×128 MiB +16 MiB** 可用本机空间：分别预留 APFS clone delta、
   `.old` 块备份、`.new` 块副本，以及少量 receipt/输入/目录元数据余量。
   APFS `cp -c` 克隆失败则停止，不改走未知文件系统方案。
2. 必须运行 `check_plaintext_root.py`，再动态 `resolve_userdata` 绑定
   live GPT/geometry/dm route；不使用历史 `/dev/block/sdc100`。
3. 从当前 root 文件和 extension 文件 raw 读取完整 **base||extension**，
   保留 `snapshot.hfs` 并逐 part SHA-256 验证。源 Android 文件不改 pin、
   immutable、inode、FIEMAP；保护范围不重分类。
4. 克隆成 `updated.hfs`；复用 `build_full_ios_root.py` 的 macOS 原生 HFS
   attach/detach 路径，交给 HFS 处理 dirty journal 和 catalog。所有 iOS
   绝对 symlink 都映射回该私有图像，绝不走到 Mac 的 `/usr` 或 `/private`。
5. live root `SystemVersion.plist` 必须为 **20H392**，`bluetoothd` 必须同时
   匹配确切原始 SHA-256 与 Mach-O UUID。**所有**原 Apple 原件、两个 jobs
   和完整 launchd cache
   验证成功、四个新路径全部 absent 后，
   备份原件。离线 VFS 先 fsync/rename 四个自有二进制，最后两个仅变更
   `Program` 的独立 jobs，再发布第七文件 append-only cache；校验
   uid0/gid0/mode 和内容。精确原 cache 与原 jobs 的备份文件及目录项在
   write-intent 前持久化。Apple 可执行原件保持逐字节不变，不删 cache/.sig，
   不变更其它 daemon/service。
6. detach 后生成最多128 MiB 的 blockdiff（包含原生 journal/metadata
   变化）。保留每范围 `.old`、`.new` 与 write-intent ledger；先 fsync
   snapshot、detached updated image 和所有块副本，再 fsync `blocks/`、
   state directory 及其父目录，确保 ledger 发布前恢复依赖的文件和目录项
   已持久化。**原生离线
   文件发布顺序不等同物理 blockdiff 顺序**；Android 期间没有 iOS HFS
   挂载，用户不得在部分写入时启动 iOS。
7. 写前重新完整 preflight，比较每 part 的**全部 live bytes hash**与
   snapshot；只使用 manifest 专用 root/extension 文件的 `pinfile
   rawwriteat`。每次最多64×4096 B，先再次验证 pin/FIEMAP/route，pinfile
   比较旧 SHA、写后 fsync、readback SHA。没有手算 extent 物理写法。
8. 最后整个 live root 的每 part SHA 必须等于已验证的 detached image，
   并通过最终 root preflight；只有私有 `state.json` 才记录
   `root_installed=true`。不修改构建元数据、不宣称蓝牙运行验收成功。

## 失败与显式恢复

普通错误和 SIGINT/SIGTERM 触发回滚；单次 compare-write 期间延后中断，
回滚前等待 `pinfile` 不再在途，避免写者与回滚竞争。发生断电、SIGKILL、
无法连接 adb 等情况时，保留整个 state directory 和所有快照/备份：

```sh
python3 -I x9/bluetooth/stage.py \
  --candidate "$CANDIDATE" --payload "$CANDIDATE/root-payload" \
  --root-manifest "$ROOT_MANIFEST" --apple-originals "$APPLE_ORIGINALS" \
  --rollback --serial "$X9_SERIAL" --state-dir x9/out/bt-root-install-UNIQUE \
  --confirm-root-modification install-native-bluetooth-root \
  --confirm-android-stays-running
```

恢复只接受原 root/包/原件/候选/设备绑定和 durable ledger；检查全部备份
hash 与范围，不重试安装。对已尝试范围逐4096 B 确认当前块是精确旧/新块
之一，再用当前 SHA compare-write 回写旧块，最后比对整个原 root SHA。
未知、并发或 torn block **拒绝盲目覆盖**；此时记录
`rollback-incomplete` 并保留材料，不得将其误报为成功。

多块 raw 更新**不是断电原子事务**。本机锁只协调本仓库里的该执行器；
执行期间不应运行其他 root raw 编辑器或删除、挂载、移动私有快照/备份。
此实现仅经过源码编译，尚未设备执行或测试套件验收。
