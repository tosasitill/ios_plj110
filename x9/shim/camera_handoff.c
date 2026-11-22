/* Restore camera metadata without restoring the old per-field tree walkers.
 * One bounded ADT walk locates all four slots. One FDT collection plus one batch
 * phandle pass retains all IRQ/CSI provenance checks. The shared collector
 * services the watchdog every 2048 tokens and has a 2s budget per pass,
 * already exercised by audio (NOTES 5.6). Refusals leave zero slots and are
 * nonfatal. No camera/GIC/SMMU/efuse MMIO, IRQ arming, or hardware grant.
 */
#include "camera_fdt.h"
#include "hw.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_camera_irq_layout.h"
#include "../kernel/src/bsd/dev/x9_camera_calibration.h"
#include "../kernel/src/bsd/dev/x9_camera_smmu_policy.h"
#include "../kernel/src/bsd/dev/x9_camera_smmu_irq_layout.h"

static const char camera_paths[CN_COUNT][FDT_CAPTURE_PATH_BYTES]={
    "","/soc","/soc/camsv1@3a600000","/interrupt-controller@6800000",
    "/soc/seninf-top@3a400000","/efuse@10160000",
    "/soc/seninf-top@3a400000/seninf-csi-port-0",
    "/soc/seninf-top@3a400000/seninf-csi-port-3",
    "/efuse@10160000/csi-data0@1d0","/efuse@10160000/csi-data3@1dc",
    "/soc/seninf-top@3a400000/seninf-csi-port-1",
    "/soc/seninf-top@3a400000/seninf-csi-port-2",
    "/efuse@10160000/csi-data1@1d4","/efuse@10160000/csi-data2@1d8",
    "/chosen","/chosen@0","/soc/iommu@30800000","/soc/mm-smmu-cam"};
_Static_assert(CN_COUNT<=FDT_COLLECT_MAX_NODES,"camera node table");
/* ~6KiB BSS, never on the shim's 64KiB stack; only this call consumes it. */
static struct camera_fdt camera;
enum { SlotIRQ,SlotCAL,SlotSMMU,SlotSMMUIRQ,Slots };
static const char slot_key[Slots][24]={"x9-camera-irq-layout","x9-camera-calibration","x9-camera-smmu-policy","x9-camera-smmu-irq"};
static const uint32_t slot_bytes[Slots]={X9_CAM_IRQ_LAYOUT_BYTES,X9_CAM_CALIBRATION_BYTES,
    X9_CAM_SMMU_POLICY_BYTES,X9_CAM_SMMU_IRQ_LAYOUT_BYTES};
static uint32_t le(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static int exact(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i+1==n&&!s[i]&&!p[i];}
static int key_is(const uint8_t *p,const char *s)
{size_t i=0;while(i<32&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<32&&!s[i]&&!p[i];}
struct walk {
    uint8_t *p;size_t size,at,found[Slots];unsigned nodes,matches[Slots],tokens,kicks;
    uint64_t began,budget;
};
static int bounded(struct walk *w)
{
    if(++w->tokens%2048==0) {
        wdt_kick();w->kicks++;
        if(rsr("cntpct_el0")-w->began>w->budget)return 0;
    }
    return 1;
}
static int node(struct walk *w,unsigned depth)
{
    if(!bounded(w)||depth>32||++w->nodes>4096||w->at>w->size||w->size-w->at<8)return 0;
    uint32_t props=le(w->p+w->at),children=le(w->p+w->at+4);w->at+=8;
    if(props>1024||children>4096)return 0;
    unsigned names=0,chosen=0,local[Slots]={0};size_t candidate[Slots]={0};
    for(unsigned i=0;i<props;i++) {
        if(!bounded(w)||w->size-w->at<36)return 0;
        uint8_t *key=w->p+w->at;uint32_t raw=le(key+32),n=raw&0x7fffffff;w->at+=36;
        size_t padded=((size_t)n+3)&~(size_t)3;
        if(padded>w->size-w->at)return 0;
        if(key_is(key,"name")) {
            if(++names!=1||(raw>>31))return 0;
            chosen=depth==1&&exact(w->p+w->at,n,"chosen");
        }
        for(unsigned s=0;s<Slots;s++)if(key_is(key,slot_key[s])) {
            if(++local[s]!=1||raw!=slot_bytes[s])return 0;
            candidate[s]=w->at;
        }
        w->at+=padded;
    }
    for(unsigned s=0;s<Slots;s++)if(local[s]) {
        if(!chosen||++w->matches[s]!=1)return 0;
        w->found[s]=candidate[s];
    }
    for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
    return 1;
}
int fdt_camera_patch_adt(uintptr_t dt,void *adt,size_t size)
{
    if(!adt||size>0x400000)return -1;
    uint64_t freq=counter_frequency();
    struct walk w={.p=(uint8_t *)adt,.size=size,.began=rsr("cntpct_el0"),.budget=freq/1000*2000};
    if(!node(&w,0)||w.at!=size||rsr("cntpct_el0")-w.began>w.budget)return -1;
    for(unsigned s=0;s<Slots;s++)if(w.matches[s]!=1)return -1;
    uint8_t *slot[Slots];
    for(unsigned s=0;s<Slots;s++) {
        slot[s]=w.p+w.found[s];for(unsigned i=0;i<slot_bytes[s];i++)slot[s][i]=0;
    }
    struct camera_fdt *a=&camera;a->handles=0;
    plog_str("X9 CAMERA FDT: bounded collection pass 1/2\n");
    int r=fdt_collect(&a->c,dt,camera_paths,CN_COUNT,0);
    plog_hex("X9 CAMERA FDT pass1 us",freq?a->c.ticks*1000000/freq:0);
    /* Also enforce the completed-pass deadline when a short tree never
     * reaches the collector's 2048-token watchdog checkpoint. */
    if(r!=FDT_PROPERTY_PRESENT||a->c.ticks>freq/1000*2000) {
        plog_str("X9 CAMERA FDT: tree refused or over budget; no hardware grant\n");return 0;
    }
    plog_hex("X9 CAMERA FDT nodes present",a->c.present);
    plog_hex("X9 CAMERA FDT properties",a->c.count);
    struct x9_camera_irq_layout q={0};uint32_t words[5]={0},features=0;
    struct x9_camera_smmu_policy smmu={0};
    struct x9_camera_smmu_irq_layout smmu_irq={0};
    if(camera_irq_qualify(a,&q))features|=CF_IRQ;
    if(camera_calibration_qualify(a,words))features|=CF_CALIBRATION;
    if(camera_smmu_qualify(a,&smmu))features|=CF_SMMU;
    if(camera_smmu_irq_qualify(a,&smmu_irq))features|=CF_SMMU_IRQ;
    uint32_t ok=0;
    /* At most one second full-tree pass, even when all candidates pass.
     * No candidate or its old partial registration is a successful grant. */
    if(a->handles) {
        uint64_t pass1=a->c.ticks;
        r=fdt_collect_phandles(&a->c,a->handle,a->owner,a->handles,&ok);
        if(r!=FDT_PROPERTY_PRESENT||a->c.ticks-pass1>freq/1000*2000) {
            features=0;plog_str("X9 CAMERA FDT: phandle pass refused; metadata withdrawn\n");
        }
        plog_hex("X9 CAMERA FDT pass1+2 us",freq?a->c.ticks*1000000/freq:0);
    }
    for(unsigned i=0;i<a->handles;i++)if(!(ok&(1u<<i)))features&=~(unsigned)a->feature[i];
    plog_hex("X9 CAMERA FDT watchdog kicks",a->c.kicks+w.kicks);
    if((features&CF_IRQ)&&x9_camera_irq_layout_valid(&q)) {
        const uint32_t *v=(const uint32_t *)&q;
        for(unsigned i=0;i<X9_CAM_IRQ_LAYOUT_BYTES/4;i++)put(slot[SlotIRQ]+4*i,v[i]);
        plog_str("X9 CAMERA IRQ FDT: live GIC4 INTID653/659/665 topology; no IRQ/DMA grant\n");
    } else {
        features&=~CF_IRQ;
        plog_str("X9 CAMERA IRQ FDT: GIC parent/banks/specifiers refused; topology absent\n");
    }
    if(features&CF_CALIBRATION) {
        uint8_t *p=slot[SlotCAL];put(p,2);put(p+4,X9_CAM_CALIBRATION_LK_DATA);
        put(p+8,words[0]);put(p+12,15);put(p+16,words[1]);put(p+20,words[2]);
        put(p+24,words[3]);put(p+28,words[4]);
        plog_str("X9 CAMERA CAL FDT: this boot's CSI0/1/2/3 devinfo copied; no PHY/rate/DMA grant\n");
    } else plog_str("X9 CAMERA CAL FDT: LK devinfo/CSI cell mapping refused; no calibration data\n");
    if((features&CF_SMMU)&&x9_camera_smmu_policy_valid(&smmu)) {
        const uint32_t *v=(const uint32_t *)&smmu;
        for(unsigned i=0;i<X9_CAM_SMMU_POLICY_BYTES/4;i++)put(slot[SlotSMMU]+4*i,v[i]);
        plog_str("X9 CAMERA SMMU FDT: live MT6993 MM SID80 TCU prefetch2; no MM/SID/DMA grant\n");
    } else {
        features&=~CF_SMMU;
        plog_str("X9 CAMERA SMMU FDT: controller/master policy refused; metadata absent\n");
    }
    if((features&CF_SMMU_IRQ)&&x9_camera_smmu_irq_layout_valid(&smmu_irq)) {
        const uint32_t *v=(const uint32_t *)&smmu_irq;
        for(unsigned i=0;i<X9_CAM_SMMU_IRQ_LAYOUT_BYTES/4;i++)put(slot[SlotSMMUIRQ]+4*i,v[i]);
        plog_str("X9 CAMERA SMMU IRQ FDT: live combined INTID511 edge-rising; no IRQ grant\n");
    } else plog_str("X9 CAMERA SMMU IRQ FDT: combined IRQ topology absent\n");
    return features?1:0;
}
