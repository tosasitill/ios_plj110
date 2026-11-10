/* Qualify sensorhub v2 layout against THIS LK FDT before XNU reuses it.
 * No MMIO, SMC, firmware load, reset, FIFO reads or device writes here.
 * memorydump_size_probe precedes allocation in OPPO rv/scp_helper.c:
 * dump id0 = five dump fields + enabled SAP secure-dump-size, NOT zero.
 * mbox_setup_pin_table packs send pins then receive pins in 64-bit slots.
 * A missing/changed contract leaves sensors unavailable, not boot fatal. */
#include "fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_scp_handoff.h"
_Static_assert(sizeof(struct x9_scp_handoff)==X9_SCP_HANDOFF_BYTES,"SCP handoff ABI");
static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int eq(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n && s[i] && p[i]==(uint8_t)s[i])i++;return i<n && !s[i] && !p[i];}
static int str(uintptr_t dt,const char *path,const char *key,const char *v)
{const uint8_t *p;size_t n;return fdt_property(dt,path,key,&p,&n)&&eq(p,n,v);}
static int word(uintptr_t dt,const char *path,const char *key,uint32_t *v)
{const uint8_t *p;size_t n;if(!fdt_property(dt,path,key,&p,&n)||n!=4)return 0;*v=be(p);return 1;}
static int value(uintptr_t dt,const char *path,const char *key,uint32_t wanted)
{uint32_t v;return word(dt,path,key,&v)&&v==wanted;}
static int cells(uintptr_t dt,const char *path,const char *key,const uint8_t **p,size_t *n,unsigned stride)
{return fdt_property(dt,path,key,p,n)&&*n&&*n<=4096&&*n%(stride*4)==0;}
static int reg(uintptr_t dt,const char *path,const char *name,uint32_t *base,uint32_t *size)
{
 const uint8_t *names,*r;size_t nb,rb;
 if(!fdt_property(dt,path,"reg-names",&names,&nb)||nb>2048||
    !cells(dt,path,"reg",&r,&rb,4))return 0;
 size_t at=0;unsigned idx=0,matches=0;
 while(at<nb) {
  size_t end=at;while(end<nb&&names[end])end++;
  if(end==nb || (size_t)idx*16>=rb)return 0;
  if(eq(names+at,end-at+1,name)) {
   if(be(r+idx*16)||be(r+idx*16+8))return 0;
   *base=be(r+idx*16+4);*size=be(r+idx*16+12);matches++;
  }
  at=end+1;idx++;
 }
 return (size_t)idx*16==rb && matches==1;
}
static int memory(uintptr_t dt,struct x9_scp_handoff *h)
{
 const char *scp="/soc/scp@1d000000";
 const char *rmem="/reserved-memory/mblock-45-reserve-memory-scp_share";
 const char *sap="/soc/sap@1d39c000";
 /* Inline strings, not pointer tables: LK relocates the whole shim, so
  * absolute .rodata addresses (R_AARCH64_ABS64) are not permitted. */
 const char fields[][32]={"scp-sram-size","scp-cache-dump-buf-size","scp-reg-dump-buf-size",
     "scp-tbuf-dump-buf-size","scp-dram-size"};
 const uint8_t *p;size_t n;
 if(!str(dt,scp,"scp-mem-key","mediatek,reserve-memory-scp_share")||
    !str(dt,rmem,"compatible","mediatek,reserve-memory-scp_share")||
    !fdt_property(dt,rmem,"no-map",&p,&n)||n||!cells(dt,rmem,"reg",&p,&n,4)||n!=16||
    be(p)||be(p+8)||be(p+4)!=0xe8000000||be(p+12)!=0x18c0000)return 0;
 h->reserve_base=be(p+4);h->reserve_size=be(p+12);
 for(unsigned i=0;i<5;i++) {
  uint32_t v;if(!word(dt,scp,fields[i],&v)||!v||v>h->reserve_size-h->dump_size)return 0;
  h->dump_size+=v;
 }
 /* Pin this contract to the proved PLJ110 SAP variant; no absent-SAP guess. */
 uint32_t sap_size;
 if(!str(dt,sap,"compatible","mediatek,sap")||!str(dt,sap,"status","okay")||
    !word(dt,sap,"secure-dump-size",&sap_size)||!sap_size||sap_size>h->reserve_size-h->dump_size)return 0;
 h->dump_size+=sap_size;
 if(!cells(dt,scp,"scp-mem-tbl",&p,&n,3)||be(p)!=0)return 0;
 uint32_t align[X9_SCP_MEM_COUNT]={0},seen=0;
 for(size_t i=0;i<n;i+=12) {
  uint32_t id=be(p+i),size=be(p+i+4),a=be(p+i+8);
  /* This OPPO source enum has 30 entries; unexpected DT id30 is skipped. */
  if(id>=X9_SCP_MEM_COUNT) {if(id!=30)return 0;continue;}
  if((seen&(1u<<id)) || (a && ((a&(a-1))||a>h->reserve_size)))return 0;
  seen|=1u<<id;align[id]=a;h->memory[id].size=i==0?h->dump_size:size;
 }
 for(unsigned id=0;id<X9_SCP_MEM_COUNT;id++) {
  uint32_t at=h->allocated_size,a=align[id];
  if(a) {if(a-1>h->reserve_size-at)return 0;at=(at+a-1)&~(a-1);}
  if(at>h->reserve_size || h->memory[id].size>h->reserve_size-at)return 0;
  h->memory[id].offset=at;h->allocated_size=at+h->memory[id].size;
 }
 return h->memory[2].size==0x100000 && h->memory[8].size==0x10000 &&
     h->memory[9].size==0x2000 && h->memory[10].size==0x2000;
}
static int pins(uintptr_t dt,struct x9_scp_handoff *h)
{
 const char *scp="/soc/scp@1d000000";
 const char keys[][16]={"send-table","recv-table"};
 const unsigned step[]={3,7},wanted[]={29,30,31,32},words[]={16,2,7,7};
 uint32_t used[5]={0},seen[2][2]={{0}},matched=0;
 if(!value(dt,scp,"mbox-count",5)||!value(dt,scp,"#recv-cells-mode",1))return 0;
 for(unsigned direction=0;direction<2;direction++) {
  const uint8_t *p;size_t n;uint32_t last=0;
  if(!cells(dt,scp,keys[direction],&p,&n,step[direction]))return 0;
  for(size_t i=0;i<n;i+=step[direction]*4) {
   uint32_t channel=be(p+i),mbox=be(p+i+4),count=be(p+i+8);
   if(channel>=64||mbox>=5||mbox<last||!count||count>64||
      (seen[direction][channel/32]&(1u<<(channel%32))))return 0;
   seen[direction][channel/32]|=1u<<(channel%32);last=mbox;
   uint32_t slots=(count+1)/2,idx=used[mbox];
   if(slots>32-idx)return 0;
   for(unsigned j=0;j<4;j++)if(channel==wanted[j] && direction==(j&1)) {
    if(mbox!=1||count!=words[j])return 0;
    /* Sensor pins use callback, no recv_opt/lock/buffer-full/cb context. */
    if(direction && (be(p+i+12)||be(p+i+16)||be(p+i+20)||be(p+i+24)))return 0;
    h->sensor_pin[j]=(struct x9_scp_pin){channel,idx*8,count*4,idx};matched|=1u<<j;
   }
   used[mbox]+=slots;
  }
 }
 return matched==15;
}
static int qualify(uintptr_t dt,struct x9_scp_handoff *h)
{
 const char *scp="/soc/scp@1d000000";
 const char paths[][32]={"","/soc","/reserved-memory"};
 const char enabled[][32]={"core-0","secure-dump","scp-dram-region","scp-protect","scp-scpsys-regmap","scp-read-infra-irq-status"};
 const uint8_t *p;size_t n;uint32_t size;
 for(unsigned i=0;i<3;i++)if(!value(dt,paths[i],"#address-cells",2)||!value(dt,paths[i],"#size-cells",2))return 0;
 if(!fdt_property(dt,"/soc","ranges",&p,&n)||n||!fdt_property(dt,"/reserved-memory","ranges",&p,&n)||n)return 0;
 if(!str(dt,scp,"compatible","mediatek,scp")||!str(dt,scp,"status","okay")||
    !value(dt,scp,"core-nums",2)||!value(dt,scp,"twohart",1))return 0;
 for(unsigned i=0;i<6;i++)if(!str(dt,scp,enabled[i],"enable"))return 0;
 if(!reg(dt,scp,"scp_sram_base",&h->sram_base,&h->sram_size)||
    h->sram_base!=0x1d000000||h->sram_size!=0x400000||
    !reg(dt,scp,"mbox1_base",&h->mbox_base,&size)||h->mbox_base!=0x1dfb0000||size!=0x100||
    !reg(dt,scp,"mbox1_set",&h->mbox_set,&size)||h->mbox_set!=0x1dfb0100||size!=4||
    !reg(dt,scp,"mbox1_clr",&h->mbox_clear,&size)||h->mbox_clear!=0x1dfb010c||size!=4||
    !reg(dt,scp,"mbox1_init",&h->mbox_init,&size)||h->mbox_init!=0x1df2a024||size!=4)return 0;
 if(!memory(dt,h)||!pins(dt,h))return 0;
 h->region_info_op=10; /* secure: GET_REGION_INFO via 0xc2000528, NOT SRAM+4 */
 h->version=1;h->flags=X9_SCP_LAYOUT_VERIFIED;return 1;
}
struct walk {uint8_t *p;size_t size,at,found;unsigned nodes,matches;};
static int node(struct walk *w,unsigned depth)
{
 if(depth>32||++w->nodes>1024||w->at>w->size||w->size-w->at<8)return 0;
 uint32_t props=le(w->p+w->at),children=le(w->p+w->at+4);w->at+=8;
 if(props>1024||children>1024)return 0;
 size_t candidate=0;unsigned found=0,chosen=0;
 for(unsigned i=0;i<props;i++) {
  if(w->size-w->at<36)return 0;
  uint8_t *key=w->p+w->at;uint32_t n=le(key+32)&0x7fffffff;w->at+=36;
  size_t padded=((size_t)n+3)&~(size_t)3;
  if(padded>w->size-w->at)return 0;
  if(eq(key,32,"name"))chosen=depth==1&&eq(w->p+w->at,n,"chosen");
  if(eq(key,32,"x9-scp-handoff")) {if(n!=X9_SCP_HANDOFF_BYTES)return 0;candidate=w->at;found++;}
  w->at+=padded;
 }
 if(chosen&&found) {w->found=candidate;w->matches+=found;}
 for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
 return 1;
}
int fdt_scp_patch_adt(uintptr_t dt,void *adt,size_t size)
{
 struct walk w={(uint8_t *)adt,size,0,0,0,0};
 if(!node(&w,0)||w.at!=size||w.matches!=1)return -1;
 uint8_t *dst=w.p+w.found;
 for(unsigned i=0;i<X9_SCP_HANDOFF_BYTES;i++)dst[i]=0;
 struct x9_scp_handoff h={0};
 if(!qualify(dt,&h)) {plog_str("X9 SCP FDT: layout contract refused; no sensor hardware access\n");return 0;}
 const uint8_t *src=(const uint8_t *)&h;
 for(unsigned i=0;i<X9_SCP_HANDOFF_BYTES;i++)dst[i]=src[i];
 plog_str("X9 SCP FDT: secure dump, sensor memory and 64-bit pins qualified; firmware not yet ready\n");
 return 1;
}
