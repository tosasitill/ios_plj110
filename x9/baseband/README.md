# X9 baseband bridge (iOS side)

`qcom/` is an import of UniXNU/QcomBaseband (BSD-2-Clause, HoshinoYumeka / chiba233; upstream commit recorded in
`qcom/UPSTREAM.json`). Only `baseband/` (AppleBaseband IOKit services, QMUX/QRTR bridge, SAHARA stub, PDP interfaces, eSIM Tape)
is used. `ipa/` and `platform/coachz` (Qualcomm IPA/GSI) are not copied: the X9 modem is MediaTek, its data plane is DPMAIF.

Status (2026-11-09): the MT6993 CCCI file/RPC service reaches HS2/READY, SIM READY and network registration in `~/139.txt`.
NV partitions are read-only; modem writes go into a 128 MiB volatile overlay. Reboot discards those writes.

With `x9_bb=1`, `X9Baseband.cpp` prepares the AppleIPAppender/PDP-manager services synchronously before launchd;
the full bridge, network interfaces and transport worker still start after physical HS2. This avoids the observed
20H392 ApplePDPHelper null singleton when CommCenter starts before the MD finishes booting. It adapts the imported iOS bridge to a local
QRTR/QMI stub: six services are announced and requests return `QMI_ERR_NOT_SUPPORTED` while logging their headers.
This is a discovery build, not working iOS cellular support. The PDP/DPMAIF data plane is not implemented.

Local changes to the imported bridge: public mbuf KPI include (see `qcom/pdp-mbuf-kpi.patch`), X9 `x9_bb` enable switch,
bounded handshake diagnostics, and pre-launchd helper publication with a reusable traffic-gate lock.
The upstream README describes Coachz/iOS 17, not X9/iOS 16 results. Its integration also supplies baseband DT metadata,
Apple firmware mounts and a patched postponement-ticket compatibility executable; these are not included here.
The latter cannot be copied under this project's original-Apple-byte constraint. X9's D22AP profile needs mav17,
not the reference J121AP/mav16 profile. Missing metadata/firmware and the remaining ABM startup gate are still pending.

`~/140.txt` lacks both the 128 MiB pool startup line and `x9_bb=1`; it repeats `~/138.txt`'s `FD27_003` write failure and
`lid_error_handle.c:259` assertion. It cannot validate the new bridge. `rev252el-md` adds a compiled startup marker plus
pool/node/handle failure counters to identify the executing image before changing another protocol variable.
