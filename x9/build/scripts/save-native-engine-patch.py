#!/usr/bin/env python3
"""Save the renderer-engine delta against the retained numbered patch chain."""
from pathlib import Path
import re,difflib
w=Path(__file__).resolve().parent;root=w.parents[1];kernel=root/'MaliMetal/X9/General/kernel'
def sections(text):
 lines=text.splitlines(True);result={};i=0
 while i<len(lines):
  if not lines[i].startswith('--- '):i+=1;continue
  old=lines[i][4:].strip();new=lines[i+1][4:].strip();i+=2;part=[]
  while i<len(lines) and not lines[i].startswith(('--- ','diff --git ')):
   part.append(lines[i]);i+=1
  result[new[2:]]=(old,part)
 return result

def apply(text,part):
 source=text.splitlines(True);out=[];at=0;i=0
 while i<len(part):
  line=part[i]
  if not line.startswith('@@ '):i+=1;continue
  match=re.match(r'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@',line)
  if not match:raise RuntimeError('invalid source hunk')
  begin=int(match.group(1))-1
  out.extend(source[at:begin]);at=begin;i+=1
  while i<len(part) and not part[i].startswith('@@ '):
   line=part[i];i+=1
   if line.startswith('\\'):continue
   if line.startswith(('+','-',' ')):
    if line[0]!='+':
     if at>=len(source) or source[at]!=line[1:]:raise RuntimeError('numbered source baseline differs')
     if line[0]==' ':out.append(source[at])
     at+=1
    else:out.append(line[1:])
  # next source hunk
 out.extend(source[at:]);return ''.join(out)
patchdir=root/'xnu/patches';baseline={}
for number in ['0243','0244','0247']:
 p=next(patchdir.glob(number+'-*.patch'))
 for path,(old,part) in sections(p.read_text()).items():
  if old=='/dev/null':baseline[path]=''.join(line[1:] for line in part if line.startswith('+'))
  elif path in baseline:baseline[path]=apply(baseline[path],part)
  elif path.endswith('SweetX9CSF.cpp') or path.endswith('sweet_x9_gpu_dependencies.c'):
   before=(root/'x9/out/pan168-desktop-workgroup-xy/kernel-source'/path).read_text()
   baseline[path]=apply(before,part)
current={}
for name in ['X9RenderPageTable.h','X9RenderPageTable.cpp','X9CSFResident.h','X9CSFRenderQueue.cpp','SweetX9RenderEngine.inc']:
 current['iokit/Kernel/arm/'+name]=(kernel/name).read_text()
for path in ['iokit/Kernel/arm/SweetX9CSF.cpp','bsd/dev/sweet_x9_gpu_dependencies.c']:
 current[path]=(w/'resident-kernel-inputs'/Path(path).name).read_text()
current['iokit/Kernel/arm/x9_render_cs.inc']=(w/'render-kernel-generated/x9_render_cs.inc').read_text()
parts=[]
for path,after in current.items():
 before=baseline.get(path,'')
 if before==after:continue
 parts.append('diff --git a/'+path+' b/'+path+'\n')
 if not before:parts.append('new file mode 100644\n')
 parts.extend(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/'+path if before else '/dev/null',tofile='b/'+path))
output=patchdir/'0249-x9-installed-resident-render-engine.patch'
output.write_text(''.join(parts))
print('0249 native engine source delta saved; custom linked object chain remains in native-kernel/kernel-link.filelist',flush=True)
