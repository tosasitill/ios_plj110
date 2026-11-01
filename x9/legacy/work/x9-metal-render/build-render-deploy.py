#!/usr/bin/env python3
"""Build owner-run Metal render probe and guarded native desktop payload.
Only compile/link/sign/generate build inputs; never execute a device probe.
"""
from pathlib import Path
import os,subprocess,plistlib,json
w=Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render';root=w.parents[1]
source=root/'MaliMetal/X9/General/probe';out=w/'desktop-deploy-inputs';out.mkdir(exist_ok=True)
dev=Path('/Users/tosasitill/Downloads/Xcode.app/Contents/Developer');tools=dev/'Toolchains/XcodeDefault.xctoolchain/usr/bin';sdk=dev/'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'
env=os.environ.copy();env['DEVELOPER_DIR']=str(dev);env['SDKROOT']=str(sdk)
records=[]
def run(args):
 args=[str(a) for a in args];records.append({'args':args,'cwd':str(w)})
 subprocess.run(args,cwd=w,env=env,check=True)
run([tools/'metal','-target','air64-apple-ios16.0','-isysroot',sdk,'-std=ios-metal2.4','-O2','-c',source/'desktop_probe.metal','-o',out/'desktop_probe.air'])
run([tools/'metallib',out/'desktop_probe.air','-o',out/'desktop_probe.metallib'])
blob=(out/'desktop_probe.metallib').read_bytes()
with (out/'x9_render_metallib.inc').open('w') as f:
 f.write('static const unsigned char x9_render_metallib[]={\n')
 for i in range(0,len(blob),32):f.write(','.join(hex(x) for x in blob[i:i+32])+',\n')
 f.write('};\n')
flags=[tools/'clang++','-target','arm64-apple-ios16.0','-isysroot',sdk,'-std=c++17','-fobjc-arc','-fblocks','-O2','-g','-I'+str(out)]
run(flags+[source/'x9-render-probe.mm','-framework','Foundation','-framework','Metal','-framework','IOSurface','-o',out/'x9-render-probe'])
run([tools/'strip','-S',out/'x9-render-probe'])
run(['/usr/bin/codesign','--force','--sign','-','--pagesize','16384',out/'x9-render-probe'])
original=(root/'x9/out/pan168-desktop-workgroup-xy/deployment-input/launchd.plist.original').read_bytes();cache=plistlib.loads(original)
job_path='/System/Library/LaunchDaemons/org.heliodarwin.x9-render-pan170.plist'
job={'Label':'org.heliodarwin.x9-render-pan170','ProgramArguments':['/private/var/x9-render-pan170/x9-render-probe'],'UserName':'root','RunAtLoad':True,'LaunchOnlyOnce':True,'ProcessType':'Background','StandardOutPath':'/dev/console','StandardErrorPath':'/dev/console'}
if job_path in cache['LaunchDaemons']:raise RuntimeError('duplicate render job')
cache['LaunchDaemons'][job_path]=job
(out/'launchd.render170.plist').write_bytes(plistlib.dumps(cache,fmt=plistlib.FMT_BINARY,sort_keys=False))
(out/'org.heliodarwin.x9-render-pan170.plist').write_bytes(plistlib.dumps(job,fmt=plistlib.FMT_XML))
files=[]
for name in ['X9GeneralMetal170.bundle','X9PanVK170.bundle']:
 bundle=w/name
 for path in sorted(bundle.rglob('*')):
  if path.is_file():files.append(('/System/Library/Extensions/'+name+'/'+str(path.relative_to(bundle)),path,0o755 if path.name in ['X9GeneralMetal','X9PanVK'] else 0o644))
files.extend([('/private/var/x9-render-pan170/x9-render-probe',out/'x9-render-probe',0o755),(job_path,out/'org.heliodarwin.x9-render-pan170.plist',0o644)])
directories={'/private/var/x9-render-pan170'}
for target,_,_ in files:
 p=Path(target).parent
 while str(p).startswith('/System/Library/Extensions/'):
  directories.add(str(p));p=p.parent
header=['/* Generated desktop payload input; never a host test. */','#pragma once','struct X9RenderPayloadFile {const char *path;const unsigned char *data;size_t bytes;int mode;};']
assembly=['.section __DATA_CONST,__const','.p2align 4']
entries=[];total=0
for i,(target,path,mode) in enumerate(files):
 length=path.stat().st_size;total+=length;name='x9_render_payload_'+str(i)
 header.append('extern const unsigned char '+name+'['+str(length)+'];')
 entries.append('{'+json.dumps(target)+','+name+','+str(length)+','+oct(mode).replace('0o','0')+'}')
 assembly+=['.p2align 4','.globl _'+name,'_'+name+':','.incbin '+json.dumps(str(path))]
# The modified cache is compared byte-for-byte and installed with fsync+rename.
path=out/'launchd.render170.plist';size=path.stat().st_size;total+=size
header+=['extern const unsigned char x9_render_launchd['+str(size)+'];','static const size_t x9_render_launchd_size='+str(size)+';',
 'static const struct X9RenderPayloadFile x9_render_files[]={'+',\n'.join(entries)+'};',
 'static const char *const x9_render_directories[]={'+','.join(json.dumps(d) for d in sorted(directories,key=lambda d:(d.count('/'),d)))+'};']
assembly+=['.p2align 4','.globl _x9_render_launchd','_x9_render_launchd:','.incbin '+json.dumps(str(path))]
(out/'x9_render_payload.h').write_text('\n'.join(header)+'\n');(out/'x9_render_payload.S').write_text('\n'.join(assembly)+'\n')
run([tools/'clang','-target','arm64-apple-macos14.0','-isysroot',dev/'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk','-c',out/'x9_render_payload.S','-o',out/'x9_render_payload.o'])
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
(out/'payload-inputs.json').write_text(json.dumps({'revision':170,'files':[{'target':t,'source':str(p),'bytes':p.stat().st_size,'mode':oct(m)} for t,p,m in files],'embedded_bytes':total,'probe':'original system Metal factory -> compiled authored metallib ->3 compute/cache roundtrips ->vertex/fragment rasterization into imported IOSurface ->4096pixel verification; NOT RUN','desktop':'general Metal discovery through IOAcceleratorES; CPU/LK scanout copy remains'},indent=2)+'\n')
print('Desktop payload generated:',len(files),'files,',total,'bytes; GPU render probe compiled/signed, NOT RUN',flush=True)
