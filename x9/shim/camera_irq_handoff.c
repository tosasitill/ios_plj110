/* Read-only this-boot topology. No GIC/SMMU/IRQ/reset/MMIO operations. */
#include "fdt.h"
#include "plog.h"
#include "../kernel/src/bsd/dev/x9_camera_irq_layout.h"
_Static_assert(sizeof(struct x9_camera_irq_layout)==X9_CAM_IRQ_LAYOUT_BYTES,"camera IRQ layout");
static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static int eq(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i+1==n&&!s[i]&&!p[i];}
static int key(const uint8_t *p,const char *s)
{size_t i=0;while(i<32&&s[i]&&p[i]==(uint8_t)s[i])i++;return i<32&&!s[i]&&!p[i];}
enum { Root,Soc,Sv,Gic };
struct capture {struct fdt_captured_property p[FDT_CAPTURE_MAX_PROPERTIES];unsigned count;};
static int get(const struct capture *c,unsigned node,const char *k,const uint8_t **p,size_t *n)
{
    for(unsigned i=0;i<c->count;i++)if(c->p[i].node==node) {
        const char *a=c->p[i].key,*b=k;while(*a&&*a==*b){a++;b++;}
        if(*a==*b){*p=c->p[i].value;*n=c->p[i].bytes;return 1;}
    }
    *p=0;*n=0;return 0;
}
static int word(const struct capture *c,unsigned node,const char *k,uint32_t v)
{const uint8_t *p;size_t n;return get(c,node,k,&p,&n)&&n==4&&be(p)==v;}
static int str(const struct capture *c,unsigned node,const char *k,const char *s)
{const uint8_t *p;size_t n;return get(c,node,k,&p,&n)&&eq(p,n,s);}
static int enabled(const struct capture *c,unsigned node)
{const uint8_t *p;size_t n;return !get(c,node,"status",&p,&n)||eq(p,n,"ok")||eq(p,n,"okay");}
static int qualify(uintptr_t dt,struct x9_camera_irq_layout *q)
{
    const char paths[][FDT_CAPTURE_PATH_BYTES]={"","/soc","/soc/camsv1@3a600000","/interrupt-controller@6800000"};
    struct capture c;const uint8_t *p;size_t n;uint32_t ph;
    if(fdt_capture_nodes(dt,paths,4,c.p,FDT_CAPTURE_MAX_PROPERTIES,&c.count)!=FDT_PROPERTY_PRESENT)return 0;
    if(!str(&c,Gic,"compatible","arm,gic-v3")||!word(&c,Gic,"#interrupt-cells",4)||
       !get(&c,Gic,"interrupt-controller",&p,&n)||n||!enabled(&c,Gic)||
       !get(&c,Gic,"phandle",&p,&n)||n!=4||!(ph=be(p))||
       fdt_phandle_unique(dt,paths[Gic],ph)!=FDT_PROPERTY_PRESENT)return 0;
    if(!get(&c,Gic,"reg",&p,&n)||n!=32||be(p)||be(p+4)!=0x06800000||be(p+8)||
       be(p+12)!=0x40000||be(p+16)||be(p+20)!=0x06880000||be(p+24)||be(p+28)!=0x200000)return 0;
    for(unsigned i=Root;i<=Sv;i++) {
        if(!enabled(&c,i)||get(&c,i,"interrupt-map",&p,&n)||
           get(&c,i,"interrupt-map-mask",&p,&n)||
           get(&c,i,"interrupts-extended",&p,&n))return 0;
        if(i==Root&&!word(&c,i,"interrupt-parent",ph))return 0;
        if(i!=Root&&get(&c,i,"interrupt-parent",&p,&n)&&(n!=4||be(p)!=ph))return 0;
        if(i!=Sv&&(!word(&c,i,"#address-cells",2)||!word(&c,i,"#size-cells",2)))return 0;
    }
    if(!get(&c,Soc,"ranges",&p,&n)||n)return 0;
    if(!str(&c,Sv,"compatible","mediatek,camsv")||!word(&c,Sv,"mediatek,camsv-id",0)||
       !word(&c,Sv,"mediatek,cammux-id",0))return 0;
    /* Reject reordered or truncated named banks, including the unused PDP/STG.
     * Inline byte strings avoid relocated-shim absolute pointer tables. */
    const char names[]="base\0base_DMA\0base_SCQ\0base_pdp\0base_stg\0inner_base\0inner_base_DMA\0inner_base_SCQ\0inner_base_pdp\0inner_base_stg";
    if(!get(&c,Sv,"reg-names",&p,&n)||n!=sizeof(names))return 0;
    for(size_t i=0;i<n;i++)if(p[i]!=(uint8_t)names[i])return 0;
    if(!get(&c,Sv,"reg",&p,&n)||n!=160)return 0;
    for(unsigned i=0;i<10;i++)if(be(p+16*i)||be(p+16*i+8)||be(p+16*i+12)!=0x1000||
        be(p+16*i+4)!=0x3a600000+(i<5?i:i+3)*0x1000)return 0;
    if(!get(&c,Sv,"interrupts",&p,&n)||n!=48)return 0;
    const uint32_t spi[3]={0x26d,0x273,0x279};
    *q=(struct x9_camera_irq_layout){.version=1,.flags=X9_CAM_IRQ_LK_TOPOLOGY,
        .gicd_base=0x06800000,.gicd_bytes=0x40000,.outer_base=0x3a600000,
        .inner_base=0x3a608000,.bank_bytes=0x1000,.irq_cells=4};
    for(unsigned i=0;i<3;i++) {
        if(be(p+16*i)||be(p+16*i+4)!=spi[i]||be(p+16*i+8)!=4||be(p+16*i+12))return 0;
        q->intid[i]=spi[i]+32;q->trigger[i]=4;
    }
    return x9_camera_irq_layout_valid(q);
}
struct walk {uint8_t *p;size_t size,at,found;unsigned nodes,matches;};
static int node(struct walk *w,unsigned depth)
{
    if(depth>32||++w->nodes>4096||w->at>w->size||w->size-w->at<8)return 0;
    uint32_t props=le(w->p+w->at),children=le(w->p+w->at+4);w->at+=8;
    if(props>1024||children>4096)return 0;
    unsigned names=0,chosen=0,local=0;size_t candidate=0;
    for(unsigned i=0;i<props;i++) {
        if(w->size-w->at<36)return 0;
        uint8_t *k=w->p+w->at;uint32_t raw=le(k+32),n=raw&0x7fffffff;w->at+=36;
        size_t padded=((size_t)n+3)&~(size_t)3;if(padded>w->size-w->at)return 0;
        if(key(k,"name")) {
            if(++names!=1||(raw>>31))return 0;
            chosen=depth==1&&eq(w->p+w->at,n,"chosen");
        }
        if(key(k,"x9-camera-irq-layout")) {
            if(++local!=1||raw!=X9_CAM_IRQ_LAYOUT_BYTES)return 0;
            candidate=w->at;
        }
        w->at+=padded;
    }
    if(local){if(!chosen||++w->matches!=1)return 0;w->found=candidate;}
    for(unsigned i=0;i<children;i++)if(!node(w,depth+1))return 0;
    return 1;
}
int fdt_camera_irq_patch_adt(uintptr_t dt,void *adt,size_t size)
{
    if(!adt||size>0x400000)return -1;
    struct walk w={(uint8_t *)adt,size,0,0,0,0};struct x9_camera_irq_layout q;
    if(!node(&w,0)||w.at!=size||w.matches!=1)return -1;
    uint8_t *p=w.p+w.found;for(unsigned i=0;i<X9_CAM_IRQ_LAYOUT_BYTES;i++)p[i]=0;
    if(!qualify(dt,&q)) {
        plog_str("X9 CAMERA IRQ FDT: GIC parent/banks/specifiers refused; topology absent\n");return 0;
    }
    const uint32_t *v=(const uint32_t *)&q;
    for(unsigned i=0;i<X9_CAM_IRQ_LAYOUT_BYTES/4;i++)put(p+4*i,v[i]);
    plog_str("X9 CAMERA IRQ FDT: live GIC4 INTID653/659/665 topology; no IRQ/DMA grant\n");return 1;
}
