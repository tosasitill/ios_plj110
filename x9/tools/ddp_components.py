#!/usr/bin/env python3
"""DDP_COMPONENT enum for MT6993 mediatek-drm (decoded from the vendor module).

The vendor ``mediatek-drm.ko`` (vendor_boot ramdisk) carries a 999-entry
name-pointer array in enum order.  Decoded offline from the module's
relocation addends; stored here as ``ddp_components.json`` so display work
does not depend on the unpacked module.

Usage:
    python3 x9/tools/ddp_components.py 652        # id -> name
    python3 x9/tools/ddp_components.py DSI0       # name -> id
    python3 x9/tools/ddp_components.py            # dump all
"""
import json
import sys
from pathlib import Path

TABLE = json.loads((Path(__file__).parent / "ddp_components.json").read_text())


def main(argv):
    if not argv:
        for k in sorted(TABLE, key=int):
            print(k, TABLE[k])
        return
    q = argv[0]
    if q.isdigit():
        print(TABLE.get(q, "?"))
    else:
        hits = [k for k, v in TABLE.items() if v == q.upper()]
        print(" ".join(hits) if hits else "?")


if __name__ == "__main__":
    main(sys.argv[1:])
