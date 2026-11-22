/* HHUWIDE/JN5 mixed protocol: banked 16-bit initialization; the vendor uses
 * ordinary 8-bit stream/group/mirror controls after returning to page4000.
 * Tables remain generated public-vendor inputs, never a Sony byte fallback.
 * A failed/ambiguous page write is terminal; no page-reset recovery/retry. */
static bool cam_uw_page(uint16_t page)
{ return page==0x4000 || page==0x4001 || page==0x2000 || page==0x2001 || page==0x2002 || page==0x2006; }
static int cam_uw_table_check(const uint16_t *p,uint32_t n,bool pre)
{
    static const uint16_t reset[]={0xfcfc,0x4000,0,0xe,0,0x38e5,0x6018,1,
        0x7002,0x408,0x6014,1,0xfcfc,0x2002,0x1e92,0x8000,
        0x1e84,0x282b,0x1e86,0x320,0xfcfc,0x4000,0x7002,8};
    uint16_t page=0;
    if(!p || !n || n%2 || n>8192)return ENOTSUP;
    if(pre && (n!=sizeof(reset)/sizeof(reset[0]) || memcmp(p,reset,sizeof(reset))))return EINVAL;
    for(uint32_t i=0;i<n;i+=2) {
        uint16_t reg=p[i],v=p[i+1];
        if(reg==0xfcfc) {if(!cam_uw_page(v))return EINVAL;page=v;continue;}
        if(!page)return EINVAL;
        /* 4001:0100 is internal RAM, NOT the live stream control. */
        if(page==0x4000 && (reg==0x0100 || reg==0x0101 || reg==0x0103 ||
           reg==0x0104 || reg==0x0016 || reg==0x0017))return EPERM;
        if(page==0x4000 && (reg==0 || reg==0x6014 || reg==0x6018) && !pre)return EPERM;
    }
    return page==0x4000?0:EINVAL;
}
static int cam_uw_table_value(const uint16_t *p,uint32_t n,uint16_t reg,uint32_t *value)
{
    uint16_t page=0;bool found=false;
    for(uint32_t i=0;i<n;i+=2) {
        if(p[i]==0xfcfc)page=p[i+1];
        else if(page==0x4000 && p[i]==reg) {*value=p[i+1];found=true;}
    }
    return found?0:ENOENT;
}
static int cam_uw_preflight(struct x9_camera_config *r)
{
    static const uint16_t controls[]={0x0100,0,0x0104,0,0x0101,0,0x0136,0x1800,
        0x0112,0x0a0a,0x0114,0x0301,0x0340,6408,0x0342,4784,
        0x034c,4096,0x034e,3072,0x0304,3,0x0306,0xe6,
        0x030e,3,0x0310,0xb2,0x0202,20,0x0702,0,0x0704,0,0x0204,32};
    const uint16_t *tables[]={x9_hhuwide_pre_long,x9_hhuwide_init_long,
        x9_hhuwide_pre_short,x9_hhuwide_init_short,x9_hhuwide_preview};
    const uint32_t counts[]={X9_HHUWIDE_PRE_LONG_COUNT,X9_HHUWIDE_INIT_LONG_COUNT,
        X9_HHUWIDE_PRE_SHORT_COUNT,X9_HHUWIDE_INIT_SHORT_COUNT,X9_HHUWIDE_PREVIEW_COUNT};
    memset(r->sample,0,sizeof(r->sample));r->flags&=~X9_CAM_CFG_TABLE_VALID;
    r->stage=X9_CAM_CFG_TABLE_CHECK;r->observed=UINT32_MAX;
    r->blockers=X9_CAM_CFG_NEED_FACTORY|X9_CAM_CFG_NEED_RECEIVER|X9_CAM_CFG_NEED_DMA_MAP|X9_CAM_CFG_NEED_DRAIN;
    for(unsigned i=0;i<5;i++) {int e=cam_uw_table_check(tables[i],counts[i],i==0 || i==2);if(e)return e;}
    for(unsigned i=0;i<sizeof(controls)/sizeof(controls[0]);i+=2) {
        uint32_t v=UINT32_MAX;uint16_t reg=controls[i];
        if(reg!=0x0100 && reg!=0x0104 && reg!=0x0101 && reg!=0x0204) {
            int e=cam_uw_table_value(x9_hhuwide_preview,X9_HHUWIDE_PREVIEW_COUNT,reg,&v);
            if(e || v!=controls[i+1]) {r->reg=reg;r->expected=controls[i+1];return EINVAL;}
        }
        r->sample[i/2]=(struct x9_camera_config_sample){reg,controls[i+1],UINT32_MAX};
    }
    /* Module-dependent init count is recorded only after the real0010 read. */
    r->init_pairs=0;r->preview_pairs=X9_HHUWIDE_PREVIEW_COUNT/2;
    r->flags|=X9_CAM_CFG_TABLE_VALID;return 0;
}
static int cam_uw_write(struct cam_sensor_io *io,uint16_t reg,uint16_t v,struct x9_camera_config *r)
{
    r->reg=reg;r->expected=v;r->observed=UINT32_MAX;
    if(mach_absolute_time()>=io->deadline)return ETIMEDOUT;
    r->attempted++;
    int e=io->write_word(io->context,reg,v);
    if(!e)r->written++;
    if(!e && mach_absolute_time()>=io->deadline)e=ETIMEDOUT;
    return e;
}
static int cam_uw_load(struct cam_sensor_io *io,const uint16_t *p,uint32_t n,struct x9_camera_config *r)
{
    for(uint32_t i=0;i<n;i+=2) {r->table_index=i/2;int e=cam_uw_write(io,p[i],p[i+1],r);if(e)return e;}
    return 0;
}
static int cam_uw_configure(struct cam_sensor_io *io,struct x9_camera_config *r)
{
    uint16_t module=0;uint8_t byte=0;
    if(!io->read_word || !io->write_word)return ENOTSUP;
    r->stage=X9_CAM_CFG_IDLE;
    int e=io->read(io->context,0x0100,&byte);if(!e && byte)e=EBUSY;
    if(!e)e=io->read(io->context,0x0104,&byte);if(!e && byte)e=EBUSY;
    if(e)return e;
    r->flags|=X9_CAM_CFG_STANDBY;
    e=cam_uw_write(io,0xfcfc,0x4000,r);
    if(!e)e=io->read_word(io->context,0x0010,&module);
    if(!e && mach_absolute_time()>=io->deadline)e=ETIMEDOUT;
    if(e)return e;
    bool short_module=(module|0xff)==0x7ff;
    const uint16_t *pre=short_module?x9_hhuwide_pre_short:x9_hhuwide_pre_long;
    const uint16_t *init=short_module?x9_hhuwide_init_short:x9_hhuwide_init_long;
    uint32_t np=short_module?X9_HHUWIDE_PRE_SHORT_COUNT:X9_HHUWIDE_PRE_LONG_COUNT;
    uint32_t ni=short_module?X9_HHUWIDE_INIT_SHORT_COUNT:X9_HHUWIDE_INIT_LONG_COUNT;
    r->init_pairs=(np+ni)/2;r->stage=X9_CAM_CFG_INIT;
    e=cam_uw_load(io,pre,np,r);
    if(!e)delay(5000); /* vendor reset phase, no blind poll */
    if(!e)e=cam_uw_load(io,init,ni,r);
    if(!e) {r->stage=X9_CAM_CFG_PREVIEW;e=cam_uw_load(io,x9_hhuwide_preview,X9_HHUWIDE_PREVIEW_COUNT,r);}
    if(!e)e=cam_uw_write(io,0x0204,32,r); /* explicit unity, not an assumed reset value */
    if(!e) {
        r->stage=X9_CAM_CFG_MIRROR;r->reg=0x0101;r->expected=0;r->observed=UINT32_MAX;
        if(mach_absolute_time()>=io->deadline)e=ETIMEDOUT;
        else {r->attempted++;e=io->write(io->context,0x0101,0);if(!e)r->written++;}
    }
    if(e)return e;
    r->flags|=X9_CAM_CFG_WRITTEN;r->stage=X9_CAM_CFG_READBACK;
    for(unsigned i=0;i<X9_CAM_CFG_SAMPLES && r->sample[i].reg;i++) {
        struct x9_camera_config_sample *s=&r->sample[i];uint16_t v=0;
        r->table_index=i;r->reg=s->reg;r->expected=s->expected;r->observed=UINT32_MAX;
        if(mach_absolute_time()>=io->deadline)return ETIMEDOUT;
        /* Byte controls must not demand an unqualified adjacent register. */
        if(s->reg==0x0100 || s->reg==0x0101 || s->reg==0x0104) {
            uint8_t b=0;e=io->read(io->context,(uint16_t)s->reg,&b);v=b;
        } else e=io->read_word(io->context,(uint16_t)s->reg,&v);
        if(e)return e;
        if(mach_absolute_time()>=io->deadline)return ETIMEDOUT;
        r->observed=s->observed=v;r->read_count++;
        if(v!=s->expected)return EIO;
    }
    r->flags|=X9_CAM_CFG_VERIFIED;return 0;
}
