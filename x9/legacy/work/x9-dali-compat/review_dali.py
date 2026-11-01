#!/usr/bin/env python3
"""Inventory every dali patch, source dependencies and mechanical applicability.
The classification records compatibility decisions; it is not a hardware test.
"""
from pathlib import Path
import concurrent.futures,hashlib,json,re,shutil,subprocess,tempfile,collections
R=Path('/Users/tosasitill/HelioDarwin-x9-lk-preserve');REF=R/'work/references/data-a-live';BASE=Path('/Users/tosasitill/HelioDarwin/xnu/build-dxb/dxb/xnu');LOCAL=R/'xnu/patches';OUT=R/'work/x9-dali-compat';OVER=R/'x9/out/pan87-keybag-bootstrap/kernel-source'
ported={80:'IPv4 route/address trace; added opt-in x9_net_trace gate',95:'optional sweetnoexec, default empty',100:'optional execswap parser and path hook; Adreno user-client portion excluded',123:'optional x9_initwait; default0, capped600s',158:'native X9 command/device/lock timing and read/write statistics, no old Qualcomm transport',199:'IOSurface explicit-zero defaults; excludes Adreno fence wait code',204:'IOSurface plane row at+0x10 while preserving established+0x08; excludes Qualcomm CPU governor',208:'X9-only console timestamps',209:'split inline AArch64 assembly instructions in timestamps',210:'IORTC readiness resource only; excludes native4KiB cs hash timing',}
equivalent={83:'CRC milestone journal and root traces0190-0194',84:'X9-specific UFS bounded host gates, no Qualcomm controller takeover',85:'X9 UPIU autosense/readiness trace and retries',86:'X9 device initialization and bounded readiness retries',88:'native X9 pinned-extent root0196, writes0201',108:'X9 board isolation0186 preserves LK display',111:'X9 board isolation0186 excludes Qualcomm input initialization',114:'X9 read-only SMMUv3 bypass verification; no SMMU takeover',115:'X9 CRC-validated GPT reader',116:'X9 native read diagnostics',117:'X9 NOP/TUR/autosense diagnostics',120:'X9 DT-verified MT6993 UFS init; incompatible vendor reset replaced',121:'dedicated pinned-root provisioning and digest/readback checks',122:'X9 max16384 extent contract and binary read lookup',129:'dedicated disk sidecar0202; automatic reset deliberately excluded',185:'keybag directory bootstrap0203; general firmware-pack installer dependency excluded'}
def decide(n,paths,text):
 if n in ported:return 'ported_compatible_parts',ported[n]
 if n in equivalent:return 'covered_by_X9_equivalent',equivalent[n]
 if n in {102,103,104,105,106,107,109,112,124,127}:
  return 'conflict','Requires PANDORA native4KiB/user-VA/memory-budget profile; X9 uses hardware-proven16KiB legacy512 and existing iOS16 mappings'
 if n in {148,149,150,151,152,153,154,155}:
  return 'conflict','Requires original Apple-kext prelink/PMGR/crypto registration and global iOS vtable-layout rebuild; current SWEET compatibility ABI and frozen source-built kernel cannot mix these partial objects'
 if n in {156,157,206}:return 'conflict','Suppresses console at logo/first image; conflicts with retained LK screen and continuous diagnostic-output policy'
 if n in {222,223,225}:return 'conflict','Reference sampler deliberately skips its current CPU and samples other CPUs via cpu_xcall; qualified X9 cpus=1 has no sampling target. AST/image report patches depend on that producer; require a local timer sampling port.'
 if n==211:return 'conflict','SHA2 CE corecrypto replacement requires verified ISA plus SIMD/kernel-state ownership and provider ABI; cannot safely activate from source match alone'
 if n==81:return 'conflict','Qualcomm DWC3 USB-network transport; X9 uses an unqualified MTU3 path and must not initialize inherited Qualcomm USB MMIO'
 if n in {82,87,89,90,91,92,93,97,128,130,131,132,133,134,135,136,137,138,139,140,141,142}:
  return 'conflict','FUXI/PANDORA board CPU affinity, SPMI/TLMM/GENI touch/backlight/display or vector-monitor dependencies; X9 MT6993 entry/interrupt/scanout and peripherals differ'
 if n in {118,119}:return 'conflict','Part of Qualcomm retained-host UFS recovery/active-power chain; cannot run it alongside X9-owned MT6993 reset/link/DMA lifecycle'
 if n in {110,125,126,159,160,212,217,218,220,221}:return 'conflict','PANDORA framebuffer dimensions, DPU/layer state, swap queue or disk-log frame-dump layout; conflicts with preserved X9 LK scanout and dedicated X9UFSLOG format'
 if n in {113,200,201,202,203,205,207,213,214,215}:return 'conflict','Oryon CPU/SCMI-fast-channel, Qualcomm PMIC RTC or thermal/idle/perf-control hardware; X9 CPU0/MediaTek register contract differs'
 if n in {143,144,145,146,147,161,182,226,227,228,229,230,231,232,233,234}:
  return 'conflict','Requires Qualcomm ADSP/SCM/GLINK/SMEM/IPCC/firmware-pack or SOC-CP provider chain absent on X9; installer components depend on that provider and cannot be linked independently'
 if any(any(v in p.lower() for v in ['adreno','pandora_gpu','gpu.c','gpu-','smmu']) for p in paths) or 162<=n<=198 or n in {96,98,99,101,216,219,224}:
  return 'conflict','Qualcomm Adreno/GMU/HFI/SQE/SMMU/PCIe driver or protocol dependencies; X9 has MediaTek/Mali and a separately verified existing SMMUv3 bypass'
 return 'needs_additional_review','No compatibility conclusion: retain source/context; not counted as completed migration'

def main():
 patches=sorted((REF/'xnu/patches').glob('*.patch'));known={hashlib.sha256(p.read_bytes()).hexdigest():p.name for p in LOCAL.glob('*.patch')};rows=[]
 with tempfile.TemporaryDirectory() as td:
  temp=Path(td);paths=set()
  for patch in patches:paths.update(re.findall(r'^\+\+\+ b/(.+)$',patch.read_text(errors="replace"),re.M))
  for rel in paths:
   src=OVER/rel
   if not src.exists():src=BASE/rel
   if src.is_file():dst=temp/rel;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst)
  def check(patch):
   text=patch.read_text(errors="replace");digest=hashlib.sha256(patch.read_bytes()).hexdigest();paths=re.findall(r'^\+\+\+ b/(.+)$',text,re.M);n=int(patch.name[:4]);item={'reference_patch':patch.name,'sha256':digest,'files':paths}
   if digest in known:item.update(status='identical_local_patch',reason='Already present byte-for-byte in local patch series',local_patch=known[digest]);return item
   status,reason=decide(n,paths,text);item.update(status=status,reason=reason)
   for label,flags in [('forward_check',[]),('reverse_check',['--reverse'])]:
    p=subprocess.run(['git','apply','--check',*flags,str(patch)],cwd=temp,capture_output=True,text=True)
    item[label]={'applies':p.returncode==0,'first_error':next((x for x in p.stderr.splitlines() if 'error:' in x),'')[:350]}
   return item
  with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:rows=list(pool.map(check,patches))
 result={'reference':'https://github.com/ShueiTsuki/data-a-live','reference_commit':subprocess.check_output(['git','-C',str(REF),'rev-parse','HEAD'],text=True).strip(),'scope':'All233 kernel patch files compared with X9 current source/isolated overlays; forward/reverse checks are mechanical only, never hardware qualification','counts':dict(collections.Counter(x['status'] for x in rows)),'additional_tools':['exact matching empty_nvram_proxy byte layout and /options','boot-policy/manifest metadata without board-MMIO changes'],'rows':rows}
 (OUT/'dali-patch-inventory.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result['counts'],indent=2));print('unresolved',[x['reference_patch'] for x in rows if x['status']=='needs_additional_review'])
if __name__=='__main__':main()
