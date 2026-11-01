from pathlib import Path
import json,subprocess,shutil,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan214-mediaserverd-budget';out=root/'x9/out/pan215-mediaserverd-inactive';out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:shutil.copyfile(base/n,out/n)
p=out/'kernel-source/bsd/kern/kern_memorystatus.c';old=p.read_text();s=old
start=s.index('#if defined(ARM64_BOARD_CONFIG_SWEET)\n\t/* X9 Metal/PanVK');end=s.index('#endif',start)+len('#endif');s=s[:start]+s[end:]
needle='#define SET_INACTIVE_LIMITS_LOCKED(p, limit, is_fatal)'
pos=s.index(needle)
helper='''static int
x9_inactive_memory_limit(proc_t p, int limit)
{
#if defined(ARM64_BOARD_CONFIG_SWEET)
 if (sweet_early_is_x9() && !strcmp(p->p_name, "mediaserverd") && limit > 0 && limit < 512) {
  printf("X9 JETSAM: mediaserverd inactive %d -> 512 MiB\\n", limit);
  return 512;
 }
#endif
 return limit;
}

'''
s=s[:pos]+helper+s[pos:];needle='(p)->p_memstat_memlimit_inactive = (limit);';assert s.count(needle)==1;s=s.replace(needle,'(p)->p_memstat_memlimit_inactive = x9_inactive_memory_limit((p), (limit));')
p.write_text(s);(root/'xnu/patches/0266-x9-mediaserverd-inactive-limit-all-paths.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/bsd/kern/kern_memorystatus.c',tofile='b/bsd/kern/kern_memorystatus.c')))
for n in ['commands.json','kernel-link.filelist']:(out/n).write_text((base/n).read_text().replace(str(base),str(out)))
print(out)
