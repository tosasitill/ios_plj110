from pathlib import Path
import json,shutil,subprocess,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan217-cache-publish';out=root/'x9/out/pan218-cache-plan-reuse'
out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:shutil.copyfile(base/n,out/n)
arm=out/'kernel-source/iokit/Kernel/arm';p=arm/'SweetX9RenderEngine.inc';old=p.read_text();s=old
s=s.replace(' RenderCachePlan cache_plan;',' RenderCachePlan cache_plan;\n bool cache_plan_valid;\n unsigned cache_plan_mappings;')
needle=' IOReturn kr=x9_render_memory_map(c,entry);';assert s.count(needle)==1
s=s.replace(needle,''' /* Invalidate before a mutation attempt, including failed transactions and
  * rollback. The service updates its mapping registry before releasing the
  * context lock; the next submit rebuilds from that completed state. */
 if(c&&c->engine_private)((RenderKernelContext *)c->engine_private)->cache_plan_valid=false;
'''+needle)
needle=' return x9_render_memory_unmap(c,entry);';assert s.count(needle)==1
s=s.replace(needle,''' if(c&&c->engine_private)((RenderKernelContext *)c->engine_private)->cache_plan_valid=false;
'''+needle)
needle=''' if(!render_cache_bounds(c)||!c->engine_private)return kIOReturnBadArgument;
 auto *plan=&((RenderKernelContext *)c->engine_private)->cache_plan;
 if(!render_cache_plan_build(c,plan))return kIOReturnBadArgument;'''
assert s.count(needle)==1
s=s.replace(needle,''' if(!c->engine_private||c->mapping_count>X9_RENDER_MAX_MAPPINGS)return kIOReturnBadArgument;
 auto *kernel_context=(RenderKernelContext *)c->engine_private;
 auto *plan=&kernel_context->cache_plan;
 bool plan_rebuilt=false;
 if(!kernel_context->cache_plan_valid||kernel_context->cache_plan_mappings!=c->mapping_count) {
  kernel_context->cache_plan_valid=false;
  if(!render_cache_bounds(c)||!render_cache_plan_build(c,plan))return kIOReturnBadArgument;
  kernel_context->cache_plan_mappings=c->mapping_count;
  kernel_context->cache_plan_valid=true;plan_rebuilt=true;
 }
 uint64_t holes_saved[2]={0,0};
 /* Map/unmap and this submit hold the context lock. A mapped resource cannot
  * be released/reused and resource permissions/backing never mutate in place.
  * CPU exposure can change without a map: evaluate it and perform all actual
  * maintenance below on EVERY submit. Cached plan is not a clean/dirty claim.
  * Private queue-storage mappings are maintained separately, unchanged. */''')
s=s.replace('   reach_saved[0]+=r.bytes-bytes;fallback_bytes+=bytes;','''   reach_saved[0]+=r.bytes-bytes;fallback_bytes+=bytes;
   uint64_t bound=r.cache_end>r.cache_begin?r.cache_end-r.cache_begin:0;
   if(bytes>bound)return kIOReturnBadArgument;
   holes_saved[0]+=bound-bytes;''')
s=s.replace('  reach_saved[1]+=r.bytes-bytes;','''  reach_saved[1]+=r.bytes-bytes;
  uint64_t bound=r.write_end>r.write_begin?r.write_end-r.write_begin:0;
  if(bytes>bound){c->disabled=true;resident->uncertain=true;return kIOReturnBadArgument;}
  holes_saved[1]+=bound-bytes;''')
s=s.replace('  static uint64_t hole_total[2]={0,0},span_total[2]={0,0};','  static uint64_t hole_total[2]={0,0},span_total[2]={0,0};\n  static uint64_t plan_rebuilds=0,plan_reuses=0;')
s=s.replace('  for(unsigned i=0;i<2;i++){hole_total[i]+=coherent?0:plan->holes[i];span_total[i]+=plan->count[i];}','''  if(plan_rebuilt)plan_rebuilds++;else plan_reuses++;
  for(unsigned i=0;i<2;i++){hole_total[i]+=holes_saved[i];span_total[i]+=plan->count[i];}''')
s=s.replace('   for(unsigned i=0;i<2;i++)hole_total[i]=span_total[i]=0;','''   IOLog("X9 RENDER cache-plan: builds=%llu reuses=%llu;mapping mutation invalidates,shared cache ops still per submission\\n",plan_rebuilds,plan_reuses);
   plan_rebuilds=plan_reuses=0;
   for(unsigned i=0;i<2;i++)hole_total[i]=span_total[i]=0;''')
s=s.replace(' * Rebuild both intervals on EVERY submit: unmap/remap, permission changes and\n * new CPU aliases cannot leave a stale smaller interval. No allocation or\n * resource-list-sized kernel stack array is needed here. */',''' * Rebuild after every map/unmap attempt, including same-count remaps and
 * rollback. Resource permissions/backing are immutable while mapped; CPU
 * exposure is evaluated at execution. No large kernel stack arrays. */''')
p.write_text(s)
patch=''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/SweetX9RenderEngine.inc',tofile='b/iokit/Kernel/arm/SweetX9RenderEngine.inc'))
(root/'xnu/patches/0269-x9-reuse-validated-cache-plan.patch').write_text(patch)
shutil.copyfile(p,out/'metal-source/kernel'/p.name)
prev=root/'x9/out/pan216-kernel-cache-manifest';records=json.loads((prev/'commands.json').read_text().replace(str(prev),str(out)))
csf=next(r for r in records if any(a.endswith('/SweetX9CSF.cpp') for a in r['args']))
current=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
(out/'commands.json').write_text(json.dumps([csf]+current[-2:],indent=2)+'\n')
(out/'kernel-link.filelist').write_text((base/'kernel-link.filelist').read_text().replace(str(base),str(out)))
shutil.copyfile(Path(__file__),out/'prepare.py')
print(out)
