#!/usr/bin/env python3
"""Bound X9's fully preallocated native tiler arena to Mesa's initial heap.
No dynamic OOM growth or firmware acknowledgement is claimed by this backend.
"""
from pathlib import Path
import difflib
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
relative='src/panfrost/vulkan/panvk_physical_device.c'
p=w/'mesa-render-source'/relative
old=p.read_text()
marker='/* X9 fixed native tiler budget: no dynamic OOM growth. */'
if marker not in old:
    anchor='''      device->csf.tiler.initial_chunks = 5;
      device->csf.tiler.max_chunks = 64;'''
    replacement='''      device->csf.tiler.initial_chunks = 5;
#ifdef X9_NATIVE_KMOD
      /* X9 fixed native tiler budget: no dynamic OOM growth. */
      device->csf.tiler.max_chunks = device->csf.tiler.initial_chunks;
      mesa_logi("X9 native tiler fixed capacity=%u bytes (%u chunks); dynamic growth unavailable",
                device->csf.tiler.chunk_size * device->csf.tiler.max_chunks,
                device->csf.tiler.max_chunks);
#else
      device->csf.tiler.max_chunks = 64;
#endif'''
    if old.count(anchor)!=1:
        raise RuntimeError('tiler policy anchor changed')
    new=old.replace(anchor,replacement,1)
    (w/'panvk-native-fixed-tiler-budget.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
    p.write_text(new)
print('X9 native tiler advertises and allocates a fixed 10 MiB; Linux growth policy unchanged')
