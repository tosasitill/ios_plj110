# Native Bluetooth 定点根盘安装

当前用户许可只覆盖四个自有 binary、两份 Program-only jobs、权威
`/System/Library/xpc/launchd.plist` 及必要 HFS 元数据；**禁止全盘读取、
整个根盘 snapshot/clone/备份或 fullhash**。使用 `stage_targeted.py`，
不要执行旧 `stage.py --apply/--rollback` 的全量快照路径。旧工具仅保留离线 plan。
安装成功不等于扫描、配对、BLE 或音频通过。

## 输入和命令

- `CANDIDATE`：同一次完整 `candidate.py` 构建输出，四组件 exit0。
- `ROOT_MANIFEST`：固定 `.local` 中当前私有 mutable-root manifest，
  包含 base/extension、UUID、保护范围、pin/immutable、inode/FIEMAP。
- `APPLE_ORIGINALS`：私有 20H392 `bluetoothd`、`BlueTool` 原件目录。
  实际根盘必须匹配两原件、两 stock jobs、原 cache；不修改 Apple binary。
- `X9_SERIAL`：私有设备标识，不写入 git，不在终端输出。
- `state-dir`：本功能 worktree 的全新、私有 `x9/out/` 子目录。

```sh
python3 -I x9/bluetooth/stage_targeted.py \
  --candidate "$CANDIDATE" --payload "$CANDIDATE/root-payload" \
  --root-manifest "$ROOT_MANIFEST" --apple-originals "$APPLE_ORIGINALS" \
  --apply --serial "$X9_SERIAL" --state-dir x9/out/bt-root-targeted-UNIQUE \
  --confirm-root-modification install-native-bluetooth-root \
  --confirm-android-stays-running
```

两个确认参数传递已经取得的许可，不代替用户确认。Android 必须保持运行，
直到 receipt 或恢复完成；工具不 boot、flash、reboot、改 GPT 或修改 Android
普通文件内容/pin/immutable。

## 定点路径和安全边界

1. 正常 metadata-only preflight：保留身份、UUID、几何、pin、immutable、
   FIEMAP、保护范围、userdata route 检查；root 只读 first64KiB，BBFW
   只检查 metadata，明确其 payload SHA 未验证。
2. macFUSE 仅导出一个固定大小 `root.hfs` 虚拟 regular file。每次读取由
   私有 Unix RPC 对实际所需 4KiB 块执行 `pinfile rawreadat`，缓存真实读取
   块；不存在 whole-image scaffold、历史克隆、未读块补零或 HTTP shadow。
3. macOS 原生 DiskImages/HFS 执行 journal replay、catalog/bitmap/extent
   分配。所有 write 仅进入本机有界 COW，没有实时设备写。以 `noatime,
   noowners` 挂载，三个 existing 目标先 `cp -p` 保留 xattrs/ACL/resource
   fork/flags，再替换内容；未知压缩目标拒绝。iOS 绝对 symlink 限定在私有卷。
4. 正常 detach，确认 DiskImages 精确 backing path 已解除，再卸 FUSE、
   停止 RPC；失败保持前台真实服务，不从仍 attached 的 HFS 下撤掉 backing。
5. 定点 HFSX BinaryCompare B-tree 查找七路径，不加载整个 catalog。
   验证真实 clean internal journal header/checksum/start==end 后，仅修七
   catalog records 的 UID/GID 八字节；验证 bytes、size、mode、owner，
   以及两 Apple binary 和 SystemVersion 原件。未知 link-chain、格式、
   compressed raw file、overflow bootstrap 均 fail-closed。
6. 冻结全部读依赖及最终 changed blocks，剔除净相同块，保存每范围
   `.old/.new`、清单 hash、durable write intent。单范围不超过256KiB；
   unique necessary cache 和 changed footprint 分别不超过128MiB。
   所需本机空间按128MiB cache +512MiB COW日志 +3×128MiB +32MiB
   余量预留；正常 root payload 读取费用上界512MiB，包含 compare-write
   内部 OLD/NEW 读取，另保留独立有界恢复 allowance。
7. 写前再次 metadata-only preflight；按冻结集合**重读全部必要依赖**。
   使用 manifest 专用 extent 的现有 `pinfile rawwriteat`，核 OLD SHA、
   写后 fsync/readback SHA；不手算物理地址，不写未获授权的其它分区。
8. 最后验证变化块与真实七文件路径/owner/mode、原 Apple 文件、clean
   journal 和 root metadata。只有私有 `state.json` 记录
   `root_installed=true`，`runtime_verified/audio_verified` 仍为 false。

## 失败和恢复

安装写入非断电原子。普通错误或中断后先等 `pinfile` quiesce，然后只对
attempted ranges 回滚；每4KiB当前内容必须是精确 OLD 或 NEW，未知/torn
块不盲目覆盖。恢复结束重读全部冻结必要依赖，不做 full-root hash。
保留整个私有 state directory，其中 cache 仅是实际必要块，`.old` 仅是
变化范围回滚前像，**不是根盘/分区备份**。

```sh
# 保持所有输入和 state-dir 与原安装一致；不重试安装。
python3 -I x9/bluetooth/stage_targeted.py \
  --candidate "$CANDIDATE" --payload "$CANDIDATE/root-payload" \
  --root-manifest "$ROOT_MANIFEST" --apple-originals "$APPLE_ORIGINALS" \
  --rollback --serial "$X9_SERIAL" --state-dir x9/out/bt-root-targeted-UNIQUE \
  --confirm-root-modification install-native-bluetooth-root \
  --confirm-android-stays-running
```

不要并发运行其它 root 编辑器、启动 iOS、移动/挂载 COW 或删除恢复材料。
macFUSE 安装/编译可用不等于系统实际允许挂载；正常安装日志才证明执行结果。
