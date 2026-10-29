# MT6993 cmd-mode frame path — extracted from kernel source

Source root: `oppo-source/modules/kernel/kernel_device_modules-6.12/drivers/gpu/drm/mediatek/mediatek_v2`

## The per-frame trigger (authoritative)

`mtk_dsi.c:8576` (MTK_TRIG_FLAG_SOF case of the DSI comp trigger), CPU path
`trigger_without_cmdq`:

```
MEM_CONTI  (+0x48) = 0x3c          # mtk_dsi_rxtx_control, mtk_dsi.c:2815
CMDQ0      (+0x400) = 0x002c3909   # mtk_dsi.c:8576
CMDQ_CON   (+0x44) = 1 (CMDQ_SIZE) then |= CMDQ_SIZE_SEL(BIT15)
START      (+0x00) = 0
START      (+0x00) = 1             # bit0 only; VM_CMD_START is BIT(16)
START      (+0x00) = 0             # mt6993 path: cleared again after trigger
```

`0x002c3909` decode (V2 short-packet word, same encoding our shim already uses):
- bits [7:0]   = 0x09  config: SHORT(0) | HSTX(8) | BTA(1) → **short packet, HS, with BTA**
- bits [15:8]  = 0x39  DCS long-write type
- bits [31:16] = 0x2c  count = 44
- payload words live at CMDQ0+0x04 and on (`dsi_cmdq` FIFO), read by the engine from
  DSI_MEM_CONTI.

The identical pair appears in the BDG self-test bridge (`mtk_disp_bdg.c:1912-1915`):
`TX_MEM_CONTI=0x3c; TX_CMDQ=0x2c3909; TX_CMDQ_CON=1` — independent confirmation.

## What is still NOT established

1. **Where the 44 payload bytes come from.** DSI_MEM_CONTI=0x3c points at the internal
   "memory continuous" buffer. In cmd mode the kernel fills it through the DDP pipeline
   (merge/DSC → DLO → DSI buffer). We have not extracted that producer path.
2. **Whether the DSI buffer registers must be enabled first** (`DSI_BUF_CON0` =
   dsi_buf_con_base 0x300 for mt6993, BUF_CON1 +0x4, BUF_SIZE +0xc, TX_BUF_RW_TIMES).
   Not yet extracted — the component agent died before reaching it.
3. **DSC**: all three panel timings are DSC (dsc_params). Whether a frame pushed as
   *uncompressed* DCS 0x2C data is even accepted by this DDIC is unverified. The panel
   dtsi has a suspected DSC-switch command `39 ... 03 8B 21 E0` in the init table.
4. **Self-pattern** (`DSI_SELF_PAT_CON0 0x230` / `CON1 0x234`, values
   `0x0fff0411` / `0x0fff0fff`) is real hardware but `DSI_SELF_PATTERN` is
   `#ifdef`-commented out (`mtk_dsi.c:87`), i.e. dead code in this kernel. It sits at
   `mtk_dsi.c:6554` in `mtk_dsi_power_on`, **before** `mtk_dsi_start()`, and
   `mtk_dsi_start()` is only called when NOT in cmd mode (`mtk_dsi.c:6561`).
   BDG's equivalent is `DSI_TX_SELF_PAT_CON0/1` at TX-domain +0x178/+0x17C with
   `SELF_PAT_PRE_MODE` (bit 0) as the enable — **different register block** from the
   real DSI's 0x230/0x234.

## MT6993 mutex topology (verified from source)

`mtk_drm_ddp.h:42-48`:
```
MUTEX_EN(n)  = 0x20 + 0x20*n
MUTEX_SOF(n) = mutex_sof_reg + 0x20*n      (mt6993: 0x30)
MUTEX_MOD0(n)= 0x34 + 0x20*n               (mt6993: MOD0 0x34, MOD2 0x38)
```
`mt6993_mutex_mod` (`mtk_drm_ddp.c:6504`) puts **DSI0, DSC0-3, MERGE0/1, SPLITTER0/1,
POSTALIGN0, DLI/DLO_ASYNC and RELAY0-4 in MUTEX1**, while AAL/C3D/COLOR/DITHER/GAMMA/
POSTMASK/PWM/SPR/ODDMR/TDSHP live in MUTEX0's banks. `BIT(31)` in a mod value is the
"write to MOD1(0x38) instead of MOD0(0x34)" routing flag.

Mutex banks (mt6993.dts:10832-10838, `disp1_mutex0@3e320000`), 7 `reg` ranges in order:
| # | base | role |
|---|------|------|
| 0 | 0x3e320000 | DISPSYS0 |
| 1 | 0x3e720000 | DISPSYS1 (`side_regs`) |
| 2 | 0x3e520000 | OVLSYS0 |
| 3 | 0x3e920000 | OVLSYS1 |
| 4 | 0x32920000 | OVLSYS2 |
| 5 | 0x32c20000 | DISPSYS_B_0 |
| 6 | 0x3e020000 | DISPSYS_B_1 |

`DISP_MUTEX_TOTAL_v2 = 8`, crtc0 uses mutex id 0.

## OVLSYS crossbar mout offsets (`platform/mtk_disp_ovlsys_reg.h:117`)

```
OVL_EXDMA_OUT_CROSSBAR0..14_MOUT_EN = 0xEC8,0xED0,0xED8,0xEE0,0xEE8,0xEF0,
                                      0xEF8,0xF00,0xF08,0xF10,0xF18,0xF20,
                                      0xF28,0xF30,0xF38
OVL_BLENDER_OUT_CROSSBAR0..11_MOUT_EN = 0xF40..0xF6C (step 4)
OVL_OUTPROC_OUT_CROSSBAR0..3_MOUT_EN  = 0xEB8,0xEBC,0xEC0,0xEC4
```
The wiki's earlier "surviving paired 16-bit selects at mmsys 0xe7c/0xe8c/0xe94/0xe98/
0xe9c/0xea4" are in the **DISPSYS** bank (0x3e300000), i.e. a different register file
from the OVLSYS crossbar above — do not conflate them.

## BDG minimum viable push (kernel's own no-DDP-path panel test)

`mtk_disp_bdg.c`:
```
bdg_tx_set_mode(CMD_MODE)     TX_MODE_CON.MODE = CMD_MODE (0)
bdg_tx_bist_pattern(on,sel,r,g,b)  SELF_PAT_R/G/B = (c&0xff)<<4, SELF_PAT_SEL=sel,
                                   SELF_PAT_PRE_MODE = 1
bdg_vm_mode_set(enable, long) VM_CMD_CON1 = 0x10; DDI_POST_CTRL = 0x00001110;
                              VM_CMD_CON0 = 0x37 (long) or 0x35
bdg_tx_cmd_mode()             TX_MEM_CONTI = 0x3c; TX_CMDQ = 0x2c3909;
                              TX_CMDQ_CON = 1
bdg_mutex_trigger()           MUTEX0_EN 0 -> 1
```
This is the closest thing to a documented "push pixels without a full DDP pipeline"
recipe in the tree — but it targets the BDG TX register block, not the real DSI at
0x3e730000, so its offsets are NOT directly usable.

## LK ground truth (logs/lk-tail.log, pulled from expdb 2026-10-25)

```
fb_va: 0xffff000000f01000, fb_pa: 0xfc910000, fb_pa_k: 0xfc910000
(mt_disp_sw_init+) check dsi state: 0x2c3909, 0x0
videolfb - fb_base    = 0xfc910000
videolfb - islcmfound = 10001
videolfb - fps        = 12048
videolfb - vram       = 57016320        (0x3660000)
videolfb - firstEXDMA = 3
videolfb - lcmname    = panel_ac287_p_7_a0020_dsi_cmd
videolfb - panel_id   = 0x950601
[INFO][LCM][AC287_P_7_A0020][lcm_pre_init_ddic_power] value=1
  pmic_vddi_enable  ... read3 enable:0x1
  pmic_vddr_enable  ... read3 enable:0x1
  pmic_vci_enable   ... read3 enable:0x1
[PROFILE] ::: mt_disp_show_boot_logo() takes 249 ms
```
LK's framebuffer is at the **same 0xfc910000** address the wiki already records, and
LK's own "dsi state" word at `mt_disp_sw_init` is **0x2c3909** — the same value as the
per-frame trigger word, which is consistent with LK leaving the DSI engine configured
to run that packet. `firstEXDMA = 3` means LK drove the logo through **EXDMA3**
(the 4th EXDMA, base 0x3e080000 per the wiki map).

## pan39: what "the engine is configured" actually requires

Three functions, all in `mtk_disp_ovl_exdma.c`, and all three were
missing from every pan through 38:

| function | line | the write that matters |
|---|---|---|
| `mtk_ovl_exdma_start()` | 825 | `GDRDY_PRD 0x208 = 0xFFFFFFFF`, `RDMA_BURST_CON1 0x1F4` DDR bits |
| `mtk_ovl_exdma_golden_setting()` | 3902 | FIFO/GMC/GREQ thresholds (`!is_dc`) |
| `mtk_ovl_exdma_layer_on()` | 1084 | **`RDMA0_CTRL 0x100 = 1`** |

`L0_EN (0x40)` gates the layer into the blender. It does not start a
G-REQ. `RDMA0_CTRL` bit 0 is the fetch enable, and `RDMA0_CTRL` bits
27:16 are `FIFO_USED_SIZE` — the live read-buffer occupancy, i.e. the
most direct "DRAM bytes are arriving" signal the engine exposes.

### AID offsets differ between the mt6991 and mt6993 tables

Same header, `platform/mtk_disp_ovlsys_reg.h`:

| | mt6991 | mt6993 |
|---|---|---|
| EXDMA0 AID | 0xB00 | 0xE0C |
| EXDMA3 AID | 0xB30 | **0xE3C** |

mt6993 sets `aid_per_layer_setting = true`, so layer N is
`base + N * MT6985_OVL_LAYER_OFFEST (0x4)`. pan38 wrote 0xE2C, which
is EXDMA2's AID — the secure bit was being cleared on the wrong engine.

### LK's live crossbar (read from the pre-write probe)

```
0xEE8 = 0x2        EXDMA3 -> BLENDER1
0xEF0 = 0x4        EXDMA4 -> BLENDER2
0xF58 = 0x00010001 BLENDER6 -> OUTPROC0
0xF40 = 0x0        BLENDER0 out unused
```

`0xF58`'s bit 16 is not part of the route encoding (the BLENDER out
crossbar uses bits 0..7); it is a separate enable that LK sets. Keep
it when clearing bit 0.

### mt6993 ovlsys0 offset map (no collisions)

```
AID      EXDMA0..7 : 0xE0C + n*0x10        (0xE0C..0xE7C)
EXDMA out CB 0..14 : 0xEC8 + n*0x8         (0xEC8..0xF38)
BLENDER out CB 0..11: 0xF40 + n*0x4        (0xF40..0xF6C)
OUTPROC out CB 0..3: 0xEB8 + n*0x4         (0xEB8..0xEC4)
```

The mt6991 table uses a completely different layout (AID 0xB00+, EXDMA
CB 0xE50+, BLENDER CB 0xE00+, OUTPROC CB 0xF10+). Never mix them.
