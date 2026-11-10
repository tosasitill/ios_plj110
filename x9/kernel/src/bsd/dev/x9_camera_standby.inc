/* Explicit mode3 AP sensor standby transaction; no receiver MMIO. */
static struct x9_camera_config cam_config_last={.version=1};
struct cam_sensor_ap { struct cam_probe_bus *bus; uint8_t slave; };
static int cam_sensor_ap_read(void *context, uint16_t reg, uint8_t *v)
{
    struct cam_sensor_ap *ap=context;
    const uint8_t tx[2]={(uint8_t)(reg>>8),(uint8_t)reg};
    return cam_bus_transfer(ap->bus,ap->slave,tx,2,v);
}
static int cam_sensor_ap_write(void *context, uint16_t reg, uint8_t v)
{
    struct cam_sensor_ap *ap=context;
    if(cam_sensor_forbidden(reg) && reg!=0x0101)return EPERM;
    const uint8_t tx[3]={(uint8_t)(reg>>8),(uint8_t)reg,v};
    return cam_bus_transfer(ap->bus,ap->slave,tx,3,NULL);
}
static int cam_standby_configure(struct cam_probe_journal *j, void *context)
{
    struct x9_camera_config *r=context;
    struct cam_sensor_ap ap={&j->sensor,r->sensor==1?0x1a:0x10};
    struct cam_sensor_io io={.context=&ap,.read=cam_sensor_ap_read,.write=cam_sensor_ap_write};
    return cam_sensor_configure(&io,r->sensor,r);
}
static int cam_standby(uint32_t which, struct x9_camera_config *r)
{
    memset(r,0,sizeof(*r));r->version=1;r->sensor=which;r->observed=UINT32_MAX;
    if(cam_resources_latched || cam_probe_latched || (cam_probe_done&(1u<<which))) {
        r->last_errno=cam_resources_latched||cam_probe_latched?EIO:EALREADY;
        if(cam_resources_latched || cam_probe_latched)r->flags=X9_CAM_CFG_LATCHED;
        return r->last_errno; /* preserve prior cached evidence, no hardware */
    }
    int e=cam_sensor_config_preflight(which,r);
    if(!e) {
        r->flags|=X9_CAM_CFG_ATTEMPTED;r->stage=X9_CAM_CFG_COLD;
        e=cam_sensor_transaction(which,&r->probe,cam_standby_configure,r);
        r->restore_errno=r->probe.restore_errno;
        if(r->probe.flags&X9_CAM_PROBE_RESTORED)r->flags|=X9_CAM_CFG_RESTORED;
        if(r->probe.flags&X9_CAM_PROBE_LATCHED)r->flags|=X9_CAM_CFG_LATCHED;
        if(!e)r->stage=X9_CAM_CFG_DONE;
        else if((r->flags&X9_CAM_CFG_VERIFIED) && r->restore_errno)r->stage=X9_CAM_CFG_RESTORE;
    }
    r->last_errno=e;cam_config_last=*r;
    printf("X9 CAMERA STANDBY: sensor=%u flags=%x stage=%u pairs=%u/%u writes=%u/%u "
        "index=%u reg=%04x expected=%02x observed=%08x read=%u errno=%u restore=%u blockers=%x (no CSI/DMA, uncalibrated)\n",
        which,r->flags,r->stage,r->init_pairs,r->preview_pairs,r->written,r->attempted,
        r->table_index,r->reg,r->expected,r->observed,r->read_count,r->last_errno,
        r->restore_errno,r->blockers);
    return e;
}
