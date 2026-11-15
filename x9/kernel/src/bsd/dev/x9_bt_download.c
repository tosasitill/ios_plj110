/* MT6653 firmware protocol, derived from observed Android source layouts:
 * linux_v2/btmtk_main.c: btmtk_send_wmt_download_cmd,
 * btmtk_load_fw_by_bin_info, btmtk_load_rom_patch_connac3;
 * uart/btmtktty.c: btmtk_uart_load_fw_patch_using_dma.
 * This executor requires a board-owned UART session. It does NOT initialize
 * connv3/UARTHUB, reset the chip, or expose HCI until those stages are done. */
#include "x9_bt_download.h"
#include <sys/errno.h>
#include "../../../generated/x9_bt_payload.inc"

#define FW_MAP 96u
#define MAP_BYTES 64u
#define SPEC_BYTES 52u
#define UNIT 1988u
#define WMT_PAYLOAD (UNIT - 10u)
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void le16_put(uint8_t *p, uint32_t n)
{ p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void copy(uint8_t *d, const uint8_t *s, size_t n)
{ while(n--) *d++=*s++; }

/* Validate all input before the first UART write, not after a partial patch. */
static int validate(const uint8_t *p, size_t size, uint32_t *count)
{
    if(!p || size<FW_MAP || size>4u*1024u*1024u || le32(p+24)!=0xffffffffu)
        return EINVAL;
    *count=le32(p+44);
    if(!*count || *count>32 || FW_MAP+MAP_BYTES * *count>size) return EINVAL;
    for(uint32_t n=0;n<*count;n++) {
        const uint8_t *m=p+FW_MAP+MAP_BYTES*n;
        uint32_t offset=le32(m+4),bytes=le32(m+8),dl=le32(m+16);
        if(offset<FW_MAP+MAP_BYTES * *count || offset>size || bytes>size-offset ||
           dl>bytes || (dl && (le32(m)&0xffff)!=2) ||
           (uint64_t)le32(m+12)+dl>0x100000000ULL || (le32(m+32)&255)>1)
            return EINVAL;
        for(uint32_t k=0;k<n && bytes;k++) {
            const uint8_t *other=p+FW_MAP+MAP_BYTES*k;
            uint32_t start=le32(other+4),length=le32(other+8);
            if(length && offset<(uint64_t)start+length && start<(uint64_t)offset+bytes)
                return EINVAL;
        }
    }
    return 0;
}
static int exchange(const struct x9_bt_download_ops *o, const uint8_t *cmd,
                    size_t size, uint32_t timeout, uint8_t *status)
{
    static const uint8_t prefix[]={4,0xe4,5,2,1,1,0};
    uint8_t reply[32]; size_t n=sizeof(reply);
    int error=o->exchange(o->context,cmd,size,reply,&n,timeout);
    if(error) return error;
    if(n!=8) return EPROTO;
    for(size_t i=0;i<sizeof(prefix);i++) if(reply[i]!=prefix[i]) return EPROTO;
    *status=reply[7]; return 0;
}
static int download_one(const struct x9_bt_download_ops *o, const uint8_t *fw,
                        const uint8_t *m, struct x9_bt_download_result *r)
{
    uint32_t size=le32(m+16),offset=le32(m+4),mode=le32(m+32)&255;
    r->last_type=le32(m+28); r->last_index=(le32(m+32)>>16)&255; r->selected++;
    if(!size) return 0;
    uint8_t announce[10+SPEC_BYTES]={1,0x6f,0xfc,58,1,1,54,0,0,0};
    announce[9]=(uint8_t)mode; copy(announce+10,m+12,SPEC_BYTES);
    uint8_t status=255;
    int error=0;
    for(unsigned tries=0;tries<20;tries++) {
        error=exchange(o,announce,sizeof(announce),1000,&status);
        if(error) return error;
        if(status==2) { r->already_ready++; return 0; }
        if(status==0) break;
        if(status!=1) return EPROTO;
        o->sleep_ms(o->context,100);
    }
    if(status==1) return EBUSY;
    if(mode==1) {
        /* Android's UART DMA mode writes unframed bytes, in <=1988 B chunks.
         * Keep its progress semantics, but bound zero writes instead of the
         * vendor implementation's diagnostic-only timeout. */
        uint64_t deadline=o->now_ms(o->context)+2000;
        for(uint32_t at=0;at<size;) {
            size_t n=size-at; if(n>UNIT) n=UNIT;
            size_t sent=0;
            error=o->raw_write(o->context,fw+offset+at,n,&sent);
            if(error) return error;
            if(sent>n) return EIO;
            at+=(uint32_t)sent; r->bytes+=sent;
            if(o->now_ms(o->context)>=deadline) return ETIMEDOUT;
            if(!sent) o->sleep_ms(o->context,1);
        }
        if((error=o->drain(o->context,2000))) return error;
        o->sleep_ms(o->context,1);
        const uint8_t finish[]={2,0x6f,0xfc,5,0,1,1,1,0,3};
        error=exchange(o,finish,sizeof(finish),4000,&status);
        if(error) return error;
        if(status) return EPROTO;
    } else {
        uint8_t cmd[UNIT];
        for(uint32_t at=0;at<size;) {
            uint32_t n=size-at; if(n>WMT_PAYLOAD) n=WMT_PAYLOAD;
            /* Preserve Android's first/full-block rule, including an exact
             * single full block: phase1, not an invented phase3 packet. */
            uint8_t phase=!at ? (n<WMT_PAYLOAD?3:1) : (size-at<=WMT_PAYLOAD?3:2);
            cmd[0]=2; cmd[1]=0x6f; cmd[2]=0xfc; le16_put(cmd+3,n+5);
            cmd[5]=1; cmd[6]=1; le16_put(cmd+7,n+1); cmd[9]=phase;
            copy(cmd+10,fw+offset+at,n);
            error=exchange(o,cmd,n+10,phase==3?4000:1000,&status);
            if(error) return error;
            if(status) return EPROTO;
            at+=n; r->bytes+=n;
        }
    }
    r->downloaded++; return 0;
}
int x9_bt_download_mt6653(const struct x9_bt_download_ops *o, const uint8_t *fw,
                          size_t size, struct x9_bt_download_result *r)
{
    static const uint32_t types[]={0,2,3,0x80,0x90,0x10000};
    if(!o || !r || !o->exchange || !o->raw_write || !o->drain ||
       !o->sleep_ms || !o->now_ms || !o->connfem) return EINVAL;
    *r=(struct x9_bt_download_result){0};
    uint32_t count; int error=validate(fw,size,&count);
    if(error) return error;
    /* First matching section only, just like btmtk_load_fw_by_bin_info. */
    for(unsigned t=0;t<sizeof(types)/sizeof(types[0]);t++) {
        for(uint32_t n=0;n<count;n++) {
            const uint8_t *m=fw+FW_MAP+MAP_BYTES*n;
            if(le32(m+28)!=types[t]) continue;
            if((error=download_one(o,fw,m,r))) return error;
            break;
        }
        /* Android sends connfem after the type3 pass, even if it is absent. */
        if(types[t]==3 && (error=o->connfem(o->context))) return error;
    }
    for(uint32_t n=0;n<count;n++) {
        const uint8_t *m=fw+FW_MAP+MAP_BYTES*n;
        if(((le32(m+32)>>16)&255)==3) return download_one(o,fw,m,r);
    }
    return 0; /* Missing optional bin is Android's no-op, not radio-ready. */
}
int x9_bt_download_builtin(const struct x9_bt_download_ops *ops,struct x9_bt_download_result *result)
{
    if(!X9_BT_PAYLOAD_BYTES) return ENOENT;
    return x9_bt_download_mt6653(ops,x9_bt_payload,X9_BT_PAYLOAD_BYTES,result);
}
int x9_bt_download_preflight_builtin(void)
{
    uint32_t count;
    if(!X9_BT_PAYLOAD_BYTES) return ENOENT;
    return validate(x9_bt_payload,X9_BT_PAYLOAD_BYTES,&count);
}
