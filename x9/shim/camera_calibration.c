/* LK devinfo is a little-endian byte array, NOT FDT cells or efuse MMIO.
 * mtk-devinfo.c reads atag,devinfo.data[reg/4]. Only CSI port0/3/1/2 words
 * are transferred; no device ID, whole table, fixed board values or grant. */
#include "camera_fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_camera_calibration.h"
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int exact(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i+1==n&&!s[i]&&!p[i];}
static int get(const struct camera_fdt *a,unsigned node,const char *key,const uint8_t **p,size_t *n)
{return fdt_collect_get(&a->c,node,key,p,n)==FDT_PROPERTY_PRESENT;}
static int string(const struct camera_fdt *a,unsigned node,const char *key,const char *s)
{const uint8_t *p;size_t n;return get(a,node,key,&p,&n)&&exact(p,n,s);}
static int enabled(const struct camera_fdt *a,unsigned node)
{
 const uint8_t *p;size_t n;int v=fdt_collect_get(&a->c,node,"status",&p,&n);
 return v==FDT_PROPERTY_ABSENT||(v==FDT_PROPERTY_PRESENT&&(exact(p,n,"okay")||exact(p,n,"ok")));
}
/* Qualify each nvmem consumer and its exact provider cell, not the node's
 * deceptive unit address. Unknown mappings fail before publishing words. */
static int cell(struct camera_fdt *a,unsigned consumer,const char *port,unsigned node,uint32_t offset)
{
 const uint8_t *p;size_t n;uint32_t handle;
 if(!enabled(a,consumer)||!enabled(a,node)||!string(a,consumer,"compatible","mediatek,seninf")||
    !string(a,consumer,"csi-port",port)||!string(a,consumer,"nvmem-cell-names","rg_csi")||
    !get(a,consumer,"nvmem-cells",&p,&n)||n!=4)return 0;
 handle=be32(p);
 if(!get(a,node,"reg",&p,&n)||n!=8||be32(p)!=offset||be32(p+4)!=4)return 0;
 return camera_fdt_need(a,handle,node,CF_CALIBRATION);
}
int camera_calibration_qualify(struct camera_fdt *a,uint32_t out[5])
{
 const uint8_t *p,*alt;size_t n,m;uint32_t count;
 const unsigned top=CN_TOP,efuse=CN_EFUSE;
 if(!enabled(a,efuse)||!enabled(a,top)||!string(a,efuse,"compatible","mediatek,devinfo")||
    !string(a,top,"compatible","mediatek,seninf-core")||
    !cell(a,CN_CSI0,"0",CN_CELL0,0x1d0)||
    !cell(a,CN_CSI3,"3",CN_CELL3,0x1dc)||
    !cell(a,CN_CSI1,"1",CN_CELL1,0x1d4)||
    !cell(a,CN_CSI2,"2",CN_CELL2,0x1d8))return 0;
 int chosen=fdt_collect_get(&a->c,CN_CHOSEN,"atag,devinfo",&p,&n);
 int other=fdt_collect_get(&a->c,CN_CHOSEN0,"atag,devinfo",&alt,&m);
 /* Never fall back from a malformed/ambiguous chosen to another. */
 if(chosen==FDT_PROPERTY_PRESENT) {if(other!=-1)return 0;}
 else if(chosen==-1&&other==FDT_PROPERTY_PRESENT) {p=alt;n=m;}
 else return 0;
 if(n<4)return 0;
 count=le32(p);
 if(count<=0x1dc/4||count>400||n!=4+(size_t)count*4)return 0;
 out[0]=count;out[1]=le32(p+4+0x1d0);out[2]=le32(p+4+0x1dc);
 out[3]=le32(p+4+0x1d4);out[4]=le32(p+4+0x1d8);return 1;
}
