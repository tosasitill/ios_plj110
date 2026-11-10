/* Current-FDT LEFT supply only. The RIGHT PA has no supply link in the
 * captured PLJ110 variant; neither this phandle nor an ACK proves a shared
 * net. Metadata has no PMIF/remote-writer/analogue-voltage authority. */
static int qualify_left_vdd(uintptr_t dt,struct adsp_capture *capture)
{
    enum { VddSPMI,VddPMIC5,VddRegulator5,VddRail,VddPA };
    const char paths[][FDT_CAPTURE_PATH_BYTES]={"/spmi@1c01a000",
        "/spmi@1c01a000/mt6661-5@5","/spmi@1c01a000/mt6661-5@5/regulators",
        "/spmi@1c01a000/mt6661-5@5/regulators/mt6661-5-lnc0-8",
        "/soc/i2c@12990000/tfa98xx@34"};
    const struct adsp_capture *c=capture;
    /* Optional topology must not make the required shared-layout capture
     * fail when these nodes are missing. One bounded pass, not per-key scans. */
    if(fdt_capture_nodes(dt,paths,5,capture->p,128,&capture->count)!=FDT_PROPERTY_PRESENT)return 0;
    const char *path="/spmi@1c01a000/mt6661-5@5/regulators/mt6661-5-lnc0-8";
    const char keys[][32]={"regulator-compatible","regulator-name",
        "regulator-min-microvolt","regulator-max-microvolt",
        "regulator-enable-ramp-delay","regulator-allowed-modes","phandle"};
    struct fdt_node_property props[7];uint32_t mask,ph;unsigned children;
    const uint8_t *p;size_t n;uint32_t base,bytes;
    if(!captured_str(c,VddSPMI,"compatible","mediatek,mt6993-spmi") ||
       !captured_value(c,VddSPMI,"#address-cells",2) ||
       !captured_value(c,VddSPMI,"#size-cells",0) ||
       !captured_value(c,VddSPMI,"spmi-dev-mask",0x55a8) ||
       !captured_named_reg(c,VddSPMI,"pmif-p",&base,&bytes) ||
       base!=0x1c018000 || bytes!=0x111c ||
       !captured_str(c,VddPMIC5,"compatible","mediatek,mt6661") ||
       !captured_get(c,VddPMIC5,"reg",&p,&n) || n!=8 || be(p)!=5 || be(p+4) ||
       !captured_str(c,VddRegulator5,"compatible","mediatek,mt6661-5-regulator"))return 0;
    const unsigned nodes[]={VddSPMI,VddPMIC5,VddRegulator5,VddRail};
    for(unsigned i=0;i<4;i++)if(captured_get(c,nodes[i],"status",&p,&n)&&!eq(p,n,"okay"))return 0;
    /* Exact leaf schema rejects boot-on/always-on/tracking/parent supplies
     * and children, rather than claiming a lease over an unknown regulator. */
    if(fdt_node_schema(dt,path,keys,7,props,&mask,&children)!=FDT_PROPERTY_PRESENT ||
       mask!=0x7f || children || props[0].bytes!=sizeof("LNC0_8") ||
       props[1].bytes!=sizeof("mt6661_5_lnc0_8") ||
       !eq(props[0].value,props[0].bytes,"LNC0_8") ||
       !eq(props[1].value,props[1].bytes,"mt6661_5_lnc0_8"))return 0;
    const uint32_t values[]={500000,2090000,200};
    for(unsigned i=0;i<3;i++)if(props[i+2].bytes!=4||be(props[i+2].value)!=values[i])return 0;
    if(props[5].bytes!=8 || be(props[5].value) || be(props[5].value+4)!=2 ||
       props[6].bytes!=4 || !(ph=be(props[6].value)) ||
       fdt_phandle_unique(dt,path,ph)!=FDT_PROPERTY_PRESENT ||
       !captured_value(c,VddPA,"tfa98xx_vdd-supply",ph))return 0;
    return 1;
}
