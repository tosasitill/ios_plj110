/* Read-only this-boot topology. No GIC/SMMU/IRQ/reset/MMIO operations. */
#include "camera_fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_camera_irq_layout.h"
_Static_assert(sizeof(struct x9_camera_irq_layout)==X9_CAM_IRQ_LAYOUT_BYTES,"camera IRQ layout");
static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int eq(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i+1==n&&!s[i]&&!p[i];}
static int get(const struct camera_fdt *a,unsigned node,const char *key,const uint8_t **p,size_t *n)
{return fdt_collect_get(&a->c,node,key,p,n)==FDT_PROPERTY_PRESENT;}
static int word(const struct camera_fdt *c,unsigned node,const char *k,uint32_t v)
{const uint8_t *p;size_t n;return get(c,node,k,&p,&n)&&n==4&&be(p)==v;}
static int str(const struct camera_fdt *c,unsigned node,const char *k,const char *s)
{const uint8_t *p;size_t n;return get(c,node,k,&p,&n)&&eq(p,n,s);}
static int enabled(const struct camera_fdt *c,unsigned node)
{const uint8_t *p;size_t n;int r=fdt_collect_get(&c->c,node,"status",&p,&n);return r==FDT_PROPERTY_ABSENT||(r==FDT_PROPERTY_PRESENT&&(eq(p,n,"ok")||eq(p,n,"okay")));}
int camera_irq_qualify(struct camera_fdt *a,struct x9_camera_irq_layout *q)
{
    const uint8_t *p;size_t n;uint32_t ph;
    const uint32_t required=(1u<<CN_ROOT)|(1u<<CN_SOC)|(1u<<CN_SV)|(1u<<CN_GIC);
    if((a->c.present&required)!=required)return 0;
    if(!str(a,CN_GIC,"compatible","arm,gic-v3")||!word(a,CN_GIC,"#interrupt-cells",4)||
       !get(a,CN_GIC,"interrupt-controller",&p,&n)||n||!enabled(a,CN_GIC)||
       !get(a,CN_GIC,"phandle",&p,&n)||n!=4||!(ph=be(p)))return 0;
    if(!get(a,CN_GIC,"reg",&p,&n)||n!=32||be(p)||be(p+4)!=0x06800000||be(p+8)||
       be(p+12)!=0x40000||be(p+16)||be(p+20)!=0x06880000||be(p+24)||be(p+28)!=0x200000)return 0;
    for(unsigned i=CN_ROOT;i<=CN_SV;i++) {
        if(!enabled(a,i)||get(a,i,"interrupt-map",&p,&n)||
           get(a,i,"interrupt-map-mask",&p,&n)||
           get(a,i,"interrupts-extended",&p,&n))return 0;
        if(i==CN_ROOT&&!word(a,i,"interrupt-parent",ph))return 0;
        if(i!=CN_ROOT&&get(a,i,"interrupt-parent",&p,&n)&&(n!=4||be(p)!=ph))return 0;
        if(i!=CN_SV&&(!word(a,i,"#address-cells",2)||!word(a,i,"#size-cells",2)))return 0;
    }
    if(!get(a,CN_SOC,"ranges",&p,&n)||n)return 0;
    if(!str(a,CN_SV,"compatible","mediatek,camsv")||!word(a,CN_SV,"mediatek,camsv-id",0)||
       !word(a,CN_SV,"mediatek,cammux-id",0))return 0;
    /* Reject reordered or truncated named banks, including the unused PDP/STG.
     * Inline byte strings avoid relocated-shim absolute pointer tables. */
    const char names[]="base\0base_DMA\0base_SCQ\0base_pdp\0base_stg\0inner_base\0inner_base_DMA\0inner_base_SCQ\0inner_base_pdp\0inner_base_stg";
    if(!get(a,CN_SV,"reg-names",&p,&n)||n!=sizeof(names))return 0;
    for(size_t i=0;i<n;i++)if(p[i]!=(uint8_t)names[i])return 0;
    if(!get(a,CN_SV,"reg",&p,&n)||n!=160)return 0;
    for(unsigned i=0;i<10;i++)if(be(p+16*i)||be(p+16*i+8)||be(p+16*i+12)!=0x1000||
        be(p+16*i+4)!=0x3a600000+(i<5?i:i+3)*0x1000)return 0;
    if(!get(a,CN_SV,"interrupts",&p,&n)||n!=48)return 0;
    const uint32_t spi[3]={0x26d,0x273,0x279};
    *q=(struct x9_camera_irq_layout){.version=1,.flags=X9_CAM_IRQ_LK_TOPOLOGY,
        .gicd_base=0x06800000,.gicd_bytes=0x40000,.outer_base=0x3a600000,
        .inner_base=0x3a608000,.bank_bytes=0x1000,.irq_cells=4};
    for(unsigned i=0;i<3;i++) {
        if(be(p+16*i)||be(p+16*i+4)!=spi[i]||be(p+16*i+8)!=4||be(p+16*i+12))return 0;
        q->intid[i]=spi[i]+32;q->trigger[i]=4;
    }
    return x9_camera_irq_layout_valid(q)&&camera_fdt_need(a,ph,CN_GIC,CF_IRQ);
}
