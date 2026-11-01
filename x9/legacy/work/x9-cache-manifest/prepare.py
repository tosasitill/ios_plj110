from pathlib import Path
import json,shutil,subprocess,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan215-mediaserverd-inactive';out=root/'x9/out/pan216-kernel-cache-manifest'
out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:shutil.copyfile(base/n,out/n)
arm=out/'kernel-source/iokit/Kernel/arm';p=arm/'SweetX9RenderEngine.inc';old=p.read_text();s=old
s=s.replace('struct RenderKernelContext {','#include "X9RenderCachePlan.inc"\nstruct RenderKernelContext {\n RenderCachePlan cache_plan;')
s=s.replace(' if(!render_cache_bounds(c))return kIOReturnBadArgument;',' if(!render_cache_bounds(c)||!c->engine_private)return kIOReturnBadArgument;\n auto *plan=&((RenderKernelContext *)c->engine_private)->cache_plan;\n if(!render_cache_plan_build(c,plan))return kIOReturnBadArgument;')
s=s.replace("mappings' bounding interval (holes included).", "mappings' exact interval union (holes excluded).")
s=s.replace('  * All CPU-readable/writable client resources have exclusive GPU ownership\n  * during this synchronous submit. No HOST_COHERENT capability is implied.', '  * The context is serialized here, but retained CPU pointers and external\n  * IOSurface aliases are NOT write-protected by that lock. They remain shared\n  * and always synchronized; no exclusive ownership or HOST_COHERENT capability\n  * is inferred from a flush or from user-provided access declarations.')
a=s.index('  uint64_t offset=0,bytes=r.bytes;');b=s.index('\n }\n times[2]',a)
s=s[:a]+'''  uint64_t bytes=0;
  IOReturn kr;
  if(r.cpu_exposed) {
   kr=render_cache_plan_resource(c,plan,r,0,&bytes);
   if(kr!=kIOReturnSuccess)return kr;
   reach_saved[0]+=r.bytes-bytes;fallback_bytes+=bytes;
   if(r.imported){import_bytes[0]+=bytes;full_import_bytes[0]+=r.bytes;}
  }else {
   bytes=r.bytes;initial_bytes+=bytes;
   if(!bytes)continue;
   X9RenderCacheRequest cache={X9_RENDER_ABI,r.handle,X9RenderClean|X9RenderInvalidate,0,0,bytes};
   kr=x9_render_memory_cache(c,&r,&cache);if(kr!=kIOReturnSuccess)return kr;
  }
  cache_bytes[0]+=bytes;'''+s[b:]
a=s.index('  uint64_t offset=r.write_begin,bytes=');b=s.index('\n }\n times[7]',a)
s=s[:a]+'''  uint64_t bytes=0;
  IOReturn kr=render_cache_plan_resource(c,plan,r,1,&bytes);
  if(kr!=kIOReturnSuccess){c->disabled=true;resident->uncertain=true;return kr;}
  reach_saved[1]+=r.bytes-bytes;
  if(r.imported){import_bytes[1]+=bytes;full_import_bytes[1]+=r.bytes;}
  cache_bytes[1]+=bytes;'''+s[b:]
s=s.replace('  static uint64_t coherent_total[2]={0,0};','  static uint64_t coherent_total[2]={0,0};\n  static uint64_t hole_total[2]={0,0},span_total[2]={0,0};')
s=s.replace('  count++;absolutetime_to_nanoseconds','  for(unsigned i=0;i<2;i++){hole_total[i]+=coherent?0:plan->holes[i];span_total[i]+=plan->count[i];}\n  count++;absolutetime_to_nanoseconds')
s=s.replace('   for(unsigned i=0;i<2;i++)reach_total', '''   IOLog("X9 RENDER cache-manifest: jobs=%llu pre_mapped_spans_avg=%llu post_mapped_spans_avg=%llu pre_holes_saved_avg=%llu post_holes_saved_avg=%llu;all mapped indirect targets included,shared aliases retained\\n",
    count,span_total[0]/count,span_total[1]/count,hole_total[0]/count,hole_total[1]/count);
   for(unsigned i=0;i<2;i++)hole_total[i]=span_total[i]=0;
   for(unsigned i=0;i<2;i++)reach_total''')
s=s.replace('full inside all GPU mapping bounds,indirect included','exact union of all GPU mappings,indirect included')
p.write_text(s)
helper=(Path(__file__).parent/'X9RenderCachePlan.inc').read_text();(arm/'X9RenderCachePlan.inc').write_text(helper)
patch=''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/SweetX9RenderEngine.inc',tofile='b/iokit/Kernel/arm/SweetX9RenderEngine.inc'))
patch+=''.join(difflib.unified_diff([],helper.splitlines(True),fromfile='/dev/null',tofile='b/iokit/Kernel/arm/X9RenderCachePlan.inc'))
(root/'xnu/patches/0267-x9-kernel-owned-cache-manifest.patch').write_text(patch)
for n in ['SweetX9RenderEngine.inc','X9RenderCachePlan.inc']:
 shutil.copyfile(arm/n,out/'metal-source/kernel'/n)
records=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
compile_csf=next(r for r in records if any(a.endswith('/SweetX9CSF.cpp') for a in r['args']))
(out/'commands.json').write_text(json.dumps([compile_csf]+records[-2:],indent=2)+'\n')
(out/'kernel-link.filelist').write_text((base/'kernel-link.filelist').read_text().replace(str(base),str(out)))
shutil.copyfile(Path(__file__),out/'prepare.py')
print(out)
