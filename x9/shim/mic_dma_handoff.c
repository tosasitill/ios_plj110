/* Pure topology qualification from the actual FDT handed to the shim by LK.
 * OPPO OF uses the DMA parent's dma-ranges and this non-PCI child's iommus.
 * No hardware access, allocation, memif enable or drain declaration.
 * Fields come from the shared audio fdt_collect table (audio_fdt.h). */
#include "audio_fdt.h"
#include "../kernel/src/bsd/dev/x9_mic_dma_handoff.h"
_Static_assert(sizeof(struct x9_mic_dma_handoff)==X9_MIC_DMA_HANDOFF_BYTES,"microphone DMA handoff ABI");

static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v)
{for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static int exact(const uint8_t *p,size_t n,const char *s)
{
 size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;
 return i+1==n&&!s[i]&&!p[i];
}
/* ABSENT only from a present node of the fully validated tree. */
static int absent(const struct audio_fdt *a,unsigned node,const char *key)
{const uint8_t *p;size_t n;return fdt_collect_get(&a->c,node,key,&p,&n)==0;}
static int empty(const struct audio_fdt *a,unsigned node,const char *key)
{const uint8_t *p;size_t n;return fdt_collect_get(&a->c,node,key,&p,&n)==1&&!n;}
static int cells(const struct audio_fdt *a,unsigned node,const char *key,const uint32_t *v,size_t count)
{
 const uint8_t *p;size_t n;if(fdt_collect_get(&a->c,node,key,&p,&n)!=1||n!=count*4)return 0;
 for(size_t i=0;i<count;i++)if(be32(p+i*4)!=v[i])return 0;
 return 1;
}
static int string(const struct audio_fdt *a,unsigned node,const char *key,const char *s)
{const uint8_t *p;size_t n;return fdt_collect_get(&a->c,node,key,&p,&n)==1&&exact(p,n,s);}
static int enabled(const struct audio_fdt *a,unsigned node)
{
 const uint8_t *p;size_t n;int r=fdt_collect_get(&a->c,node,"status",&p,&n);
 return r==0||(r==1&&(exact(p,n,"okay")||exact(p,n,"ok")));
}
static int qualify(const struct audio_fdt *a)
{
 static const unsigned nodes[]={AN_ROOT,AN_SOC,AN_AFE};
 static const char unsupported[][24]={"iommus","iommu-map","iommu-map-mask","dma-coherent",
     "dma-noncoherent","interconnects","interconnect-names","memory-region","memory-region-names"};
 static const uint32_t two[]={2},range[]={0,0,0,0,0x10,0},reg[]={0,0x1a110000,0,0x10000};
 for(unsigned i=0;i<3;i++) {
  if(!enabled(a,nodes[i]))return 0;
  /* interconnect-names=dma-mem changes OF's DMA parent. A restricted pool
   * or inherited coherent profile is not the native owned-DRAM contract. */
  for(unsigned k=0;k<sizeof(unsupported)/sizeof(unsupported[0]);k++)
   if(!absent(a,nodes[i],unsupported[k]))return 0;
 }
 return cells(a,AN_ROOT,"#address-cells",two,1)&&cells(a,AN_ROOT,"#size-cells",two,1)&&
     cells(a,AN_SOC,"#address-cells",two,1)&&cells(a,AN_SOC,"#size-cells",two,1)&&
     string(a,AN_SOC,"compatible","simple-bus")&&empty(a,AN_SOC,"ranges")&&
     cells(a,AN_SOC,"dma-ranges",range,6)&&absent(a,AN_ROOT,"dma-ranges")&&
     absent(a,AN_AFE,"dma-ranges")&&string(a,AN_AFE,"compatible","mediatek,mt6993-sound")&&
     cells(a,AN_AFE,"reg",reg,4);
}
int mic_dma_qualify(const struct audio_fdt *a,uint8_t *p)
{
 if(!qualify(a))return 0;
 put32(p,1);put32(p+4,X9_MIC_DMA_HANDOFF_FLAGS);
 put64(p+8,0);put64(p+16,0);put64(p+24,0x1000000000ULL);
 put64(p+32,0x1a110000);put64(p+40,0x10000);
 return 1;
}
