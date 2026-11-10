/* LK devinfo is a little-endian byte array, NOT FDT cells or efuse MMIO.
 * mtk-devinfo.c reads atag,devinfo.data[reg/4]. Only CSI port0/3 words
 * are transferred; no device ID, whole table, fixed board values or grant. */
#include "fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_camera_calibration.h"
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static int exact(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i+1==n&&!s[i]&&!p[i];}
static int key_is(const uint8_t *p,const char *s)
{size_t i=0;while(i<32&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<32&&!s[i]&&!p[i];}
static int string(uintptr_t dt,const char *path,const char *key,const char *s)
{const uint8_t *p;size_t n;return fdt_property(dt,path,key,&p,&n)&&exact(p,n,s);}
static int enabled(uintptr_t dt,const char *path)
{
 const uint8_t *p;size_t n;int v=fdt_property_lookup(dt,path,"status",&p,&n);
 return v==FDT_PROPERTY_ABSENT||(v==FDT_PROPERTY_PRESENT&&(exact(p,n,"okay")||exact(p,n,"ok")));
}
/* Qualify each nvmem consumer and its exact provider cell, not the node's
 * deceptive unit address. Unknown mappings fail before publishing words. */
static int cell(uintptr_t dt,const char *consumer,const char *port,const char *path,uint32_t offset)
{
 const uint8_t *p;size_t n;uint32_t handle;
 if(!enabled(dt,consumer)||!enabled(dt,path)||!string(dt,consumer,"compatible","mediatek,seninf")||
    !string(dt,consumer,"csi-port",port)||!string(dt,consumer,"nvmem-cell-names","rg_csi")||
    !fdt_property(dt,consumer,"nvmem-cells",&p,&n)||n!=4)return 0;
 handle=be32(p);
 if(fdt_phandle_unique(dt,path,handle)!=FDT_PROPERTY_PRESENT||
    !fdt_property(dt,path,"reg",&p,&n)||n!=8||be32(p)!=offset||be32(p+4)!=4)return 0;
 return 1;
}
static int qualify(uintptr_t dt,uint32_t out[3])
{
 const uint8_t *p,*alt;size_t n,m;uint32_t count;
 const char *top="/soc/seninf-top@3a400000",*efuse="/efuse@10160000";
 if(!enabled(dt,efuse)||!enabled(dt,top)||!string(dt,efuse,"compatible","mediatek,devinfo")||
    !string(dt,top,"compatible","mediatek,seninf-core")||
    !cell(dt,"/soc/seninf-top@3a400000/seninf-csi-port-0","0","/efuse@10160000/csi-data0@1d0",0x1d0)||
    !cell(dt,"/soc/seninf-top@3a400000/seninf-csi-port-3","3","/efuse@10160000/csi-data3@1dc",0x1dc))return 0;
 int a=fdt_property_lookup(dt,"/chosen","atag,devinfo",&p,&n);
 int b=fdt_property_lookup(dt,"/chosen@0","atag,devinfo",&alt,&m);
 /* Never fall back from a malformed/ambiguous chosen to another. */
 if(a==FDT_PROPERTY_PRESENT) {if(b!=FDT_PROPERTY_NODE_MISSING)return 0;}
 else if(a==FDT_PROPERTY_NODE_MISSING&&b==FDT_PROPERTY_PRESENT) {p=alt;n=m;}
 else return 0;
 if(n<4)return 0;
 count=le32(p);
 if(count<=0x1dc/4||count>400||n!=4+(size_t)count*4)return 0;
 out[0]=count;out[1]=le32(p+4+0x1d0);out[2]=le32(p+4+0x1dc);return 1;
}
/* Whole ADT validation, independent of property order. Only one unflagged
 * 32B property in /chosen is accepted; a misplaced duplicate is rejected. */
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
  if(key_is(key,"x9-camera-calibration")) {
   if(++local!=1||raw!=X9_CAM_CALIBRATION_BYTES)return 0;
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
int fdt_camera_calibration_patch_adt(uintptr_t dt,void *adt,size_t size)
{
 if(!adt||size>0x400000)return -1;
 struct walk w={(uint8_t *)adt,size,0,0,0,0};uint32_t words[3];
 if(!node(&w,0)||w.at!=size||w.matches!=1)return -1;
 uint8_t *p=w.p+w.found;
 for(unsigned i=0;i<X9_CAM_CALIBRATION_BYTES;i++)p[i]=0;
 if(!qualify(dt,words)) {
  plog_str("X9 CAMERA CAL FDT: LK devinfo/CSI cell mapping refused; no calibration data\n");return 0;
 }
 put32(p,1);put32(p+4,X9_CAM_CALIBRATION_LK_DATA);put32(p+8,words[0]);put32(p+12,9);
 put32(p+16,words[1]);put32(p+20,words[2]);
 plog_str("X9 CAMERA CAL FDT: this boot's CSI0/3 devinfo copied; no PHY/rate/DMA grant\n");return 1;
}
