# X9 工作约定

- 遇到 XNU/iOS 用户态兼容问题，优先直接检查本地 UniXNU 与 hackios-xnu-kernel 的现成修复、构建输入及验证脚本，再结合当前实机日志选择移植；确认参考没有覆盖后再自行设计。默认参考路径为 work/references/UniXNU 与 work/references/hackios-xnu-kernel；记录使用的 commit、适用版本和具体文件，检查 MT6993/16KiB/原版 Apple 字节约束，不能把参考 QEMU 成功写成 X9 实机成功。

- 设备由用户手动刷入候选并测试；不要自行 flash、reboot 或写分区。
- 2026-10-25 用户要求：以后不再做构建产物的二次检查。正常构建内置检查保留；构建成功后不要再额外解包、重复算镜像 hash、反汇编验证或重建对比，除非用户明确要求。
- 2026-10-28 用户进一步要求：后续直接编译、打包，不额外运行测试脚本或测试套件；仍可读取源码与实机日志定位问题。
- 用户报告已刷入测试后，优先提取该次运行的 pstore 日志；不能因为设备已恢复 stock Android，就判断候选未刷入。
- 原始日志与可读副本分开保存，不覆盖前一版本；ECC 损坏字段不得猜成完整读数。

- 后续默认不设诊断检查点复位：保留屏幕和持久日志，持续运行到实际等待或故障；除非用户另有明确要求。
- 不新增过程报告、timeline 或回读 Markdown；仅保留必要原始日志、解码结果和构建输入。

- 用户当前选择 data-a-live 路线：在专用 F2FS pinned 文件占用的 extent 内保存明文根盘，保留 Android userdata，不清除、不格式化、不切分，也不复用 X676C 的分区写入流程。用户已授权为此创建专用文件及写入其独占块；其他块和 boot 分区不在自动写入范围。
- 当前持久根盘为 `/data/local/tmp/x9-iosroot-20261026-202513/iosroot.hfs`，64 MiB、29 extents、pin + immutable；块表与 SHA256 在 `work/x9-ufs/plaintext-root-20261026-202513/root-manifest.json`。不得正常覆盖、删除或解除 pin/immutable；访问镜像内容须按 extent 从 raw userdata 读，Android 文件读取结果是解密后的噪声。
- pan73 从 UFS 读明文根盘到已验证 legacy512 RAM 后以 md0 启动；不是原生 XNU UFS 挂载。刷机前运行该候选 `check-root.sh` 核对当前 pin、immutable、inode、extent、映射和裸读 SHA256；shim 端仅只读且完整 SHA256/HFS 校验失败不进入 XNU。

- X9 实机为 12 GiB RAM；用户硬件采集在 `/Users/tosasitill/Downloads/01-Android/内测/x9内测/final/x9`。不要将当前 512 MiB XNU 默认窗口说成硬件容量限制；boot 分区 64 MiB，用户希望在实际加载边界内尽量补全系统内容。

- 用户随后要求完整系统并模仿 data-a-live 原生系统加载路径。完整 OS+SystemOS+AppOS 已复制为 16 GiB HFSX，Apple 可执行文件字节未改；保存在 `/data/local/tmp/x9-ios-full-20261026-222518/iosroot-full.hfs`，pin + immutable、3814 extents、完整裸读 SHA256 已验证。清单为 `work/x9-full-root/20261026-222518/native-root-manifest.json`。旧 64 MiB 根盘保留。
- pan80-native-full-root 新增 patch0196 原生 X9 `ufsroot0`：shim 只初始化 UFS、验证镜像头并交接 active/idle controller，XNU 自有连续 DMA 和块设备按需读，`rd=ufsroot0 rootrw=0 ufsrw=0`。无完整镜像 RAM 复制；原生内核接管/挂载尚待实机验证。启动失败不得描述为完整系统已启动；当前驱动拒绝介质写入，后续 /var 写入可能 EROFS。

- pan80 已有完整实机证据：原生内核 UFS 接管、16GiB/3814 extents 的 `ufsroot0` 注册和 HFS 挂载成功，`vfs_mountroot returned 0`。证据 `logs/x9-boot-pan80-native-20261026-231351.console.raw`（原始工作树日志目录）；CRC345条通过、0拒绝。随后 exec `/sbin/launchd` 因 decmpfs 未注册压缩类型4而 errno45/ENOTSUP；PID1 尚未启动。应解开存储压缩或补解压器，保持 Apple 可执行文件原字节，不能把该错误误判为 UFS 挂载失败。

- pan81-decmpfs-zlib 加入 patch0197：X9 decmpfs 初始化时注册 zlib xattr type3 / resource-fork type4，只读分块解压。实际 launchd/dyld/xpcproxy 压缩资源叉的完整输出与原字节一致，60次随机读通过，8项测试和内核编译链接/打包通过；未刷机，处理器实机尚待验证。16GiB根盘未改写。

- pan81 实机记录 `logs/x9-boot-pan81-decmpfs-20261026-233301.console.raw` 确认 type3/4 解压器注册、原生根盘挂载及 launchd PID1 进入 libignition（hello from launchd.1）；随后 Preboot 查找 errno25/ENOTTY，dyld 未加载 shared cache，因 `/usr/lib/libSystem.B.dylib` 找不到退出（namespace6/subcode1）并 panic。并未完成完整用户态/UI启动。CRC345通过0拒绝。下一步是 Preboot/Cryptex 与原版 dyld cache 加载语义，不再误判为解压器未注册或 UFS 挂载失败。

- pan82-cryptex-cache 加入 patch0198，配 `ignition_level=1 x9_init_cache=1`。仅在已激活的 X9 原生根盘上给 PID1 初始环境设置 DYLD_SHARED_CACHE_DIR 指向现存 Cryptex 缓存目录，不改 Apple 二进制、不修改根盘、不伪造 APFS ioctl。原版 dyld 机器码回放确认 level1 跳过 Preboot、显式缓存 FD 在 ignition 返回无缓存时保留；42个子缓存头 UUID 与主表相符，libSystem.B.dylib 位于缓存。11项 copyout/布局/禁用/边界检查及内核编译链接打包通过。未刷机；实际共享缓存映射与 launchd 主程序仍待实机。

- pan82 实机回读 `logs/x9-boot-pan82-cache-20261027-000016.*` 明显损坏，CRC24通过/13拒绝；能看到受损 PID1 缓存环境诊断、ignition level1 跳过 Preboot 和最后 no dyld cache，不能据此重建具体 syscall errno/FD。UniXNU ae687009348296e97e49ce6e925795044f363ce6 的 offline-owner-systemos-mount.patch 与对应检查脚本记录过 ignition cleanup 后缓存目录 FD 失效，采用原版 dyld skipIgnition(bit3)。
- pan83-skip-ignition-ufs / patch0199 在已启用 X9 原生根盘初始环境复制成功后，以 commpage_update_dyld_flags 发布 bit3，保留其他位；原版机器码回放确认不调用 ignite，显式 fd0/3/71 保留。并消除每次原生读重复完整 extent 排序，初始化完整校验和每读选中 extent/请求范围校验保留；SCSI DMA 仅同步使用区域。4KiB读同步160KiB降至9808字节；23项分离CPU/设备DMA模型测试、11项初始环境检查、内核编译链接和打包通过。首4MiB及每64MiB记录累计IO耗时，非逐次读日志。没有发现带宽节流，未改变链路gear；CPU微基准不是硬件吞吐。尚未刷机，根盘与Apple文件未改。

- pan83 实机 `logs/x9-boot-pan83-type14-20261027-001418.*` CRC345通过/0拒绝：skipIgnition=1/dyld_flags=8 和 PID1 缓存目录已生效，随后3次 decmpfs 未注册 type14、dyld cache加载失败，PID1 namespace6/subcode1退出并panic。type14为LZBITMAP资源叉，不是zlib或LZFSE。只读宿主镜像检查44个缓存文件（含symbols）发现3个type14，清单 work/x9-decmpfs/pan83-cache-compression-types.json；具体运行时报错文件未打印，不可仅凭次数对应。IO统计4297216字节/98次、7177819us、584KiB/s，包含DMA/CPU而排除console，不是链路gear测量。下一步补LZBITMAP/type14并用实际资源叉验证，根盘未修改。

- pan84-all-decmpfs / patch0200 为 X9 注册3/4、7/8、9/10、11/12、13/14（zlib/LZVN/raw/LZFSE/LZBITMAP）的内联和资源叉读取，builtin type1保留。完整只读宿主镜像清点 type9=137169、type4=28682、type14=1640；全量137169个type9和1640个type14、1031174209字节解压内容与宿主原字节一致，0错误。ZBM尾部允许受frame/子流边界约束的填充token和数据，不输出到声明大小之外。8项全类型、8项zlib回归和27000次ASan/UBSan异常流检查通过，内核编译链接/补丁应用/打包通过；尚未刷机，真实共享缓存映射和完整用户态仍待实机。根盘和Apple原文件未改。冻结源/fixture/检查结果位于候选目录和 work/x9-decmpfs/all-codecs。
- 用户要求对比UniXNU：已按 ae687009348296e97e49ce6e925795044f363ce6 的 profile/组件/补丁/检查脚本审计，结果 work/x9-decmpfs/all-codecs/kernel-gap-comparison.json。主要差距：原版组件预链接框架、可写Data/卷语义、完整Image4/AMFI/CoreTrust/Sandbox/DER权限接口、原版图形ABI及MT6993 GPU/触控/外围驱动；不能称UEFI或4KiB页本身为我们必须补的缺项。它参考profile为iPadOS17/XNU10063且包含dyld缓存/BTAudioHAL text_fixups；不可照搬这些地址、二进制或指令改写，继续保持20H392 Apple字节原样。

- pan84 实机日志 `logs/x9-boot-pan84-all-codecs-20261027-005934.*` 已提取；console73320字节、pmsg64475、expdb256MiB。console严重损坏，CRC21通过/9拒绝，不可修补猜测错误码和firmware路径。可辨认shared-region多文件映射、原版launchd bootstrapper日志/system域创建，随后fsck与mount-phase-1；说明已进入launchd主程序和原版引导任务，不能声称完整UI或全部缓存页面已验证。当前为mount-phase-1失败，记录含只读文件系统、根挂载失败、firmware bindfs查找失败以及mount[3]退出/panic；精确syscall errno需后续观测。当前根盘介质只读且缺可写Data是已知约束。没有再次出现type14未注册错误；本次未改设备/根盘。

- 用户要求按data-a-live继续后，pan85-writable-extent-root / patch0201实现专用extent内可写HFS：`rootrw=1 ufsrw=1`，WRITE10/16、512字节请求的4KiB读改写、SYNCHRONIZE CACHE，内核私有map绑定与每命令范围门，shim仍只读。新增副本 `/data/local/tmp/x9-ios-rw-20261027-010328/iosroot-full.hfs` 16GiB、300extents、pin+immutable；初始全镜像写入及独立裸读SHA均 a49953b8c02720732527ed1382f56ddb589e77fbc50ab4591a7feb8389a5d4d8。原16GiB/64MiB/8MiB保留，不写入、不解pin、不删，3844个保护extent在ADT禁止重叠。manifest `work/x9-writable-root/20261027-010328/native-root-manifest.json`。
- 可写副本只有新ADT合约与显式ufsrw双门才启用写；旧候选无合约即拒绝开启写。可写副本启动后HFS头与内容可合法变化，初始全SHA仅为provisioning provenance；后续preflight检查设备/inode/pin+immutable/map/dm/分区/HFS UUID与容量，shim/kernel保留UUID/几何/map保护，不再把旧卷头hash当成不可变断言。旧只读根盘仍严格hash。HFSX副本初始非journaled，异常复位后有dirty/recovery风险；继承0031的dirty非journaled root行为。28项可写transport(含读回归)、23项只读、4项manifest以及实际enable_writes的14种mock/ASan/UBSan门检查通过；内核编译链接、0201应用、shim打包和只读preflight通过。尚未刷机或重启，X9原生介质写及mount-phase-1通过仍未实机验证。

- pan85 实机 `logs/x9-boot-pan85-rw-20261027-011911.*` console缺失（65字节是cat错误文本，不是日志），pmsg严重损坏CRC9通过/3拒绝，最高有效stage99；expdb未恢复XNU console。只读裸读可写副本卷头writeCount73695->73696；catalog326204条记录与源相比仅204字节accessDate变化，包含dyld/cache/launchd/mount/mount_hfs，表明元数据落盘和后期启动文件读取，最终mount错误码不可还原。work/x9-writable-root/pan85-runtime保留header/catalog及差异JSON，root-after-pan85.partial.raw是不完整528MiB捕获，禁止当成完整HFS镜像挂载。
- pan86-persistent-mount-log / patch0202补独立磁盘console日志，新增8MiB pinned+immutable `/data/local/tmp/x9-bootlog-20261027-012658/console.bin`，3extents，与根盘/回退/测试保留范围全不重叠；新log_policy在候选root-manifest内，原根盘不重写。共享同一idlehost/DMA和mutex，单独已绑定map视图，不二次接管controller。console ring每2s增量落盘，mount syscall前后打印参数/errno并flush，critical exit在core/sync/panic前flush；不自动reset/reboot。日志头X9UFSLOG含first/total/seq和CRC32，仅头部CRC不代表payload每字节验证。pull_disk_log.py只读校验pin/inode/map后从raw读出并解码，输出必须唯一不能覆盖。
- pan86实际logger源码ASan/UBSan模型测试(增量/wrap/失败/poison/decoder)、5项manifest、只读preflight、0202应用和内核编译链接/打包通过；未刷机。此版修复日志证据缺口，不得声称已修复未知的最终挂载根因。继续由用户手动测试后读取磁盘sidecar。

- pan86首次独立disklog成功：`logs/x9-pan86-disk-20261027-013915.{raw,payload.raw,log,json}`，header CRC有效、seq25、total20934，payload无独立CRC。实测HFS根更新mount errno0，mount-phase1/2继续，corecrypto FIPS所有测试通过；GetMasterBlock errno16与firmware/Pearl/factory bindfs ENOENT本次并非致命点。最终原版keybagd无法创建/private/var/keybags/systembag.kb.writing(ENOENT)，NoCrypto kb_load=-7，主动请求recovery重启并正常卸载HFS。不要继续误判为root mount失败。
- pan87-keybag-bootstrap / patch0203：参考dali mkadt_pandora.py补空/options（原no-effaceable-storage=1已存在），加显式x9_bootstrap_dirs=1，在已激活可写X9根盘上、exec launchd前mkdir /private/var/keybags 0700；只创建缺失目录，EEXIST需NOFOLLOW确认目录，错误上抛。不预制keybag，不改Apple二进制，由原版keybagd生成自己的plist。实际helper11项ASan/UBSan检查、ADT回环、0203应用、内核编译链接/打包及根盘+sidecar只读preflight通过。构建未写根盘或刷机；keybag文件创建仍待实机。已有SweetKeyStore与dali0038基本相同，无后续可照搬的完整keybag实现。

- pan87 实机pstore `logs/x9-pan87-pstore-20261027-014750.*` CRC240通过/0拒绝：PE_init_iokit时IONVRAM找不到nvram-bank-size，IONVRAMCHRPHandler创建失败并panic @IONVRAM.cpp1380。是pan87补空/options触发当前内核IONVRAM初始化的回归，尚未到BSD/UFS/keybag目录创建。`logs/x9-pan87-disk-20261027-014739.raw`与上次pan86逐字节相同(seq25/20934)，必须标记stale，不可当成本次keybag结果。下一步补齐准确NVRAM DT/后端契约，或限定兼容实现；不能仅因dali有空/options就当作可直接套用。

- 用户要求“把dali有的全都加上，只要不冲突”：已以91f1f2468f30841c39123c7ba20ae520ecd82c84为输入清点全部233份kernel patch，work/x9-dali-compat/dali-patch-inventory.json逐项记录路径、forward/reverse check和理由。79份与本地patch字节一致、16项由X9对应实现覆盖、10份迁入兼容部分、128份存在硬件/ABI/策略/采样拓扑依赖冲突。不能把机械apply成功当成MT6993兼容或硬件验证。
- pan88-dali-compatible / patch0204修pan87 NVRAM回归：dali mkadt_pandora.py实际已有empty_nvram_proxy，不是只有/options。现迁入匹配的8KiB RAM-only CHRP proxy（bank-size/count/current/data，common+free空分区，header checksum/Adler），真实当前XNU解析函数ASan/UBSan接受，且与dali生成字节一致；无物理NVRAM/持久化声明。补matching software policy/BNCH和no-home-button元数据，原/options及keybags目录创建保留。
- pan88兼容部分：0080可选IPv4追踪(x9_net_trace)；0095/0100可选sweetnoexec/execswap；0123可选x9_initwait(max600)；0158原生X9 UFS command/device/lock详细计时；0199/0204 IOSurface零值row/plane-size回退+范围校验，plane row写+0x10且保留+0x08；0208/0209 X9console时间戳与拆分asm；0210仅IORTC readiness资源（仍是软件时间回退），不带4KiB hash或QualcommRTC。可选进程屏蔽/替换/trace/wait均未在候选bootargs启用。
- dali0222/0223/0225采样器会跳过自身CPU，仅cpu_xcall采其他CPU，与当前已验证cpus1冲突；已从候选撤下，未链接profiler或ASThook。移植中间源码仅在work/x9-dali-compat/deferred-profiler留作将来本地timer适配，不算完成。其余Adreno/SMMU/SCM/SPMI/触控/DPU/音频、4KiB或全局iOSvtable/prelink变更不可混入现有16KiB/MT6993。
- pan88 NVRAM/Surface/exec/timestamp实际helper ASan+UBSan、28项UFS写回归、5项manifest、0204源码应用、内核编译链接/打包通过；本次只读root+sidecar preflight因adb设备列表为空、serial not found而未能运行（不能写为通过）。未刷机/重启或改根盘，真实IONVRAM启动和keybag创建仍待实机。

- pan88实机 logs/x9-pan88-20261027-090814.disk.* 为新sidecar(seq24/25742、headerCRC有效)，NVRAM8KiB bank/common解析成功，1.684s keybags目录ready、60.677s root HFS update errno0、111.379s corecrypto FIPS全通过；114.783s keybagd仍打不开systembag.kb.writing ENOENT，kb_load=-7并主动recovery重启/正常卸载。目录ready不能证明后期创建语义，不能推断目录消失；pstoreCRC18通过7拒绝损坏明显。
- 按“优先UniXNU/hackios现成修复”查到 UniXNU ae687009348296e97e49ce6e925795044f363ce6 scripts/probe-keybag-bypass.py 使用原版 -apfs_shared_datavolume。其验证为iPadOS17/QEMU，不能照抄“no system keybag”日志或当成X9成功。hackios c84b4634277a70beac73921029b1fb6afc2530af MiniAppleKeyStore.cpp为无真密钥的软件服务，没有找到相同bootarg修复；当前日志未触发其额外selector，不盲换实现。
- pan89-shared-data-keybag只在pan88 bootargs增加-apfs_shared_datavolume，复用冻结pan88内核，不增内核patch，不造keybag。原版20H392 keybagd实际机器码0x10000cee0读取kern.bootargs识别此flag；init gate0x100012bec命中则跳到0x100012bfc，跳过触发fatal的load_devicebag0x100012c30。work/x9-keybag-shared-data/replay_keybagd.py回放4种输入及完整224字节候选args均通过；OS接口mock，不是完整用户态/硬件验证。host原版HFS只读挂载提取keybagd后已detach。
- pan89打包和当前根盘+sidecar只读preflight通过，未刷机/重启/改根盘。实机应检查iOS16的****** DONE MKB_INIT ****及下一boot task，且无FATAL KEYBAG ERROR/recovery。这个改动选择原版shared-Data初始化路径，不等于已修复底层ENOENT、不等于真实systembag/SEP/DataProtection，也不等于完整UI启动。

- pan89实机已验证shared-Data参数生效：logs/x9-pan89-20261027-093056.disk.* seq35/35834/headerCRC有效，内容不同于pan88；pstore CRC345通过/0拒绝，最高stage99。114.616s原版keybagd DONE MKB_INIT、129.059s原版usermanagerd DONE UMD_INIT，随后xpcroleaccountd/featureflags/tzinit/PRNG等boot tasks继续；此前kb_load fatal/recovery路径本次未出现。这是引导步骤通过，不是真systembag/SEP/DataProtection或完整UI成功。
- 新精确致命点：165.925s launchd_cache_loader abort Unable to find AMFI；pid22 namespace7/subcode1，165.933s critical exit，pstore166.019s同因panic。现有SWEET AMFI MAC policy早期已注册，不能称为AMFI完全没初始化；需按优先UniXNU/hackios规则查实际AMFI用户态lookup/服务接口。UMD sel125/120 Unsupported、foreground/multiuser commpage、APFS默认session mount49154及notify错误存在，但其后仍DONE UMD_INIT并进入后续任务，不能据此替换本次精确致命原因。详细JSON为work/x9-keybag-shared-data/pan89-runtime-assessment.json。

- pan90-launchd-cache：先按规则查UniXNU ae687009348296e97e49ce6e925795044f363ce6 audit-amfi-pmgr-contract.py（完整原版AMFI/PMGR预链接及provider/vtable依赖，不能直接混入SWEET），再查hackios c84b4634277a70beac73921029b1fb6afc2530af kern_minimac.c（0x60 bootarg-state供ExtensionKit，当前loader并不走此hook）。原版20H392 launchd_cache_loader实际IOKit契约是AppleMobileFileIntegrity、type0 userclient、method6 scalar1+cdhash；不能只注册同名空服务就当修复。
- dali 91f1f2468f30841c39123c7ba20ae520ecd82c84 tools/ios_test.sh:17已有launchd_unsecure_cache=1。原版loader0x100002488读kern.bootargs、0x1000024a4找launchd_unsecure_cache=，真实十进制parser命中非零后转0x10000281c，调用0x1000028c8读原版缓存并复制到launchd提供的Mach memory-entry。pan90仅加此flag，保留pan89所有参数/冻结pan88内核；无新增kernel patch，不运行dali自动flash/reset工作流。
- work/x9-amfi-cache-loader/replay_cache_loader.py用原始Apple机器码验证7种输入：缺省/0/无效值/sysctl失败走签名服务路径，1/logfopen失败/完整249字节候选args走缓存交付；原始约1.4MiB launchd.plist逐字节复制一致且函数返回，覆盖真实sysctl128字节初缓冲扩容。libc/OS/Mach为mock，不代表真实PID1接收/解析或硬件启动。完整args也保留keybagd已验证的shared-Data分支。
- pan90已打包，根盘与sidecar只读preflight通过；未刷机/重启/写根盘。实机需看Using unsecure cache、Cache sent to launchd successfully及后续boot进展。此flag是原版loader未签名缓存模式，跳过detached-signature验证，不能称实现或加载原版AMFI；AKS identity/APFS session/PRNG等gap仍在。

- 用户pan90仍在运行时要求离线自检UFS低速原因：只读冻结源码，无ADB/设备操作/产物二次检查/候选修改。work/x9-ufs/pan90-speed-source-audit.json记录确切调用。确认shim full reset/link-startup分支没有后续HS power-mode/gear/lane协商；DME_SET仅d0a8/1585/d0a6/d0a1/d09e，未设PA_TXGEAR1568/PA_RXGEAR1583/PA_PWRMODE1571/active lanes/HSSERIES。native kernel接管后也不补。若入口继承HCE/HCS ready则早退保留原link，不能一概说每次都reset或已实测PWM_G1。
- 正常完成轮询为10us，100ms仅初始化TUR busy/notready重试；普通read路径无固定100ms等待。kernel计时/延迟实际读取CNTFRQ_EL0。单槽串行/64KiB请求是已知限制。OPPO MT6993原驱动pre_pwr_change(1812+)有我们遗漏的HS协商阶段；复位后停默认低速是当前主要嫌疑，尚未读回当前PA mode/gear，不能称已证明唯一根因。先保护正在运行的测试；下版先读回实际模式/gear/lane/能力，再按MT6993实证序列处理，不能直接猜最高gear。

- 用户已明确授权直接设置UFS提速；当前pan90仍在运行，本轮不得ADB干扰/自动flash/reboot。pan91-ufs-hs-g1新增shim ufs_hs.c/.h和probe fDeviceInit后调用，主动切HS_G1/RateB/FASTAUTO(0x44)，按localTX/peerRX与localRX/peerTX分别取最多2条物理连接lane。已有HS(gear1..5、1..2lane、A/B)保留；不假设G4/5电压/时钟/ADAPT已实现。
- HS依据OPPO MT6993 blobf0f71e00183f725699df5f54f6de338e34fa1b5f pre_pwr_change以及ufshcd-linux612的标准PA顺序/独立UPMS完成契约；NO_ADAPT3用于G1，term1、timeouts和series2明确设置。若refclk未REQ+ACK，使用OPPO寄存器0x144及已知kernelSiP0xc2000276 cmd8(on,stage0/1,host0)，3ms ACK bound+32us wait；该cmd8此前未实机验证，不能声称clock/PHY已工作。
- pan91要求UIC完成+独立UPMS+UPMCRS=PWR_LOCAL1，之后严格读回mode/gear/lane/series，失败不标rootvalid、不继续未知链路I/O、不自动复位。patch0205 native kernel在x9_ufs_hs1下独立DME_GET验证并输出X9 UFS LINK到持久console，接管不重新切模式。
- 11项实际HS模块模型测试/18故障情形ASan+UBSan、19项旧link、28项旧write回归、0205应用、内核编译链接/正常shim打包通过；完整261字节bootargs原版keybag/cache-loader回放通过。模型不模拟MT6993 PHY；硬件模式/SiP8/吞吐未验证。boot.img在x9/out/pan91-ufs-hs-g1，未进行ADB/preflight(当前测试仍运行)、刷机/重启/根盘写入。旧pan90保持冻结。

- pan91实机 logs/x9-pan91-20261027-103928.disk.*新sidecar(seq255,total921681,headerCRC有效)：0.211s内核独立读回HS mode0x44/TXG1/RXG1/TX2lane/RX2lane/series2(B)，HS切换已硬件验证。UFS统计约133k–159kKiB/s(约130–155MiB/s，按io_us活跃IO耗时，非整机wall吞吐)，不再是0.6MiB/s。2.102s DONE MKB_INIT、2.629s DONE UMD_INIT、3.209s Cache sent to launchd successfully。
- 本次屏幕停Apple logo/backboardd arm-io并非完整日志停止：backboardd17.044s解析arm-io后仍查询chosen/disp0/backlight等；SpringBoard自7.627s起反复sig6崩溃，ns8/code1明确缺/usr/lib/libobjc-trampolines.dylib(no such file,not in dyld cache)，其他多个daemon相同。大量累计IO为崩溃重启重复读取，不是固定总量加载进度。磁盘日志截至约490s，不能据此宣称完整UI或硬件合成已工作。优先看UniXNU check-native-ram-data-checkpoints.py同错及check-owner-union-checkpoints.py对SystemOS/usr/lib/libobjc-trampolines.dylib的路径/union交付验证，再核对实际20H392文件；不改Apple字节。
- 本次pstore console缺失，65字节cat错误不能当console；pmsg CRC11通过13拒绝；以disklog为主。work/x9-ufs-hs/pan91-runtime-assessment.json保存逐项结论与崩溃计数。
- pan91进一步可见backboardd17.101s open AppleCLCD type0成功、多个FB selector返回0；随后反复匹配IOAcceleratorES为0 found，图形加速服务缺口也存在。不能承诺只补libobjc-trampolines就能出桌面，先处理明确的SpringBoard87次崩溃循环，再定位实际图形ABI/等待。

- pan92-cryptex-objc-graphics / patch0206按UniXNU/hackios先查：UniXNU check-native-ram-data-checkpoints.py记录同缺库，check-owner-union-checkpoints.py验证SystemOS/usr/lib正路径暴露；不搬iPadOS17库/hash/4KiB unionABI。当前20H392原HFS实际已有/private/preboot/Cryptexes/OS/usr/lib/libobjc-trampolines.dylib(68080bytes,原内容清单SHA16a48e7784630faa0aa893443033a13f6bb465ce986b11bf0bbd8d9b9b77e46c)，/usr/lib正路径缺失。hostHFS仅只读检查后detach。
- pan92新增x9_cryptex_aliases1：exec原版PID1前、只在active可写native root，用普通symlink syscall补缺失/usr/lib/libobjc-trampolines.dylib指向现有原版Cryptex文件；保留已有regular，核对已有link和新建link的解析vnode等于持有source，冲突不覆盖、copyout有界、错误上抛。此次构建未写根盘，链接将在用户启动候选时由XNU创建，不改Apple库数据。
- 图形复用现有SweetFramebuffer IOAcceleratorES类（不能重复定义）：去掉X9分配排除，仅在exactX9/QCOM分支发布无driver properties服务，参照hackios MiniIOMobileFramebuffer.cpp1471+。sweetaccel0仍可禁用；exactx676c GPU power/MMIO条件保持，不加X9 Metal/OpenGL插件名、ID或userclient，不宣称硬件GPU/软件合成/桌面已工作。
- pan92实际alias helper15种POSIX namespace场景ASan/UBSan、实际passive服务7种场景ASan/UBSan、冻结源测试、0206应用精确匹配、内核编译链接/打包和完整282字节参数原版keybag/cache-loader回放通过。此次ADBdevices为空、serial not found，root+sidecar preflight未能重跑(不是通过)。未刷机/重启/写根盘。实机看Cryptex helperready、IOAcceleratorES匹配、SpringBoard是否越过缺库及下一具体失败；不能直接承诺出桌面。

- pan92实机 logs/x9-pan92-20261027-110344.disk.* seq75/161206 headerCRC有效：0.215s HS0x44/G1/2lane，0.301s passive IOAcceleratorES发布，0.325s原版ObjC Cryptex链接ready；backboardd10.079s找到IOAcceleratorES并建1256×2760 IOSurface、10.080s调用SwapBegin/SwapEnd，launchd同秒Early boot complete；SpringBoard到AppleKeyStore/IOPMrootDomain/buttons，捕获中无CRASH记录或缺libobjc错误。计数至218.967s 12000 swaps仅证明提交/计数，不是面板扫描/桌面证明；owner画面未反馈。pstore里程碑CRC19通过12拒绝，侧盘payload无单独CRC。
- pan93-userspace-compat patch0207按hackios/UniXNU迁入：X9 AMFI策略名AMFI及syscall0x60/0x5a（不改变签名页校验）；40项仅当前cred且validated XML entitlement匹配的MAC privilege授权；AKS selector31软件无密码配置DER（profiled实测unsupported）；X9 CoreAudio work-interval 6/7/8有限参数成功；X9重复CA_CLIENT标记ignored；X9禁用未验证系统sleep。非X9沿用既有策略；不声称真实AMFI/SEP/密钥保护。
- pan92 securityd selector10实际32次unsupported，hackios MiniAppleKeyStore的固定XOR可逆wrap无秘密，不能直接套到持久用户keychain并谎称Data Protection；当前仍未实现，AKS 120/125/93/34、backlight/实际图形仍待处理。新候选policy/workinterval/selector31 helper ASan/UBSan测试、最终0207源应用匹配、内核编译链接/打包、完整bootargs回放及根盘+sidecar只读preflight通过；未刷机/重启/写设备。
- work/x9-userspace-compat/reference-inventory.json列出UniXNU59个patch与hackios73个patch文件段及110个added-source路径，并保留dali233项旧清单；其中UniXNU22项及hackios37段仍须逐项语义审查，added source并未全部审完，不可称三项目所有可移植项已完成。

- pan93用户反馈无Apple logo；新日志logs/x9-pan93-20261027-112247.disk.* seq27/134376，内容区别pan92且AMFI注册名AMFI。0.213s HS G1双lane；0.322s Cryptex链接ready；2.123s DONE MKB_INIT、2.652s DONE UMD_INIT、10.298s launchd Early boot complete；10.297s GUI takeover/text console muted且swap2，10.627s swap3，32.818s 1000 swaps，最后34.141s。捕获无panic/CRASH；不代表实际显示或后续一直存活。用户无logo为事实，swap仅提交不证明扫描/像素/亮度。pstore console65字节错误文本，pmsg CRC13通过11拒绝；磁盘头CRC有效但payload无独立CRC。work/x9-userspace-compat/pan93-runtime-assessment.json记录。下一步对比pan92/93及实际帧缓存/scanout，不能猜测MT6993面板重初始化或把用户画面说成已显示。

- pan93无logo离线检查：pan92/93冻结SweetFramebuffer.cpp相同，GUI takeover/前几次surface/swap日志基本相同，不能归因pan93 policy变化。X9不分配QCOM fVig0/fCtl0 flip，present() copy路径先bzero目的fPixels(固定0xfc910000)，再复制source；shim可能有多个继承LK scanout slot，kernel只写fPixels，当前没有本次active地址/像素/亮度证明，不能凭代码断定是哪一环。
- pan94-frame-observe / patch0208只在前4次X9 copy present打印32×32网格source/destination的sample/nonblack/alpha/rolling checksum、surface/mask/is_boot_fb，无原始像素/屏幕快照、无额外帧写/MMIO寄存器改动。可区分采样黑/复制内容异同，无法证明全帧内容、DMA实际扫描/DSC/亮度；source仅在合格全屏层打印，缺行不等于黑。sample helper ASan/UBSan、0208应用匹配、内核编译链接/打包通过；ADB设备不在，root+sidecar preflight未通过/未运行；无刷机/重启。

- 最新回读 logs/x9-display-followup-20261027-112946.disk/console/pmsg.* 为新日志，disk seq30 total153547 headerCRC有效；console+pmsg里程碑CRC345通过0拒绝。未捕获X9 FB PROBE，不能据此推断用户刷了哪版。10.966s GUI takeover/swap2和Early boot complete，11秒SpringBoard，之后多次swap；console 41.247s明确panic IOGMD: not wired for the IODMACommand @IOMemoryDescriptor.cpp:3248，未见调用栈/进程身份，发生在首次swap之后，不能直接断定是无logo最初原因，也不能排除0207间接影响。
- 同次0.277s起60次HFS BlockMarkFreeInternal释放未分配块及60次hfs_mark_inconsistent；只读裸取专用可写根盘头 logs/x9-display-followup-20261027-112946.root-header.raw，HFSX属性0x80004000：kHFSVolumeUnmountedMask0x100未置、JournaledMask0x2000未置、InconsistentMask0x4000已置；UUID仍4f2e61fc7c8f54fb。根盘明确非正常卸载且标记不一致。不能继续把普通inode/map/UUID preflight通过当作文件系统健康；在保留/捕获和只读检查前停止反复启动该可写副本，不自动fsck修复/覆盖旧数据。后续在干净隔离副本上做单变量回归并追IOGMD对象生命周期/调用栈。详情work/x9-display-probe/followup-runtime-assessment.json。
