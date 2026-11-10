/* Pure topology qualification from the actual FDT handed to the shim by LK.
 * OPPO OF uses the DMA parent's dma-ranges and this non-PCI child's iommus.
 * No hardware access, allocation, memif enable or drain declaration. */
#include "fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_mic_dma_handoff.h"
_Static_assert(sizeof(struct x9_mic_dma_handoff)==X9_MIC_DMA_HANDOFF_BYTES,"microphone DMA handoff ABI");

static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v)
{for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static int exact(const uint8_t *p,size_t n,const char *s)
{
 size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;
 return i+1==n&&!s[i]&&!p[i];
}
/* ADT property keys are NUL-padded to 32 bytes, unlike value strings. */
static int key_is(const uint8_t *p,const char *s)
{size_t i=0;while(i<32&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<32&&!s[i]&&!p[i];}
static int absent(uintptr_t dt,const char *path,const char *key)
{const uint8_t *p;size_t n;return fdt_property_lookup(dt,path,key,&p,&n)==FDT_PROPERTY_ABSENT;}
static int empty(uintptr_t dt,const char *path,const char *key)
{const uint8_t *p;size_t n;return fdt_property(dt,path,key,&p,&n)&&!n;}
static int cells(uintptr_t dt,const char *path,const char *key,const uint32_t *v,size_t count)
{
 const uint8_t *p;size_t n;if(!fdt_property(dt,path,key,&p,&n)||n!=count*4)return 0;
 for(size_t i=0;i<count;i++)if(be32(p+i*4)!=v[i])return 0;
 return 1;
}
static int string(uintptr_t dt,const char *path,const char *key,const char *s)
{const uint8_t *p;size_t n;return fdt_property(dt,path,key,&p,&n)&&exact(p,n,s);}
static int enabled(uintptr_t dt,const char *path)
{
 const uint8_t *p;size_t n;int r=fdt_property_lookup(dt,path,"status",&p,&n);
 return r==FDT_PROPERTY_ABSENT||(r==FDT_PROPERTY_PRESENT&&(exact(p,n,"okay")||exact(p,n,"ok")));
}
static int qualify(uintptr_t dt)
{
 const char *afe="/soc/mt6993-afe-pcm@1a110000";
 const char *paths[]={"","/soc",afe};
 const char *unsupported[]={"iommus","iommu-map","iommu-map-mask","dma-coherent",
     "dma-noncoherent","interconnects","interconnect-names","memory-region","memory-region-names"};
 static const uint32_t two[]={2},range[]={0,0,0,0,0x10,0},reg[]={0,0x1a110000,0,0x10000};
 for(unsigned i=0;i<3;i++) {
  if(!enabled(dt,paths[i]))return 0;
  /* interconnect-names=dma-mem changes OF's DMA parent. A restricted pool
   * or inherited coherent profile is not the native owned-DRAM contract. */
  for(unsigned k=0;k<sizeof(unsupported)/sizeof(unsupported[0]);k++)
   if(!absent(dt,paths[i],unsupported[k]))return 0;
 }
 return cells(dt,"","#address-cells",two,1)&&cells(dt,"","#size-cells",two,1)&&
     cells(dt,"/soc","#address-cells",two,1)&&cells(dt,"/soc","#size-cells",two,1)&&
     string(dt,"/soc","compatible","simple-bus")&&empty(dt,"/soc","ranges")&&
     cells(dt,"/soc","dma-ranges",range,6)&&absent(dt,"","dma-ranges")&&
     absent(dt,afe,"dma-ranges")&&string(dt,afe,"compatible","mediatek,mt6993-sound")&&
     cells(dt,afe,"reg",reg,4);
}

/* Whole ADT validation, independent of property order. Only one unflagged
 * 64B property in /chosen is accepted; a misplaced duplicate is rejected. */
struct walk {uint8_t *p;size_t size,at,found;unsigned nodes,matches;};
static int node(struct walk *w,unsigned depth)
{
 if(depth>32||++w->nodes>4096||w->at>w->size||w->size-w->at<8)return 0;
 uint32_t props=le32(w->p+w->at),children=le32(w->p+w->at+4);w->at+=8;
 if(props>1024||children>4096)return 0;
 unsigned names=0,chosen=0,local=0;size_t candidate=0;
 for(unsigned i=0;i<props;i++) {
  if(w->size-w->at<36)return 0;
  uint8_t *key=w->p+w->at;uint32_t raw=le32(key+32),n=raw&0x7fffffff;w->at+=36;
  size_t padded=((size_t)n+3)&~(size_t)3;
  if(padded>w->size-w->at)return 0;
  if(key_is(key,"name")) {
   if(++names!=1||(raw>>31))return 0;
   chosen=depth==1&&exact(w->p+w->at,n,"chosen");
  }
  if(key_is(key,"x9-mic-dma-handoff")) {
   if(++local!=1||raw!=X9_MIC_DMA_HANDOFF_BYTES)return 0;
   candidate=w->at;
  }
  w->at+=padded;
 }
 if(local) {
  if(!chosen||++w->matches!=1)return 0;
  w->found=candidate;
 }
 for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
 return 1;
}
int fdt_mic_dma_patch_adt(uintptr_t dt,void *adt,size_t size)
{
 if(!adt||size>0x400000)return -1;
 struct walk w={(uint8_t *)adt,size,0,0,0,0};
 if(!node(&w,0)||w.at!=size||w.matches!=1)return -1;
 uint8_t *p=w.p+w.found;
 for(unsigned i=0;i<X9_MIC_DMA_HANDOFF_BYTES;i++)p[i]=0;
 if(!qualify(dt)) {
  plog_str("X9 MIC DMA FDT: unknown/invalid topology; native DMA mapping disabled\n");return 0;
 }
 put32(p,1);put32(p+4,X9_MIC_DMA_HANDOFF_FLAGS);
 put64(p+8,0);put64(p+16,0);put64(p+24,0x1000000000ULL);
 put64(p+32,0x1a110000);put64(p+40,0x10000);
 plog_str("X9 MIC DMA FDT: this boot's 64GiB direct bus window qualified; not AIN0 or drain\n");
 return 1;
}
