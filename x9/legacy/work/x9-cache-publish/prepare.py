from pathlib import Path
import json,subprocess,shutil,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan216-kernel-cache-manifest';out=root/'x9/out/pan217-cache-publish'
out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:shutil.copyfile(base/n,out/n)
arm=out/'kernel-source/iokit/Kernel/arm';changed={}
p=arm/'X9RenderPageTable.cpp';s=p.read_text()
s=s.replace('uint64_t *entries,pa; };','uint64_t *entries,pa; bool dirty; };')
s=s.replace('vm->tables[index]={m,entries,pa};','vm->tables[index]={m,entries,pa,true};')
s=s.replace('   entry=vm->tables[child].pa|0x403ULL;','   entry=vm->tables[child].pa|0x403ULL;\n   vm->tables[table].dirty=true;')
s=s.replace(' return &vm->tables[table].entries[(va>>12)&511];',' /* Every caller which can write a leaf passes here, including rollback.\n  * Read-only preflight may conservatively dirty a page; steady submissions\n  * never call leaf(). CPU-only tables cannot be modified by imported aliases. */\n vm->tables[table].dirty=true;\n return &vm->tables[table].entries[(va>>12)&511];')
s=s.replace('bzero(&vm->tables[child],sizeof(Table));entry=0;','bzero(&vm->tables[child],sizeof(Table));entry=0;vm->tables[table].dirty=true;')
old=''' for(unsigned i=0;i<vm->count;i++)if(vm->tables[i].memory)clean_dcache64(vm->tables[i].pa,4096,1);
 __asm__ volatile("dsb sy":::"memory");vm->attached=true;return vm->tables[0].pa;'''
new=''' /* No cache-attribute/coherency assumption: skip only CPU-private page
  * tables which have not changed since their last completed clean. Parent
  * links, leaf writes, removals and rollbacks all retain dirty tracking.
  * Translation installation/invalidation and GPU detach are unchanged. */
 uint64_t cleaned=0,unchanged=0;
 for(unsigned i=0;i<vm->count;i++)if(vm->tables[i].memory) {
  if(vm->tables[i].dirty){clean_dcache64(vm->tables[i].pa,4096,1);cleaned+=4096;}
  else unchanged+=4096;
 }
 __asm__ volatile("dsb sy":::"memory");
 for(unsigned i=0;i<vm->count;i++)if(vm->tables[i].memory)vm->tables[i].dirty=false;
 static uint64_t calls=0,clean_total=0,skip_total=0;
 clean_total+=cleaned;skip_total+=unchanged;
 if(++calls%512==0) {
  IOLog("X9 RENDER table-cache: calls=%llu cleaned_avg=%llu unchanged_skip_avg=%llu;CPU-private tables only,TLB/AS protocol unchanged\\n",calls,clean_total/calls,skip_total/calls);
 }
 vm->attached=true;return vm->tables[0].pa;'''
assert s.count(old)==1;s=s.replace(old,new);changed[p]=s
p=arm/'X9RenderMemory.cpp';s=p.read_text();start=s.index(' for(uint64_t at=begin;at<end;at+=line) {');end=s.index('\n __asm__ volatile("dsb sy"',start)
s=s[:start]+''' /* Choose the cache opcode once, then issue four independent lines per
  * loop. Both paths cover exactly [begin,end); no prefetch or cache op may
  * cross an allocation boundary. Keep DSB SY on both sides for all callers. */
#define X9_CACHE_LINES(instruction) do { \\
 uint64_t at=begin; \\
 for(;end-at>=4*line;at+=4*line) { \\
  __asm__ volatile(instruction ", %0\\n\\t" instruction ", %1\\n\\t" \\
                   instruction ", %2\\n\\t" instruction ", %3" \\
   ::"r"(at),"r"(at+line),"r"(at+2*line),"r"(at+3*line):"memory"); \\
 } \\
 for(;at<end;at+=line)__asm__ volatile(instruction ", %0"::"r"(at):"memory"); \\
} while(0)
 if(request->operation==(X9RenderClean|X9RenderInvalidate)) { X9_CACHE_LINES("dc civac"); }
 else if(request->operation==X9RenderClean) { X9_CACHE_LINES("dc cvac"); }
 else { X9_CACHE_LINES("dc ivac"); }
#undef X9_CACHE_LINES'''+s[end:];changed[p]=s
patch=''
for p,s in changed.items():
 old=(base/'kernel-source/iokit/Kernel/arm'/p.name).read_text();assert old!=s;p.write_text(s)
 patch+=''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/'+p.name,tofile='b/iokit/Kernel/arm/'+p.name))
 shutil.copyfile(p,out/'metal-source/kernel'/p.name)
(root/'xnu/patches/0268-x9-dirty-page-tables-and-cache-loop.patch').write_text(patch)
previous=root/'x9/out/pan215-mediaserverd-inactive';records=json.loads((previous/'commands.json').read_text().replace(str(previous),str(out)))
selected=[]
for name in ['X9RenderMemory.cpp','X9RenderPageTable.cpp']:
 matches=[r for r in records if any(a.endswith('/'+name) for a in r['args'])];assert len(matches)==1;selected+=matches
# Keep pan216 CSF object and every inherited object; rebuild just changed units.
current=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
(out/'commands.json').write_text(json.dumps(selected+current[-2:],indent=2)+'\n')
(out/'kernel-link.filelist').write_text((base/'kernel-link.filelist').read_text().replace(str(base),str(out)))
shutil.copyfile(Path(__file__),out/'prepare.py')
print(out)
