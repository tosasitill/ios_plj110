/* Current-FDT LEFT supply only. The RIGHT PA has no supply link in the
 * captured PLJ110 variant; neither this phandle nor an ACK proves a shared
 * net. Metadata has no PMIF/remote-writer/analogue-voltage authority. */
static int qualify_left_vdd(struct audio_fdt *a)
{
    static const char keys[][32]={"regulator-compatible","regulator-name",
        "regulator-min-microvolt","regulator-max-microvolt",
        "regulator-enable-ramp-delay","regulator-allowed-modes","phandle"};
    struct fdt_node_property props[7];uint32_t mask,ph;unsigned children;
    const uint8_t *p;size_t n;uint32_t base,bytes;
    const uint32_t need=(1u<<AN_SPMI)|(1u<<AN_PMIC5)|(1u<<AN_REG5)|(1u<<AN_LNC08)|(1u<<AN_PA_L);
    /* Optional topology: missing nodes withdraw only this capability. */
    if((a->c.present&need)!=need)return 0;
    if(!str(a,AN_SPMI,"compatible","mediatek,mt6993-spmi") ||
       !value(a,AN_SPMI,"#address-cells",2) ||
       !value(a,AN_SPMI,"#size-cells",0) ||
       !value(a,AN_SPMI,"spmi-dev-mask",0x55a8) ||
       !named_reg(a,AN_SPMI,"pmif-p",&base,&bytes) ||
       base!=0x1c018000 || bytes!=0x111c ||
       !str(a,AN_PMIC5,"compatible","mediatek,mt6661") ||
       !get(a,AN_PMIC5,"reg",&p,&n) || n!=8 || be(p)!=5 || be(p+4) ||
       !str(a,AN_REG5,"compatible","mediatek,mt6661-5-regulator"))return 0;
    const unsigned nodes[]={AN_SPMI,AN_PMIC5,AN_REG5,AN_LNC08};
    for(unsigned i=0;i<4;i++)if(get(a,nodes[i],"status",&p,&n)&&!eq(p,n,"okay"))return 0;
    /* Exact leaf schema rejects boot-on/always-on/tracking/parent supplies
     * and children, rather than claiming a lease over an unknown regulator. */
    if(fdt_collect_schema(&a->c,AN_LNC08,keys,7,props,&mask,&children)!=FDT_PROPERTY_PRESENT ||
       mask!=0x7f || children || props[0].bytes!=sizeof("LNC0_8") ||
       props[1].bytes!=sizeof("mt6661_5_lnc0_8") ||
       !eq(props[0].value,props[0].bytes,"LNC0_8") ||
       !eq(props[1].value,props[1].bytes,"mt6661_5_lnc0_8"))return 0;
    static const uint32_t values[]={500000,2090000,200};
    for(unsigned i=0;i<3;i++)if(props[i+2].bytes!=4||be(props[i+2].value)!=values[i])return 0;
    if(props[5].bytes!=8 || be(props[5].value) || be(props[5].value+4)!=2 ||
       props[6].bytes!=4 || !(ph=be(props[6].value)) ||
       !audio_fdt_need(a,ph,AN_LNC08,AF_LEFT_VDD) ||
       !value(a,AN_PA_L,"tfa98xx_vdd-supply",ph))return 0;
    return 1;
}
