/* Included under the existing board mutex. This cycle owns no DMA buffer,
 * pad/PA lease or backend. All engines/memifs stay disabled throughout. */
static X9AudioPathStatus path_s={.version=X9_AUDIO_PATH_STATUS_VERSION};
static bool path_captured;
static struct field path_fields[]={
    X9_SPEAKER_OUT4_COWORK_DISABLED,
    X9_SPEAKER_DL24_DISABLED_FORMAT,
    X9_SPEAKER_DL24_DISABLED_ROUTES
};
static const unsigned path_dma_regs[]={0x46b0,0x46b4,0x46c0,0x46c4};
static const unsigned path_control_regs[]={0x46d8,0x46dc,0x98};
static int path_refuse(unsigned off,uint32_t mask,uint32_t wanted)
{
    int e=refuse(j.afe,off,mask,wanted);
    if(e){path_s.last_register=off;path_s.last_value=s.last_value;}return e;
}
static int path_access(void)
{
    int e=idle();
    if(e){path_s.last_register=s.last_register;path_s.last_value=s.last_value;return e;}
    e=pll_consumers();
    if(e){path_s.last_register=pll_s.last_register;path_s.last_value=pll_s.last_value;}
    return e;
}
static void path_snapshot(bool after)
{
    uint32_t *routes=after?path_s.routes_after:path_s.routes_before;
    uint32_t *dma=after?path_s.dma_after:path_s.dma_before;
    uint32_t *control=after?path_s.control_after:path_s.control_before;
    for(unsigned ch=0;ch<2;ch++)for(unsigned n=0;n<7;n++)
        routes[ch*7+n]=rd(j.afe,0x2f00+ch*32+x9_mic_route_words[n]*4);
    for(unsigned n=0;n<4;n++)dma[n]=rd(j.afe,path_dma_regs[n]);
    for(unsigned n=0;n<3;n++)control[n]=rd(j.afe,path_control_regs[n]);
}
static int path_guard(void)
{
    int e=path_access();if(e)return e;
    e=path_refuse(X9_SPEAKER_DL24_CON0,X9_SPEAKER_DL24_INACTIVE_MASK,0);
    /* Only our two word2 route registers may change. Never erase another
     * source or write the reserved word3. Addresses/MEM_UP/pad stay inherited. */
    for(unsigned ch=0;!e && ch<2;ch++)for(unsigned n=0;!e && n<7;n++) {
        if(n==2)continue;
        e=path_refuse(0x2f00+ch*32+x9_mic_route_words[n]*4,UINT32_MAX,0);
    }
    for(unsigned n=0;!e && n<4;n++)e=path_refuse(path_dma_regs[n],UINT32_MAX,path_s.dma_before[n]);
    for(unsigned n=0;!e && n<3;n++)e=path_refuse(path_control_regs[n],UINT32_MAX,path_s.control_before[n]);
    return e;
}
static int path_change(unsigned n,uint32_t value)
{
    int e=path_guard();if(e)return e;
    struct field *p=&path_fields[n];uint32_t old=rd(j.afe,p->off),bit=1u<<n;
    if((old&p->mask)!=(p->expected&p->mask) &&
       !((path_s.uncertain_mask&bit) && (old&p->mask)==(p->saved&p->mask))) {
        path_s.last_register=p->off;path_s.last_value=old;return EBUSY;
    }
    uint32_t next=(old&~p->mask)|(value&p->mask);
    /* The guard requires clear/action bits zero; do not race-replay them. */
    if(p->off==X9_SPEAKER_DL24_CON0) {
        if(old&X9_SPEAKER_DL24_INACTIVE_MASK) {
            path_s.last_register=p->off;path_s.last_value=old;return EBUSY;
        }
        next&=~X9_SPEAKER_DL24_CLEAR_MASK;
    }
    if((next&p->mask)==(old&p->mask)){p->expected=next;path_s.uncertain_mask&=~bit;return 0;}
    path_s.dirty_mask|=bit;path_s.uncertain_mask|=bit;p->expected=next;
    wr(j.afe,p->off,next);uint32_t actual=rd(j.afe,p->off);
    if((actual&p->mask)!=(next&p->mask)) {
        path_s.last_register=p->off;path_s.last_value=actual;return EIO;
    }
    path_s.uncertain_mask&=~bit;return 0;
}
static int path_verify(bool saved)
{
    int e=path_guard();
    for(unsigned n=0;!e && n<4;n++) {
        struct field *p=&path_fields[n];
        e=path_refuse(p->off,p->mask,(saved?p->saved:p->expected)&p->mask);
    }
    return e;
}
static int path_take(void)
{
    path_s.stage=X9PathAdmission;
    int e=path_access();if(e)return e;
    path_snapshot(false);
    e=path_refuse(X9_SPEAKER_DL24_CON0,X9_SPEAKER_DL24_INACTIVE_MASK,0);
    for(unsigned ch=0;!e && ch<2;ch++)for(unsigned n=0;!e && n<7;n++)
        e=path_refuse(0x2f00+ch*32+x9_mic_route_words[n]*4,UINT32_MAX,0);
    if(e)return e;
    for(unsigned n=0;n<4;n++) {
        struct field *p=&path_fields[n];
        p->saved=p->expected=path_s.before[n]=rd(j.afe,p->off);
    }
    /* Reject an admission race before journal ownership or the first write. */
    if(path_fields[1].saved&X9_SPEAKER_DL24_INACTIVE_MASK) {
        path_s.last_register=X9_SPEAKER_DL24_CON0;path_s.last_value=path_fields[1].saved;return EBUSY;
    }
    for(unsigned n=2;n<4;n++)if(path_fields[n].saved) {
        path_s.last_register=path_fields[n].off;path_s.last_value=path_fields[n].saved;return EBUSY;
    }
    path_captured=true;path_s.flags|=X9PathCaptured;
    path_s.stage=X9PathConfigure;
    for(unsigned n=0;!e && n<4;n++)e=path_change(n,path_fields[n].value);
    if(e)return e;
    path_s.stage=X9PathVerify;e=path_verify(false);if(e)return e;
    for(unsigned n=0;n<4;n++)path_s.configured[n]=rd(j.afe,path_fields[n].off);
    path_s.flags|=X9PathConfigured;return 0;
}
static int path_restore(void)
{
    if(!path_captured)return 0;
    if(j.stuck || pj.stuck)return EIO;
    int e=path_guard();if(e)return e;
    for(unsigned n=4;n>0;n--)if(path_s.dirty_mask&(1u<<(n-1))) {
        e=path_change(n-1,path_fields[n-1].saved);if(e)return e;
        path_s.dirty_mask&=~(1u<<(n-1));
    }
    e=path_verify(true);if(e)return e;
    path_snapshot(true);
    for(unsigned n=0;n<4;n++)path_s.after[n]=rd(j.afe,path_fields[n].off);
    path_captured=false;return 0;
}
