/* Module-local sensor calibration, under camera_control_lock.
 * Authority: hhmain/hhfront/hhtele eeprom_info, get_sensor_cali and
 * set_sensor_cali in camera_vendor_root; ISP8s probe_eeprom compares the
 * four bytes at EEPROM+6 as a little-endian u32. Live DT agrees on the
 * EEPROM addresses/buses: main/tele 50, front54. No EEPROM data writes,
 * guessed checksums, cross-module cache, fixed calibration or ISP tuning.
 *
 * Two complete matching EEPROM observations precede ANY sensor table store.
 * The exact payload stays in this cold epoch's journal. Sensor SRAM writes
 * are ACK receipts, not proof of physical correction or readable SRAM.
 * Preview retains its original QSC policy (front preview sets3621=0).
 * Cold physical reset/power close retires sensor-local SRAM; exported native
 * epochs archive this journal before another module can replace it.
 */
enum { CAM_CAL_MAX_BYTES=0xd80 };
struct cam_cal_block { uint16_t eeprom,bytes,sensor; };
struct cam_cal_profile {
    uint32_t header;
    uint8_t slave;
    uint16_t enable;
    bool mirror;
    struct cam_cal_block block[3];
};
static const struct cam_cal_profile cam_cal_profiles[X9_CAMERA_SENSOR_COUNT]={
    {0x015e012c,0x50,0x3206,true,
        {{0x1e30,0xc00,0xc000},{0x7c20,0xc0,0xd200},{0x7ce0,0xc0,0xd300}}},
    {0x01510008,0x54,0x3621,false,
        {{0x0c90,0x618,0xc500},{0x14b0,0x82,0x7a98},{0x1532,0x82,0x7b1c}}},
    {0}, /* Samsung's banked calibration is a separate, unimplemented path. */
    {0x01b1012f,0x50,0x3206,false,
        {{0x2210,0xc00,0xc000},{0x2e20,0xc0,0xd200},{0x2ee0,0xc0,0xd300}}}
};
static struct {
    uint64_t epoch;
    uint32_t sensor,header[2],header_after[2],read_bytes[2],bytes,writes,acked;
    uint32_t pass,block,offset,reg,final_enable;
    bool attempted,preloaded,apply_attempted,applied,mode_checked;
    int error;
    uint8_t payload[CAM_CAL_MAX_BYTES];
} cam_sensor_cal;
static int cam_cal_fault(int e)
{
    if(!cam_sensor_cal.error)cam_sensor_cal.error=e;
    return cam_sensor_cal.error;
}
static int cam_cal_guard(uint64_t end)
{
    uint32_t which=cam_sensor_cal.sensor;
    struct cam_sensor_ap ap={&cam_journal.sensor,cam_sensor_slave(which)};
    if(!x9_camera_sensor_valid(which) || cam_sensor_cal.epoch!=cam_sensor_live.epoch ||
       cam_sensor_live.probe.sensor!=which ||
       cam_sensor_live.phase!=CAM_SENSOR_CONFIGURING ||
       cam_sensor_live.close_attempted || session.capture || session.stream)return ESTALE;
    if(cam_sensor_cal.error)return cam_sensor_cal.error;
    if(mach_absolute_time()>=end)return ETIMEDOUT;
    return cam_sensor_ap_guard(&ap);
}
static int cam_cal_read(uint64_t end,uint16_t reg,uint8_t *bytes,unsigned count)
{
    const struct cam_cal_profile *p=&cam_cal_profiles[cam_sensor_cal.sensor-1];
    const uint8_t tx[2]={(uint8_t)(reg>>8),(uint8_t)reg};
    int e=cam_cal_guard(end);
    if(!count || count>2 || !bytes)return EINVAL;
    cam_sensor_cal.reg=reg;
    /* This is only the EEPROM address-pointer read used by Android, never
     * its programming/write-protection commands or an arbitrary slave. */
    if(!e)e=cam_bus_transfer_n(&cam_journal.sensor,p->slave,tx,2,bytes,count);
    if(!e)e=cam_cal_guard(end);
    return e;
}
static void cam_cal_log(const char *phase,int e)
{
    printf("X9 CAMERA SENSOR CAL: phase=%s sensor=%u epoch=%llu header=%x/%x end=%x/%x "
        "read=%u/%u bytes=%u pass=%u block=%u offset=%u reg=%x "
        "writes=%u acked=%u preloaded=%u applied=%u mode=%u/%x errno=%d "
        "(module EEPROM only; ACK is not optical calibration validation)\n",
        phase,cam_sensor_cal.sensor,cam_sensor_cal.epoch,
        cam_sensor_cal.header[0],cam_sensor_cal.header[1],
        cam_sensor_cal.header_after[0],cam_sensor_cal.header_after[1],
        cam_sensor_cal.read_bytes[0],cam_sensor_cal.read_bytes[1],cam_sensor_cal.bytes,
        cam_sensor_cal.pass,cam_sensor_cal.block,cam_sensor_cal.offset,cam_sensor_cal.reg,
        cam_sensor_cal.writes,cam_sensor_cal.acked,cam_sensor_cal.preloaded,
        cam_sensor_cal.applied,cam_sensor_cal.mode_checked,cam_sensor_cal.final_enable,e);
}
static int cam_cal_preload(uint32_t which)
{
    const struct cam_cal_profile *p=&cam_cal_profiles[which-1];
    uint64_t span,end;
    int e=0;
    /* configure is single-attempt for this epoch. A later genuine cold
     * epoch may replace only after the preceding physical close/recycle. */
    if(cam_sensor_cal.attempted && cam_sensor_cal.epoch==cam_sensor_live.epoch)return EALREADY;
    bzero(&cam_sensor_cal,sizeof(cam_sensor_cal));
    cam_sensor_cal.epoch=cam_sensor_live.epoch;cam_sensor_cal.sensor=which;
    cam_sensor_cal.attempted=true;
    if(!p->header){cam_cal_log("unsupported",0);return 0;}
    for(unsigned b=0;b<3;b++)cam_sensor_cal.bytes+=p->block[b].bytes;
    if(cam_sensor_cal.bytes>CAM_CAL_MAX_BYTES)return cam_cal_fault(EOVERFLOW);
    nanoseconds_to_absolutetime(15000000000ULL,&span);end=mach_absolute_time()+span;
    for(unsigned pass=0;pass<2 && !e;pass++) {
        uint8_t header[4]={0};
        unsigned index=0;
        cam_sensor_cal.pass=pass;
        for(unsigned n=0;n<4 && !e;n+=2)e=cam_cal_read(end,(uint16_t)(6+n),header+n,2);
        if(!e) {
            cam_sensor_cal.header[pass]=(uint32_t)header[0]|((uint32_t)header[1]<<8)|
                ((uint32_t)header[2]<<16)|((uint32_t)header[3]<<24);
            if(cam_sensor_cal.header[pass]!=p->header)e=ENODEV;
        }
        for(unsigned b=0;b<3 && !e;b++) {
            const struct cam_cal_block *d=&p->block[b];
            cam_sensor_cal.block=b;
            for(unsigned n=0;n<d->bytes && !e;n+=2) {
                uint8_t bytes[2]={0};
                unsigned count=d->bytes-n>1?2:1;
                cam_sensor_cal.offset=n;e=cam_cal_read(end,(uint16_t)(d->eeprom+n),bytes,count);
                if(!e) {
                    cam_sensor_cal.read_bytes[pass]+=count;
                    if(!pass)memcpy(cam_sensor_cal.payload+index+n,bytes,count);
                    else if(memcmp(cam_sensor_cal.payload+index+n,bytes,count))e=ESTALE;
                }
            }
            index+=d->bytes;
        }
        /* Fence each complete payload by another actual same header read. */
        for(unsigned n=0;n<4 && !e;n+=2)e=cam_cal_read(end,(uint16_t)(6+n),header+n,2);
        if(!e) {
            cam_sensor_cal.header_after[pass]=(uint32_t)header[0]|((uint32_t)header[1]<<8)|
                ((uint32_t)header[2]<<16)|((uint32_t)header[3]<<24);
            if(cam_sensor_cal.header_after[pass]!=p->header)e=ESTALE;
        }
    }
    if(!e)cam_sensor_cal.preloaded=true;
    else cam_cal_fault(e);
    cam_cal_log("preload",e);return e;
}
static int cam_cal_write(struct cam_sensor_io *io,uint16_t reg,uint8_t value,
    struct x9_camera_config *r)
{
    uint32_t acked=r->written;
    int e=cam_cal_guard(io->deadline);if(e)return e;
    cam_sensor_cal.reg=reg;cam_sensor_cal.writes++; /* before ambiguous START */
    e=cam_sensor_config_write(io,reg,value,r,false);
    /* Keep a real ACK even when the enclosing deadline expired just after
     * it; the common writer distinguishes that from an ambiguous transfer. */
    cam_sensor_cal.acked+=r->written-acked;
    return e;
}
static int cam_cal_apply(struct cam_sensor_io *io,uint32_t which,struct x9_camera_config *r)
{
    const struct cam_cal_profile *p=&cam_cal_profiles[which-1];
    unsigned index=0;
    int e=cam_cal_guard(io->deadline);
    if(!e && (!cam_sensor_cal.preloaded || cam_sensor_cal.sensor!=which ||
       cam_sensor_cal.apply_attempted || !p->header))e=ESTALE;
    if(e)return cam_cal_fault(e);
    cam_sensor_cal.apply_attempted=true;
    r->stage=X9_CAM_CFG_CALIBRATION;r->table_index=0;
    /* Main set_sensor_cali applies mounting mirror before QSC. Front/tele
     * have no such write. The ordinary end-of-mode mirror remains intact. */
    if(p->mirror)e=cam_cal_write(io,0x0101,(uint8_t)sensors[which-1].mirror,r);
    for(unsigned b=0;b<3 && !e;b++) {
        const struct cam_cal_block *d=&p->block[b];
        cam_sensor_cal.block=b;
        for(unsigned n=0;n<d->bytes && !e;n++) {
            cam_sensor_cal.offset=n;r->table_index=index+n;
            e=cam_cal_write(io,(uint16_t)(d->sensor+n),cam_sensor_cal.payload[index+n],r);
        }
        index+=d->bytes;
        /* Android enables QSC immediately after QSC data, before SPC/LRC. */
        if(!b && !e)e=cam_cal_write(io,p->enable,1,r);
    }
    if(!e){cam_sensor_cal.applied=true;r->flags|=X9_CAM_CFG_MODULE_ACKED;}
    else cam_cal_fault(e);
    cam_cal_log("apply",e);return e;
}
static int cam_cal_mode_check(uint32_t which,struct x9_camera_config *r)
{
    const struct cam_cal_profile *p=&cam_cal_profiles[which-1];
    struct cam_sensor_ap ap={&cam_journal.sensor,cam_sensor_slave(which)};
    struct cam_sensor_tables t=cam_sensor_tables(which);
    uint32_t expected=1;
    uint8_t actual=0;
    int e=0;
    if(!p->header)return 0;
    if(cam_sensor_cal.epoch!=cam_sensor_live.epoch || cam_sensor_cal.sensor!=which ||
       !cam_sensor_cal.applied || cam_sensor_cal.error)return ESTALE;
    /* Preview is allowed to override the open-time QSC enable. In
     * particular front preview's3621=0 must not be "repaired" to1. */
    for(unsigned n=0;n<t.np;n+=2)if(t.preview[n]==p->enable)expected=t.preview[n+1];
    r->stage=X9_CAM_CFG_CAL_MODE;r->reg=p->enable;r->expected=expected;r->observed=UINT32_MAX;
    e=cam_sensor_ap_read(&ap,p->enable,&actual);
    if(!e){cam_sensor_cal.final_enable=actual;r->observed=actual;}
    if(!e && actual!=expected)e=EIO;
    if(!e)cam_sensor_cal.mode_checked=true;
    else cam_cal_fault(e);
    cam_cal_log("mode",e);return e;
}
