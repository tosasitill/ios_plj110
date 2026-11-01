#!/usr/bin/env python3
"""CPU-only 3814-extent microbenchmark with modeled UFS, not hardware throughput."""
import importlib.util, ctypes as C, time, json, argparse
from pathlib import Path
r=Path('/Users/tosasitill/HelioDarwin-x9-lk-preserve')
s=importlib.util.spec_from_file_location('ufs_tests',r/'x9/tools/test_ufs_ro.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
t=m.UFSTests('test_extent_image_reads_cross_nonadjacent_runs_with_bounds');t.setUpClass()
try:
 t.setUp();disk=bytes(range(256))*8192;t.put_disk(disk,blocks=10000);t.opened();assert t.lib.test_capacity(0)==0
 n=3814;p=m.Partition(1,5000);mapping=(m.Extent*n)(*[m.Extent(i,10+(i*1543)%n,1) for i in range(n)])
 assert t.lib.x9u_extent_validate(mapping,n,n,5000)==0
 out=C.create_string_buffer(4096);results={};iterations=1000
 for name,fn in [('validate_each_read',t.lib.test_image),('validated_map_read',t.lib.test_image_mapped)]:
  start=time.perf_counter_ns()
  for _ in range(iterations):assert fn(0,C.byref(p),mapping,n,n,0,out,4096)==0
  elapsed=time.perf_counter_ns()-start;assert out.raw==disk[11*4096:12*4096]
  results[name]={'calls':iterations,'elapsed_ns':elapsed,'ns_per_read':elapsed/iterations}
 results.update({'scope':'CPU only, synthetic3814-extent map, modeled device completions/cache synchronization; not real UFS or boot throughput','copyout_bytes_correct':True,'previous_sync_bytes_per_4KiB_read':163840,'current_sync_bytes_per_4KiB_read':9808})
 a.output.write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
finally:t.tearDownClass()
