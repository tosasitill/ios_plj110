> **Latest candidate pan184** (built 2026-10-29, not yet run on hardware). Boot
> `x9/out/pan184-coherent-prime/boot.img` (9,388,032 bytes), plugins staged in a
> pinned mailbox instead of the boot partition. Changes since pan183: the
> metal2vulkan device priming accepts a negotiated noncoherent host-visible
> memory type for its never-submitted probe command buffer, and native IOKit
> CPU mappings are released through the matching IOKit unmap.
>
> **Last hardware run: pan183.** The mailbox installed the signed plugin files
> and XNU verified them, backboardd created the render context and three CS
> streams, and four GPU submissions completed with the independent fence,
> queue suspend, GPU cache flush and AS1 detach all confirmed. The earlier
> `EXC_GUARD` virtual-memory crash no longer appeared. Device creation then
> failed with `device refused: device: OutOfMemory(Device)` because the runtime's
> priming probe demanded `HOST_COHERENT`; pan184 addresses exactly that. Those
> four submissions are device-init work, not desktop rendering: the Metal
> device was never handed to backboardd, and no desktop frame has been
> produced by the GPU yet.
>
> **Earlier milestones** (pan47–pan169) cover LK console preservation, native
> UFS extent root, decmpfs codecs, keybag/shared-Data boot, userspace
> compatibility, the persistent CSF/GPUEB session, hardware shader compute,
> and render-engine bring-up. Their logs are deliberately not committed.

# OPPO Find X9 — direct LK bring-up

Experimental HelioDarwin port for **OPPO Find X9, PLJ110 / OP5E17L1, MT6993**.
It uses this repository's boot flow, not the UEFI/UniXNU flow from the reference
workspaces:

```text
MediaTek LK
  -> Android boot.img v4 / legacy LZ4 arm64 Image
  -> x9 bare-metal shim
  -> embedded SWEETXNU v1 payload
  -> this repository's plain arm64 SWEET XNU (xnu-8796.141.3)
```

**Current result: the phone boots this XNU and runs the unmodified iOS 16.7.16
userland, through the GUI.** What is still missing is GPU display: the render
engine and its Metal/PanVK plugins are loaded and complete GPU submissions, but
Metal device creation fails, so the compositor never receives a GPU device and
frames are still produced on the CPU. The generated default `boot.img` with no
`--kernel` is a loader stub, not XNU. This repository's tools never write a
device; the owner flashes by hand.

## Hardware status (2026-10-29)

Everything in this table was observed on the physical PLJ110 (12 GiB RAM,
1256x2760 panel, 64 MiB boot partition) through the pstore/disk-log channels.
Host builds and service registration are not evidence and are not listed.

| Area | Verified on hardware | Notes |
| --- | --- | --- |
| Boot chain | LK accepts the v4 image, decompresses the legacy LZ4 arm64 Image, jumps at `+0x1000`; the shim loads SWEETXNU and enters XNU `8796.141.3` | `lk finished --> jump to linux kernel 64Bit` in every run |
| Kernel | Board selection, early page tables, `arm_vm_init`, `startup_initialize_upto`, BSD init, `vfs_mountroot returned 0` | milestones 14-80 and BSD stages |
| Root disk | Kernel's own X9 UFS driver takes over the controller, DMA and block device, then mounts the 16 GiB HFSX volume read-write | root is a dedicated extent copy inside `userdata`; Android data preserved |
| UFS link | HS-G1, Rate B, 2 lanes per direction, read back from the device after switching (`mode 0x44`) | ~130-155 MiB/s in-kernel IO accounting; not a wall-clock benchmark |
| iOS userland | Unmodified launchd PID1 and system daemons run: decmpfs codecs, keybag `DONE MKB_INIT`, `DONE UMD_INIT`, `Cache sent to launchd successfully`, cryptex aliases, `Early boot complete` | Apple binary bytes unchanged |
| Display | LK scanout preserved; kernel publishes `AppleCLCD` 1256x2760 stride 5120 at `0xfc910000`; `GUI takeover, text console muted` and continuous swaps | frames are still copied into the panel buffer by the CPU |
| Touch | S3910 controller initialises and its IRQ path stays alive | taps were confirmed by the owner in earlier candidates; idle runs log `reports=0 contacts=000` |
| Keys / battery | PMIC power/volume keys arrive as HID consumer keys (lock/wake works), and the battery service answers | backlight is not controlled yet, see below |
| Network identity | Network stack, status-bar provider and device identity come up, so SpringBoard renders a status bar | no real Wi-Fi/modem data path |
| GPU session | Persistent CSF MCU: boot plus two PINGs, 8 CSG slots / 8 resident groups, AS0 live, GPUEB power retained, `caps=0x3f`, cache atom 256 | one session is kept for the life of the kernel |
| GPU compute | Hardware shader compute passes: `ADD32` cases, a 75-dispatch vector campaign with matching digests, private tile copy of 2048 pixels with intact guards | the same engine that desktop Metal submits through |
| Metal plugins | The versioned `X9GeneralMetal*.bundle` / `X9PanVK*.bundle` install from the pinned mailbox and are hash-verified at boot; backboardd reads `MetalPluginName` / `MetalPluginClassName` and opens a real render client | plugin binaries live on the iOS root, never in `boot` |
| GPU submission | backboardd creates a 3-stream CS group and four submissions complete with independent fence, extract, guards, group suspend, GPU cache flush and AS1 detach | pan181/pan183 |
| Completion fence | The 64-bit fence is only visible to the CPU after a bounded GPU cache acquire of the private fence page | without it every submission timed out with `fence=0` |

### What still does not work

| Item | State |
| --- | --- |
| Metal device | pan183 ended in `native-metal: device refused: device: OutOfMemory(Device)`: the runtime's device priming demanded `HOST_COHERENT`, which this backend deliberately does not advertise. pan184 relaxes that for its never-submitted probe command buffer and is **built but not yet flashed** |
| Desktop GPU frames | none yet. No submission has carried a desktop draw, and no GPU-produced pixel has reached the panel |
| Tiling / fragment / 3D | unverified. Tiler teardown reported "uncertain" and its heap and descriptor references were deliberately retained rather than recycled |
| Scanout | still CPU copy; no GPU mapping of the panel, no DSC or command-mode re-initialisation, no MIPI work |
| Frame pacing | no hardware vsync; the compositor is paced by the kernel's swap accounting |
| Backlight | no `backlight-control` service matches, so brightness stays whatever LK left; brightness changes do not reach the panel |
| Daemons | some still crash on every boot (e.g. `locationd`, the speech synthesisers); SpringBoard crash-looped in pan169 |
| Submit plan | no cross-stream waits, no internal dependencies, no cross-process fd/sync-file payloads |
| Storage semantics | a dedicated extent copy with an HFSX volume, not APFS; Data-volume and persona semantics are emulated |

### How to read a run

The owner flashes every candidate by hand and restores the stock Android boot
image afterwards, so `boot_a` read back from a running Android always holds the
Android image and says nothing about what was flashed. Evidence comes from the
persistent console (pstore) and from the dedicated disk-log sidecar; those logs
are not committed here. A candidate that hangs and is force-restarted leaves no
record at all, so "no log" is not "no boot".

## Early bring-up history (stage 14-80)

The records below are the original bring-up survey, kept because they name what
actually executed. Their log files are not part of this repository.

One attempt was flashed by the device owner (not by this repository's tools) and
read back from `expdb` afterwards. It did **not** reach a boot:

- LK accepted the v4 header and the legacy LZ4 payload. Its log shows
  `kernel_sz=0x0000256d`, `decompress kernel image...` with no failure line, and
  `boot_linux_fdt:883: lk finished --> jump to linux kernel 64Bit`.
- The size `0x256d` matches the non-diagnostic stub image exactly, so the record
  belongs to this build.
- The next preloader log reports `[RGU] rst from: lk` and
  `parse g_rgu_status: 2`, i.e. a **software reset**, not a watchdog timeout.
  That is consistent with the stub's own `wait_ms(2000); reboot()` (PSCI
  `SYSTEM_RESET`) running, which means the shim and the stub both executed.
- The screen garbled for a few seconds and then the phone restarted. The shim
  writes no pixels; the stub's copy targets are all inside
  `0x140000000..0x160000000`, far from the framebuffer at `0xfc910000`. LK itself
  has already stopped driving the display at that point, so the garbled output is
  not explained by this code and remains unexplained.

Later attempts used the pstore channel and a real SWEET kernel built with 0186
and 0187. What is now **hardware-observed**:

- A stub image with `--diag-only --pstore-mirror` leaves a readable
  `/sys/fs/pstore/console-ramoops-0` containing the shim log: `x9 shim`, the EL1
  report, `load=0x80000000`, `dtb=0x87c80000` and
  `loader: diagnostic stop before kernel copy`. This proves the console zone
  address is right and the shim runs. It says nothing about the kernel.
- A real-kernel candidate with the 0187 reset produced a record containing the
  shim log (`boot EL=0x1`, `cntfrq=0x3b9aca00`, `jump=0x1414bc488`), the board
  tag `HelioDarwin-X9-0186`, `SWEET EARLY 01` through
  `SWEET EARLY 14: early I/O page tables ready`, and then
  `diag: pstore durable, requesting PSCI reset`. Board selection, the ADT, the
  framebuffer geometry, page-table setup and `arm_vm_init` (including
  `enable_preemption()`) therefore ran. The reset came from
  `sweet_early_late_init`, which only runs after `arm_vm_init` returns.
- `expdb` on those attempts shows LK loading and decompressing our ~8.9 MB image
  (`kernel_sz=0x0088f993`, `decompress_kernel takes 153 ms / 185 ms`) and
  `lk finished --> jump to linux kernel 64Bit`. The reset *after* the jump
  classified cold in the preloader log, which is why a candidate that hangs and
  is force-restarted leaves no record at all.

Two further attempts carried the reset to later milestones, and the records name
exactly what ran:

- **stage 21** (`logs/x9-boot-stage21.log`): shim log, marks 1–14, then
  `SWEET EARLY 15: arm_vm_init returned`, `SWEET EARLY 21: pstore mapped; early
  log flushed`, `SWEET EARLY 21: checkpoint after pstore flush`, and
  `diag: pstore durable, requesting PSCI reset`. `arm_vm_init` returns, late-init
  returns, and the ramoops flush completes.
- **stage 24** (`logs/x9-boot-stage24.log`): the same, plus
  `SWEET EARLY 22: patch_low_glo done`, `SWEET EARLY 23: sleep token buffer done`
  and `SWEET EARLY 24: consistent debug inherited`. Everything through
  `PE_consistent_debug_inherit()` executes.
- **stage 30** (`logs/x9-boot-stage30.log`): the same, plus
  `SWEET EARLY 16: calling serial_init`, `SWEET EARLY 17: fbcon config read;
  mapping`, `SWEET EARLY 18: fbcon mapping returned`,
  `SWEET EARLY 19: serial_init returned` and
  `SWEET EARLY 30: calling kprintf startup`. The framebuffer-console mapping and
  `serial_init()` complete.
- **stage 35** (`logs/x9-boot-stage35.log`): the same through stage 30, then
  `SWEET EARLY 35: startup_initialize_upto returned`. Marks 17/18 come from
  `pexpert/arm/pe_serial.c`, so the 1256×2760 framebuffer mapping itself succeeds.
  This was the first record that started at offset 0, with no Android TEE
  overwrite in front of it.
- **stage 36** (`logs/x9-boot-stage36.log`): the same, plus
  `SWEET EARLY 36: kprintf returned` and the reset announcement in the same
  record. `kernel_startup_initialize_upto(KPRINTF)` returns and `kprintf()`
  itself returns.

So the earlier "the kernel hangs after `arm_vm_init`" reading was wrong: 0187's
reset fired from inside `sweet_early_late_init`, which runs *before* stage 21, so
stage 15 could never appear. Later captures supersede this early checkpoint
survey: `logs/x9-boot-ckpt71-bare.kernel-progress.txt` contains VM bootstrap
output and bootstrap-thread checkpoints through `sched_startup` and
`thread_daemon_init`; the corresponding new expdb software-reset sequence
supports arrival at the stage-71 boundary. The combined stage-80/pan47 run did
not retain the later checkpoints, so stage 80 remains unverified.

The records also show the observation channel's own damage: on some attempts the
first ~2 KiB of the console zone holds Android's Trustonic TEE output, which
overwrote the start of our record after Android came up, and scattered characters
inside the surviving tail are mangled. The milestone numbers and labels are still
readable, but this zone is shared with Android and is not a lossless channel.

What is **not** hardware-observed: completed `clock_service_create`, later IOKit
initialization, interrupt delivery, watchdog servicing from timer IRQs, BSD,
root mounting, userland or visible pan47 scanout. Bootstrap-thread execution is
observed; an interrupt-driven, interactive system is not. Software resets do
not prove that watchdog servicing is load-bearing. Host builds are not hardware
boot results; keep the original captures alongside the interpretation.

The device owner flashes every candidate by hand and restores the stock Android
boot image afterwards, so a `boot_a` read from a running Android always holds the
Android image. It is not evidence about whether a candidate was flashed.

## Diagnostic build

`--diag-only` stops in `loader_boot` after payload validation, before the first
store or jump, logs, waits, and resets. It separates "the shim ran" from "the copy
did something" without needing a working console. `--pstore-mirror` also writes
the log into the ramoops console zone so it survives the reset and can be read
back from Android:

```bash
python3 -I x9/tools/boot-xnu.py --stub --diag-only --pstore-mirror
adb shell su -c "cat /sys/fs/pstore/console-ramoops-0"
```

The zone is **0xaec0f000**, not the `0xaec00000` carveout base. Live X9 ramoops
parameters are mem `0xaec00000` + `0x310000`, console `0x200000`, ftrace `0x1000`,
pmsg `0x100000`, record `0x1000`; ramoops places its dump records first, so the
console zone starts after `0xf000`. The same arithmetic reproduces the proven
X676C console at `0x4811f000`. Writing the base instead would land in the dump
records and lose the log. `PstoreLayoutTests` pins both. Mirroring overwrites the
previous boot's record, so it is off by default.

## Kernel-side diagnostic reset

The shim can prove it ran; it cannot prove the kernel did. The kernel side has a
separate opt-in, **patch 0187**, which resets from inside the kernel once the
ramoops record is mapped and the buffered pre-VM log has been flushed into it.
The reset is deliberately after that flush, so the record always contains the
board tag, the boot_args geometry and every `sweet_early_mark` stage that ran.
Because the reset is a PSCI warm reset, DRAM keeps the record and the next
Android boot can snapshot it; a kernel that hangs and is force-restarted instead
is cold-reset and leaves nothing. On a kernel that would have booted, this turns
every boot into a reset loop, so it defaults off and a build script flips it.

**Patch 0188** makes that reset selectable and adds post-`arm_vm_init`
milestones. It also moves the reset out of `sweet_early_late_init` into
`sweet_diag_checkpoint`, so exactly one reset site exists and `arm_init.c`
decides which milestone reaches it — a second copy inside late-init would fire
first and make every later milestone unreachable.
`SWEET_X9_CHECKPOINT_STAGE` picks the milestone; the define lives in
`sweet_early.h` because `arm_init.c` is what consults it, and a define visible
only to `sweet_early.c` silently turns every checkpoint off.

`sweet_diag_checkpoint()` must stay `noinline`. Inlined into
`sweet_bootstrap_mark()` the compiler folded the comparison against
`SWEET_X9_CHECKPOINT_STAGE` into a test against zero, because the stage comes
from a table it cannot evaluate; every bootstrap checkpoint would then compare
correctly but never fire the reset. Nothing warns about it — it was found by
disassembling `sweet_early.o` and seeing `cbnz x19` where `cmp w19, #0x32` was
expected. The host test now pins the attribute in the source text.

| Milestone | Meaning |
|---|---|
| 15 | `arm_vm_init` returned (so its page-table setup and `enable_preemption()` completed) |
| 21 | pstore mapped and the early log flushed |
| 22 | `patch_low_glo` done |
| 23 | sleep-token buffer initialized |
| 24 | `PE_consistent_debug_inherit` done |
| 30 | `serial_init` returned and `kernel_startup_initialize_upto(KPRINTF)` about to run |
| 31 | `PE_init_printf` done |
| 32 | `cpu_machine_idle_init` done |
| 33 | second `PE_init_platform` done |
| 34 | `PE_init_cpu` and `fiq_context_init` done |
| 35 | `kernel_startup_initialize_upto(KPRINTF)` returned |
| 36 | `kprintf` returned |
| 37 | `PE_create_console` about to run |
| 38 | `PE_create_console` returned |
| 39 | `PE_init_printf` returned |
| 40 | `cpu_machine_idle_init` returned |

Every `arm_init.c` milestone calls `sweet_diag_checkpoint()`, which compares its
stage against `SWEET_X9_CHECKPOINT_STAGE` at run time, so choosing the reset point
is a one-line edit to that define. A checkpoint refuses to reset before the
ramoops record is mapped, because those earlier milestones run out of local
buffers and a reset there would leave nothing for Android to snapshot.

Milestones 50–56 exist because the gap between 49 and the first bootstrap stage
could not be observed at all: a build that stopped at that first stage hung
without resetting, so nothing between `machine_startup()` being called and
`kernel_bootstrap()` reaching `vm_mem_bootstrap` was visible. That gap contains
the boot's first `printf()` (the version string) and `scale_setup()`, neither of
which any earlier milestone had exercised.

| Milestone | Meaning |
|---|---|
| 50 | `machine_startup` entered |
| 51 | `fastipi` boot-arg parsed |
| 52 | `machine_conf` returned |
| 53 | `kernel_bootstrap` about to be called |
| 54 | `kernel_bootstrap` entered |
| 55 | version string printed |
| 56 | `scale_setup` returned |

Past that, the same mechanism covers `kernel_bootstrap()` stages 60–80:

| Milestone | Stage name |
|---|---|
| 60 | `vm_mem_bootstrap` |
| 61 | `console_init` |
| 62 | `sched_init` |
| 63 | `PMAP_ACTIVATE_KERNEL` |
| 64 | `machine_init` |
| 65 | `clock_init` |
| 66 | `task_init` |
| 67 | `thread_init` |
| 68 | `kernel_thread_create` |
| 69 | `sched_startup` |
| 70 | `thread_daemon_init` |
| 71 | `clock_service_create` |
| 72 | `device_service_create` |
| 73 | `PE_init_iokit` |
| 74 | `early_boot_ready` (early-boot startup phase returned) |
| 75 | `interrupts_enabled` (`spllo` returned; delivery unverified) |
| 76 | `screen_acquired` (`initialize_screen` returned) |
| 77 | `commpage_ready` |
| 78 | `trust_cache_init` |
| 79 | `machine_lockdown` |
| 80 | `bsd_init` (before the call) |

Patch 0189 adds 72–80. Patch 0190 adds the opt-in
`SWEET_X9_DIAG_PMSG` journal (default 0): it adopts the shim's record only at the
captured X9 pmsg address `0xaee0f000` and uses the lower 512 KiB to stay away
from the ECC tail. CRC-protected `x9m` records carry the stage and mapping flags;
the selected bootstrap target is recorded before the console-free PSCI probe.
Use `python3 -I x9/tools/decode_boot_journal.py LOG...` to decode these and the
pan48 `q48` cascade-selector records without repairing damaged fields.

The hook sits in `kernel_bootstrap_log()` and `kernel_bootstrap_thread_log()`,
the one funnel every stage already goes through, instead of at each of the 40+
call sites. Stages are identified **by name**, not by call order:
`kernel_bootstrap_log()` is `static inline` at those sites and is also reached
from the startup-entry loop, so a counter would depend on inlining and drift
between builds. Unknown names are ignored, which keeps the record to the stages
worth naming (and is why the mark cap moved to `SWEET_X9_MAX_STAGE`, 96).

**A trace build that lets the kernel keep running is not usable as a diagnostic
here.** It was tried: `SWEET_X9_TRACE_ALL` records every milestone and does not
reset. That attempt produced no record at all — `console-ramoops-0` was simply
absent from `/sys/fs/pstore` on the next boot, while earlier reset-at-a-stage
builds all left readable records. When the kernel hangs instead of resetting, the
only way back is a forced restart, and that is a *cold* reset: DRAM is
reinitialized and the record is gone before Android can snapshot it. The
diagnostic kernel must reset itself at a known milestone; that warm reset is the
entire reason the record survives. Milestones past the chosen one stay
unobservable by design, so each attempt moves the stop point forward.

The exception would be a hang that the hardware watchdog itself converts into a
warm reset. The X9 watchdog is only petted from the timer IRQ, so a hang before
interrupts are up should be reset by it — but that path has never been observed
here, and the trace attempt produced nothing, so it is not a channel to rely on.

A mark's presence means it ran; the *last* mark in the record is where execution
got to. Nothing between these milestones is claimed to work merely because an
earlier one appears.

The local build wrapper is not part of this repository; what matters is that the
patch chain through 0188 is applied and the kernel is packaged with
`--console-base 0xaec0f000 --console-size 0x200000 --pstore-mirror`, otherwise
the record never reaches DRAM.

## Build offline

Requirements:

- Python 3.10+ and the `lz4` Python package (`python3 -m pip install lz4`).
- Native LLVM: `clang`, `clang++`, `ld.lld`, `llvm-objcopy`, `llvm-nm`, including
  AArch64 support. A Zig fallback exists but has not been independently tested.
- A native C compiler for the host payload validator and regression fixtures.

From the repository root:

```bash
# Loader ABI test only; no XNU or operating-system image required.
python3 -I x9/tools/boot-xnu.py --stub

# Pack a rebuilt SWEET kernel containing patch 0186.
python3 -I x9/tools/boot-xnu.py \
  --kernel xnu/out/kernel.development.sweet
```

Output under ignored `x9/out/`:

- `boot.img`, `boot.json`: final candidate and manifest with hashes, sizes,
  `kernel_kind`, `hardware_tested: false`, and `device_written: false`.
- `x9.adt`, `payload.bin`: Apple Device Tree and embedded SWEETXNU container.
- `shim/`: ELF, expanded Image, cross-compiled objects and packaging manifest.
- `stub.macho`: present for `--stub`, explicitly a loader ABI test.

The stub logs its boot_args and requests a PSCI reset after eight seconds. A host
build does not demonstrate that LK accepts the image or that these messages
executed on the phone. The eight-second wait is not a hardware timing result; it
exists so the pstore mirror settles before the warm reset.

Tool overrides: `X9_CC`, `X9_LD`, `X9_OBJCOPY`, `X9_NM` each select one executable.
For example, use `X9_LD=/path/to/matching-ld.lld` when the installed lld and LLVM
libraries are mismatched. Do not globally force old LLVM libraries with
`DYLD_LIBRARY_PATH`: that can break a newer clang. A machine-local linker wrapper
may be passed through `X9_LD`; no installed Homebrew files need changing.

These tools do not download a kernel, dispatch CI, invoke adb/fastboot, flash,
reboot, unlock, or alter partitions. Do not run the X676C flashing script against
X9.

## Kernel integration

The XNU source is not vendored. `.github/workflows/xnu.yml` builds the existing
**SWEET** machine configuration with numbered patches; append
[`0186-x9-board-isolation.patch`](../xnu/patches/0186-x9-board-isolation.patch),
[`0187-x9-opt-in-early-psci-reset.patch`](../xnu/patches/0187-x9-opt-in-early-psci-reset.patch)
and
[`0188-x9-post-vm-init-milestones.patch`](../xnu/patches/0188-x9-post-vm-init-milestones.patch)
after 0185. Use a rebuilt **plain arm64**, not arm64e/PAC, kernel.

Patch 0186:

- Selects X9 from the root `compatible` after `PE_init_platform` initializes
  SecureDT; keeps X9, X676C and Qualcomm peripheral paths distinct. The match
  key is `mediatek,MT6993` because that is what the DT LK hands over actually
  carries at `/`; `oppo,x9` is not present on the device. An earlier revision
  matched `oppo,x9`, so board detection silently failed and every X9 isolation
  path stayed disabled on hardware.
- Removes raw pre-ADT framebuffer drawing and unconditional X676C pstore aliases.
- Validates boot video, uses its physical block/geometry, enlarges the shared
  framebuffer shadow, and maps only the first frame rather than the full
  multi-buffer carveout.
- Maps the optional persistent console record as Normal-NC DRAM after VM setup,
  with a bounded keep-head append. Earlier messages remain in a local buffer
  until this mapping is available. The known X676C console is retained for that
  board only.
- Maps MT6993 TOPRGU after VM setup and pets its restart register from virtual
  timer IRQs, independent of display callbacks. This still depends on delivered
  timer interrupts and is not proof of a watchdog handoff.
- Rejects legacy UFS/input initialization on X9; guards MT6789 DSI and Mali
  mappings, Qualcomm DSI/MDP/modem/battery probing, and accelerator publication.
- Keeps CPU0-only topology and bounds redistributor walking, accounting for an
  explicit stride or `GICR_TYPER.VLPIS` rather than guessing from region size.

The packer requires the compiled `HelioDarwin-X9-0186` marker before accepting a
real kernel. This catches accidental old-kernel input; it is not a cryptographic
attestation or proof of a full kernel build. Stub mode has its own marker and is
explicit in the manifest.

Patch application was checked against selected files reconstructed from Apple's
matching `xnu-8796.141.3` source plus this repository's patch chain through 0185.
The early-console source also cross-compiles with fixture declarations. **A full
SWEET XNU compile/link has not been run here**: this host has Command Line Tools,
not the workflow's Xcode 15-compatible SDK/generated-header environment. The
existing `xnu-buildtree` CI artifact supplies that environment for individual
compile checks.

An optional `--ramdisk /path/to/root.img` embeds a root image in SWEETXNU using
this repository's existing `RAMDisk` segment support. Use matching XNU boot
arguments, for example `--boot-args 'debug=0xc serial=3 cpus=1 keepsyms=1 -v rd=md0'`.
This is **md0**, not an Android vendor/init ramdisk, and does not establish an X9
UFS/root-filesystem port.

## Entry and memory contracts

- Linux arm64 entry: **MMU off**, EL1 or EL2, x0 = physical FDT. EL2 is normalized
  to EL1: `HCR_EL2` is written first (clearing `E2H`/`TGE`), then `VBAR_EL2` is
  installed, so the vectors land in the real EL2 table rather than the VHE alias.
  FEAT_FGT traps, `CPTR_EL2`, `VPIDR_EL2`/`VMPIDR_EL2` and, when `ID_AA64PFR0_EL1.GIC`
  is nonzero, `ICC_SRE_EL2`/`ICH_HCR_EL2` are set before the `ERET`, because XNU's
  `pe_fiq.c` drives the GIC through `ICC_SRE_EL1`/`ICC_PMR_EL1`. Cache maintenance
  runs at EL1, where `DC CISW`/`IC IALLU` are defined. Unknown MMU-on LK mappings
  are unsupported; no guessed VA aliases or exception-skipping probes are used.
- Both EL1 and EL2 traps report through this shim's own vector tables instead of
  resuming firmware vectors; there is no `ELR += 4` skip harness.
- The Image's branch at +4 and its PE-compatible entry target the actual shim
  at **+0x1000**. A PE-compatible header does not introduce UEFI execution.
- Header **v4**, matching the captured stock `boot_a.img`: 1584-byte header,
  4096-byte pages, zero reserved fields and `signature_size`. v3+ dropped the
  in-header kernel/ramdisk/tags addresses, so the builder no longer writes them;
  LK chooses the load addresses. A nonempty valid `newc` CPIO trailer
  accompanies the kernel. The builder does not append another DTB; LK's
  vendor_boot/DTBO selection remains in place.
- Legacy LZ4 has one initial magic, followed by 8 MiB compressed-block lengths
  and blocks, with EOF termination — the same block size the stock image uses.
  Packaging verifies an exact decompression round-trip and image-load/boot
  partition bounds. **No AVB footer or vbmeta is emitted.** The stock image
  carries both; whether X9 LK requires them on an unlocked bootloader is not
  established here, and the packer does not claim otherwise. LK acceptance
  remains untested.
- XNU uses **16 KiB** pages. The shim checks TGran16 support; virtual base must be
  32 MiB aligned. It supplies revision/version 2 boot_args and a kernel-virtual
  `deviceTreeP`, not a physical pointer.
- Kernel memory is only the captured contiguous **512 MiB** run
  `0x140000000..0x160000000`. Segment destinations, ADT, boot_args and source/shim
  overlap are checked before copies. The phone's full approximately 12 GiB RAM
  is not handed to XNU.

## Captured board values and limitations

| Item | X9 value | Implementation boundary |
|---|---|---|
| Boot CPU | MPIDR `0x000` | Only cpu0 exposed; no SMP/heterogeneous-topology claim |
| GICD | `0x06800000`, size `0x40000` | Existing ADT-driven GIC path |
| GICR | `0x06880000`, size `0x200000` | Affinity/stride bounds; hardware delivery unverified |
| Virtual timer | INTID 27 | Default timebase 1 GHz from captured DT, not a new CNTFRQ measurement |
| Watchdog restart | `0x1c010008`, key `0x1971` | Shim pet; kernel timer-IRQ hook |
| LK framebuffer | `0xfc910000`, 1256×2760, stride 5120 | Linear memory writes only |
| Display carveout | `0xfc910000..0xfff6ffff`, size `0x3660000` | First frame `0xd7a000` fits the early block at `0xfc000000` |
| UFS HC | `0x16810000` | No MT6993 UFS/PHY/reset driver yet |
| DSI | `0x3e730000` | No X9 OVL/RDMA/M4U/DSC/SPR/DSI scanout driver yet |

The captured panel is `panel_ac287_p_7_a0020_dsi_cmd`, with DSC v1.2 and SPR.
Writing the LK physical framebuffer is **not** evidence that the command-mode
panel updated. Android's captured scanout uses M4U IOVAs, not this LK PA. Do not
reuse MT6789 display, GPU, PMIC or clock sequences on MT6993.

Storage, native display scanout, GPU, physical input, audio, wireless and an iOS
userland boot are not implemented/verified by this candidate. The optional
framebuffer service is a software memory path, not a functioning X9 display
controller.

## Console output

Console exists; a later Android boot can overwrite it, and fastboot extraction
is not assumed. This work does not revisit console availability.

Default shim/kernel early logging stays in local 16 KiB buffers. Persistent
output is opt-in with **one explicitly known console record**:

```bash
python3 -I x9/tools/boot-xnu.py --stub \
  --console-base KNOWN_CONSOLE_PA --console-size KNOWN_CONSOLE_SIZE
```

Both values must describe a real single console zone inside the captured pstore
carveout `0xaec00000..0xaef10000`. The carveout base is **not** a default console
record address. The same values reach the shim build and the ADT's
`x9-console-base` (u64) / `x9-console-size` (u32) properties. No scanning or writing
of guessed zones is performed. Local buffers alone do not survive a reset;
optional record output does not guarantee recovery after Android overwrites it.

## Offline one-try A/B preparation

Given an existing **32-byte AOSP bootloader_control snapshot**, prepare a separate
output file:

```bash
python3 -I x9/tools/bootctl.py /path/to/bctl-snapshot.bin \
  x9/out/bctl_slot_a.bin --slot a

# Or produce the same metadata alongside a boot candidate:
python3 -I x9/tools/boot-xnu.py --stub \
  --boot-control /path/to/bctl-snapshot.bin --slot a
```

The helper validates magic/version/two-slot layout/CRC, requires the other slot
already be a successful, non-corrupt fallback, and selects the candidate for
**one try**, clearing its stale verity-corruption flag while retaining reserved
bits and the other slot's status.
It neither obtains the snapshot nor writes `misc`, boot partitions or active
slot state. It does not invent an A/B record or assume an X9 partition offset.
Whether X9 LK honors this exact format and fallback is not verified here.
Fallback is conditional on intact firmware, metadata and a usable other slot;
it is not a guarantee against an unbootable phone.

## Tests

```bash
python3 -I -m unittest discover -s x9/tools -p 'test_*.py' -v
python3 -I x9/tools/test_boot.py ImageTests.test_multiblock_lz4_roundtrip -v
python3 -I x9/tools/test_kernel.py \
  KernelPatchTests.test_x9_no_console_only_maps_its_watchdog -v
```

Coverage includes ADT identity/geometry, SWEETXNU bounds, bounded FDT parsing,
ARM64/PE entry agreement, multi-block LZ4, boot-header tampering, old-kernel
rejection, and successful-fallback A/B snapshots. `PstoreLayoutTests` recomputes
the ramoops console-zone offset from the live X9 parameters and checks it against
the hardware-proven X676C zone, because a wrong base silently loses the log.
`ShimEntryTests` assembles
`start.S` and asserts the EL2 normalisation order in the machine code: `HCR_EL2`
before `VBAR_EL2`, GIC/identification registers before `ERET`, and cache
maintenance reached only through the EL1 path. That is a disassembly check, not
execution of the entry path. Kernel fixtures reconstruct added sources from
repository patches and test pre-init no-MMIO behavior, optional console bounds,
prefix-preserving keep-head writes, the exact watchdog map, X676C isolation, C++
linkage and ARM64 early-console compilation.

Fixtures replace only ARM barriers for the native-host console test and provide
mock declarations/MMIO allocations. They do **not** emulate the phone, full XNU,
LK entry state, watchdog timeout, GIC delivery or panel scanout. Existing shared
payload code can emit `ResourceWarning` for its read handles; this does not mean
the tests failed, and the X676C packer was not changed for this port.

## Source boundaries

Reference workspaces and captures were read-only. No UEFI code, device dumps,
proprietary firmware or operating-system images are included in `x9/`. The
existing X676C shim/tools were not rewritten. Future phone testing, partition
writes or CI dispatch require a separate explicit request.
