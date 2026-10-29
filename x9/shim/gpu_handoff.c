/* Establish GPU mailbox/shared-memory ownership from THIS LK FDT.
 * This does not read or modify GPUEB hardware. A missing/mismatched property
 * disables only the GPU probe and leaves the rest of boot running. */
#include "fdt.h"
#include "plog.h"
#include "gpu_fdt_expected.h"

static uint32_t be32(const uint8_t *p) {
 return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static uint32_t le32(const uint8_t *p) {
 return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static void put64(uint8_t *p,uint64_t v) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(i*8));}
static int eq(const uint8_t *p,size_t n,const char *s) {
 size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;
 return i<n&&!s[i]&&!p[i];
}
static int cells(uintptr_t dt,const char *path,const char *key,const uint32_t *expected,size_t count) {
 const uint8_t *p;size_t n;
 if(!fdt_property(dt,path,key,&p,&n)||n!=count*4)return 0;
 for(size_t i=0;i<count;i++)if(be32(p+i*4)!=expected[i])return 0;
 return 1;
}
static int string(uintptr_t dt,const char *path,const char *key,const char *wanted) {
 const uint8_t *p;size_t n;
 return fdt_property(dt,path,key,&p,&n)&&eq(p,n,wanted);
}
static int live_contract(uintptr_t dt) {
 const char *eb="/soc/gpueb@4b000000";
 const char *gh="/soc/ghpm@4b800000";
 const char *res="/reserved-memory/mblock-57-me_GPUEB_SHARED";
 static const uint32_t two[]={2},one[]={1},zero[]={0},four[]={4},count[]={0xa0};
 static const uint32_t address[]={0,0xf1080000},size[]={0,0x180000};
 static const uint32_t reserved[]={0,0xf1080000,0,0x180000};
 static const uint32_t mali_reg[]={0,0x48000000,0,0x480000};
 const uint8_t *p;size_t n;
 if(!cells(dt,"","#address-cells",two,1)||!cells(dt,"","#size-cells",two,1)||
    !cells(dt,"/soc","#address-cells",two,1)||!cells(dt,"/soc","#size-cells",two,1)||
    !cells(dt,"/reserved-memory","#address-cells",two,1)||!cells(dt,"/reserved-memory","#size-cells",two,1))return 0;
 if(!fdt_property(dt,"/soc","ranges",&p,&n)||n)return 0;
 if(!fdt_property(dt,"/reserved-memory","ranges",&p,&n)||n)return 0;
 if(!string(dt,eb,"compatible","mediatek,gpueb")||!string(dt,gh,"compatible","mediatek,ghpm")||
    !string(dt,res,"compatible","mediatek,me_GPUEB_SHARED"))return 0;
 if(!fdt_property(dt,res,"no-map",&p,&n)||n)return 0;
 if(!cells(dt,res,"reg",reserved,4)||!cells(dt,eb,"reg",eb_reg,sizeof(eb_reg)/4)||
    !cells(dt,gh,"reg",ghpm_reg,sizeof(ghpm_reg)/4)||
    !cells(dt,"/soc/mali@48000000","reg",mali_reg,4)||
    !cells(dt,"/soc/mali@48000000","system-coherency",zero,1)||
    !cells(dt,"/soc/gpufreq@48500000","reg",gpufreq_reg,sizeof(gpufreq_reg)/4))return 0;
 if(!cells(dt,eb,"gpueb_mem_addr",address,2)||!cells(dt,eb,"gpueb_mem_size",size,2)||
    !cells(dt,eb,"gpueb-mem-table",mem_table,sizeof(mem_table)/4)||
    !cells(dt,eb,"send-table",send_table,sizeof(send_table)/4)||
    !cells(dt,eb,"recv-table",recv_table,sizeof(recv_table)/4)||
    !cells(dt,eb,"mbox-count",one,1)||!cells(dt,eb,"mbox-size",count,1)||
    !cells(dt,eb,"slot-size",four,1)||!cells(dt,eb,"gpueb-support",one,1)||
    !cells(dt,eb,"ghpm-support",one,1))return 0;
 return 1;
}
/* Validate the whole ADT before publishing a 64-byte contract in /chosen. */
struct walk {uint8_t *p;size_t size,at,found;unsigned nodes,matches;};
static int node(struct walk *w,unsigned depth) {
 if(depth>32||++w->nodes>1024||w->at>w->size||w->size-w->at<8)return -1;
 uint32_t props=le32(w->p+w->at),children=le32(w->p+w->at+4);w->at+=8;
 if(props>1024||children>1024)return -1;
 unsigned chosen=0;
 for(unsigned i=0;i<props;i++) {
  if(w->size-w->at<36)return -1;
  uint8_t *key=w->p+w->at;uint32_t n=le32(key+32)&0x7fffffff;w->at+=36;
  size_t padded=((size_t)n+3)&~(size_t)3;
  if(padded>w->size-w->at)return -1;
  if(eq(key,32,"name"))chosen=depth==1&&eq(w->p+w->at,n,"chosen");
  if(eq(key,32,"x9-gpu-handoff")&&chosen) {
   if(n!=64)return -1;
   w->found=w->at;w->matches++;
  }
  w->at+=padded;
 }
 for(unsigned i=0;i<children;i++)if(node(w,depth+1))return -1;
 return 0;
}
int fdt_gpu_patch_adt(uintptr_t dt,void *adt,size_t size) {
 struct walk w={(uint8_t *)adt,size,0,0,0,0};
 if(node(&w,0)||w.at!=size||w.matches!=1)return -1;
 uint8_t *p=w.p+w.found;
 for(unsigned i=0;i<64;i++)p[i]=0;
 if(!live_contract(dt)) {
  plog_str("X9 GPU FDT: contract mismatch, GPU power probe disabled\n");return 0;
 }
 put32(p,3);put32(p+4,1);
 put64(p+8,0xf1080000);put64(p+16,0x180000);
 put32(p+24,0x4000);put32(p+28,4);put32(p+32,55);put32(p+36,8);
 put64(p+40,0x4b09fd80);put64(p+48,0x4b170000);put64(p+56,0x4b09fc80);
 plog_str("X9 GPU FDT: GPUEB carveout/mailbox/MT6993 + MFG SMMU registers verified at this boot\n");
 return 0;
}
