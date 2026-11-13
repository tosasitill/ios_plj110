/* Current-FDT I2S4 metadata only. OPPO mt6993-afe-gpio.c requests BOTH
 * I2SIN4 and I2SOUT4 for OUT4; pinctrl-paris.c input-schmitt-enable sets
 * DIR=0/SMT=1, NOT IES. No MMIO or ownership is granted by this walker. */
static int i2s4_index(const struct audio_fdt *a,unsigned node,const char *key,const char *name,unsigned index)
{
    const uint8_t *p;size_t n;
    if(!get(a,node,key,&p,&n)||!n||n>2048)return 0;
    size_t at=0;unsigned i=0,matches=0;
    while(at<n) {
        size_t end=at;while(end<n&&p[end])end++;
        if(end==n||end==at)return 0;
        if(eq(p+at,end-at+1,name)){if(i!=index)return 0;matches++;}
        at=end+1;i++;
    }
    return matches==1;
}
static int i2s4_cell(const struct audio_fdt *a,unsigned node,const char *key,uint32_t v)
{
    const uint8_t *p;size_t n;
    return get(a,node,key,&p,&n)&&n==28&&be(p+16)==v;
}
/* Group node: only a phandle and exactly `count` children, which are the
 * collected pins-cmd{1..count}-dat leaves (fixed paths, AN_* order). */
static int i2s4_group(struct audio_fdt *a,unsigned group,unsigned leaf,unsigned first,unsigned count,uint32_t *handle)
{
    static const char group_keys[][32]={"phandle"};
    static const char leaf_keys[][32]={"pinmux","input-schmitt-enable","bias-disable","phandle"};
    struct fdt_node_property props[4];uint32_t mask;unsigned children;
    if(fdt_collect_schema(&a->c,group,group_keys,1,props,&mask,&children)!=FDT_PROPERTY_PRESENT||
       mask!=1||children!=count||props[0].bytes!=4)return 0;
    uint32_t ph=be(props[0].value);
    if(!audio_fdt_need(a,ph,group,AF_I2S4))return 0;
    for(unsigned i=0;i<count;i++) {
        if(fdt_collect_schema(&a->c,leaf+i,leaf_keys,4,props,&mask,&children)!=FDT_PROPERTY_PRESENT||
           (mask!=7&&mask!=15)||children||props[0].bytes!=4||
           be(props[0].value)!=((first+i)<<8|5)||props[1].bytes||props[2].bytes)return 0;
        if((mask&8)&&(props[3].bytes!=4||!audio_fdt_need(a,be(props[3].value),leaf+i,AF_I2S4)))return 0;
    }
    *handle=ph;return 1;
}
static int qualify_i2s4(struct audio_fdt *a)
{
    const uint8_t *p;size_t n;uint32_t base,bytes,in,out,ph;
    if(!str(a,AN_PINCTRL,"compatible","mediatek,mt6993-pinctrl")||!enabled(a,AN_PINCTRL)||
       !named_reg(a,AN_PINCTRL,"gpio",&base,&bytes)||base!=0x1002d000||bytes!=0x1000||
       !named_reg(a,AN_PINCTRL,"iocfg_lb2",&base,&bytes)||base!=0x12860000||bytes!=0x1000||
       !str(a,AN_AFE,"compatible","mediatek,mt6993-sound")||!enabled(a,AN_AFE)||
       !get(a,AN_AFE,"reg",&p,&n)||n!=16||be(p)||be(p+4)!=0x1a110000||
       be(p+8)||be(p+12)!=0x10000||
       !i2s4_index(a,AN_AFE,"pinctrl-names","aud-gpio-i2sin4-on",15)||
       !i2s4_index(a,AN_AFE,"pinctrl-names","aud-gpio-i2sout4-on",17)||
       !i2s4_group(a,AN_IN4,AN_IN4_1,108,3,&in)||
       !i2s4_group(a,AN_OUT4,AN_OUT4_1,111,1,&out)||in==out||
       !value(a,AN_AFE,"pinctrl-15",in)||!value(a,AN_AFE,"pinctrl-17",out))return 0;
    static const char cells[][24]={"etdm-out-ch","etdm-in-ch","etdm-out-sync","etdm-in-sync",
        "etdm-out-mclk","etdm-in-mclk","etdm-vlp","etdm-ip-mode"};
    static const uint32_t values[]={2,2,0,1,0,0,1,0};
    for(unsigned i=0;i<8;i++)if(!i2s4_cell(a,AN_AFE,cells[i],values[i]))return 0;
    /* mtk-sp-spk-amp.h's SEPARATE machine enum: IN4=24 / OUT4=25;
     * mtk-sp-spk-amp.c maps them by DAI names, not mt6993 DAI enum ids. */
    if(!str(a,AN_SOUND,"compatible","mediatek,mt6993-mt6681-sound")||!enabled(a,AN_SOUND)||
       !word(a,AN_AFE,"phandle",&ph,0)||!audio_fdt_need(a,ph,AN_AFE,AF_I2S4)||
       !value(a,AN_SOUND,"mediatek,platform",ph)||
       !get(a,AN_SOUND,"mediatek,spk-i2s",&p,&n)||n!=8||be(p)!=25||be(p+4)!=24)return 0;
    return 1;
}
