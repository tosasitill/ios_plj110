/* PLJ110 PA reset topology ONLY. OPPO tfa98xx_ext_reset with polarity0
 * asserts LOW for 1ms, then deasserts HIGH for 1ms. No right-rail inference
 * from the LEFT supply property, dummy regulator, ACK or reset metadata. */
static int pa_reset_group(uintptr_t dt,const char *path,unsigned pin,uint32_t *handle)
{
    const char keys[][32]={"phandle"};
    const char leaf_keys[][32]={"pinmux","output-low","phandle"};
    struct fdt_node_property p[3];uint32_t mask;unsigned children;
    if(fdt_node_schema(dt,path,keys,1,p,&mask,&children)!=FDT_PROPERTY_PRESENT||
       mask!=1||children!=1||p[0].bytes!=4)return 0;
    uint32_t ph=be(p[0].value);
    if(fdt_phandle_unique(dt,path,ph)!=FDT_PROPERTY_PRESENT)return 0;
    char leaf[128];size_t at=0;const char *tail="/pins_cmd_dat";
    while(path[at]){if(at>=sizeof(leaf)-16)return 0;leaf[at]=path[at];at++;}
    unsigned i=0;do{leaf[at++]=tail[i];}while(tail[i++]);
    if(fdt_node_schema(dt,leaf,leaf_keys,3,p,&mask,&children)!=FDT_PROPERTY_PRESENT||
       (mask!=3&&mask!=7)||children||p[0].bytes!=4||be(p[0].value)!=(pin<<8)||p[1].bytes)return 0;
    if((mask&4)&&(p[2].bytes!=4||
       fdt_phandle_unique(dt,leaf,be(p[2].value))!=FDT_PROPERTY_PRESENT))return 0;
    *handle=ph;return 1;
}
static int qualify_pa_reset(uintptr_t dt)
{
    const char *spmi="/spmi@1c01a000",*pmic="/spmi@1c01a000/mt6661-3@3";
    const char *pc="/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl";
    const char paths[][48]={"/soc/i2c@12990000/tfa98xx@34","/soc/i2c@12990000/tfa98xx@35"};
    const char groups[][104]={"/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl/tfa98xx_gpio_L",
        "/spmi@1c01a000/mt6661-3@3/mt6661-3-pinctrl/tfa98xx_gpio_R"};
    const uint8_t *p;size_t n;uint32_t base,bytes,ph;
    if(!str(dt,spmi,"compatible","mediatek,mt6993-spmi")||!i2s4_enabled(dt,spmi)||
       !value(dt,spmi,"#address-cells",2)||!value(dt,spmi,"#size-cells",0)||
       !value(dt,spmi,"spmi-dev-mask",0x55a8)||
       !named_reg(dt,spmi,"pmif-p",&base,&bytes)||base!=0x1c018000||bytes!=0x111c||
       !str(dt,pmic,"compatible","mediatek,mt6661")||!i2s4_enabled(dt,pmic)||
       !fdt_property(dt,pmic,"reg",&p,&n)||n!=8||be(p)!=3||be(p+4)||
       !str(dt,pc,"compatible","mediatek,mt6661-3-pinctrl")||!i2s4_enabled(dt,pc)||
       !value(dt,pc,"#gpio-cells",2)||!fdt_property(dt,pc,"gpio-controller",&p,&n)||n||
       !word(dt,pc,"phandle",&ph,0)||fdt_phandle_unique(dt,pc,ph)!=FDT_PROPERTY_PRESENT)return 0;
    const char *bus="/soc/i2c@12990000";
    if(!str(dt,bus,"compatible","mediatek,mt6991-i2c")||!i2s4_enabled(dt,bus)||
       !value(dt,bus,"#address-cells",1)||!value(dt,bus,"#size-cells",0)||
       !fdt_property(dt,bus,"reg",&p,&n)||n!=32||be(p)||be(p+4)!=0x12990000||
       be(p+8)||be(p+12)!=0x20000||be(p+16)||be(p+20)!=0x16410000||
       be(p+24)||be(p+28)!=0x10000)return 0;
    uint32_t handles[2]={0};
    for(unsigned i=0;i<2;i++) {
        if(!str(dt,paths[i],"compatible","tfa,tfa98xx")||!i2s4_enabled(dt,paths[i])||
           !value(dt,paths[i],"reg",0x34+i)||!value(dt,paths[i],"reset-polarity",0)||
           !value(dt,paths[i],"channel-config",i+1)||
           !fdt_property(dt,paths[i],"reset-gpio",&p,&n)||n!=12||
           be(p)!=ph||be(p+4)!=10+i||be(p+8)||
           !str(dt,paths[i],"pinctrl-names","default")||
           !pa_reset_group(dt,groups[i],10+i,&handles[i])||
           !value(dt,paths[i],"pinctrl-0",handles[i]))return 0;
    }
    return handles[0]!=handles[1];
}
