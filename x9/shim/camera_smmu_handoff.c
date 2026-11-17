/* MTK parse_driver_properties + setup_features, qualified from this boot's
 * FDT. No SMMU/controller access, table walk, SMC or IRQ operation. */
#include "camera_fdt.h"
#include "../kernel/src/bsd/dev/x9_camera_smmu_policy.h"
_Static_assert(sizeof(struct x9_camera_smmu_policy)==X9_CAM_SMMU_POLICY_BYTES,"camera SMMU policy");
static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int eq(const uint8_t *p,size_t n,const char *s)
{size_t i=0;while(i<n&&s[i]&&p[i]==(uint8_t)s[i])i++;return i+1==n&&!s[i]&&!p[i];}
static int get(const struct camera_fdt *a,unsigned node,const char *key,const uint8_t **p,size_t *n)
{return fdt_collect_get(&a->c,node,key,p,n)==FDT_PROPERTY_PRESENT;}
static int word(const struct camera_fdt *a,unsigned node,const char *key,uint32_t value)
{const uint8_t *p;size_t n;return get(a,node,key,&p,&n)&&n==4&&be(p)==value;}
static int enabled(const struct camera_fdt *a,unsigned node)
{
    const uint8_t *p;size_t n;
    int r=fdt_collect_get(&a->c,node,"status",&p,&n);
    return r==FDT_PROPERTY_ABSENT ||
        (r==FDT_PROPERTY_PRESENT&&(eq(p,n,"ok")||eq(p,n,"okay")));
}
int camera_smmu_qualify(struct camera_fdt *a,struct x9_camera_smmu_policy *q)
{
    const uint8_t *p;size_t n;uint32_t ph,prefetch;
    const unsigned nodes[]={CN_ROOT,CN_SOC,CN_SMMU,CN_SMMU_CAM};
    const char compatibles[]="arm,smmu-v3\0mtk,smmu-v700\0mediatek,mt6993-mm-smmu";
    for(unsigned i=0;i<4;i++)
        if(!(a->c.present&(1u<<nodes[i])) || !enabled(a,nodes[i]))return 0;
    for(unsigned i=0;i<2;i++)
        if(!word(a,nodes[i],"#address-cells",2)||!word(a,nodes[i],"#size-cells",2))return 0;
    if(!get(a,CN_SOC,"ranges",&p,&n)||n)return 0;
    if(!get(a,CN_SMMU,"compatible",&p,&n)||n!=sizeof(compatibles))return 0;
    for(size_t i=0;i<n;i++)if(p[i]!=(uint8_t)compatibles[i])return 0;
    if(!word(a,CN_SMMU,"#iommu-cells",1)||
       !get(a,CN_SMMU,"phandle",&p,&n)||n!=4||!(ph=be(p)))return 0;
    if(!get(a,CN_SMMU,"reg",&p,&n)||n!=32||be(p)||be(p+8)||be(p+16)||be(p+24))return 0;
    *q=(struct x9_camera_smmu_policy){.version=1,.flags=X9_CAM_SMMU_POLICY_LK_FDT,
        .normal_base=be(p+4),.normal_bytes=be(p+12),
        .wrapper_base=be(p+20),.wrapper_bytes=be(p+28)};
    if(!get(a,CN_SMMU,"tcu-prefetch",&p,&n)||n!=4)return 0;
    prefetch=be(p);q->tcu_prefetch=prefetch;
    if(!get(a,CN_SMMU_CAM,"compatible",&p,&n)||!eq(p,n,"mediatek,smmu-share-group")||
       !get(a,CN_SMMU_CAM,"iommus",&p,&n)||n!=8||be(p)!=ph)return 0;
    q->sid=be(p+4);
    if(!word(a,CN_SMMU_CAM,"smmu-supply",ph))return 0;
    /* These features would change the STE attribute/bypass contract.
     * No ATS device or alternate IOMMU map is present on this fixed master.
     * Require genuine absence; a malformed/empty value is not absence. */
    const char absent[][24]={"mtk,smmu-dma-mode","mtk,smmu-dcp","dma-coherent",
        "ats-supported","iommu-map","iommu-map-mask"};
    for(unsigned i=0;i<sizeof(absent)/sizeof(absent[0]);i++)
        if(fdt_collect_get(&a->c,CN_SMMU_CAM,absent[i],&p,&n)!=FDT_PROPERTY_ABSENT)return 0;
    return x9_camera_smmu_policy_valid(q) && camera_fdt_need(a,ph,CN_SMMU,CF_SMMU);
}
