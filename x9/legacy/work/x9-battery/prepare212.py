from pathlib import Path
import json,subprocess,shutil,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan211-battery-id';out=root/'x9/out/pan212-cached-import-map';out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:
 shutil.copyfile(base/n,out/n)
arm=out/'kernel-source/iokit/Kernel/arm';orig=base/'kernel-source/iokit/Kernel/arm'; changed={}
p=arm/'X9RenderKernel.h';s=p.read_text();s=s.replace(' uint64_t cache_begin,cache_end,write_begin,write_end;',' uint64_t cache_begin,cache_end,write_begin,write_end;\n /* Kernel alias retained only while this prepared import remains alive. */\n IOMemoryMap *cache_mapping;');changed[p]=s
p=arm/'X9RenderMemory.cpp';s=p.read_text();s=s.replace('  mapping=r->memory->createMappingInTask(kernel_task,0,options);\n  if(!mapping)return kIOReturnNoMemory;','  mapping=r->cache_mapping;\n  if(!mapping) {\n   mapping=r->memory->createMappingInTask(kernel_task,0,options);\n   if(!mapping)return kIOReturnNoMemory;\n   r->cache_mapping=mapping;\n  }');s=s.replace('  if(mapping)mapping->release();return kIOReturnBadArgument;','  if(mapping){mapping->release();r->cache_mapping=nullptr;}return kIOReturnBadArgument;');s=s.replace(' if(mapping)mapping->release();return kIOReturnSuccess;',' return kIOReturnSuccess;');changed[p]=s
p=arm/'SweetX9RenderService.cpp';s=p.read_text();s=s.replace('   r->memory->release();context->allocated_bytes-=r->bytes;', '   if(r->cache_mapping){r->cache_mapping->release();r->cache_mapping=nullptr;}\n   r->memory->release();context->allocated_bytes-=r->bytes;');s=s.replace('for(auto &r:context->resources)if(r.memory){if(r.prepared)r.memory->complete(r.memory->getDirection());r.memory->release();}', 'for(auto &r:context->resources)if(r.memory){if(r.cache_mapping){r.cache_mapping->release();r.cache_mapping=nullptr;}if(r.prepared)r.memory->complete(r.memory->getDirection());r.memory->release();}')
# release alias before unpin on the explicit free path as well
s=s.replace('   if(r->prepared)r->memory->complete(r->memory->getDirection());\n   if(r->cache_mapping){r->cache_mapping->release();r->cache_mapping=nullptr;}','   if(r->cache_mapping){r->cache_mapping->release();r->cache_mapping=nullptr;}\n   if(r->prepared)r->memory->complete(r->memory->getDirection());')
changed[p]=s
patch=''
for p,s in changed.items():
 old=(orig/p.name).read_text();assert old!=s;p.write_text(s);patch+=''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/'+p.name,tofile='b/iokit/Kernel/arm/'+p.name))
(root/'xnu/patches/0263-x9-cache-import-kernel-mapping.patch').write_text(patch)
oldroot=str(root/'x9/out/pan209-gpu-coherent-skip');records=json.loads((root/'x9/out/pan209-gpu-coherent-skip/commands.json').read_text().replace(oldroot,str(out)))
selected=[]
for name in ['SweetX9CSF.cpp','SweetX9RenderService.cpp','X9RenderMemory.cpp','X9RenderPageTable.cpp','X9CSFQueueAccess.cpp','X9CSFRenderQueue.cpp']:
 matches=[r for r in records if any(a.endswith('/'+name) for a in r['args'])]
 assert len(matches)==1,(name,len(matches));selected+=matches
current=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
(out/'commands.json').write_text(json.dumps(selected+current[-2:],indent=2)+'\n')
(out/'kernel-link.filelist').write_text((base/'kernel-link.filelist').read_text().replace(str(base),str(out)))
print(out)
