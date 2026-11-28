# APFS on userdata（data-a-live 风格 pinned 文件）设计 — 2026-11-28

用户要求：「开始做 apfs 不用管数据但还是用 data-a-live 设计在 userdata 上 pin，不删我的安卓数据」。
iOS 侧数据可丢弃；Android 数据不动。HFS root（`/dev/ufsroot0`）保持不变。

## 为什么要做

`APFSContainerGetBootDevice`（49154）要求存在一个 Role=System 的 AppleAPFSVolume
（任务 #1 结论）。usermanagerd 的会话卷，以及 `mount -P` 的 Data 卷查找
（`APFSVolumeRoleFind(container, 0x40)` → 挂载到 `/private/var`），都要先有一个真正的 APFS 容器。

## 链路

```
userdata (sdc100, F2FS)
 └─ iosapfs-container-1.img   新建 pinned+immutable 文件（pinfile create，O_EXCL）
     │ extents → manifest apfs_policy → /chosen x9-ufs-apfs-{bytes,extents}
     │           并精确追加进 x9-ufs-protected-extents
 kernel: sweet_x9_ufs.c 窗口 1（xa）
     读：root host + 本窗口 extents；写：root host 的簿记副本，只绑定本窗口 extents（同 disk log 模式）
     契约：x9u_extent_validate；与 root/log/update mailbox 及其他 protected run 均不重叠；
           自身 extents 必须在 protected 表中精确出现；block 0 必须是 NXSB
 SweetX9Storage.cpp: 发布 IOBlockStorageDevice "X9APFSContainer"（x9_apfs=1）
     发布前在内核内显式加载 com.apple.filesystems.apfs（allowDefer=false）
 IOBlockStorageDriver → IOMedia(Whole) → AppleAPFSContainerSchemeWholeDisk（无需 GPT）
     → AppleAPFSContainer → AppleAPFSVolume(System[, Data])
```

## Prelink 闭包（静态核实）

`com.apple.filesystems.apfs` 闭包共 8 个 kext：apfs、AppleEffaceableStorage、CoreAnalyticsFamily、
IOCryptoAcceleratorFamily、IOStorageFamily、kec.corecrypto、CoreTrust、AppleImage4。
KPI 接口：bsd/dsep/iokit/libkern/mach/private/unsupported。

- 缺失的内核导出只有一个：`_vfs_getvfs_with_vfsops`，已在 `bsd/kern/sweet_prelink_kpi.c` 补上
  （语义照 20H392 kernelcache 0xfffffff00734c520：取第一个 fsid 匹配的 mount，mnt_op 相同才返回）。
- emit 的外部 PC 相对编码检查：闭包内 92885 处全部是内部引用，可以通过。
- 必须拦截的注册（来自 plan 绑定表，每个名字只有一个导入方）：

| 导入 | 来源 | 风险 | 处理 |
| --- | --- | --- | --- |
| `register_and_init_prng` | kec.corecrypto | SWEET 在 EARLY_BOOT 已注册 PRNG；第二次调用会撞 DEVELOPMENT 的 `assert(!prng_ready)`，并在运行中替换掉生成器 | 重绑到 no-op shim |
| `img4_interface_register` | AppleImage4 | img4if 在本内核是有意留空的（trust cache runtime = NULL） | 重绑到 no-op shim |
| `mac_policy_register` | AppleImage4 | 运行中晚注册 MAC policy | shim 返回 0，句柄 0xffffffff |
| `coretrust_interface_register` | CoreTrust | 信任由 SWEET AMFI 负责 | 重绑到 no-op shim |
| `register_crypto_functions` | kec.corecrypto | g_crypto_funcs 当前为 NULL | **放行**（原厂行为） |

重绑由 `x9/build/scripts/prelink_rebind.py` 在指针绑定层完成；只有传 `build-usb-prelink.py --apfs` 时才使用，
默认构建产物逐字节不变。

## 镜像（Mac，无需 sudo）

`apfs_userdata.py image`：`mkfile -n` 稀疏文件 → `newfs_apfs -C` → `diskutil apfs addVolume … -role S`
（阶段 B 再加 `-role D`）→ 用 iOS 16 白名单校验特性位：
NX incompat ⊆ {VERSION2}；卷 incompat ⊆ {CASE_INSENSITIVE, NORM_INSENSITIVE}，要求大小写敏感；
SEALED/INCOMPLETE_RESTORE/PFK/PREALLOC 一律拒绝。macOS 27.2.1 实测：容器 incompat=0x2，
System 卷 features=0x12、incompat=0x8，Data 卷 features=0x2、incompat=0x8，全部在白名单内。
容器每 512 MiB 才允许一个卷，所以 System+Data 至少要 1 GiB。

## 设备端写入

`apfs_userdata.py provision --yes`：preflight → 记录 root 头 + 所有已有 pinned 文件的身份 →
`pinfile create`（新路径，O_EXCL）→ `chattr +i` → 抓取 extents → **任何写入之前**先做 manifest 校验和重叠检查 →
先把 block 0 清零 → 写入镜像的非零 256 KiB chunk 和 checkpoint 描述区 → 最后写 block 0
（每个 chunk 都先读旧内容、带旧 sha 写入并回读核对）→ 再次核对已有文件和 root 头未变 → 输出私有 manifest。
新建容器的实际写入量：8 GiB 镜像约 0.5–1 MiB。
只写稀疏 chunk 就够的理由：容器只能经由 block 0 和 checkpoint 描述环进入（这两处都显式写入，零也写），
其余块只会通过它们的引用被访问。

## 阶段

- **A**（本分支）：只建 System 卷。预期结果：发布 `X9APFSContainer`，4 条 `SWEET PRELINK: declined`，
  apfs 加载结果 0x0，容器 attach 成功，`mount -P` 仍报 data volume missing（与现在一致），观察 49154 是否消失。
- **B**：加 Data 卷，并把 `/private/var` 灌进去（需要正确的属主：Mac 上要 `sudo`，或者在设备上首次启动时做；待定），
  然后由 `mount -P` 把它挂到 `/private/var`，usermanagerd 会话卷才能解析。

## 回滚

boot_b 刷回上一个候选，或者去掉 `x9_apfs=1` → 窗口不发布，apfs 不加载。pinned 文件留在 userdata，无害；
删除它需要 `chattr -i` 并解除 pin，必须先征得用户同意。Android 数据始终不受影响。

## 尚未证明

- apfs 2142 在 SWEET 上 start 是否成功（effaceable/AppleImage4 的 shim 路径没有上机验证过）。
- P0 的 `x9_iomedia=1` 发布路径本身还没上机启动过（之前只构建了 prelink2）。
- 49154 是否只需要 System 角色，这是静态分析结论（任务 #1），尚未上机。
- 闭包重叠检查是 O(n·m)：实际 extent 数量很小；最坏情况下启动阶段会有约 0.1–0.3 s 的扫描。
