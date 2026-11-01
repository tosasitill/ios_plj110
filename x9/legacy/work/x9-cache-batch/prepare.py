from pathlib import Path
import json,shutil,subprocess,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan219-short-cache-waits';out=root/'x9/out/pan220-batched-cache-ranges'
out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:shutil.copyfile(base/n,out/n)
arm=out/'kernel-source/iokit/Kernel/arm';changes={}
p=arm/'X9RenderMemory.h';s=p.read_text();s+='''
/* Kernel-only batch. No user pointer or access declaration is accepted here.
 * Each span belongs to one retained resource, sorted and disjoint. */
struct X9RenderCacheSpan { uint64_t begin,end; unsigned resource; };
IOReturn x9_render_memory_cache_spans(X9RenderContext *,X9RenderResource *,
 const X9RenderCacheSpan *,unsigned,uint32_t,uint64_t *);
''';changes[p]=s
p=arm/'X9RenderCachePlan.inc';s=p.read_text();s=s.replace('struct RenderCacheSpan {\n uint64_t begin,end;\n unsigned resource;\n};','using RenderCacheSpan = X9RenderCacheSpan;')
a=s.index(' *bytes=0;\n for(unsigned i=p->first[phase][id]');s=s[:a]+''' unsigned first=p->first[phase][id],last=p->first[phase][id+1];
 if(first>last||last>p->count[phase])return kIOReturnBadArgument;
 return x9_render_memory_cache_spans(c,&r,p->spans[phase]+first,last-first,
  X9RenderClean|X9RenderInvalidate,bytes);
}
''';changes[p]=s
p=arm/'X9RenderMemory.cpp';s=p.read_text().replace('#include <kern/task.h>','#include <kern/task.h>\n#include <kern/cpu_data.h>');a=s.index('IOReturn x9_render_memory_cache(');b=s.index('uint64_t x9_render_memory_publish(',a)
previous=s[a:b];alias=previous[previous.index(' IOOptionBits options='):previous.index(' uint64_t begin=address+')]
alias=alias.replace('(address&(line-1))','(address&(X9_RENDER_CACHE_ATOM-1))')
# The persistent alias setup remains unchanged; all spans are validated first.
function='''IOReturn x9_render_memory_cache_spans(X9RenderContext *c,X9RenderResource *r,
 const X9RenderCacheSpan *spans,unsigned count,uint32_t operation,uint64_t *completed_bytes) {
 if(!c||!r||!r->memory||!r->prepared||!completed_bytes||count>X9_RENDER_MAX_MAPPINGS||
    (count&&!spans)||!operation||(operation&~3u))return kIOReturnBadArgument;
 *completed_bytes=0;
 if(c->disabled)return kIOReturnNotReady;if(c->exposed)return kIOReturnBusy;
 if(!count)return kIOReturnSuccess;
 uint64_t total=0,prior_end=0;
 for(unsigned i=0;i<count;i++) {
  const auto &span=spans[i];
  if(span.begin>=span.end||span.end>r->bytes||
     ((span.begin|span.end)&(X9_RENDER_CACHE_ATOM-1))||
     (i&&span.begin<prior_end))return kIOReturnBadArgument;
  total+=span.end-span.begin;prior_end=span.end;
 }
'''+alias+'''
 /* Adapted from UniXNU Adreno740 8638683: close each bounded run's DSB
  * on the same CPU as its DC instructions. Cacheability/coherency is NOT
  * inferred. One batch spans only one descriptor; aliases across resources
  * are never deduplicated. 64KiB cap bounds preemption-off latency. */
 unsigned index=0;uint64_t cursor=spans[0].begin,runs=0;
 while(index<count) {
  disable_preemption();
  uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
  uint64_t line=4ULL<<((ctr>>16)&15);
  if(line>X9_RENDER_CACHE_ATOM||X9_RENDER_CACHE_ATOM%line||(address&(line-1))) {
   enable_preemption();return kIOReturnNotAligned;
  }
  uint64_t budget=64u<<10;
  __asm__ volatile("dsb sy":::"memory");
  while(index<count&&budget) {
   uint64_t bytes=spans[index].end-cursor;
   if(bytes>budget)bytes=budget;
   uint64_t begin=address+cursor,end=begin+bytes;
#define X9_CACHE_LINES(instruction) do { \\
 uint64_t at=begin; \\
 for(;end-at>=4*line;at+=4*line) { \\
  __asm__ volatile(instruction ", %0\\n\\t" instruction ", %1\\n\\t" \\
                   instruction ", %2\\n\\t" instruction ", %3" \\
   ::"r"(at),"r"(at+line),"r"(at+2*line),"r"(at+3*line):"memory"); \\
 } \\
 for(;at<end;at+=line)__asm__ volatile(instruction ", %0"::"r"(at):"memory"); \\
} while(0)
   if(operation==(X9RenderClean|X9RenderInvalidate)) { X9_CACHE_LINES("dc civac"); }
   else if(operation==X9RenderClean) { X9_CACHE_LINES("dc cvac"); }
   else { X9_CACHE_LINES("dc ivac"); }
#undef X9_CACHE_LINES
   budget-=bytes;cursor+=bytes;
   if(cursor==spans[index].end&&++index<count)cursor=spans[index].begin;
  }
  __asm__ volatile("dsb sy":::"memory");
  enable_preemption();runs++;
 }
 if((operation&X9RenderClean)&&total==r->bytes&&spans[0].begin==0)
  r->initial_cache_clean=true;
 *completed_bytes=total;
 /* Existing owner/context serialization covers this telemetry. Count phase
  * barriers versus old one-pair-per-range behavior; runs may be GREATER for
  * large buffers because CPU migration safety takes priority. */
 static uint64_t batches=0,range_total=0,run_total=0;
 range_total+=count;run_total+=runs;
 if(++batches%8192==0)IOLog("X9 RENDER cache-batch: batches=%llu ranges=%llu bounded_runs=%llu;64KiB CPU-pinned runs,no cache bytes omitted\\n",batches,range_total,run_total);
 return kIOReturnSuccess;
}
IOReturn x9_render_memory_cache(X9RenderContext *c,X9RenderResource *r,const X9RenderCacheRequest *request) {
 if(!r||!request||request->abi!=X9_RENDER_ABI||request->handle!=r->handle||
    request->reserved||!request->bytes||request->offset>r->bytes||request->bytes>r->bytes-request->offset)
  return kIOReturnBadArgument;
 X9RenderCacheSpan span={request->offset,request->offset+request->bytes,0};uint64_t completed=0;
 return x9_render_memory_cache_spans(c,r,&span,1,request->operation,&completed);
}
'''
s=s[:a]+function+s[b:];changes[p]=s
patch=''
for p,s in changes.items():
 old=(base/'kernel-source/iokit/Kernel/arm'/p.name).read_text();p.write_text(s)
 patch+=''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/'+p.name,tofile='b/iokit/Kernel/arm/'+p.name))
 shutil.copyfile(p,out/'metal-source/kernel'/p.name)
(root/'xnu/patches/0271-x9-batched-bounded-cache-maintenance.patch').write_text(patch)
# Both callers of the kernel-only cache interface must see the same definition.
records=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
prior=root/'x9/out/pan215-mediaserverd-inactive';old=json.loads((prior/'commands.json').read_text().replace(str(prior),str(out)))
memory=next(r for r in old if any(a.endswith('/X9RenderMemory.cpp') for a in r['args']))
(out/'commands.json').write_text(json.dumps([records[0],memory]+records[-2:],indent=2)+'\n')
(out/'kernel-link.filelist').write_text((base/'kernel-link.filelist').read_text().replace(str(base),str(out)))
shutil.copyfile(Path(__file__),out/'prepare.py')
print(out)
