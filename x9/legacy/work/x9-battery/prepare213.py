from pathlib import Path
import json,shutil,subprocess,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan212-cached-import-map';out=root/'x9/out/pan213-aks-log-scope';out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:
 shutil.copyfile(base/n,out/n)
orig=root/'x9/out/pan117-x9-eight-cpu/kernel-source/iokit/Kernel/arm/SweetKeyStore.cpp';s=orig.read_text()
needle='\tchar name[32];\n\tproc_selfname(name, sizeof(name));\n\tIOLog("SWEET AKS: %s sel %u arg0 %llu in %u/%u out %u/%u -> 0x%x\\n", name, selector,\n\t    arg0, in_scalars, in_struct, out_scalars, out_struct, ret);'
assert s.count(needle)==1
replace='\t/* Selector 17 succeeds thousands of times during normal X9 UI work. */\n\tif (!(sweet_early_is_x9() && selector == 17 && ret == kIOReturnSuccess)) {\n'+ '\n'.join('\t'+line for line in needle.splitlines())+'\n\t}'
s=s.replace(needle,replace);s=s.replace('#include <IOKit/IOService.h>','#include <IOKit/IOService.h>\n#include <arm64/sweet_early.h>')
target=out/'kernel-source/iokit/Kernel/arm/SweetKeyStore.cpp';target.write_text(s)
(root/'xnu/patches/0264-x9-aks-hot-path-log-suppression.patch').write_text(''.join(difflib.unified_diff(orig.read_text().splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/SweetKeyStore.cpp',tofile='b/iokit/Kernel/arm/SweetKeyStore.cpp')))
records=json.loads((base/'commands.json').read_text().replace(str(base),str(out)));r=json.loads((root/'work/x9-battery/compile-command.json').read_text());a=[];skip=False
for v in r['args']:
 if skip: skip=False;continue
 if v in ['-o','-MF','-MT']:skip=True;continue
 if v.endswith('.cpp'):continue
 a.append(v)
a += ['-I'+str(target.parent),'-c',str(target),'-o',str(out/'kernel-objects/SweetKeyStore.o')]
records.insert(-2,{'args':a,'cwd':r['cwd']});(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
f=(base/'kernel-link.filelist').read_text().replace(str(base),str(out)).replace(str(root/'x9/out/pan117-x9-eight-cpu/kernel-objects/SweetKeyStore.cpo'),str(out/'kernel-objects/SweetKeyStore.o'));(out/'kernel-link.filelist').write_text(f)
print(out)
