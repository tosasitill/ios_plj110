/* Existing mode3 standby now exercises the persistent engine, then closes
 * immediately. No new diagnostics or receiver MMIO. */
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
        struct x9_camera_probe probe;
        uint64_t epoch=0;
        int off=0;
        r->flags|=X9_CAM_CFG_ATTEMPTED;r->stage=X9_CAM_CFG_COLD;
        e=cam_sensor_session_open_locked(which,&epoch,&probe);
        if(!epoch)r->flags&=~X9_CAM_CFG_ATTEMPTED;
        if(!e)e=cam_sensor_session_configure_locked(epoch,r);
        /* Failed open may own pins/rails/FIFO. Only an admitted epoch may
         * unwind: EBUSY must not restore another session's journal. */
        if(epoch)off=cam_sensor_session_close_locked(epoch,&probe);
        r->probe=probe;
        r->restore_errno=r->probe.restore_errno;
        if(r->probe.flags&X9_CAM_PROBE_RESTORED)r->flags|=X9_CAM_CFG_RESTORED;
        if(r->probe.flags&X9_CAM_PROBE_LATCHED)r->flags|=X9_CAM_CFG_LATCHED;
        if(!e && !off){r->stage=X9_CAM_CFG_DONE;cam_probe_done|=1u<<which;}
        else if((r->flags&X9_CAM_CFG_VERIFIED) && r->restore_errno)r->stage=X9_CAM_CFG_RESTORE;
        if(!e)e=off;
        if(epoch)cam_sensor_cold_log(&probe);
    }
    r->last_errno=e;cam_config_last=*r;
    printf("X9 CAMERA STANDBY: sensor=%u flags=%x stage=%u pairs=%u/%u writes=%u/%u "
        "index=%u reg=%04x expected=%02x observed=%08x read=%u errno=%u restore=%u blockers=%x (no CSI/DMA, uncalibrated)\n",
        which,r->flags,r->stage,r->init_pairs,r->preview_pairs,r->written,r->attempted,
        r->table_index,r->reg,r->expected,r->observed,r->read_count,r->last_errno,
        r->restore_errno,r->blockers);
    return e;
}
