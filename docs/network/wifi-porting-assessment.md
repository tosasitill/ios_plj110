# Wi-Fi bring-up assessment — MT6653 (CONNAC 3.0) on MT6993, XNU target

- Read-only assessment. **Nothing here is implemented and nothing here authorises a flash,
  a partition write or a power-on sequence run against the device.**
- Assessment date: 2026-11-03. Branch `feat/G1Ultra` = `main` = `2c9b167`.
- Inputs cited:
  - `oppo-source` = `/Users/tosasitill/HelioDarwin/oppo-source` (user-supplied: MediaTek
    vendor kernel modules + GKI `android_kernel_oppo_mt6993`),
  - `x9` = this repository,
  - reference snapshot `x9/.local/analysis-blur212/unixnu-current` (partial).
- External guide: `https://unixnu.com/zh/porting/network` (Wi-Fi 适配). It documents a
  **Qualcomm WCN7850 on SM8550** route whose central asset is a prebuilt, OS-neutral driver
  library reached through `nw_abi.h` callbacks (`lx_platform`, `nw_board`). Labels below:
  **MEASURED** = read from a cited file/line; **INFERRED** = reasoned, not confirmed.

## Verdict

Wi-Fi is the hardest subsystem attempted in this port, harder than the GPU stack. The driver
body is not the problem — MediaTek's core is already OS-portable. The problem is the two
layers underneath it, for which this port has zero precedent, and the iOS contract above it,
which expects a Broadcom-shaped kernel interface.

| Area | State in this port | Difficulty |
|---|---|---|
| Wi-Fi driver core | Source-complete, has non-Linux ports (`os/windows`, `os/uefi`, `os/none`) | L, mechanical |
| PCIe host controller | **Absent entirely.** No PCI code, no ADT node, no kext loader | XL, new substrate |
| Connectivity power (conninfra) | **Absent entirely.** PMIC/pinctrl/clock/co-clock state machine | L, highest risk |
| BSD/socket/ifnet | MEASURED compiled in (`-DINET`, `-DNETWORKING`, `-DSOCKETS`, `-DNECP`, `-DPRIVATE_WIFI_ONLY=1`) | done |
| ifnet producer | MEASURED none — naming stub only | L |
| iOS Wi-Fi userspace (wifid/Apple80211) | MEASURED never exercised on X9; Apple80211Family is not open source | XL, research |
| Firmware/cal blobs | MEASURED absent from Apple rootfs; delivery pipeline exists (mailbox + allowlist) | S |

## What the guide's four steps map to here

| Guide step (Qualcomm) | Our equivalent | Delta |
|---|---|---|
| 1 区分集成层次 (identify the component) | MT6653, PCIe HIF, `wlan_drv_gen4m_6993_6653` | MEASURED, see below |
| 2 启动 PCIe 端点 (power/clock/reset/BAR/DMA/MSI) | Implement MediaTek gen3 RC at `0x16910000` in XNU | Same shape, but the guide assumes a host that already has PCI |
| 3 提供组件回调 (`nw_abi.h`, `lx_platform`, `nw_board`) | **No equivalent.** MTK offers no prebuilt-ABI library; instead write `os/darwin/` against `gl_os.h` + KAL | We trade a callback ABI for full source; better debuggability, more LOC |
| 4 接入 OS 网络接口 (scan/connect/TX → OS) | The P4/P5 fork below | Identical problem, and the guide's own last step |
| Gotchas: firmware `c7-00108` ok / `c2-00259` fails; skip 6 GHz scan; MAC zero → self-generate | Our equivalents: firmware name set below; regulatory/scan cost; and MEASURED `CFG_TC1_FEATURE` off ⇒ MAC comes from the NVRAM cal file, so a missing/blank cal gives a zero MAC exactly like their case | Directly transferable advice |

## Measured facts

### Chip and module

- `wlan/core/gen4m/Kbuild.6993_6653:5-13` — `WLAN_CHIP_ID:=6653`, `MTK_COMBO_CHIP:=MT6653`,
  `CONNAC_VER:=3_0`, `CONFIG_MTK_COMBO_WIFI_HIF:=pcie`, `CONFIG_WLAN_PLATFORM:=mt6993`,
  `MODULE_NAME:=wlan_drv_gen4m_6993_6653`.
- `gen4-mt79xx` builds only 7902/7921 variants and has no `6993` reference → not our path.
- Unresolved: plain vs `_triband` vs `_mcl50` (three Bazel platforms exist). Resolve from the
  shipped `.ko` name on the device (P0).

### OS abstraction already exists

- Boundary header `gen4m/os/linux/include/gl_os.h` = 2,217 lines; library API
  `gen4m/include/wlan_lib.h` = 2,788 lines.
- `gen4m/os/` contains `linux`, `none`, `uefi`, `windows` — i.e. the core is compiled against
  a KAL that non-Linux targets already implement. A Darwin port is `os/darwin/`.
- Coupling is at the edge, not in the core: `net_device` 1287 hits with only 24 outside `os/`;
  the core makes 3 direct `mtk_cfg80211_*` calls and 0 `netif_*` calls. `sk_buff` 298 (7
  outside, leaking as `prMsduInfo->prPacket` in `nic/nic_tx.c`). Total gen4m ≈1.74 M lines, of
  which ≈1.04 M are register/header tables; `os/linux/*.c` = 185,390 lines.

### PCIe root port contract (for step 2)

- HIF driver `gen4m/os/linux/hif/pcie/pcie.c` = 4,051 lines; `mtk_pci_probe` at :1940,
  `mtk_wifi_probe` at :1499; IDs `14c3` / connac `0e8d` / device `0x6653`.
- Host controller is MediaTek gen3, not plain DesignWare; the only driver in the supplied
  kernel is `android_kernel_oppo_mt6993/drivers/pci/controller/pcie-mediatek-gen3.c`.
  Register map MEASURED there: `PCIE_RST_CTRL_REG 0x148` (MAC/PHY/BRG/PE_RSTB bits),
  `PCIE_LINK_STATUS_REG 0x154` bit 8 `PORT_LINKUP`, LTSSM state `0x150`,
  `PCIE_INT_ENABLE_REG 0x180` (MSI enable `GENMASK(15,8)`),
  `PCIE_TRANS_TABLE_BASE_REG 0x800` (stride 0x20, 8 entries, mem/io type), MSI set base
  `0xc00 + i*0x10` with high-address `0xc80 + i*4`, config access via `0x140` + `0x1000*port`.
- RC MMIO base used by the Wi-Fi path: `ioremap(0x16910000, 0x2000)` at
  `gen4m/chips/mt6653/mt6653.c:3564` (the `0x112f0000` branch is the MT6989 case, disabled).
- PERST is honoured with a 100 ms wait before de-assert (`pcie-mediatek-gen3.c:410-415`).
- MT6653 uses **one** MSI vector (`include/chips/mt6653.h:73-77` default
  `WFDMA_AP_MSI_NUM=1`; the 8-slot version is MT6639-only), EP-side MSI is 64-bit masked, DMA
  mask 36-bit. EP BAR must be ≥2 MiB (static bus→chip remap table reaches `0x00200000`,
  `mt6653.c:307-395`).
- Post-link vendor pokes the driver makes that a reimplementation must copy: host `+0x74`
  bit 27, L1SS/SR votes reading `+0x194`/`0x150`, EP ASPM at chip `0x74030074`
  (`mt6653.c:3581-3761`).

### conninfra (the shared connectivity subsystem)

- Wi-Fi power is `wlanFuncOn` → `connsys_power_on()` → impl → `connsys_power_done()`
  (`gen4m/os/linux/gl_init.c:10285-10322`); the MT6653 ASIC path calls
  `connv3_pwr_on(CONNV3_DRV_TYPE_WIFI)` then `kalMdelay(10)` "for pcie phy ready"
  (`cmm_asic_connac3x.c:2885-2901`).
- The real sequence lives in `connectivity/conninfra/.../core/connv3_core.c:279-441`:
  lock/fmd check → pre-cal → battery via power_supply → optional POR-reset pulse (pinctrl
  state names `connsys-pin-por-reset-trigger`/`-done`, 30 ms) → full power recycle →
  `PRE_PWR_ON` broadcast to all radios → `connv3_hw_pwr_on()`.
- `connv3_hw_pwr_on()` (`platform/connv3_hw.c:218-242`) ends in MT6993-specific PMIC writes
  (`platform/mt6993/mt6993_pmic.c:502-599`, register/value/mask triples such as
  `{0x0004,0xFF,0x88}`, `{0x0410,0xFF,0x10}`, `{0x140B,7,0}`) and antenna-power control;
  `pmic_mng_vsel_ctrl` is a no-op on MT6993 (`:603-606`).
- Clocks: `connv3_clk_init_mt6993()` (`platform/mt6993/mt6993.c:188-306`) does MT6687
  clock-buffer writes with an unlock pair (`0x3a9=0x87`, `0x3aa=0x66`); ext-32k is a pinctrl
  call with an ULPOSC fallback register sequence from `mt6653.c:4487+`; SKU detection uses an
  IIO channel named `mt6661_6_aio3`.
- A 4-byte platform-option blob is pushed to the chip at `CONNAC3X_PLAT_CFG_ADDR 0x7C05B28C`
  (`chips/mt6653/mt6653.c:314-379`, `wlan_lib.c:1287`).
- Prerequisites are separate modules: `conninfra` (module `conninfra`, exports
  `connv3_pwr_on/_pwr_off/_ext_32k_on/_trigger_whole_chip_rst/_sub_drv_ops_register`) and
  `connfem` (FEM/ePA-LNA/SKU), plus `wlan/adaptor` (`/dev/wmtWifi`). Config artifacts:
  `conninfra.cfg`, `connfem.cfg`, `WMT.cfg`. MEASURED: conninfra loads **no** connector/CLSP
  MCU firmware, contrary to the Qualcomm parallel.
- INFERRED risk: this is a multi-radio arbitration state machine driven by Linux DT,
  pinctrl, regmap and IIO. Reimplementing it as a single-owner XNU driver means hardcoding
  every name-based lookup, and getting it wrong is a power/hold-off failure, not a log line.

### XNU side today (this repository)

- Network stack is compiled in: `-DINET` (11 sites), `-DNETWORKING`, `-DSOCKETS`, `-DNECP`,
  `-DCONTENT_FILTER`, `-DPRIVATE_WIFI_ONLY=1` in `x9/kernel/recipe.json` (:3137, :5060, …);
  `if.o` :414, `dlil.o` :416, `ether_if_module.o` :417, `bpf.o` :411, `ndrv.o` :428,
  `uipc_socket*.o` :723 in `x9/kernel/link-order.txt`. **No `-DINET6` anywhere** → INFERRED
  IPv6 is compiled-out stubs.
- `x9/kernel/src/iokit/Kernel/SweetX9NetworkStack.cpp:8-10` states the truth: "X9 has no
  native IONetworkInterface producer yet… naming request unsupported". It exists only so
  configd's InterfaceNamer completes (`x9_network_stack=1`, `x9/build/boot.json:24`).
- `x9/tools/mkadt.py` publishes cpus/gic/arm-io nodes only (:79-143) — **no pcie, no wlan, no
  connectivity interrupt**. Any new driver must grow the ADT and the shim interrupt routing.
- MEASURED absent: PCIe host, xhci/USB host, conninfra/wcn register code, and any MediaTek
  coprocessor mailbox (SCP/SSPM/mcupm). The nearest reusable precedent is the UFS PHY refclk
  REQ/ACK handshake in `x9/shim/ufs/ufs_hs.c:64-90` (INFERRED as an MCU-side command channel).
- Working and reusable: `IOMemoryDescriptor`/`IODMACommand` (UFS continuous DMA), GICv3 init,
  the mailbox + allowlist + `/System/Library/Extensions` bundle pipeline used by
  `X9GeneralMetal<N>`/`X9PanVK<N>` (`x9/mailbox/README.md`, `x9/build/scripts/package-plugins.py`).
- Historical note: the USB-ECM `ue0` success in NOTES.md (~:3600) is the **SM7150** device;
  those objects are not in the X9 link order. There is no working network on X9 today.

### Artifacts a driver needs from Android

`gen4m` requests, via `kalFirmwareOpen` (`os/linux/gl_kal.c:345-388`, dirs
`/etc/firmware`, `/vendor/firmware`, `/firmware/image`): `mt6653_wifi.bin` (+ versioned and
`WIFI_RAM_CODE_6653*` aliases), `mt6653_patch_e1_hdr.bin` / `WIFI_MT6653_PATCH_MCU_1_1_hdr.bin`
(load `0x00900000`), `WIFI_MT6653_PHY_RAM_CODE_1_1.bin`, idx-log bin, TxPwr `.dat`, NVRAM/cal
(fragment support on, `mt6653.c:1291`), XO-nvram module param, plus `conninfra.cfg`,
`connfem.cfg`, `connfem_internal`, `WMT.cfg`. MAC: `.is_support_efuse = TRUE` but
`CFG_TC1_FEATURE` is off for this Kbuild → **MAC read from the NVRAM cal file**
(`gl_init.c:2978-2987`); the QA/efuse bulk path (`gl_qa_agent.c:2027-2272`) is the alternative.
Delivery must go through the mailbox pipeline; per `x9/AGENTS.md`, device dumps and
cal/nvram blobs stay out of git (`.local/` is gitignored — keep them there).

## Plan and difficulty

Effort is expressed in build+flash cycles, and only the user flashes.

| Phase | Work | Diff | Milestone (log-observable) |
|---|---|---|---|
| P0 | Collect facts from stock Android (read-only; appendix) | S | Exact `.ko`, real MT6993 DT, firmware set, IRQ/SPI list, reserved-memory map |
| P1 | Reproduce the MT6993 conninfra power-on in the pre-XNU shim, gated behind a boot arg | L | Connsys ID register readable, 32k + co-clock confirmed before any PCI access |
| P2 | Minimal MediaTek gen3 RC in XNU: clocks/reset, PERST 100 ms, link poll, iATU trans-table, config reads, one MSI → GIC, ADT node, built-in class | **XL** | `X9 PCIE: link up 14c3:6653 bar0=<size>` in the persistent console |
| P3 | `os/darwin/`: implement `gl_os.h`/KAL over IOKit + the PCIe HIF backend, then firmware download and MCU boot | L (large, mechanical; `uefi`/`windows` are the templates) | `fw_dl` EMI download ok, MCU running, host-command reply |
| P4 | Own control ABI (scan/connect/TX) + an ifnet producer + WPA2 supplicant | M | Associate, DHCP, `ping` from the device |
| P5 | iOS userspace contract: enough IO80211Family/Apple80211 ABI that wifid + Settings work | **XL (research)** | Wi-Fi toggle lists networks in Settings |

**The decision fork is P4 vs P5.** P0–P4 yields real IP connectivity but no system UI, because
the Apple 802.11 kext family is not open source and its ABI must be reverse-engineered, and
because wifid/CoreWiFi/NECP have never been observed running on X9 (AMFI is still a stub and
the keybag path runs in shared-Data mode). Recommendation: treat P5 as a separate later
research item, never as a prerequisite for P4.

## Top blockers, ranked

1. **No MT6993 device tree in the supplied sources** (`arch/arm64/boot/dts/mediatek` has only
   legacy chips). Without it the PCIe/conninfra GIC SPI numbers, `memory-region` and
   `iommu-map` are unknown, and **whether the connectivity RC sits behind the SMMU is
   unresolved** — if IOVA is mandatory, P2/P3 grow an SMMU driver. This is the single fact
   that most changes the estimate; P0 closes it.
2. **conninfra multi-radio power/clk/pinctrl state machine** — biggest correctness risk.
3. **Memory model** — several MiB of physically contiguous, 8-byte-aligned WFDMA rings plus
   7 MiB/1 MiB coredump EMI and a 128 KiB MCU EMI, normally supplied by DT reserved-memory
   (`hif_mem.c:220-245`, `gl_emi.c:26-58`), inside a 36-bit DMA window.
4. **iOS userspace contract** (P5), possibly unfinishable at acceptable cost.
5. Housekeeping: the boot-arg line is effectively full (607/608 bytes, current `x9lo` token),
   so P1/P2 gating needs a token merged into an existing umbrella, not a new one.

## Standing rules

The user flashes and tests; no step here authorises a flash, a reboot, a partition write, or
energising connsys hardware outside a gated candidate. Device dumps, serials, cal/NVRAM and
firmware blobs stay out of git. Never describe a successful compile as observed behaviour.

## Appendix — P0 collection checklist (read-only)

Run against stock Android while it still boots. Nothing below writes to the device. Stage
everything under `x9/.local/x9-wifi-probe/` (gitignored) — never in the worktree.

1. Identity/variant: `ls /vendor/lib/modules /vendor_dlkm/lib/modules | grep -iE 'wlan|conn'`;
   `cat /proc/modules | grep -iE 'wlan|conninfra|connfem|wmt'`; `getprop | grep -iE
   'wifi|wmt|connsys|connac'`. Resolves plain vs `_triband` vs `_mcl50`.
2. **The device tree** (highest value): `cat /sys/firmware/fdt` → `fdt.dtbs`, then locally
   `dtc -I dtb -O dts fdt.dtbs > mt6993.dts` (`/opt/homebrew/bin/dtc` is installed). From it
   extract the nodes whose `compatible` matches `mediatek,mt6993-*pcie*`,
   `mediatek,*connv3*`/`conninfra`, `mediatek,wifi`, `mediatek,wifi_tx_cma`,
   `mediatek,wifi_misc`, plus `reserved-memory`. Record `reg`, `interrupts`,
   `interrupts-extended`, `iommus`, `iommu-map`, `memory-region`, `clocks`, `resets`,
   `pinctrl-*` state names. **This decides blocker 1 (SMMU yes/no).**
3. Live interrupts: `cat /proc/interrupts` before and while toggling Wi-Fi on/off → the PCIe
   MSI/`wlan` SPI numbers and the `MT6376_FAULT`-style PMIC line.
4. PCI enumeration ground truth: `ls /sys/bus/pci/devices/*/`, and for the Wi-Fi endpoint read
   `vendor`, `device`, `resource`, `iommu_group` (existence answers the SMMU question from the
   other side) and `local_cpulist`.
5. Firmware inventory: `ls -la` over `/vendor/firmware`, `/vendor/firmware_mnt`,
   `/system/etc/firmware`, `/odm/firmware` filtered by `mt6653|WIFI_RAM_CODE|PATCH|PHY|nvram|
   conninfra|connfem|WMT`; `md5sum` each; pull them. These are the exact bytes P3 must feed.
6. MAC source check: `cat /sys/class/net/wlan0/address` on Android, then locate which of the
   pulled cal/NVRAM files literally contains those 6 bytes — this proves the NVRAM path the
   Kbuild implies rather than assuming it.
7. Clock/power names: `cat /sys/kernel/debug/clk/clk_summary | grep -iE 'pcie|conn'`,
   `cat /sys/kernel/debug/pm_genpm_info` / `sleepstate` if present, and
   `cat /sys/kernel/debug/pinctrl/*/pinmaps | grep -i connsys`. If debugfs is locked, say so —
   a GKI build usually is, and then item 2 plus dynamic-debug (next) carry the load.
8. Power-sequence trace: with root, `echo 'module conninfra +p' >
   /sys/kernel/debug/dynamic_debug/control` (if available) and `echo
   'module wlan_drv_gen4m_6993_6653 +p' > ...`, then toggle Wi-Fi and capture `dmesg` +
   `logcat -b kernel`. Purpose: the real order and timing of `connv3_pwr_on` → 10 ms → PCIe
   probe, which is what P1 must reproduce byte-for-byte.
9. Connsys state introspection: `/proc/wmt`, `/sys/class/misc/wmt*`, `cat
   /sys/kernel/debug/wmt/*` if present — gives the radio arbitration view that
   `PRE_PWR_ON` broadcasting implies.
10. Cross-check against our own inputs afterwards: which ADT nodes `mkadt.py` must add, and
    which boot-arg token to merge the P1/P2 gates into given the 607-byte budget.

Deliverable of P0: one `mt6993.dts` text file, one firmware file list with hashes, one IRQ
table, one reserved-memory table, and a yes/no on IOMMU. Everything in the difficulty table
above is then estimable to within one cycle instead of one order of magnitude.
