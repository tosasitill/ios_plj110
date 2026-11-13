/* PLJ110 PA reset topology ONLY. OPPO tfa98xx_ext_reset with polarity0
 * asserts LOW for 1ms, then deasserts HIGH for 1ms. No right-rail inference
 * from the LEFT supply property, dummy regulator, ACK or reset metadata. */
static int pa_reset_group(struct audio_fdt *a,unsigned group,unsigned leaf,unsigned pin,uint32_t *handle)
{
    static const char keys[][32]={"phandle"};
    static const char leaf_keys[][32]={"pinmux","output-low","phandle"};
    struct fdt_node_property p[3];uint32_t mask;unsigned children;
    if(fdt_collect_schema(&a->c,group,keys,1,p,&mask,&children)!=FDT_PROPERTY_PRESENT||
       mask!=1||children!=1||p[0].bytes!=4)return 0;
    uint32_t ph=be(p[0].value);
    if(!audio_fdt_need(a,ph,group,AF_PA_RESET))return 0;
    if(fdt_collect_schema(&a->c,leaf,leaf_keys,3,p,&mask,&children)!=FDT_PROPERTY_PRESENT||
       (mask!=3&&mask!=7)||children||p[0].bytes!=4||be(p[0].value)!=(pin<<8)||p[1].bytes)return 0;
    if((mask&4)&&(p[2].bytes!=4||!audio_fdt_need(a,be(p[2].value),leaf,AF_PA_RESET)))return 0;
    *handle=ph;return 1;
}
static int qualify_pa_reset(struct audio_fdt *a)
{
    const uint8_t *p;size_t n;uint32_t base,bytes,ph;
    if(!str(a,AN_SPMI,"compatible","mediatek,mt6993-spmi")||!enabled(a,AN_SPMI)||
       !value(a,AN_SPMI,"#address-cells",2)||!value(a,AN_SPMI,"#size-cells",0)||
       !value(a,AN_SPMI,"spmi-dev-mask",0x55a8)||
       !named_reg(a,AN_SPMI,"pmif-p",&base,&bytes)||base!=0x1c018000||bytes!=0x111c||
       !str(a,AN_PMIC3,"compatible","mediatek,mt6661")||!enabled(a,AN_PMIC3)||
       !get(a,AN_PMIC3,"reg",&p,&n)||n!=8||be(p)!=3||be(p+4)||
       !str(a,AN_PMIC3_PC,"compatible","mediatek,mt6661-3-pinctrl")||!enabled(a,AN_PMIC3_PC)||
       !value(a,AN_PMIC3_PC,"#gpio-cells",2)||!get(a,AN_PMIC3_PC,"gpio-controller",&p,&n)||n||
       !word(a,AN_PMIC3_PC,"phandle",&ph,0)||!audio_fdt_need(a,ph,AN_PMIC3_PC,AF_PA_RESET))return 0;
    if(!str(a,AN_I2C6,"compatible","mediatek,mt6991-i2c")||!enabled(a,AN_I2C6)||
       !value(a,AN_I2C6,"#address-cells",1)||!value(a,AN_I2C6,"#size-cells",0)||
       !get(a,AN_I2C6,"reg",&p,&n)||n!=32||be(p)||be(p+4)!=0x12990000||
       be(p+8)||be(p+12)!=0x20000||be(p+16)||be(p+20)!=0x16410000||
       be(p+24)||be(p+28)!=0x10000)return 0;
    uint32_t handles[2]={0};
    for(unsigned i=0;i<2;i++) {
        unsigned pa=AN_PA_L+i,group=i?AN_GPIO_R:AN_GPIO_L,leaf=i?AN_GPIO_R_PIN:AN_GPIO_L_PIN;
        if(!str(a,pa,"compatible","tfa,tfa98xx")||!enabled(a,pa)||
           !value(a,pa,"reg",0x34+i)||!value(a,pa,"reset-polarity",0)||
           !value(a,pa,"channel-config",i+1)||
           !get(a,pa,"reset-gpio",&p,&n)||n!=12||
           be(p)!=ph||be(p+4)!=10+i||be(p+8)||
           !str(a,pa,"pinctrl-names","default")||
           !pa_reset_group(a,group,leaf,10+i,&handles[i])||
           !value(a,pa,"pinctrl-0",handles[i]))return 0;
    }
    return handles[0]!=handles[1];
}
