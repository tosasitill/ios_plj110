from pathlib import Path
import json,subprocess,shutil,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan213-aks-log-scope';out=root/'x9/out/pan214-mediaserverd-budget';out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:shutil.copyfile(base/n,out/n)
orig=Path('/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb/xnu/bsd/kern/kern_memorystatus.c');s=orig.read_text();needle='\tSET_INACTIVE_LIMITS_LOCKED(p, p_entry->memlimit_inactive, p_entry->memlimit_inactive_attr);';assert s.count(needle)==1
s=s.replace(needle,needle+'''\n#if defined(ARM64_BOARD_CONFIG_SWEET)
\t/* X9 Metal/PanVK currently needs ~300 MiB during mediaserverd startup.
\t * Keep a finite inactive limit; preserve fatal attributes and all other
\t * processes. Active/inactive transitions still use standard jetsam logic. */
\tif (sweet_early_is_x9() && !strcmp(p->p_name, "mediaserverd") &&
\t    p->p_memstat_memlimit_inactive > 0 && p->p_memstat_memlimit_inactive < 512) {
\t\tSET_INACTIVE_LIMITS_LOCKED(p, 512, p_entry->memlimit_inactive_attr);
\t\tprintf("X9 JETSAM: mediaserverd inactive limit raised to 512 MiB\\n");
\t}
#endif''')
s=s.replace('#include <sys/', '#include <arm64/sweet_early.h>\n#include <sys/',1)
target=out/'kernel-source/bsd/kern/kern_memorystatus.c';target.write_text(s)
(root/'xnu/patches/0265-x9-mediaserverd-memory-budget.patch').write_text(''.join(difflib.unified_diff(orig.read_text().splitlines(True),s.splitlines(True),fromfile='a/bsd/kern/kern_memorystatus.c',tofile='b/bsd/kern/kern_memorystatus.c')))
rs=json.loads((base/'commands.json').read_text().replace(str(base),str(out)));raw=json.loads((root/'x9/out/pan208-uikit-statusbar/commands.json').read_text());r=next(r for r in raw if any(a.endswith('/kern_exec.c') for a in r['args']));a=[];skip=False
for v in r['args']:
 if skip:skip=False;continue
 if v in ['-o','-MF','-MT']:skip=True;continue
 if v.endswith('/kern_exec.c'):continue
 a.append(v.replace(str(root/'x9/out/pan208-uikit-statusbar'),str(out)))
a+=['-c',str(target),'-o',str(out/'kernel-objects/kern_memorystatus.o')];rs.insert(-2,{'args':a,'cwd':r['cwd']});(out/'commands.json').write_text(json.dumps(rs,indent=2)+'\n')
f=(base/'kernel-link.filelist').read_text().replace(str(base),str(out)).replace('/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/bsd/DEVELOPMENT/kern_memorystatus.o',str(out/'kernel-objects/kern_memorystatus.o'));(out/'kernel-link.filelist').write_text(f)
print(out)
