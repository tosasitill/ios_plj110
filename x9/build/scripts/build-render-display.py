#!/usr/bin/env python3
"""Prepare/compile opt-in RAM default surface and existing LK presentation.
No partition writes, deployment or runtime validation. Base source identities
follow pan168's link filelist, including its pan117 SweetSurface object.
"""
from pathlib import Path
import json,difflib,subprocess,os
w=Path(__file__).resolve().parent;root=w.parents[1]
base=root/'x9/out/pan168-desktop-workgroup-xy'
surface_root=root/'x9/out/pan117-x9-eight-cpu/kernel-source/iokit/Kernel/arm'
out=w/'render-display-inputs';out.mkdir(exist_ok=True)
canonical=Path('/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb/xnu/iokit/Kernel/arm')
header_source=surface_root/'SweetSurface.h'
if not header_source.exists():header_source=canonical/'SweetSurface.h'
header=header_source.read_text();before_h=header
anchor='mach_port_name_t sweet_surface_port_in_task(SweetSurface *surface, task_t task);'
if header.count(anchor)!=1:raise RuntimeError('SweetSurface declaration anchor changed')
header=header.replace(anchor,anchor+'\n\n/* Owned RAM surface for the opt-in X9 render target; never a panel alias. */\nSweetSurface *sweet_surface_create_buffer(uint32_t width, uint32_t height,\n    uint32_t bytesPerRow, uint32_t pixelFormat);')
(out/'SweetSurface.h').write_text(header)
s=(surface_root/'SweetSurface.cpp').read_text();before_s=s
anchor='mach_port_name_t\nsweet_surface_port_in_task'
if s.count(anchor)!=1:raise RuntimeError('surface creator anchor changed')
creator='''SweetSurface *
sweet_surface_create_buffer(uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat)
{
	if (!width || !height || width > UINT32_MAX / 4 || bytesPerRow < width * 4)
		return NULL;
	uint64_t used = (uint64_t)bytesPerRow * height;
	if (!used || used > (128ULL << 20) - page_size)
		return NULL;
	SweetSurface *s = sweet_surface_alloc();
	if (!s) return NULL;
	s->width = width; s->height = height; s->bytesPerRow = bytesPerRow;
	s->bytesPerElement = 4; s->elementWidth = s->elementHeight = 1;
	s->pixelFormat = pixelFormat; s->allocSize = (uint32_t)round_page(used);
	IOBufferMemoryDescriptor *memory = NULL;
	if ((uint64_t)OSAddAtomic64(s->allocSize, (volatile SInt64 *)&sweet_surface_wired) +
	    s->allocSize <= SWEET_SURFACE_WIRED_MAX) {
		memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared, s->allocSize, page_size);
	}
	if (!memory) {
		OSAddAtomic64(-(SInt64)s->allocSize, (volatile SInt64 *)&sweet_surface_wired);
		s->release(); return NULL;
	}
	s->memory = memory; s->mapCache = kIOMapDefaultCache;
	if (!memory->getBytesNoCopy()) { s->release(); return NULL; }
	bzero(memory->getBytesNoCopy(), s->allocSize);
	if (!sweet_surface_publish(s)) { s->release(); return NULL; }
	return s;
}

'''
s=s.replace(anchor,creator+anchor);(out/'SweetSurface.cpp').write_text(s)
f=(base/'kernel-source/iokit/Kernel/arm/SweetFramebuffer.cpp').read_text();before_f=f
anchor='\tfPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;'
block='''
	int x9_render = 0;
	if (sweet_board_x9() && PE_parse_boot_argn("x9_metal_render", &x9_render, sizeof(x9_render)) &&
	    x9_render == 1 && fSurface != NULL && fPixels != 0) {
		SweetSurface *target = sweet_surface_create_buffer(fWidth, fHeight, fRowBytes, SWEET_FB_FORMAT_BGRA);
		uintptr_t targetPixels = target != NULL ? target->kernelAddress() : 0;
		if (targetPixels != 0) {
			memcpy((void *)targetPixels, (const void *)fPixels, (size_t)fRowBytes * fHeight);
			/* The published physical surface remains retained by the ID table,
			 * keeping the panel CPU mapping alive; fPixels remains scanout. */
			fSurface->release(); fSurface = target;
			IOLog("X9 RENDER DISPLAY: RAM default surface %u; panel copied through existing LK path, no GPU panel mapping\\n", fSurface->id);
		} else {
			OSSafeReleaseNULL(target);
			IOLog("X9 RENDER DISPLAY: RAM surface allocation failed; framebuffer setup refused\\n");
			fPixels = 0;
		}
	}
'''
if f.count(anchor)!=1:raise RuntimeError('default framebuffer anchor changed')
f=f.replace(anchor,anchor+block)
anchor='\t\tif (s == NULL || s == fSurface) {'
if f.count(anchor)!=1:raise RuntimeError('default surface skip anchor changed')
f=f.replace(anchor,'\t\tif (s == NULL || (s == fSurface && s->kernelAddress() == fPixels)) {')
# Native Metal discovery is selected only after this boot's guarded payload
# installation and real native render service registration. Renderer owners
# are intentionally included: this is the full frontend, not compute-only.
f=f.replace('extern "C" bool sweet_x9_metal_discovery_ready(void);',
 'extern "C" bool sweet_x9_metal_discovery_ready(void);\nextern "C" bool sweet_x9_render_discovery_ready(void);')
key=' virtual OSPtr<OSObject> copyProperty(const char *key) const APPLE_KEXT_OVERRIDE {'
if f.count(key)!=1:raise RuntimeError('render discovery property anchor changed')
f=f.replace(key,key+'''
  if(key&&sweet_board_x9()&&(!strcmp(key,"MetalPluginName")||!strcmp(key,"MetalPluginClassName"))&&
     sweet_x9_render_discovery_ready()) {
   char name[32]={0};proc_selfname(name,sizeof(name));
   IOLog("X9 RENDER DISCOVERY: caller=%s property=%s native desktop Metal\\n",name,key);
   return OSString::withCString(!strcmp(key,"MetalPluginName")?"X9GeneralMetal%s":"X9GeneralMetalFactory");
  }
''')
f=f.replace('X9GeneralMetal%s','X9GeneralMetal'+os.environ.get('X9_RENDER_REVISION','170'))
(out/'SweetFramebuffer.cpp').write_text(f)
patch=[]
for name,old,new in [('SweetSurface.h',before_h,header),('SweetSurface.cpp',before_s,s),('SweetFramebuffer.cpp',before_f,f)]:
 rel='iokit/Kernel/arm/'+name
 patch.extend(difflib.unified_diff(old.splitlines(True),new.splitlines(True),fromfile='a/'+rel,tofile='b/'+rel))
(w/'native-default-surface-display.patch').write_text(''.join(patch))
commands=[]
for name,metadata in [('SweetSurface',root/'x9/out/pan141-identity-display/commands.json'),('SweetFramebuffer',base/'commands.json')]:
 entries=json.loads(metadata.read_text());entry=next(e for e in entries if any(a.endswith('/'+name+'.cpp') for a in e['args']))
 args=list(entry['args']);args[0]='/Users/tosasitill/Downloads/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++'
 for flag,suffix in [('-o','.o'),('-MF','.d')]:args[args.index(flag)+1]=str(out/(name+suffix))
 args=[str(out/(name+'.cpp')) if a.endswith('/'+name+'.cpp') else a for a in args]
 args[1:1]=['-I'+str(out)]
 commands.append({'args':args,'cwd':entry['cwd']})
(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
for entry in commands:subprocess.run(entry['args'],cwd=entry['cwd'],check=True)
print('Opt-in RAM default IOSurface and LK copy presentation compiled; not linked to a candidate or hardware-validated.',flush=True)
