/* Common bounded sensor table/readback path. No MMIO or platform registration.
 * Expected controls are public hhmain/hhfront preview mode facts, not a copied
 * vendor initialization program. Generated register tables stay out of git.
 * This is nominal standby only: no QSC/LRC, front fab OTP or AF/OIS access. */
_Static_assert(sizeof(struct x9_camera_config)==720,"camera config ABI");
struct cam_sensor_io {
    void *context;
    int (*read)(void *, uint16_t, uint8_t *);
    int (*write)(void *, uint16_t, uint8_t);
    uint64_t deadline;
};
struct cam_sensor_tables { const uint16_t *init, *preview; uint32_t ni, np; };
static struct cam_sensor_tables cam_sensor_tables(uint32_t which)
{
    if(which==X9_CAMERA_BACK)return (struct cam_sensor_tables){x9_hhmain_init,
        x9_hhmain_preview,X9_HHMAIN_INIT_COUNT,X9_HHMAIN_PREVIEW_COUNT};
    return (struct cam_sensor_tables){x9_hhfront_init,x9_hhfront_preview,
        X9_HHFRONT_INIT_COUNT,X9_HHFRONT_PREVIEW_COUNT};
}
static bool cam_sensor_forbidden(uint16_t reg)
{
    /* 0101 is applied separately with verified mounting-specific value.
     * Do not let a generated table start streaming, reset, hold updates,
     * enter OTP programming/read mode or alter sensor ID registers. */
    return reg==0x0100 || reg==0x0101 || reg==0x0103 || reg==0x0104 ||
        reg==0x0016 || reg==0x0017 || (reg>=0x0a00 && reg<=0x0aff);
}
static int cam_sensor_validate_table(const uint16_t *p, uint32_t n)
{
    if(!n || n%2 || n>4096)return ENOTSUP;
    for(uint32_t i=0;i<n;i+=2)
        if(cam_sensor_forbidden(p[i]) || p[i+1]>255)return EINVAL;
    return 0;
}
static int cam_sensor_table_value(struct cam_sensor_tables t, uint16_t reg, uint32_t *v)
{
    bool found=false;
    for(uint32_t i=0;i<t.ni;i+=2)if(t.init[i]==reg){*v=t.init[i+1];found=true;}
    for(uint32_t i=0;i<t.np;i+=2)if(t.preview[i]==reg){*v=t.preview[i+1];found=true;}
    return found?0:ENOENT;
}
static void cam_sensor_sample(struct x9_camera_config *r, uint32_t *n,
                              uint16_t reg, uint8_t value)
{
    if(*n<X9_CAM_CFG_SAMPLES)r->sample[(*n)++]=
        (struct x9_camera_config_sample){reg,value,UINT32_MAX};
}
static int cam_sensor_config_preflight(uint32_t which, struct x9_camera_config *r)
{
    struct cam_sensor_tables t=cam_sensor_tables(which);
    memset(r->sample,0,sizeof(r->sample));r->flags&=~X9_CAM_CFG_TABLE_VALID;
    r->stage=X9_CAM_CFG_TABLE_CHECK;r->observed=UINT32_MAX;
    r->init_pairs=t.ni/2;r->preview_pairs=t.np/2;
    r->blockers=X9_CAM_CFG_NEED_FACTORY|X9_CAM_CFG_NEED_RECEIVER|
        X9_CAM_CFG_NEED_DMA_MAP|X9_CAM_CFG_NEED_DRAIN;
    int e=cam_sensor_validate_table(t.init,t.ni);
    if(!e)e=cam_sensor_validate_table(t.preview,t.np);if(e)return e;
    bool back=which==X9_CAMERA_BACK;uint32_t n=0;
#define CAM_SAMPLE(reg,value) cam_sensor_sample(r,&n,reg,value)
#define CAM_WORD(reg,value) do { CAM_SAMPLE(reg,(value)>>8); CAM_SAMPLE((reg)+1,(value)&255); } while(0)
    CAM_SAMPLE(0x0100,0);CAM_SAMPLE(0x0104,0);CAM_SAMPLE(0x0101,back?3:0);
    CAM_WORD(0x0136,0x1800);CAM_SAMPLE(0x0111,back?3:2);
    CAM_SAMPLE(0x0112,10);CAM_SAMPLE(0x0113,10);CAM_SAMPLE(0x0114,back?2:3);
    CAM_WORD(0x0342,back?9696:3768);CAM_WORD(0x0340,back?7546:7642);
    CAM_WORD(0x034c,back?4096:3264);CAM_WORD(0x034e,back?3072:2448);
    CAM_WORD(0x040c,back?4096:3264);CAM_WORD(0x040e,back?3072:2448);
    CAM_SAMPLE(0x0900,1);CAM_SAMPLE(0x0901,0x22);CAM_SAMPLE(0x0902,back?0:8);
    CAM_SAMPLE(0x0301,back?8:5);CAM_SAMPLE(0x0303,2);CAM_SAMPLE(0x0305,back?4:3);
    CAM_WORD(0x0306,back?0x016e:0x010e);CAM_SAMPLE(0x030b,1);
    CAM_SAMPLE(0x030d,back?4:12);CAM_WORD(0x030e,back?0x0226:0x0438);
    if(!back)CAM_SAMPLE(0x0310,1);
    CAM_WORD(0x0202,back?1000:7594);CAM_WORD(0x0204,back?0:112);
    if(back){CAM_SAMPLE(0x0110,1);CAM_SAMPLE(0x30a2,3);CAM_SAMPLE(0x30a4,3);CAM_SAMPLE(0x30a6,3);}
#undef CAM_WORD
#undef CAM_SAMPLE
    for(uint32_t i=0;i<n;i++) {
        struct x9_camera_config_sample *s=&r->sample[i];uint32_t value=UINT32_MAX;
        if(s->reg==0x0100 || s->reg==0x0101 || s->reg==0x0104)continue;
        e=cam_sensor_table_value(t,(uint16_t)s->reg,&value);
        if(e || value!=s->expected) {
            r->reg=s->reg;r->expected=s->expected;
            /* This is a source-table mismatch, not a hardware observation. */
            return EINVAL;
        }
    }
    r->flags|=X9_CAM_CFG_TABLE_VALID;return 0;
}
static int cam_sensor_io_read(struct cam_sensor_io *io, uint16_t reg, uint8_t *v)
{
    if(mach_absolute_time()>=io->deadline)return ETIMEDOUT;
    return io->read(io->context,reg,v);
}
static int cam_sensor_io_write(struct cam_sensor_io *io, uint16_t reg, uint8_t v)
{
    if(mach_absolute_time()>=io->deadline)return ETIMEDOUT;
    return io->write(io->context,reg,v);
}
static int cam_sensor_load_table(struct cam_sensor_io *io, const uint16_t *p,
                                 uint32_t n, struct x9_camera_config *r)
{
    for(uint32_t i=0;i<n;i+=2) {
        r->table_index=i/2;r->reg=p[i];r->expected=p[i+1];r->observed=UINT32_MAX;
        /* Attempted precedes I2C: a failure can follow an ambiguous write. */
        r->attempted++;
        int e=cam_sensor_io_write(io,p[i],(uint8_t)p[i+1]);if(e)return e;
        r->written++;
    }
    return 0;
}
static int cam_sensor_configure(struct cam_sensor_io *io, uint32_t which,
                                 struct x9_camera_config *r)
{
    uint64_t span;nanoseconds_to_absolutetime(5000000000ULL,&span);
    io->deadline=mach_absolute_time()+span;
    struct cam_sensor_tables t=cam_sensor_tables(which);uint8_t v=0;
    /* Cold reset plus explicit standby/hold read before the FIRST table write.
     * Refuse unexpected streaming, never issue stream-off as a blind recovery. */
    r->stage=X9_CAM_CFG_IDLE;
    const uint16_t idle[]={0x0100,0x0104};
    for(unsigned i=0;i<2;i++) {
        r->reg=idle[i];r->expected=0;r->observed=UINT32_MAX;
        int e=cam_sensor_io_read(io,idle[i],&v);if(e)return e;
        r->observed=v;if(v)return EBUSY;
    }
    r->flags|=X9_CAM_CFG_STANDBY;r->stage=X9_CAM_CFG_INIT;
    int e=cam_sensor_load_table(io,t.init,t.ni,r);
    if(!e){r->stage=X9_CAM_CFG_PREVIEW;e=cam_sensor_load_table(io,t.preview,t.np,r);}
    if(e)return e;
    r->stage=X9_CAM_CFG_MIRROR;r->reg=0x0101;r->expected=which==1?3:0;
    r->table_index=0;r->observed=UINT32_MAX;r->attempted++;
    e=cam_sensor_io_write(io,0x0101,(uint8_t)r->expected);if(e)return e;
    r->written++;r->flags|=X9_CAM_CFG_WRITTEN;r->stage=X9_CAM_CFG_READBACK;
    /* Read only stable controls, NOT vendor microcode/write-only commands.
     * First two samples recheck stream/hold after all table writes. */
    for(unsigned i=0;i<X9_CAM_CFG_SAMPLES && r->sample[i].reg;i++) {
        struct x9_camera_config_sample *s=&r->sample[i];
        r->table_index=i;r->reg=s->reg;r->expected=s->expected;r->observed=UINT32_MAX;
        e=cam_sensor_io_read(io,(uint16_t)s->reg,&v);if(e)return e;
        r->observed=s->observed=v;r->read_count++;
        if(v!=s->expected)return EIO;
    }
    r->flags|=X9_CAM_CFG_VERIFIED;return 0;
}
