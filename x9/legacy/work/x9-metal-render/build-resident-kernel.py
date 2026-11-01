#!/usr/bin/env python3
"""Derive resident lifecycle inputs from frozen pan168 without modifying it.
This compiles lifecycle primitives, not a complete render engine/candidate.
"""
from pathlib import Path
import json,subprocess,difflib
w=Path(__file__).resolve().parent;root=w.parents[1]
base=root/'x9/out/pan168-desktop-workgroup-xy'
subprocess.run(['python3',str(w/'build-render-cs.py')],check=True)
maintenance=root/'MaliMetal/X9/General/kernel'
out=w/'resident-kernel-inputs';out.mkdir(exist_ok=True)
rel='iokit/Kernel/arm/SweetX9CSF.cpp';original=(base/'kernel-source'/rel).read_text();s=original
s=s.replace('#include "X9Desktop.h"','#include "X9Desktop.h"\n#include "X9CSFResident.h"\n#include "X9RenderMemory.h"')
anchor='\n}\nextern "C" void sweet_x9_csf_prepare(void)'
if s.count(anchor)!=1:raise RuntimeError('CSF namespace anchor changed')
s=s.replace(anchor,'\n#include "SweetX9CSFResident.inc"\n#include "SweetX9RenderEngine.inc"\n'+anchor)
s+='''
extern "C" int sweet_x9_csf_resident_begin(vm_offset_t gpu) {return resident_begin(gpu);}
extern "C" int sweet_x9_csf_resident_end(void) {return resident_end();}
extern "C" bool sweet_x9_csf_resident_caps(X9RenderCaps *caps) {
 if(!caps||!resident||!resident->ready||resident->uncertain)return false;
 *caps=resident->caps;return true;
}
extern "C" X9CSFResidentSlot *sweet_x9_csf_resident_slot(unsigned index) {
 if(!resident||!resident->ready||resident->uncertain||index>=resident->slot_count||resident->slots[index].borrowed)return nullptr;
 resident->slots[index].borrowed=true;return &resident->slots[index];
}
'''
s+='''
extern "C" bool sweet_x9_csf_resident_release_slot(unsigned index,uint64_t epoch) {
 if(!resident||!resident->ready||resident->uncertain||index>=resident->slot_count||resident->epoch!=epoch||
    !resident->slots[index].borrowed||gr64(resident->gpu,0x2440))return false;
 const auto &f=resident->slots[index].access.interface;
 if((input_word(resident->image,f.group_input)|output_word(resident->image,f.group_output))&7u)return false;
 for(unsigned i=0;i<3;i++)if((input_word(resident->image,f.streams[i].input)|output_word(resident->image,f.streams[i].output))&7u)return false;
 if(!no_faults(resident->gpu)||!flush_gpu(resident->gpu)){resident->uncertain=true;return false;}
 unsigned char *io=cpu_address(resident->image,resident->slots[index].user_io,0x10000);
 if(!io){resident->uncertain=true;return false;}
 bzero(io,0x10000);
 for(unsigned off=0;off<0x10000;off+=gpu_page)
  if(!clean_page(resident->image,resident->slots[index].user_io+off)){resident->uncertain=true;return false;}
 resident->slots[index].borrowed=false;return true;
}
'''
s+='''
extern "C" bool sweet_x9_render_owner_enter(void) {
 if(!metal_service_lock)return false;IOLockLock(metal_service_lock);return true;
}
extern "C" void sweet_x9_render_owner_leave(void) {IOLockUnlock(metal_service_lock);}
extern "C" void sweet_x9_render_bootstrap(void) {
 int opt=0;
 if(!PE_parse_boot_argn("x9_metal_render",&opt,sizeof(opt))||opt!=1||render_engine_installed)return;
 /* The render flag replaces the legacy compute flag. Bootstrap cannot
  * depend on compute publication to allocate the shared owner lock. This is
  * the sole startup worker; no service is published before allocation. */
 if(!metal_service_lock)metal_service_lock=IOLockAlloc();
 if(!sweet_x9_render_owner_enter())return;
 int result=sweet_x9_gpu_render_begin();
 if(result==2)render_engine_installed=true;
 sweet_x9_render_owner_leave();
 if(result==2) {
  sweet_x9_render_install(&resident_engine);sweet_x9_render_publish();
  X9RenderCaps caps={};
  if(render_caps(&caps))IOLog("X9 RENDER CAPS: abi=%u capabilities=0x%x streams=%u cache-atom=%u;no legacy compute publication\\n",
   caps.abi,caps.capabilities,caps.max_streams,X9_RENDER_CACHE_ATOM);
  IOLog("X9 RENDER: resident multi-stream engine callbacks installed;experimental opt-in,no hardware rendering claim\\n");
 }else IOLog("X9 RENDER: resident engine unavailable result=%d;no render service published\\n",result);
}
'''
cpp=out/'SweetX9CSF.cpp';cpp.write_text(s)
patch=list(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel))
rel_c='bsd/dev/sweet_x9_gpu_dependencies.c';original_c=(base/'kernel-source'/rel_c).read_text();c=original_c
c=c.replace(' if(id_ok)sweet_x9_compute_publish();',''' if(id_ok) {
  int render=0;
  if(PE_parse_boot_argn("x9_metal_render",&render,sizeof(render))&&render==1)sweet_x9_render_bootstrap();
  else sweet_x9_compute_publish();
 }''')
c=c.replace('static int gpu_owned_request(void *job,int desktop) {','''extern int sweet_x9_csf_resident_begin(vm_offset_t);
extern int sweet_x9_csf_resident_end(void);
static int render_active_refs=-1;
static int gpu_owned_request(void *job,int desktop) {''')
a=' int result=desktop?sweet_x9_csf_desktop(gpu,job):sweet_x9_csf_user(gpu,job);'
b=''' int result=desktop==2?sweet_x9_csf_resident_begin(gpu):
  (desktop?sweet_x9_csf_desktop(gpu,job):sweet_x9_csf_user(gpu,job));
 if(desktop==2&&result==2) {
  render_active_refs=active;
  printf("X9 RENDER: resident session owns GPUEB power1/active%d;no per-request shutdown\\n",active);
  return 2;
 }'''
if c.count(a)!=1:raise RuntimeError('GPUEB submit anchor changed')
c=c.replace(a,b)
c+='''
/* Caller holds the shared GPU owner lock; begin/end are not root RPCs. */
int sweet_x9_gpu_render_begin(void) {
 static int token;
 if(render_active_refs>=0)return -1;
 return gpu_owned_request(&token,2);
}
int sweet_x9_gpu_render_end(void) {
 if(render_active_refs<0||channel_uncertain||sweet_x9_csf_resident_end()!=0)return -1;
 int32_t count=0;
 if(render_active_refs&&(power_command(CMD_ACTIVE,0,&count)||count!=0))return -1;
 if(power_command(CMD_POWER,0,&count)||count!=0||wait_counts(0,0,0))return -1;
 render_active_refs=-1;return 1;
}
'''
c='extern void sweet_x9_render_bootstrap(void);\n'+c
csource=out/'sweet_x9_gpu_dependencies.c';csource.write_text(c)
patch.extend(difflib.unified_diff(original_c.splitlines(True),c.splitlines(True),fromfile='a/'+rel_c,tofile='b/'+rel_c))
(w/'resident-lifecycle-integration.patch').write_text(''.join(patch))
commands=json.loads((base/'commands.json').read_text());records=[]
for source,frozen in [(cpp,'/SweetX9CSF.cpp'),(csource,'/sweet_x9_gpu_dependencies.c')]:
 original_command=next(x for x in commands if any(a.endswith(frozen) for a in x['args']))
 args=list(original_command['args']);args[0]='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/'+('clang++' if source.suffix=='.cpp' else 'clang')
 for flag,suffix in [('-o','.o'),('-MF','.d')]:args[args.index(flag)+1]=str(out/(source.stem+suffix))
 args=[str(source) if a.endswith(frozen) else a for a in args]
 args[1:1]=['-I'+str(maintenance),'-I'+str(maintenance.parent),'-I'+str(w/'render-kernel-generated')]
 records.append({'args':args,'cwd':original_command['cwd']})
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
for record in records:subprocess.run(record['args'],cwd=record['cwd'],check=True)
print('Resident CSF/GPUEB lifecycle sources compiled; not linked to a boot candidate or published as renderer.',flush=True)
