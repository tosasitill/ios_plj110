/* Current-FDT I2S4 metadata only. OPPO mt6993-afe-gpio.c requests BOTH
 * I2SIN4 and I2SOUT4 for OUT4; pinctrl-paris.c input-schmitt-enable sets
 * DIR=0/SMT=1, NOT IES. No MMIO or ownership is granted by this walker. */
static int i2s4_enabled(uintptr_t dt,const char *path)
{
    const uint8_t *p;size_t n;int r=fdt_property_checked(dt,path,"status",&p,&n);
    return r==0 || (r==1&&eq(p,n,"okay"));
}
static int i2s4_index(uintptr_t dt,const char *path,const char *key,const char *name,unsigned index)
{
    const uint8_t *p;size_t n;
    if(!fdt_property(dt,path,key,&p,&n)||!n||n>2048)return 0;
    size_t at=0;unsigned i=0,matches=0;
    while(at<n) {
        size_t end=at;while(end<n&&p[end])end++;
        if(end==n||end==at)return 0;
        if(eq(p+at,end-at+1,name)){if(i!=index)return 0;matches++;}
        at=end+1;i++;
    }
    return matches==1;
}
static int i2s4_cell(uintptr_t dt,const char *path,const char *key,uint32_t v)
{
    const uint8_t *p;size_t n;
    return fdt_property(dt,path,key,&p,&n)&&n==28&&be(p+16)==v;
}
static int i2s4_group(uintptr_t dt,const char *group,unsigned first,unsigned count,uint32_t *handle)
{
    const char group_keys[][32]={"phandle"};
    const char leaf_keys[][32]={"pinmux","input-schmitt-enable","bias-disable","phandle"};
    const char leaves[][16]={"/pins-cmd1-dat","/pins-cmd2-dat","/pins-cmd3-dat"};
    struct fdt_node_property props[4];uint32_t mask;unsigned children;
    if(fdt_node_schema(dt,group,group_keys,1,props,&mask,&children)!=FDT_PROPERTY_PRESENT||
       mask!=1||children!=count||props[0].bytes!=4)return 0;
    uint32_t ph=be(props[0].value);
    if(fdt_phandle_unique(dt,group,ph)!=FDT_PROPERTY_PRESENT)return 0;
    for(unsigned i=0;i<count;i++) {
        char path[96];size_t at=0;
        while(group[at]){if(at>=sizeof(path)-16)return 0;path[at]=group[at];at++;}
        unsigned j=0;do{path[at++]=leaves[i][j];}while(leaves[i][j++]);
        if(fdt_node_schema(dt,path,leaf_keys,4,props,&mask,&children)!=FDT_PROPERTY_PRESENT||
           (mask!=7&&mask!=15)||children||props[0].bytes!=4||
           be(props[0].value)!=((first+i)<<8|5)||props[1].bytes||props[2].bytes)return 0;
        if((mask&8)&&(props[3].bytes!=4||
           fdt_phandle_unique(dt,path,be(props[3].value))!=FDT_PROPERTY_PRESENT))return 0;
    }
    *handle=ph;return 1;
}
static int qualify_i2s4(uintptr_t dt)
{
    const char *pc="/soc/pinctrl",*afe="/soc/mt6993-afe-pcm@1a110000",*sound="/soc/sound";
    const uint8_t *p;size_t n;uint32_t base,bytes,in,out,ph;
    if(!str(dt,pc,"compatible","mediatek,mt6993-pinctrl")||!i2s4_enabled(dt,pc)||
       !named_reg(dt,pc,"gpio",&base,&bytes)||base!=0x1002d000||bytes!=0x1000||
       !named_reg(dt,pc,"iocfg_lb2",&base,&bytes)||base!=0x12860000||bytes!=0x1000||
       !str(dt,afe,"compatible","mediatek,mt6993-sound")||!i2s4_enabled(dt,afe)||
       !fdt_property(dt,afe,"reg",&p,&n)||n!=16||be(p)||be(p+4)!=0x1a110000||
       be(p+8)||be(p+12)!=0x10000||
       !i2s4_index(dt,afe,"pinctrl-names","aud-gpio-i2sin4-on",15)||
       !i2s4_index(dt,afe,"pinctrl-names","aud-gpio-i2sout4-on",17)||
       !i2s4_group(dt,"/soc/pinctrl/aud-gpio-i2sin4-on",108,3,&in)||
       !i2s4_group(dt,"/soc/pinctrl/aud-gpio-i2sout4-on",111,1,&out)||in==out||
       !value(dt,afe,"pinctrl-15",in)||!value(dt,afe,"pinctrl-17",out))return 0;
    const char cells[][24]={"etdm-out-ch","etdm-in-ch","etdm-out-sync","etdm-in-sync",
        "etdm-out-mclk","etdm-in-mclk","etdm-vlp","etdm-ip-mode"};
    const uint32_t values[]={2,2,0,1,0,0,1,0};
    for(unsigned i=0;i<8;i++)if(!i2s4_cell(dt,afe,cells[i],values[i]))return 0;
    /* mtk-sp-spk-amp.h's SEPARATE machine enum: IN4=24 / OUT4=25;
     * mtk-sp-spk-amp.c maps them by DAI names, not mt6993 DAI enum ids. */
    if(!str(dt,sound,"compatible","mediatek,mt6993-mt6681-sound")||!i2s4_enabled(dt,sound)||
       !word(dt,afe,"phandle",&ph,0)||fdt_phandle_unique(dt,afe,ph)!=FDT_PROPERTY_PRESENT||
       !value(dt,sound,"mediatek,platform",ph)||
       !fdt_property(dt,sound,"mediatek,spk-i2s",&p,&n)||n!=8||be(p)!=25||be(p+4)!=24)return 0;
    return 1;
}
