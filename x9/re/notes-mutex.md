# MT6993 (MMSYS_MT6993) display mutex — bare-metal extraction

Source: OPPO kernel tree
`oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/gpu/drm/mediatek/mediatek_v2/`
(file paths below are relative to that directory; "ddp.c" = `mtk_drm_ddp.c`,
"crtc.c" = `mtk_drm_crtc.c`). DTS:
`arch/arm64/boot/dts/mediatek/mt6993.dts`. Clock driver:
`drivers/clk/mediatek/clk-mt6993-mmsys.c`.

All facts below are read from code, with file:line citations. Anything not
directly derivable is flagged **AMBIGUOUS**.

---

## 1. Mutex banks: count, physical bases, mutex id

### Count

- `DISP_MUTEX_TOTAL_v2 = 8` (`mtk_drm_ddp.h:19`; `DISP_MUTEX_TOTAL = 16` at :18).
- MT6993 driver data uses it: `mt6993_ddp_driver_data.disp_mutex_total =
  DISP_MUTEX_TOTAL_v2` (ddp.c:9517). This value is used for the VDO-SOF
  interrupt bit offset in INTEN (`id + disp_mutex_total`, ddp.c:37095-37097).
- The allocation pool itself is `ddp->mutex[0..9]` (`for (i = 0; i < 10; i++)
  ddp->mutex[i].id = i`, ddp.c:44641; `mtk_disp_mutex_get` rejects `id >= 10`,
  ddp.c:36358-36360). So: 10 software mutex slots, hardware layout is 8 mutexes
  per bank at 0x20 stride.

### Bank bases (from DT, mapped in probe)

DT node `mt6993.dts:10815-10833`:

```
disp1_mutex0: disp1-mutex0@3e320000 {
        compatible = "mediatek,disp_mutex0", "mediatek,mt6993-disp-mutex";
        dispsys-num = <4>;
        ovlsys-num  = <3>;
        reg = <0 0x3e320000 0 0x1000>,   // idx 0
              <0 0x3e720000 0 0x1000>,   // idx 1
              <0 0x3e520000 0 0x1000>,   // idx 2
              <0 0x3e920000 0 0x1000>,   // idx 3
              <0 0x32920000 0 0x1000>,   // idx 4
              <0 0x32c20000 0 0x1000>,   // idx 5
              <0 0x3e020000 0 0x1000>;   // idx 6
};
```

`mtk_ddp_probe` (ddp.c:44607-44760) maps the IORESOURCE_MEM entries **in this
order** into the driver fields:

| idx | driver field          | role                          | physical base |
|-----|-----------------------|-------------------------------|---------------|
| 0   | `ddp->regs`           | DISPSYS0  (MMSYS disp0)       | **0x3e320000**|
| 1   | `ddp->side_regs`      | DISPSYS1  (MMSYS disp1)       | **0x3e720000**|
| 2   | `ddp->sys_b_regs`     | MMSYS_B   (second pipe)       | **0x3e520000**|
| 3   | `ddp->sys_b_side_regs`| MMSYS_B side                  | **0x3e920000**|
| 4   | `ddp->ovlsys0_regs`   | OVLSYS0 mutex bank            | **0x32920000**|
| 5   | `ddp->ovlsys1_regs`   | OVLSYS1 mutex bank            | **0x32c20000**|
| 6   | `ddp->ovlsys2_regs`   | OVLSYS2 mutex bank            | **0x3e020000**|

Probe order: regs (idx0) is always mapped (ddp.c:44650-44657); side (idx1),
sys_b (idx2), sys_b_side (idx3) are mapped while `num_cnt < dispsys_num`
(=4, ddp.c:44659-44713); ovlsys0/1/2 (idx4-6) while `ovlsys_num` (=3)
allows (ddp.c:44722-44761). OF match: `"mediatek,mt6993-disp-mutex"` →
`mt6993_ddp_driver_data` (ddp.c:44842).

Bank selection per component is `mt6993_dispsys_map` (ddp.c:8095) with bank
ids `DISPSYS0=0 DISPSYS1=1 OVLSYS0=2 OVLSYS1=3 OVLSYS2=4 DISPSYS_B_0=5
DISPSYS_B_1=6` (ddp.c:57-63). In `mtk_disp_mutex_add_comp`
(ddp.c:36433-36600): DISPSYS1 → `side_regs`, DISPSYS_B_* → sys_b banks,
everything else (DISPSYS0) → `regs`; components mapped OVLSYS0/1/2 additionally
(or instead — see §3) get `reg_addr1 = ovlsys{0,1,2}_regs`.

### Which mutex id a CRTC uses

- `disp_mutex_dispatch` (crtc.c:24482-24528) claims ids 0,1,2,… from the pool
  (`mtk_disp_mutex_get(priv->mutex_dev, i)`, crtc.c:24512) and stores them in
  `mtk_crtc->mutex[cur]` (mutex[0] = primary path, mutex[1] = sub path).
- `mtk_crtc_get_mutex_id` (crtc.c:26677-26738): crtc 0 path **with connector →
  mutex id 0**, sub-path without connector → mutex id 1. Same id (0) is used
  **within every bank** — the same offset `0x20 + 0x20*id` applies in dispsys0,
  dispsys1, ovlsys0/1/2 (all mutex functions pass `mutex->id` to every bank,
  e.g. enable ddp.c:37221-37247).

**For our single-CRTC DSI0 case: mutex id 0 in every bank.** Everything below
computes offsets for `id = 0`.

---

## 2. Register layout (per bank, per mutex n, stride 0x20)

Header macros (`mtk_drm_ddp.h:42-48`) and MT6993 offsets (ddp.c:1805-1808):

| register | macro / define                        | offset (mutex 0) |
|----------|---------------------------------------|------------------|
| INTEN    | `DISP_REG_MUTEX_INTEN` ddp.c:435      | 0x00             |
| INTSTA   | `DISP_REG_MUTEX_INTSTA` ddp.c:436     | 0x04             |
| CFG      | `DISP_REG_MUTEX_CFG` ddp.c:437        | 0x08             |
| EN       | `DISP_REG_MUTEX_EN(n)=0x20+0x20*n`    | **0x20**         |
| SW acquire/status | `DISP_REG_MUTEX(n)=0x24+0x20*n` | **0x24**     |
| RST      | `MT6993_DISP_MUTEX_RST` ddp.c:1808 (`mutex_rst_reg`) | **0x2C** |
| SOF      | `MT6993_DISP_MUTEX0_SOF` ddp.c:1807   | **0x30**         |
| MOD0     | `MT6993_DISP_MUTEX0_MOD0` ddp.c:1805  | **0x34**         |
| MOD1     | `MT6993_DISP_MUTEX0_MOD2` ddp.c:1806 (=`mutex_mod_reg[1]`) | **0x38** |

`DISP_REG_MUTEX_MOD(i,data,n) = data->mutex_mod_reg[i] + 0x20*n`
(mtk_drm_ddp.h:47); MT6993 `mutex_mod_reg = {0x34, 0x38}` (ddp.c:9510). The
same two MOD offsets are used in **all** banks (dispsys0/1, sys_b, ovlsys0/1/2)
because `add_comp` passes the same macro regardless of `reg_addr`/`reg_addr1`
(ddp.c:36471-36499).

### (a) Reset

There is **no separate reset API**. The only RST writes are in
`mtk_disp_mutex_disable` (ddp.c:37345-37363) and only when `mutex->id == 2`:
`RST(0x2C)=1` across regs/ovlsys0/side/ovlsys1, `udelay(1)`, `RST=0`.
**For mutex id 0 the driver never touches RST.** (The `DISP_REG_MUTEX_RST_REG`
generic value is 0x28, mtk_drm_ddp.h:44, but MT6993 overrides it to 0x2C,
ddp.c:9512.)

### (b) Adding modules (MOD0 0x34 / MOD1 0x38 routing)

`mtk_disp_mutex_add_comp` (ddp.c:36433-36600):

- `reg_addr` chosen by `dispsys_map[id]` (DISPSYS1→side_regs, DISPSYS_B_*→
  sys_b banks, else dispsys0 regs; ddp.c:36445-36453).
- If `mutex_mod[id] > 0`:
  - `mutex_mod[id] <= BIT(31)` → **read-modify-write OR into MOD0 (0x34)** at
    `reg_addr` (ddp.c:36470-36475). Note a bare `BIT(31)` still goes to MOD0.
  - `mutex_mod[id] > BIT(31)` (i.e. `(BIT(x)|BIT(31))`) → OR
    `mutex_mod[id] & ~BIT(31)` into **MOD1 (0x38)** (ddp.c:36475-36482).
- If the component maps to an OVLSYS bank, `reg_addr1` is set and the *same*
  split applies to `mutex_ovlsys_mod[id]` written into the ovlsys bank
  (ddp.c:36484-36499). OVL components have no `mt6993_mutex_mod` entry, so
  only the ovlsys bank gets written; dispsys components only the dispsys bank.
- Writes are read-OR-write (RMW), so multiple modules accumulate in one MOD
  register.
- Removal (`mtk_disp_mutex_remove_comp`, ddp.c:36858-36946) does RMW clear of
  the same bits, and for output comps (DSI0/DVO/…) writes
  `MUTEX_SOF_SINGLE_MODE` (=0) into SOF (ddp.c:36904-36916).

The cmdq variant `mtk_disp_mutex_add_comp_with_cmdq` (ddp.c:36693-36856) is
identical routing; for DSI0 in cmd mode it writes SOF=mutex_sof[SINGLE_MODE]=0
to both the dispsys and ovlsys bank (ddp.c:36839-36855); special case: DSC0
with dual-port panel and `!dual_dsc_enable` also sets bit<<1 (DSC core1,
ddp.c:36790-36797).

### (c) SOF (0x30)

MT6993 SOF source values (ddp.c:2332-2338 region, exact lines 2332-2337 +
2350-2356):

```
MT6993_MUTEX_SOF_SINGLE_MODE 0        (ddp.c:2332)
MT6993_MUTEX_SOF_DSI0        1        (ddp.c:2333)
MT6993_MUTEX_SOF_DSI1        2
MT6993_MUTEX_SOF_DSI2        4
MT6993_MUTEX_SOF_DVO         5
MT6993_MUTEX_EOF_DSI0  (DSI0 << 7) = 0x80   (ddp.c:2338)
MT6993_MUTEX_OVLSYS_SOF_DSI0 1 ; MT6993_MUTEX_OVLSYS_EOF_DSI0 = 0x80 (ddp.c:2343-2356)
```

`mt6993_mutex_sof[]` (ddp.c:9275-9289):

| index (DDP_MUTEX_SOF_*)            | value written to 0x30 |
|------------------------------------|-----------------------|
| SINGLE_MODE (0)                    | 0x00000000            |
| DSI0 (1)                           | 0x00000081 (1 \| 1<<7)|
| DSI1 (2)                           | 0x00000102            |
| DSI2 (3)                           | 0x00000204            |
| DVO (4)                            | 0x00000285            |
| SINGLE_EOF_DSI0 (5)                | 0x00000080            |
| SINGLE_EOF_DSI1 (6)                | 0x00000100            |

`mt6993_mutex_ovlsys_sof[]` (ddp.c:9291-9301) has the same values for DSI0/1/2
(OVLSYS_SOF_DSI0=1, OVLSYS_EOF_DSI0=1<<7 → 0x81).

SOF selection at stream-on is `mtk_disp_mutex_src_set` (ddp.c:36501-36672):

- **cmd mode** (frame-trigger): MT6993 has `qos_sideband_use_eof = true`
  (ddp.c:9514), so `val = DDP_MUTEX_SOF_SINGLE_EOF_DSI0` (ddp.c:36520-36532)
  and it writes `mutex_sof[val] = 0x80` to:
  - dispsys0 `regs + 0x30` (ddp.c:36550-36552)
  - ovlsys0 `ovlsys0_regs + 0x30` (ddp.c:36553-36556)
  - dispsys1 `side_regs + 0x30` (ddp.c:36562-36564)
  - ovlsys1 `ovlsys1_regs + 0x30` (ddp.c:36565-36569)
  - (ovlsys2 and sys_b banks are NOT written in cmd mode; they are in vdo
    mode, ddp.c:36606-36645)
- **vdo mode**: `val = DDP_MUTEX_SOF_DSI0` → 0x81 to dispsys0; 
  `mutex_ovlsys_sof[DSI0]=0x81` to ovlsys0, ovlsys2, ovlsys1;
  `mutex_sof[DSI0]=0x81` to sys_b, sys_b_side and dispsys1 (MT6993 explicitly
  skips the DSI0↔DSI1 remap other chips do, ddp.c:36647-36656).

So for **DSI0 cmd-mode: SOF(0x30) = 0x80 in banks dispsys0, ovlsys0, dispsys1,
ovlsys1.** (0x80 = SINGLE_MODE source with EOF-from-DSI0 sideband.)

### (d) Enable (EN 0x20)

`mtk_disp_mutex_enable` (ddp.c:37221-37247) — CPU path (used in vdo mode and
in bringup): writes `1` to `EN(0x20)` in **every mapped bank**: dispsys0,
ovlsys0, dispsys1 (side), ovlsys1, ovlsys2, sys_b, sys_b_side, then calls
`mtk_disp_mutex_inten_enable` (ddp.c:37080-37143): INTEN(0x00) |=
`BIT(mutex_id)` and, if vdo mode, `BIT(id + 8)` (disp_mutex_total=8); written
to dispsys0 and dispsys1 INTEN with a write-`val&~mask`-then-`val` pulse
(ddp.c:37089-37092). Ovlsys/sys_b INTEN writes are commented out
(ddp.c:37098-37140). For cmd mode (mutex id 0, is_vdo=false): INTEN = 0x1.

`mtk_disp_mutex_enable_cmdq` (ddp.c:37248-37334) — the GCE path actually used
at runtime for cmd mode — per bank (dispsys0, ovlsys0, dispsys1, ovlsys1,
ovlsys2, sys_b, sys_b_side):
1. `CFG(0x08)` value 0, mask 0x1 → **clear CFG bit0** (ddp.c:37258-37260)
2. `EN(0x20)` value 0, mask ~0 → EN=0 (ddp.c:37261-37263)
3. `EN(0x20)` value 1, mask ~0 → EN=1 (ddp.c:37264-37266)
then `mtk_disp_mutex_inten_enable_cmdq` (ddp.c:37145-37180): INTEN(0x00)
value=mask= `BIT(id)` (+BIT(id+8) if vdo) in dispsys0 and dispsys1.

Disable (`mtk_disp_mutex_disable`, ddp.c:37335-37363): EN=0 to side, ovlsys1,
dispsys0, ovlsys0, ovlsys2, sys_b, sys_b_side; then (id==2 only) the RST pulse.

### (e) Software trigger / acquire

`mtk_disp_mutex_acquire` (ddp.c:37370-37420): `EN(0x20)=1`, then
`DISP_REG_MUTEX(0x24)=1`, poll `0x24 & INT_MUTEX` (INT_MUTEX=BIT(1),
ddp.c:439) with 1 µs/10000 µs timeout; repeated for side/ovlsys1/ovlsys2/sys_b
banks. `mtk_disp_mutex_release` writes 0 to 0x24. This is the CPU-side
hardware-mutex grab used around config windows (crtc.c:23001-23003), not a
per-frame trigger. There is no other software "trigger" register: the mutex
fires when SOF/EOF arrives from DSI (or single-mode immediately once EN=1).

---

## 3. OVLSYS mutex bank

Yes — the OVLSYS banks use the **same register layout**: EN 0x20, SOF 0x30,
MOD0 0x34, MOD1 0x38, same `DISP_REG_MUTEX_MOD` macro and same MT6993
`mutex_mod_reg = {0x34,0x38}` (ddp.c:9510, used for `reg_addr1` in
add_comp ddp.c:36484-36499). SOF for ovlsys in cmd mode is the same 0x80
written with `mutex_sof[]` (not the ovlsys table — see ddp.c:36553-36556);
`mutex_ovlsys_sof` is only used in vdo mode (ddp.c:36617-36628). INTEN for
ovlsys is not programmed (commented out, ddp.c:37098-37105).

Module bit values, `mt6993_ovlsys_mutex_mod` (ddp.c:6345-6491) with defines
ddp.c:2220-2272, bank OVLSYS0 = 0x32920000:

| component                        | define                        | reg  | value      |
|----------------------------------|-------------------------------|------|------------|
| OVL_EXDMA0                       | MOD0_OVL_EXDMA0 (2220)        | 0x34 | 0x00000001 |
| OVL_EXDMA1                       | MOD0_OVL_EXDMA1 (2221)        | 0x34 | 0x00000002 |
| **OVL_EXDMA2**                   | MOD0_OVL_EXDMA2 (2222)        | 0x34 | 0x00000004 |
| **OVL_EXDMA3**                   | MOD0_OVL_EXDMA3 (2223)        | 0x34 | 0x00000008 |
| **OVL_EXDMA4**                   | MOD0_OVL_EXDMA4 (2224)        | 0x34 | 0x00000010 |
| **OVL_EXDMA5**                   | MOD0_OVL_EXDMA5 (2225)        | 0x34 | 0x00000020 |
| OVL_EXDMA6                       | MOD0_OVL_EXDMA6 (2226)        | 0x34 | 0x00000040 |
| OVL_EXDMA7                       | MOD0_OVL_EXDMA7 (2227)        | 0x34 | 0x00000080 |
| **OVL0_BLENDER0**                | MOD0_OVL_BLENDER0 (2228)      | 0x34 | 0x00000100 |
| OVL0_BLENDER1                    | MOD0_OVL_BLENDER1 (2229)      | 0x34 | 0x00000200 |
| OVL0_BLENDER2                    | MOD0_OVL_BLENDER2 (2230)      | 0x34 | 0x00000400 |
| OVL0_BLENDER3                    | MOD0_OVL_BLENDER3 (2231)      | 0x34 | 0x00000800 |
| OVL0_BLENDER4                    | MOD0_OVL_BLENDER4 (2232)      | 0x34 | 0x00001000 |
| OVL0_BLENDER5                    | MOD0_OVL_BLENDER5 (2233)      | 0x34 | 0x00002000 |
| OVL0_BLENDER6                    | MOD0_OVL_BLENDER6 (2234)      | 0x34 | 0x00004000 |
| OVL0_BLENDER7                    | MOD0_OVL_BLENDER7 (2235)      | 0x34 | 0x00008000 |
| **OVL0_OUTPROC0** (outproc)      | MOD0_OVL_OUTPROC0 (2236)      | 0x34 | 0x00010000 |
| OVL0_OUTPROC1..3                 | 2237-2239                     | 0x34 | 0x20000..0x80000 |
| OVL0_MDP_RSZ0                    | MOD0_OVL_MDP_RSZ0 (2240)      | 0x34 | 0x00100000 |
| OVL0_ODPM0                       | MOD0_OVL_ODPM0 (2241)         | 0x34 | 0x00200000 |
| INLINE_ROTATE0                   | MOD0_INLINEROT0 (2242)        | 0x34 | 0x00400000 |
| OVLSYS_DLI_ASYNC0                | MOD0_OVL_DLI_ASYNC0 (2243)    | 0x34 | 0x00800000 |
| OVLSYS_DLI_ASYNC1..7             | 2244-2250                     | 0x34 | 0x01000000..0x40000000 |
| OVLSYS_DLI_ASYNC8                | MOD0_OVL_DLI_ASYNC8 (2251)    | 0x34 | 0x80000000 |
| OVLSYS_DLI_ASYNC9                | MOD1_OVL_DLI_ASYNC9 (2252)    | 0x38 | 0x00000001 |
| OVLSYS_DLI_ASYNC10..12           | 2253-2255                     | 0x38 | 0x2, 0x4, 0x8 |
| OVLSYS_DLO_ASYNC0                | MOD1_OVL_DLO_ASYNC0 (2256)    | 0x38 | 0x00000010 |
| OVLSYS_DLO_ASYNC1..15            | 2257-2271                     | 0x38 | 0x20 .. 0x80000 |
| OVLSYS_RELAY0                    | MOD1_OVLSYS_RELAY0 (2272)     | 0x38 | 0x00100000 |

(outproc components in the mutex table are `DDP_COMPONENT_OVL0_OUTPROC0..3` /
`OVL1_*` / `OVL2_*`, ddp.c:6395-6403; the driver data name is
`DDP_COMPONENT_OVL0_OUTPROC0` → `MT6993_MUTEX_MOD0_OVL_OUTPROC0`.)

OVLSYS1 (0x32c20000) and OVLSYS2 (0x3e020000) are selected by
`mt6993_dispsys_map` for the `OVL1_*`/`OVL2_*` component variants and
`OVLSYS1_*`/`OVLSYS2_*` DLI/DLO variants (ddp.c:8100-8289). The CRTC0 main
path uses **OVLSYS0** components only; CRTC1 (ext DP) uses OVLSYS2 + SYS_B
(mtk_drm_drv.c:4311-4330).

Reference ovl path actually compiled for CRTC0
(`mt6993_mtk_ovlsys_main_bringup`, mtk_drm_drv.c:4187-4205, wired as
`ovl_path[DDP_MAJOR][0]` at mtk_drm_drv.c:6352): EXDMA3, BLENDER1, EXDMA4,
BLENDER2, EXDMA5, BLENDER3, EXDMA6, BLENDER4, EXDMA7, BLENDER5, EXDMA2,
`OVL0_EXDMA_OUT_CB3`, BLENDER6, **OUTPROC0**, **OVLSYS_DLO_ASYNC12**, EXDMA0.
`OVLSYS_DLO_ASYNC12` → MOD1 bit16 = 0x00010000 (define 2268).

---

## 4. DISPSYS module bits (`mt6993_mutex_mod`, ddp.c:6504-6760)

### DISPSYS0 bank (0x3e320000) — defines ddp.c:2274-2353

| component               | reg  | value      | define line |
|-------------------------|------|------------|-------------|
| AAL0                    | 0x34 | 0x00000001 | 2274 |
| C3D0 / C3D1             | 0x34 | 0x2 / 0x4  | 2275-2276 |
| CCORR0 / CCORR1         | 0x34 | 0x8 / 0x10 | 2277-2278 |
| COLOR0                  | 0x34 | 0x00000020 | 2279 |
| DITHER0 / DITHER1       | 0x34 | 0x40 / 0x80| 2280-2281 |
| **DLI_ASYNC0**          | 0x34 | 0x00000100 | 2282 |
| DLI_ASYNC1..15          | 0x34 | 0x200 .. 0x800000 | 2283-2297 |
| **DLO_ASYNC0**          | 0x34 | 0x01000000 | 2298 |
| DLO_ASYNC1..6           | 0x34 | 0x02000000..0x40000000 | 2299-2305 |
| DLO_ASYNC7              | 0x34 | 0x80000000 (still MOD0, `<= BIT(31)`) | 2306-2307 |
| DLO_ASYNC8..20          | 0x38 | 0x1 .. 0x1000 | 2308-2320 |
| RELAY5                  | 0x38 | 0x00002000 | 2321 |
| GAMMA0                  | 0x38 | 0x00004000 | 2322 |
| DMDP_AAL0 (MDP_AAL0)    | 0x38 | 0x00008000 | 2323 |
| POSTMASK0               | 0x38 | 0x00010000 | 2324 |
| MDP_RDMA0               | 0x38 | 0x00020000 | 2325 |
| SPR0                    | 0x38 | 0x00040000 | 2326 |
| ODDMR0                  | 0x38 | 0x00080000 | 2327 |
| MDP_RSZ0                | 0x38 | 0x00100000 | 2328 |
| TDSHP0                  | 0x38 | 0x00200000 | 2329 |
| PWM0                    | 0x38 | 0x00400000 | 2330 |

### DISPSYS1 bank (0x3e720000) — defines ddp.c:2355-2389

| component                        | reg  | value      | define line |
|----------------------------------|------|------------|-------------|
| DLI_ASYNC20..38                  | 0x34 | 0x1 .. 0x40000 | 2355-2373 |
| DLO_ASYNC31..38                  | 0x34 | 0x80000..0x4000000 | 2374-2381 |
| RELAY0                           | 0x34 | 0x08000000 | 2382 |
| RELAY1..4                        | 0x34 | 0x10000000..0x80000000 | 2383-2386 |
| **DSI0** (DSI0_MAC0)             | 0x38 | 0x00000001 | 2387-2388 |
| DSI0_MAC1                        | 0x38 | 0x00000002 | 2389 |
| DSI1_MAC0/1                      | 0x38 | 0x4 / 0x8  | 2390-2391 |
| DVO0                             | 0x38 | 0x10       | 2392 |
| WDMA0/WDMA1                      | 0x38 | 0x20 / 0x40| 2393-2394 |
| DBI_COUNT0                       | 0x38 | 0x80       | 2395 |
| CHIST0..2                        | 0x38 | 0x100..0x400 | 2396-2398 |
| **POSTALIGN0**                   | 0x38 | 0x00000800 | 2399-2400 |
| **SPLITTER0**                    | 0x38 | 0x00001000 | 2401-2402 |
| SPLITTER1                        | 0x38 | 0x2000     | 2403 |
| **DSC0** (DSC_WRAP0_CORE0)       | 0x38 | 0x00004000 | 2404-2405 |
| DSC1/2/3 (WRAP0_CORE1/WRAP1_*)   | 0x38 | 0x8000 / 0x10000 / 0x20000 | 2406-2408 |
| R2Y0                             | 0x38 | 0x40000    | 2409 |
| GDMA0                            | 0x38 | 0x80000    | 2410 |
| **MERGE0**                       | 0x38 | 0x00100000 | 2411-2412 |
| MERGE1                           | 0x38 | 0x200000   | 2413 |

(Exact define names: `MT6993_MUTEX1_MOD1_DISP_DSI0_MAC0 (BIT(0)|BIT(31))`
etc., ddp.c:2355-2389; the `|BIT(31)` flag is what routes them to MOD1/0x38
in the dispsys1 bank — `dispsys_map` sends DSI0/DSC0/MERGE0/SPLITTER0/
POSTALIGN0/RELAY0-4 to `side_regs`, ddp.c:8325-8360.)

### Which of these the CRTC0 DSI0 cmd-mode path needs

Compiled CRTC0 path `mt6993_mtk_ddp_main_bringup` (mtk_drm_drv.c:4207-4304,
wired at 6354-6355; `PQ_PATH_9` is the active variant, mtk_drm_drv.h:39;
`DRM_BYPASS_PQ_MT6993` is commented out, mtk_drm_drv.c:139). Components with
mutex bits:

- DISPSYS0 (0x3e320000): DLI_ASYNC0 (0x34 bit8), AAL0 (0x34 bit0),
  DMDP_AAL0 (0x38 bit15), MDP_RSZ0 (0x38 bit20), TDSHP0 (0x38 bit21),
  COLOR0 (0x34 bit5), CCORR0/CCORR1 (0x34 bits3/4), C3D0/C3D1 (0x34 bits1/2),
  GAMMA0 (0x38 bit14), POSTMASK0 (0x38 bit16), DITHER0 (0x34 bit6),
  DITHER1 (0x34 bit7), SPR0 (0x38 bit18), ODDMR0 (0x38 bit19),
  **RELAY5 (0x38 bit13 = 0x2000)**, DLO_ASYNC16 (0x38 bit8 = 0x100),
  DLO_ASYNC0/1 (0x34 bits24/25).
- DISPSYS1 (0x3e720000): DLI_ASYNC29 (0x34 bit9), **POSTALIGN0 (0x38 bit11)**,
  **SPLITTER0 (0x38 bit12)**, **DSC0 (0x38 bit14)**, **MERGE0 (0x38 bit20)**,
  **DSI0 (0x38 bit0)**; also DLI_ASYNC20/21 (0x34 bits0/1), DLO_ASYNC17
  (0x34 bit20? — DLO_ASYNC31..38 are bits19..26; DLO_ASYNC17 is a DISPSYS0
  MOD1 bit9), DBI_COUNT0 (0x38 bit7), CHIST0/1 (0x38 bits8/9).
- `PQ0_IN_CB0`, `SPLITTER0_IN_CB9/OUT_CB0`, `COMP0_OUT_CB0`,
  `MERGE0_OUT_CB0`, `PC_*`, `INSIDE_PC_CB*`, `OVL0_EXDMA_OUT_CB3` are
  **connector pseudo-components** (`*_CB*` in the EXPR list,
  mtk_drm_ddp_comp.h:438-484) with **no mutex_mod entries** — they contribute
  no mutex bits; the DSC wrap enters the mutex as `DDP_COMPONENT_DSC0` via the
  addon connect (`mtk_disp_mutex_add_comp_with_cmdq`, ddp.c:36693).

**Minimum set for the bare-metal single-layer DSC cmd-mode path
(EXDMA→blender→outproc→DLI/DLO→splitter→DSC→merge→DSI0)** — one (offset,
value) per register, RMW-OR them together per register:

OVLSYS0 @0x32920000, mutex0:
- 0x34 MOD0 = EXDMA + blender + outproc + ovlsys DLI bits, e.g. single layer
  on EXDMA2/3 with BLENDER1: `0x00010000 (OUTPROC0) | 0x00000200 (BLENDER1)
  | 0x00000008 (EXDMA3) | 0x00800000 (OVLSYS_DLI_ASYNC0 if used)`.
  **AMBIGUOUS**: which EXDMA/blender indices a single layer gets is decided
  by the runtime layering code, not fixed here; the bringup array
  (mtk_drm_drv.c:4187-4205) is the best template.
- 0x38 MOD1 = 0x00010000 (OVLSYS_DLO_ASYNC12) | 0x00100000 (OVLSYS_RELAY0) if
  relay is in path. **AMBIGUOUS**: whether the shim needs OVLSYS_RELAY0
  depends on whether the ovlsys output passes through the relay; the bringup
  path uses OVLSYS_DLO_ASYNC12 only.
- 0x30 SOF = 0x80
- 0x20 EN  = 1

DISPSYS0 @0x3e320000, mutex0:
- 0x34 MOD0 = 0x00000100 (DLI_ASYNC0) | 0x01000000 (DLO_ASYNC0) | PQ bits
  actually enabled
- 0x38 MOD1 = 0x00002000 (RELAY5) | PQ bits (e.g. 0x00200000 TDSHP0,
  0x00008000 DMDP_AAL0, 0x00100000 MDP_RSZ0)
- 0x30 SOF = 0x80
- 0x20 EN = 1, 0x00 INTEN = 0x1

DISPSYS1 @0x3e720000, mutex0:
- 0x34 MOD0 = 0x00000200 (DLI_ASYNC29) | 0x00000001 (DLI_ASYNC20)
- 0x38 MOD1 = 0x00000001 (DSI0) | 0x00000800 (POSTALIGN0) |
  0x00001000 (SPLITTER0) | 0x00004000 (DSC0) | 0x00100000 (MERGE0)
- 0x30 SOF = 0x80
- 0x20 EN = 1, 0x00 INTEN = 0x1

SYS_B banks (0x3e520000/0x3e920000): no components from the CRTC0 path; the
driver still pulses EN there in enable/enable_cmdq (ddp.c:37233-37247,
37308-37334). Mirror or skip — driver behavior is EN=1 with no MOD bits set.

---

## 5. Stream-on / stream-off order (mt6993)

Stream-on (CRTC resume, crtc.c:16560-16720 `mtk_drm_crtc_atomic_resume`
body — numbered steps in source):

1. Power/clocks: `mtk_crtc_ddp_prepare` (step 3, crtc.c:16593) — MTCMOS +
   CCF clocks per `mt6993_pwr_on_order` = CLK_DSI_PHY0, DIS0_A, DIS0_B,
   DIS1_A, DIS1_B, OVL0, OVL1, OVL2 (mtk_drm_drv.c:4146-4155).
2. Start GCE trigger loop (step 5, crtc.c:16604-16613) — GCE only.
3. **`mtk_crtc_connect_default_path`** (step 6, crtc.c:16633 →
   crtc.c:14491-14563):
   a. path MOUT/SEL routing (`mtk_ddp_add_comp_to_path`) per component;
   b. `mtk_disp_mutex_add_comp(mutex[0], comp->id)` for each path component
      (crtc.c:14537-14545) — MOD0/MOD1 OR-writes (§2b);
   c. `mtk_disp_mutex_src_set(mtk_crtc, is_cmd_mode)` (crtc.c:14558) — SOF
      writes (§2c); cmd mode → 0x80 in dispsys0/ovlsys0/dispsys1/ovlsys1;
   d. if **vdo** mode only: `mtk_disp_mutex_enable` here on CPU
      (crtc.c:14560-14561). **In cmd mode the mutex is NOT enabled here.**
4. `mtk_crtc_config_default_path` (step 7) — component config; then addon
   module connect (step 8) which adds DSC0 etc. to the mutex via cmdq.
5. Per-frame cmd-mode trigger loop (crtc.c:13380-13620): wait
   STREAM_BLOCK/STREAM_DIRTY/TE events → `mtk_crtc_comp_trigger(PRE_TRIGGER)`
   (crtc.c:13434) → then **`mtk_disp_mutex_enable_cmdq(mutex[0])`**
   (crtc.c:13448/13492/13582/13596-13597 — after TE wait, before component
   trigger; the plain `if prefetch_te_en == false` path at crtc.c:13582 is
   reached every frame) → `mtk_crtc_comp_trigger(TRIGGER)` (crtc.c:13596)
   which for DSI writes the DSI START 0→1 pulse (mtk_dsi.c:8577-8590) and for
   MT6993 skips the config_regs 0xF0 write (mtk_dsi.c:8551-8562) → EOF
   trigger. So the cmd-mode order is **mutex EN after TE, immediately before
   DSI start**, every frame (EN=0 then EN=1 pulse, §2d).
   - MT6993-specific write in the loop: `0x03E730300 |= (BIT(1)|BIT(3))`
     before mutex enable and cleared back after trigger (HRT urgent WA,
     crtc.c:13568-13570 and 13602). 0x3e730300 is inside dispsys1 config
     space. **AMBIGUOUS** whether bare metal needs it (it is a workaround
     tagged "For HRT urgent WA").
6. Stream-off: `mtk_crtc_disconnect_default_path` (crtc.c:16158-16200):
   mutex disable on CPU only in vdo mode (crtc.c:16161-16162); remove path
   MOUT/SEL; `mtk_disp_mutex_remove_comp` clears MOD bits and sets SOF=0 for
   outputs. In cmd mode the trigger loop stop path disables the mutex via
   `mtk_disp_mutex_disable` (crtc.c:16179) — EN=0 all banks; RST pulse only
   for mutex id 2.

`mutex_sof_ns` is only a timestamp (`ktime_get()` at ddp.c:37665), not a
register. `qos_sideband_use_eof` is the MT6993 driver-data flag (ddp.c:9514)
that switches cmd-mode SOF from 0 (SINGLE_MODE) to 0x80 (SINGLE_EOF_DSI0) —
§2c.

---

## 6. What gates the mutex (clocks / power / events)

### Clock gates (must be ON — bit cleared in the CLR register / read 0 in status)

Per-mutex-node clocks (mt6993.dts:10819-10823): `MM0_DISP_MUTEX0_DISP`,
`MM1_DISP_MUTEX0_DISP`, `OVL_MUTEX0_DISP`, `OVL1_OVL_MUTEX0_DISP`,
`OVL2_OVL_MUTEX0_DISP`. Gate blocks (clk-mt6993-mmsys.c; set=enable, clr =
disable via setclr ops, sta = status 1=off):

| clock provider (dts)            | base       | regs (set/clr/sta)  | gate bit |
|---------------------------------|------------|---------------------|----------|
| `dispsys_config_clk` @0x3e300000 (dts:1711) | 0x3e300000 | 0xA64 / 0xA68 / 0xA60 (mm00, clk .c:961-965) | MM0_DISP_MUTEX0 = **1** (clk .c:1029-1032) |
| `dispsys1_config_clk` @0x3e700000 (dts:1725)| 0x3e700000 | 0xA68 / 0xA6C / 0xA64 (mm10, 647-651) | MM1_DISP_MUTEX0 = **2** (719-722) |
| `ovlsys_config_clk` @0x32900000 (dts:1475)  | 0x32900000 | 0xACC / 0xAD0 / 0xAC8 (ovl0, 2516-2520) | OVL_MUTEX0 = **1** (2584-2587) |
| `ovl1_ovlsys_config_clk` @0x32c00000 (dts:1482) | 0x32c00000 | same offsets (ovl10, 1880-1884) | **1** (1948-1951) |
| `ovl2_ovlsys_config_clk` @0x3e000000 (dts:1704) | 0x3e000000 | same offsets (ovl20, 2198-2202) | **1** (2266-2269) |

Driver-side aliases matching these (used by the debug dump to read status):
`MT6993_DISP_REG_CONFIG_MMSYS_CG_CON0 0xA60` / `..._CG_CON1 0xA6C`
(ddp.c:2480-2481), `MT6993_DISP1_REG_CONFIG_MMSYS_CG_CON0 0xA64` /
`..._CG_CON1 0xA70` (ddp.c:2496-2497),
`DISP_REG_CONFIG_OVLSYS_CG_CON0_MT6993 0xAC8` /
`..._CG_CON1_MT6993 0xAD4` (ddp.c:2403-2404) — note these are the *status*
offsets; enabling a gate writes the bit to the set offset (0xA64/0xA68/0xACC…).

The trigger loop explicitly snapshots `0x3e700a64` (dispsys1 mutex CG status)
"check mutex cg and dsi cg in disp1a_cg_con" (crtc.c:13436-13440, 13508-13511)
— i.e. the driver itself watches that gate during streaming; if mutex/DSI
gates are off, the frame stalls. All *other* components in the pipeline have
their own gates on their own config blocks (their DT nodes list them, e.g.
dts:10774-10811 for outproc, dsi0 at dts:10793-10804) — each must be enabled
for its mutex bit to matter.

### MTCMOS / power

`mt6993_pwr_on_order` = CLK_DSI_PHY0, CLK_DIS0_A, CLK_DIS0_B, CLK_DIS1_A,
CLK_DIS1_B, CLK_OVL0, CLK_OVL1, CLK_OVL2 (mtk_drm_drv.c:4146-4155); power is
managed through `disp_pwr_dev` (dts `pwr-handle` on dsi0, dts:10803). The
mutex banks 0x3e32xxxx/0x3e72xxxx/0x3e52xxxx/0x3e92xxxx/0x3e02xxxx live in
the DIS0/DIS1/OVL2 power domains, ovlsys0/1 (0x3292xxxx/0x32c2xxxx) in
OVL0/OVL1. Without those domains, mutex register writes are dead.

### GCE event select

Not needed without GCE: the mutex's GCE events (STREAM_EOF, TE, etc.) are
consumed by GCE; the mutex hardware itself only needs EN/SOF/MOD + clocks.
The driver additionally programs GCE frame-done select registers
(`mtk_gce_event_config_MT6991`, ddp.c:31646+, MT6993 variant analogous) —
GCE-only, safe to skip. INTEN (0x00) only raises CPU interrupts
(`mtk_disp_mutex_irq_handler`, ddp.c:44766-44775) and is optional bare-metal,
but the driver sets bit0 (cmd) / bits0+8 (vdo) in dispsys0+dispsys1 INTEN
(ddp.c:37080-37143) — recommended to mirror for parity.

---

## 7. Concrete cheat-sheet (mutex id 0, DSI0 cmd-mode, single CRTC)

Addresses = bank base + offset:

```
# --- clocks on first (1 = enable into set regs) ---
w 0x3e300000+0xA64 : 0x2            # MM0_DISP_MUTEX0 gate on (bit1)
w 0x3e700000+0xA68 : 0x4            # MM1_DISP_MUTEX0 gate on (bit2)
w 0x32900000+0xACC : 0x2            # OVL_MUTEX0 gate on (bit1)
w 0x32c00000+0xACC : 0x2            # OVL1_MUTEX0 gate on (bit1)
w 0x3e000000+0xACC : 0x2            # OVL2_MUTEX0 gate on (bit1)

# --- SOF (cmd mode, EOF from DSI0) ---
w 0x3e320030 = 0x80   # dispsys0 mutex0 SOF
w 0x32920030 = 0x80   # ovlsys0 mutex0 SOF
w 0x3e720030 = 0x80   # dispsys1 mutex0 SOF
w 0x32c20030 = 0x80   # ovlsys1 mutex0 SOF

# --- module bits (RMW-OR) ---
# ovlsys0 (EXDMA/blender/outproc/DLO12 as composed)
w 0x32920034 |= <EXDMA|BLENDER|OUTPROC|OVL_DLI bits, see §3>
w 0x32920038 |= 0x00010000        # OVLSYS_DLO_ASYNC12 (bringup path)
# dispsys0
w 0x3e320034 |= 0x00000100        # DLI_ASYNC0 (+ PQ bits as enabled)
w 0x3e320038 |= 0x00002000        # RELAY5 (+ DLO_ASYNC16 etc.)
# dispsys1
w 0x3e720034 |= 0x00000200        # DLI_ASYNC29 (+DLI_ASYNC20 =0x1)
w 0x3e720038 |= 0x00101C01        # MERGE0|DSC0|SPLITTER0|POSTALIGN0|DSI0

# --- enable, per bank dispsys0/ovlsys0/dispsys1/ovlsys1 (driver also does
#     ovlsys2/sys_b/sys_b_side) ---
w +0x08 : bit0 = 0                # CFG clear
w +0x20 : 0x0                     # EN = 0
w +0x20 : 0x1                     # EN = 1
w +0x00 : 0x1                     # INTEN bit0 (dispsys0/1 only)
# then DSI start (DSI_START 0→1 pulse), each frame after TE
```

`0x00101C01` = MERGE0(bit20) + DSC0(bit14) + SPLITTER0(bit12) +
POSTALIGN0(bit11) + DSI0(bit0).

Open questions flagged above: (1) exact EXDMA/blender indices for a single
layer (runtime layering decision — take them from the bringup array or from a
captured working config); (2) whether the HRT-urgent-WA write to 0x3e730300 is
required on bare metal; (3) mutex RST is never used for id 0 — no reset needed.
