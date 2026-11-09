# X9 baseband bridge (iOS side)

`qcom/` is an import of UniXNU/QcomBaseband (BSD-2-Clause, HoshinoYumeka / chiba233; upstream commit recorded in
`qcom/UPSTREAM.json`). Only `baseband/` (AppleBaseband IOKit services, QMUX/QRTR bridge, SAHARA stub, PDP interfaces, eSIM Tape)
is used. `ipa/` and `platform/coachz` (Qualcomm IPA/GSI) are not copied: the X9 modem is MediaTek, its data plane is DPMAIF.

Status (2026-11-09): the MT6993 CCCI file/RPC service reaches HS2/READY, SIM READY and network registration in `~/139.txt`.
NV partitions are read-only; modem writes go into a 128 MiB volatile overlay. Reboot discards those writes.

`X9Baseband.cpp` is linked into the kernel and starts after HS2 when `x9_bb=1`. It adapts the imported iOS bridge to a local
QRTR/QMI stub: six services are announced and requests return `QMI_ERR_NOT_SUPPORTED` while logging their headers.
This is a discovery build, not working iOS cellular support. The PDP/DPMAIF data plane is not implemented.

Local changes to the imported bridge: public mbuf KPI include (see `qcom/pdp-mbuf-kpi.patch`) and X9 `x9_bb` enable switch
in `coachz_baseband.cpp`. The upstream README describes Coachz/iOS 17, not X9/iOS 16 results.

`~/140.txt` lacks both the 128 MiB pool startup line and `x9_bb=1`; it repeats `~/138.txt`'s `FD27_003` write failure and
`lid_error_handle.c:259` assertion. It cannot validate the new bridge. `rev252el-md` adds a compiled startup marker plus
pool/node/handle failure counters to identify the executing image before changing another protocol variable.
