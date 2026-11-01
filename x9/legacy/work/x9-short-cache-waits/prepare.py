from pathlib import Path
import json,shutil,subprocess,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan218-cache-plan-reuse';out=root/'x9/out/pan219-short-cache-waits'
out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:shutil.copyfile(base/n,out/n)
arm=out/'kernel-source/iokit/Kernel/arm';changes={}
p=arm/'SweetX9CSF.cpp';s=p.read_text();pos=s.index('static uint64_t deadline_ms(')
s=s[:pos]+'''/* Short render cache commands frequently complete below 1 ms. Sleep in
 * 100 us quanta for the first 2 ms, then return to the original 1 ms backoff.
 * This changes observation latency only: callers retain absolute deadlines,
 * completion/error checks and all cache/AS ordering. No busy-spin loop. */
static void render_cache_wait(unsigned *polls) {
 if((*polls)++>=20){sleep_ms();return;}
 uint64_t until;clock_interval_to_deadline(100,NSEC_PER_USEC,&until);
 (void)assert_wait_deadline(&sleep_cookie,THREAD_UNINT,until);thread_block(THREAD_CONTINUE_NULL);
}
'''+s[pos:]
a=s.index('static bool flush_gpu(vm_offset_t gpu)');b=s.index('\nstatic bool capabilities',a);part=s[a:b]
part=part.replace(' uint64_t end=deadline_ms(1000);',' uint64_t end=deadline_ms(1000);unsigned cache_polls=0;').replace('  sleep_ms();','  render_cache_wait(&cache_polls);')
s=s[:a]+part+s[b:];changes[p]=s
p=arm/'SweetX9RenderEngine.inc';s=p.read_text();a=s.index('static bool render_page_sync(');b=s.index('\nstatic bool render_range_sync(',a);part=s[a:b]
part=part.replace('bool done=false;uint64_t end=deadline_ms(1000);','bool done=false;uint64_t end=deadline_ms(1000);unsigned cache_polls=0;').replace('break;}sleep_ms();','break;}render_cache_wait(&cache_polls);');assert part!=s[a:b];s=s[:a]+part+s[b:];changes[p]=s
patch=''
for p,s in changes.items():
 old=(base/'kernel-source/iokit/Kernel/arm'/p.name).read_text();p.write_text(s)
 patch+=''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/'+p.name,tofile='b/iokit/Kernel/arm/'+p.name))
 shutil.copyfile(p,out/'metal-source/kernel'/p.name) if (out/'metal-source/kernel'/p.name).exists() else None
(root/'xnu/patches/0270-x9-short-render-cache-waits.patch').write_text(patch)
records=json.loads((base/'commands.json').read_text().replace(str(base),str(out)))
(out/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
(out/'kernel-link.filelist').write_text((base/'kernel-link.filelist').read_text().replace(str(base),str(out)))
shutil.copyfile(Path(__file__),out/'prepare.py')
print(out)
