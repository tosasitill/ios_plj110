/* THIS LK FDT -> soc721a HIF layout. No MMIO/SMC/firmware/ownership inference.
 * OPPO gps_dl_linux_reserved_mem_v2 uses CONNSYS_EMI_FW, and MT6993 probe
 * selects soc721a. gps node's 1MiB EMI is a different legacy allocation.
 * KO buffer getters 0x38f9c/0x38fd0 add 0x70, NOT the header's 64 constant:
 * union inf has two arrays of three 16-byte descriptors and occupies 112B. */
#include "fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_gps_handoff.h"
_Static_assert(sizeof(struct x9_gps_handoff)==X9_GPS_HANDOFF_BYTES,"GPS handoff ABI");
static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int eq(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<n&&!s[i]&&!p[i];}
static int str(uintptr_t dt,const char *path,const char *key,const char *v)
{const uint8_t *p;size_t n;return fdt_property(dt,path,key,&p,&n)&&eq(p,n,v);}
static int word(uintptr_t dt,const char *path,const char *key,uint32_t *v)
{const uint8_t *p;size_t n;if(!fdt_property(dt,path,key,&p,&n)||n!=4)return 0;*v=be(p);return 1;}
static int value(uintptr_t dt,const char *path,const char *key,uint32_t v)
{uint32_t got;return word(dt,path,key,&got)&&got==v;}
static int pair(uintptr_t dt,const char *path,const char *key,uint32_t a,uint32_t b)
{const uint8_t *p;size_t n;return fdt_property(dt,path,key,&p,&n)&&n==8&&be(p)==a&&be(p+4)==b;}
static int range(uintptr_t dt,const char *path,uint32_t *base,uint32_t *size)
{
 const uint8_t *p;size_t n;
 if(!fdt_property(dt,path,"reg",&p,&n)||n!=16||be(p)||be(p+8))return 0;
 *base=be(p+4);*size=be(p+12);return *size&&*base<=UINT32_MAX-*size;
}
static int named_reg(uintptr_t dt,const char *path,const char *name,uint32_t base,uint32_t size)
{
 const uint8_t *names,*r;size_t nb,rb;
 if(!fdt_property(dt,path,"reg-names",&names,&nb)||nb>2048||
    !fdt_property(dt,path,"reg",&r,&rb)||!rb||rb>4096||rb%16)return 0;
 size_t at=0;unsigned idx=0,matches=0;
 while(at<nb) {
  size_t end=at;while(end<nb&&names[end])end++;
  if(end==nb||(size_t)idx*16>=rb)return 0;
  if(eq(names+at,end-at+1,name)) {
   if(be(r+idx*16)||be(r+idx*16+4)!=base||be(r+idx*16+8)||be(r+idx*16+12)!=size)return 0;
   matches++;
  }
  at=end+1;idx++;
 }
 return (size_t)idx*16==rb&&matches==1;
}
static int enabled(uintptr_t dt,const char *path)
{
 const uint8_t *p;size_t n;
 return !fdt_property(dt,path,"status",&p,&n)||eq(p,n,"okay")||eq(p,n,"ok");
}
static int power_topology(uintptr_t dt)
{
 const char *spmi="/spmi@1c01a000",*chip="/spmi@1c01a000/mt6661-3@3";
 const char *rail="/spmi@1c01a000/mt6661-3@3/regulators/mt6661-3-lnc0-8";
 const char *pd="/clock-controller@1c040000",*hwv="/syscon@1c040000";
 uint32_t rp,cp,hp,base,size;
 /* Resolve the phandle edges, not the captured Android phandle numbers.
  * No fallback to SID5/6, connv3 or another clock index is allowed. */
 if(!enabled(dt,"/soc")||!enabled(dt,"/soc/consys@40000000")||
    !enabled(dt,spmi)||!enabled(dt,chip)||
    !enabled(dt,"/spmi@1c01a000/mt6661-3@3/regulators")||
    !enabled(dt,rail)||!enabled(dt,pd)||!enabled(dt,hwv)||
    !str(dt,spmi,"compatible","mediatek,mt6993-spmi")||
    !value(dt,spmi,"#address-cells",2)||!value(dt,spmi,"#size-cells",0)||
    !value(dt,spmi,"spmi-dev-mask",0x55a8)||
    !named_reg(dt,spmi,"pmif-p",0x1c018000,0x111c)||
    !str(dt,chip,"compatible","mediatek,mt6661")||!pair(dt,chip,"reg",3,0)||
    !str(dt,rail,"regulator-compatible","LNC0_8")||
    !str(dt,rail,"regulator-name","mt6661_3_lnc0_8")||
    !value(dt,rail,"regulator-min-microvolt",500000)||
    !value(dt,rail,"regulator-max-microvolt",2090000)||
    !word(dt,rail,"phandle",&rp)||!rp||
    !value(dt,"/soc/consys@40000000","vcn18-supply",rp)||
    !str(dt,"/soc/consys@40000000","clock-names","clk_pd_conn_connsys")||
    !str(dt,pd,"compatible","mediatek,mt6993-ap_pd_clk")||
    !range(dt,pd,&base,&size)||base!=0x1c040000||size!=0x74900||
    !value(dt,pd,"#clock-cells",1)||!word(dt,pd,"phandle",&cp)||!cp||
    !pair(dt,"/soc/consys@40000000","clocks",cp,1)||
    !str(dt,hwv,"compatible","mediatek,mt6993-ap_hwv")||
    !range(dt,hwv,&base,&size)||base!=0x1c040000||size!=0x74900||
    !word(dt,hwv,"phandle",&hp)||!hp||!value(dt,pd,"hw-voter-regmap",hp))return 0;
 return 1;
}
static int pos_topology(uintptr_t dt,unsigned rf)
{
 const uint8_t *r;size_t n;
 /* Actual conninfra.ko g_conn_reg uses FDT indices 0/4/9/11 for D-die,
  * and 2/7 for WT_SLP/AFE. Address tuples never grant ownership. */
 const uint32_t tuple[][3]={{0,0x40000000,0x480},{4,0x40011000,0x138},
     {9,0x4004b000,0x414},{11,0x40050000,0x2000},
     {2,0x40003000,0x204},{7,0x40041000,0x128}};
 if(!enabled(dt,"/soc/consys@40000000")||
    !fdt_property(dt,"/soc/consys@40000000","reg",&r,&n)||n!=17*16)return 0;
 for(unsigned i=0;i<(rf ? 6u : 4u);i++) {
  const uint8_t *p=r+tuple[i][0]*16;
  if(be(p)||be(p+4)!=tuple[i][1]||be(p+8)||be(p+12)!=tuple[i][2])return 0;
 }
 return 1;
}
static int qualify(uintptr_t dt,struct x9_gps_handoff *h)
{
 const char *cfg="/connsys-emi-config",*consys="/soc/consys@40000000";
 const char paths[][32]={"","/soc","/reserved-memory"};
 const uint32_t sizes[]={0xc00000,0x3c0000,0x85000,0x4b000,0x12000,0xe000,
     0x40000,0x140000,0x2fa000,0xd6000,0x100000,0x1900000};
 const uint8_t *p;size_t n;uint32_t wifi_base,wifi_size,at=0,base,size,off,gsize;
 for(unsigned i=0;i<3;i++)if(!value(dt,paths[i],"#address-cells",2)||!value(dt,paths[i],"#size-cells",2))return 0;
 if(!fdt_property(dt,"/soc","ranges",&p,&n)||n||
    !fdt_property(dt,"/reserved-memory","ranges",&p,&n)||n)return 0;
 if(!str(dt,"/soc/gps@40c00000","compatible","mediatek,mt6993-gps")||
    !str(dt,consys,"compatible","mediatek,mt6993-consys")||
    !str(dt,consys,"radio-support","gps")||
    !str(dt,"/reserved-memory/mblock-53-consys_emi","compatible","mediatek,consys_emi")||
    !fdt_property(dt,"/reserved-memory/mblock-53-consys_emi","no-map",&p,&n)||n||
    !range(dt,"/reserved-memory/mblock-53-consys_emi",&h->reserve_base,&h->reserve_size)||
    !str(dt,"/reserved-memory/mblock-52-consys_wifi_emi","compatible","mediatek,consys_wifi_emi")||
    !fdt_property(dt,"/reserved-memory/mblock-52-consys_wifi_emi","no-map",&p,&n)||n||
    !range(dt,"/reserved-memory/mblock-52-consys_wifi_emi",&wifi_base,&wifi_size)||
    (wifi_base&0xffffff)||wifi_size!=sizes[0]||wifi_base+wifi_size!=h->reserve_base||
    !value(dt,cfg,"emi-alignment",0x1000000)||
    !pair(dt,cfg,"emi-region-to-atf",1,7)||!pair(dt,cfg,"gps-emi-to-atf",7,7)||
    !fdt_property(dt,cfg,"emi-layout-region",&p,&n)||n!=sizeof(sizes)*2)return 0;
 for(unsigned i=0;i<12;i++) {
  if(be(p+8*i)!=i||be(p+8*i+4)!=sizes[i])return 0;
  if(i==7)h->legacy_offset=at;
  if(i>0)at+=sizes[i];
  if(i==7)h->layout_size=at;
  if(i==9 && at!=h->reserve_size)return 0;
 }
 if(!fdt_property(dt,consys,"emi-addr",&p,&n)||n!=8||be(p)||be(p+4)!=h->reserve_base||
    /* LK advertises the whole multi-block allocation (including Wi-Fi).
     * Only the actual no-map consys reserved tuple is safe to access. */
    !word(dt,consys,"emi-size",&size)||size!=at+wifi_size||
    !word(dt,consys,"gps-emi-offset",&off)||off!=h->legacy_offset||
    !word(dt,consys,"gps-emi-size",&gsize)||gsize!=sizes[7]||
    !word(dt,cfg,"emi-min-addr",&base)||wifi_base<base||
    !word(dt,cfg,"emi-max-addr",&base)||h->reserve_base+h->reserve_size>base)return 0;
 /* Verify the separately served GPS legacy area lies in region7. */
 if(!word(dt,"/soc/gps@40c00000","emi-addr",&base)||base!=h->reserve_base+off||
    !value(dt,"/soc/gps@40c00000","emi-size",0x100000))return 0;
 h->legacy_size=0x78000;
 h->ap2mcu_offset=off+0x78000+0xc0000;h->mcu2ap_offset=h->ap2mcu_offset+0x4000;
 h->region_size=0x4000;h->data_offset=0x70;h->data_size=0x1000;
 if(h->mcu2ap_offset+h->region_size!=h->layout_size||h->layout_size>h->reserve_size)return 0;
 /* v062 gps_dl_bus_read resolves the CCIF bus address within conn_gps_base. */
 if(!named_reg(dt,"/soc/gps@40c00000","conn_gps_base",0x40000000,0x100000))return 0;
 h->mcu_bus_base=0x70000000;h->ccif_base=0x4003d000;h->ccif_size=0x100;
 h->version=1;h->flags=X9_GPS_LAYOUT_VERIFIED;
 if(power_topology(dt))h->flags|=X9_GPS_POWER_LAYOUT_VERIFIED;
 if(pos_topology(dt,0))h->flags|=X9_GPS_POS_LAYOUT_VERIFIED;
 if(pos_topology(dt,1))h->flags|=X9_GPS_RF_LAYOUT_VERIFIED;
 return 1;
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
  if(eq(key,32,"x9-gps-handoff")){if(n!=X9_GPS_HANDOFF_BYTES)return 0;candidate=w->at;found++;}
  w->at+=padded;
 }
 if(chosen&&found){w->found=candidate;w->matches+=found;}
 for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
 return 1;
}
int fdt_gps_patch_adt(uintptr_t dt,void *adt,size_t size)
{
 struct walk w={(uint8_t *)adt,size,0,0,0,0};
 if(!node(&w,0)||w.at!=size||w.matches!=1)return -1;
 uint8_t *dst=w.p+w.found;for(unsigned i=0;i<X9_GPS_HANDOFF_BYTES;i++)dst[i]=0;
 struct x9_gps_handoff h={0};
 if(!qualify(dt,&h)){plog_str("X9 GPS FDT: consys/HIF contract refused; no GNSS hardware access\n");return 0;}
 const uint8_t *src=(const uint8_t *)&h;for(unsigned i=0;i<X9_GPS_HANDOFF_BYTES;i++)dst[i]=src[i];
 plog_str("X9 GPS FDT: soc721a consys EMI / HIF +0x70 qualified; MCU not yet owned/ready\n");
 if(h.flags&X9_GPS_POWER_LAYOUT_VERIFIED)
  plog_str("X9 GPS FDT: SID3 LNC0_8 / PMIF-P / CONN clock topology qualified; not a power lease\n");
 if(h.flags&X9_GPS_POS_LAYOUT_VERIFIED)
  plog_str("X9 GPS FDT: D-die CFG/bus/SYSRAM tuples qualified; not SRAM/DMA ownership\n");
 if(h.flags&X9_GPS_RF_LAYOUT_VERIFIED)
  plog_str("X9 GPS FDT: WT_SLP/AFE tuples qualified; not RF/clock ownership\n");
 return 1;
}
