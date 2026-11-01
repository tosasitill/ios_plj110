from pathlib import Path
import json,shutil,subprocess,difflib
root=Path(__file__).resolve().parents[2];base=root/'x9/out/pan210-native-battery';out=root/'x9/out/pan211-battery-id'
out.mkdir(exist_ok=False)
for n in ['kernel-source','kernel-objects','generated','packaging-source','build-tools','firmware','compute-source','plugin-package','metal-source']:
 subprocess.run(['/bin/cp','-cR',str(base/n),str(out/n)],check=True)
for n in ['build.sh','boot-args.txt','root-manifest.json','x9-metal-plugins-ios.tar.gz','external-metal-verification.patch','build-kernel.py']:
 shutil.copyfile(base/n,out/n)
for n in ['commands.json','kernel-link.filelist']:
 (out/n).write_text((base/n).read_text().replace(str(base),str(out)))
p=out/'kernel-source/iokit/Kernel/arm/X9Gauge.inc';old=p.read_text();s=old
s=s.replace('static bool gx(const uint8_t *tx, unsigned n, uint16_t *word)', 'static bool gx(const uint8_t *tx, unsigned n, uint8_t *data, unsigned count)')
s=s.replace('if(word)ctl|=2u|16u;', 'if(count)ctl|=2u|16u;').replace('word?2:0','count').replace('word?2:1','count?2:1')
s=s.replace('    if(word) {\n        unsigned lo=*(volatile uint8_t *)gaugeBus,hi=*(volatile uint8_t *)gaugeBus;\n        *word=(uint16_t)(lo|(hi<<8));\n    }','    for(unsigned i=0;i<count;i++)data[i]=*(volatile uint8_t *)gaugeBus;')
s=s.replace('static bool gaugeWord(uint8_t reg,uint16_t *v) {return gx(&reg,1,v);}', 'static bool gaugeWord(uint8_t reg,uint16_t *v) { uint8_t b[2]; if(!gx(&reg,1,b,2))return false; *v=b[0]|((unsigned)b[1]<<8);return true;}')
a=s.index('    uint8_t idcmd[]');b=s.index('    gaugeType=id;return true;',a)
s=s[:a]+'''    /* Captured DT support_extern_cmd: stock OPlus writes MAC device-type
     * request then reads back echoed command + type after 5ms. */
    uint8_t idcmd[]={0x3e,1,0}, reg=0x3e, response[4];
    if(!gx(idcmd,3,NULL,0))return false;
    IOSleep(5);
    if(!gx(&reg,1,response,4))return false;
    if(response[0]!=1 || response[1]!=0) {
        IOLog("X9 BATTERY: external ID response invalid %02x %02x %02x %02x\\n",response[0],response[1],response[2],response[3]);return false;
    }
    uint16_t id=response[2]|((unsigned)response[3]<<8);
    IOLog("X9 BATTERY: external gauge type=0x%04x\\n",id);
    /* OPlus selects BQ27541 registers by default for this DT; NFG8011B
     * additionally uses register 0x0c for current. */
    if(!id || id==0xffff)return false;
''' +s[b:]
s=s.replace('gaugeType==0xffa5||gaugeType==0xa5ff','gaugeType==0x4231')
p.write_text(s)
(root/'xnu/patches/0262-x9-external-gauge-identification.patch').write_text(''.join(difflib.unified_diff(old.splitlines(True),s.splitlines(True),fromfile='a/iokit/Kernel/arm/X9Gauge.inc',tofile='b/iokit/Kernel/arm/X9Gauge.inc')))
print(out)
