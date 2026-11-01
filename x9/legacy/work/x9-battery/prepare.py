from pathlib import Path
import json,subprocess,shutil,difflib
root=Path(__file__).resolve().parents[2]; src=root/'work/x9-battery/source'
base=root/'x9/out/pan209-gpu-coherent-skip'; out=root/'x9/out/pan210-native-battery'
p=src/'X9Gauge.inc';s=p.read_text().replace('0x4e0','0x4c0').replace('for(unsigned i=0;i<n;i++)gw(gaugeBus,0,tx[i]);','for(unsigned i=0;i<n;i++) *(volatile uint8_t *)gaugeBus=tx[i];').replace('unsigned lo=gr(gaugeBus,0)&255,hi=gr(gaugeBus,0)&255;','unsigned lo=*(volatile uint8_t *)gaugeBus,hi=*(volatile uint8_t *)gaugeBus;').replace('id!=0x0541&&id!=0x0421&&','id!=0x0541&&');p.write_text(s)
out.mkdir(exist_ok=False)
for name in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/name),str(out/name)],check=True)
for name in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch']:
 shutil.copyfile(base/name,out/name)
arm=out/'kernel-source/iokit/Kernel/arm'
for p in src.iterdir(): shutil.copyfile(p,arm/p.name)
old=Path('/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb/xnu/iokit/Kernel/arm/SweetBattery.cpp');s=old.read_text()
s=s.replace('bool\nSweetBattery::start(IOService *provider)\n{','extern "C" bool X9BatteryStart(void);\nbool\nSweetBattery::start(IOService *provider)\n{\n\tif (sweet_board_x9()) {\n\t\tbool ok = X9BatteryStart();\n\t\tIOLog("X9 BATTERY: sampler start=%d; static source suppressed\\n", ok);\n\t\treturn false;\n\t}\n')
(arm/'SweetBattery.cpp').write_text(s)
touchold=root/'x9/out/pan143-pmic-keys/kernel-source/bsd/dev/sweet_x9_touch.c';t=touchold.read_text();t=t.replace('static int pmic_read(', 'static int pmic_read_unlocked(').replace('static int pmic_write(', 'static int pmic_write_unlocked(')
pos=t.index('static int touch_rail(')
t=t[:pos]+'''extern int X9PMIFAcquire(void);
extern void X9PMIFRelease(void);
static int pmic_read(unsigned sid,unsigned addr,uint8_t *value) {
 if(X9PMIFAcquire())return -1;
 int rc=pmic_read_unlocked(sid,addr,value);X9PMIFRelease();return rc;
}
static int pmic_write(unsigned sid,unsigned addr,uint8_t value) {
 if(X9PMIFAcquire())return -1;
 int rc=pmic_write_unlocked(sid,addr,value);X9PMIFRelease();return rc;
}
'''+t[pos:]
touch=out/'kernel-source/bsd/dev/sweet_x9_touch.c';touch.write_text(t)
records=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
cxx=json.load(open(root/'work/x9-battery/compile-command.json'))
def compile_record(template,source,obj):
 a=[];skip=False
 for v in template['args']:
  if skip:skip=False;continue
  if v in ['-o','-MF','-MT']:skip=True;continue
  if v.endswith(('.cpp','.c')):continue
  a.append(v)
 a+=['-I'+str(arm),'-c',str(source),'-o',str(obj)]
 return dict(args=a,cwd=template['cwd'])
new=[compile_record(cxx,arm/n,out/'kernel-objects'/ (n+'.o')) for n in ['SweetX9Battery.cpp','SweetBattery.cpp']]
tr=next(r for r in json.load(open(root/'x9/out/pan143-pmic-keys/commands.json')) if any(a.endswith('sweet_x9_touch.c') for a in r['args']))
new.append(compile_record(tr,touch,out/'kernel-objects/sweet_x9_touch.o'))
filelist=(base/'kernel-link.filelist').read_text().replace(str(base),str(out))
filelist=filelist.replace('/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT/SweetBattery.cpo',str(out/'kernel-objects/SweetBattery.cpp.o')).replace(str(root/'x9/out/pan143-pmic-keys/kernel-objects/sweet_x9_touch.o'),str(out/'kernel-objects/sweet_x9_touch.o'))
filelist=filelist.rstrip()+'\n'+str(out/'kernel-objects/SweetX9Battery.cpp.o')+'\n';(out/'kernel-link.filelist').write_text(filelist)
(out/'commands.json').write_text(json.dumps(new+records[-2:],indent=2)+'\n')
(out/'build-kernel.py').write_text((base/'build-kernel.py').read_text())
patch=[]
for before,after,name in [(old.read_text(),s,'iokit/Kernel/arm/SweetBattery.cpp'),(touchold.read_text(),t,'bsd/dev/sweet_x9_touch.c')]+[('',(arm/n).read_text(),'iokit/Kernel/arm/'+n) for n in ['SweetX9Battery.cpp','X9Gauge.inc','X9BatterySample.h']]:
 patch.extend(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='a/'+name if before else '/dev/null',tofile='b/'+name))
(root/'xnu/patches/0261-x9-native-battery.patch').write_text(''.join(patch))
shutil.copyfile(Path(__file__),out/'prepare.py')
print(out)
