/* Independent packed-container reader: no struct bitfields/unaligned casts.
 * OPPO's CRC uses total size, despite the misleading header size comment.
 * This is deliberately only register-prefix configuration. Probus speaker
 * DSP payloads/calibration remain unsupported; prefix success is NOT READY.
 */
#if defined(KERNEL)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "x9_tfa98xx.h"
struct image {const uint8_t *p;uint32_t n,min;};
static uint16_t le16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8));}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int range(const struct image *c,uint32_t off,uint32_t bytes)
{return off>=c->min && off<=c->n && bytes<=c->n-off?0:EINVAL;}
static uint32_t tfa_crc32(const uint8_t *p,uint32_t n)
{
    uint32_t crc=~0u;
    for(uint32_t i=0;i<n;i++) {
        crc^=p[i];for(unsigned b=0;b<8;b++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
    return ~crc;
}
static int string_at(const struct image *c,uint32_t d,char name[64])
{
    uint32_t off=d&0xffffffu;if((d>>24)!=3 || range(c,off,1))return EINVAL;
    for(unsigned i=0;i<64;i++) {
        if(range(c,off+i,1))return EINVAL;
        uint8_t ch=c->p[off+i];
        if(!ch){name[i]=0;return i?0:EINVAL;}
        if(ch<32 || ch>126)return EINVAL;name[i]=(char)ch;
    }
    return EINVAL;
}
/* Selected list items are all validated before bus access. Opaque file/patch
 * descriptors are bounded but not executed or interpreted as firmware-ready. */
static int item(const struct image *c,uint32_t d)
{
    uint32_t type=d>>24,off=d&0xffffffu;
    if(type==2)return range(c,off,5); /* reg8,valueLE16,maskLE16 */
    if(type==16) {
        if(range(c,off,4))return EINVAL;
        uint16_t field=le16(c->p+off+2);unsigned width=(field&15)+1,pos=(field>>4)&15;
        return width+pos<=16?0:EINVAL;
    }
    if(type==4 || type==5) {
        if(range(c,off,8))return EINVAL;
        char name[64];if(string_at(c,le32(c->p+off),name))return EINVAL;
        return range(c,off+8,le32(c->p+off+4));
    }
    if(type==3){char name[64];return string_at(c,d,name);}
    if(type==6 || type==17)return 0; /* data-less marker/default */
    if(type>=29 || type==0 || type==1)return EINVAL;
    /* Other command types remain opaque, and are never passed to MMIO/I2C. */
    return range(c,off,1);
}
static int list(const struct image *c,uint32_t off,int profile,uint32_t *prefix,uint32_t *opaque)
{
    uint32_t head=profile?8u:12u;
    if(range(c,off,head))return EINVAL;
    uint32_t count=c->p[off];
    if(profile) {
        if(!count || le16(c->p+off+2)!=0x1234)return EINVAL;count--;
    }
    if(range(c,off+head,count*4))return EINVAL;
    int leading=1;*prefix=*opaque=0;
    for(uint32_t i=0;i<count;i++) {
        uint32_t d=le32(c->p+off+head+i*4),type=d>>24;
        if(!profile && type==1){leading=0;continue;} /* separately parsed profile */
        int e=item(c,d);if(e)return e;
        if((type==2 || type==16) && leading)(*prefix)++;
        else {leading=0;(*opaque)++;}
    }
    return 0;
}
int x9_tfa_select_profile(const void *data,uint32_t size,const char *name,struct x9_tfa_profile *out)
{
    if(!data || !name || !out || !*name || size<54 || size>X9_TFA_CONTAINER_MAX)return EINVAL;
    const uint8_t *p=data;
    if(p[0]!='P' || p[1]!='M' || (p[2]!='1' && p[2]!='3') || p[3]!='_' ||
       p[4]!='0' || p[5]!='1' || le32(p+6)!=size || le16(p+40)!=2)return EINVAL;
    uint32_t crc=tfa_crc32(p+14,size-14);if(crc!=le32(p+10))return EBADMSG;
    /* Only ndev descriptors are required by the actual tfaContGetDevList. */
    struct image c={p,size,54};struct x9_tfa_profile result;memset(&result,0,sizeof(result));
    result.crc=crc;unsigned seen=0;
    for(unsigned i=0;i<2;i++) {
        uint32_t d=le32(p+46+i*4),off=d&0xffffffu;
        if((d>>24)!=0 || range(&c,off,12))return EINVAL;
        unsigned slave=p[off+2];/* Container bus is a transport-kind flag, NOT Linux adapter number:
         * tfaRunStartup skips I2C when dev->bus != 0. Physical I2C6 belongs
         * to the board lease, never to this untrusted descriptor field. */
        if(p[off+1]!=0 || (slave!=0x34 && slave!=0x35))return ENXIO;
        unsigned idx=slave-0x34;if(seen&(1u<<idx))return EINVAL;seen|=1u<<idx;
        result.device_offset[idx]=off;
        if(string_at(&c,le32(p+off+8),result.device_name[idx]))return EINVAL;
        uint32_t prefix,opaque;int e=list(&c,off,0,&prefix,&opaque);if(e)return e;
        uint32_t count=p[off];unsigned found=0;
        for(uint32_t k=0;k<count;k++) {
            uint32_t pd=le32(p+off+12+k*4),po=pd&0xffffffu;
            if((pd>>24)!=1)continue;
            if(range(&c,po,8))return EINVAL;
            char profile_name[64];if(string_at(&c,le32(p+po+4),profile_name))return EINVAL;
            uint32_t pp,op;e=list(&c,po,1,&pp,&op);if(e)return e;
            if(!strcmp(profile_name,name)) {
                if(found++)return EINVAL;
                result.profile_offset[idx]=po;result.prefix_items[idx]=prefix+pp;
                result.opaque_items[idx]=opaque+op;
                memcpy(result.profile_name[idx],profile_name,strlen(profile_name)+1);
            }
        }
        if(!found)return ENOENT;
    }
    *out=result;return 0;
}
#if defined(KERNEL)
static int valid_bus(const struct x9_tfa_bus *b)
{
    if(!b || b->version!=1 || b->verified!=X9_TFA_BUS_REQUIRED || !b->read16 ||
       !b->write16 || !b->allow_patch)return EINVAL;
    for(unsigned i=0;i<2;i++) {
        unsigned rev=b->expected_revision[i]&255;
        if(rev!=0x72 && rev!=0x73 && rev!=0x74)return ENOTSUP;
    }
    return 0;
}
static int mute_one(const struct x9_tfa_bus *b,unsigned i)
{
    uint16_t old,check;int e=b->read16(b->context,(uint8_t)(0x34+i),0,&old);
    /* AMPE=0x0030 in all three confirmed family2 field tables. The
     * set_mute_nodsp callback itself is a no-op; AMPE readback is essential. */
    if(!e)e=b->write16(b->context,(uint8_t)(0x34+i),0,(uint16_t)(old&~8u));
    if(!e)e=b->read16(b->context,(uint8_t)(0x34+i),0,&check);
    if(!e && (check&8))e=EIO;return e;
}
static int mute_both(const struct x9_tfa_bus *b,struct x9_tfa_status *s)
{
    int first=0;s->muted=0;
    for(unsigned i=0;i<2;i++) {
        /* Never write unknown silicon even on the failure cleanup path. */
        if(s->revision[i]!=b->expected_revision[i]){if(!first)first=ENXIO;continue;}
        int e=mute_one(b,i);if(!e)s->muted|=1u<<i;else if(!first)first=e;
    }
    s->mute_error=(uint32_t)first;return first;
}
int x9_tfa_probe_and_mute(const struct x9_tfa_bus *b,struct x9_tfa_status *s)
{
    if(!s)return EINVAL;memset(s,0,sizeof(*s));int e=valid_bus(b);if(e){s->error=(uint32_t)e;return e;}
    int first=0;
    for(unsigned i=0;i<2;i++) {
        int r=b->read16(b->context,(uint8_t)(0x34+i),3,&s->revision[i]);
        if(!r && s->revision[i]!=b->expected_revision[i])r=ENXIO;
        if(r && !first)first=r;
    }
    int muted=mute_both(b,s);e=first?first:muted;s->error=(uint32_t)e;return e;
}
static int patch(const struct image *c,uint32_t d,uint8_t *reg,uint16_t *mask,uint16_t *value)
{
    uint32_t off=d&0xffffffu;
    if((d>>24)==2){*reg=c->p[off];*value=le16(c->p+off+1);*mask=le16(c->p+off+3);}
    else {
        uint16_t f=le16(c->p+off+2);unsigned width=(f&15)+1,pos=(f>>4)&15;
        *reg=(uint8_t)(f>>8);*mask=(uint16_t)(((1u<<width)-1u)<<pos);
        *value=(uint16_t)(le16(c->p+off)<<pos);
    }
    /* Never let a profile enable AMPE or write chip ID/OTP/key ranges by
     * default. Remaining registers need the revision-specific policy too. */
    if(!*mask || *reg==3 || (*reg==0 && (*mask&8)))return EPERM;
    return 0;
}
static int prefix(const struct x9_tfa_bus *b,const struct image *c,unsigned i,uint32_t off,
                  int profile,struct x9_tfa_status *s,int apply)
{
    unsigned n=c->p[off]-(profile?1u:0u),head=profile?8u:12u;
    for(unsigned k=0;k<n;k++) {
        uint32_t d=le32(c->p+off+head+k*4);if((d>>24)!=2 && (d>>24)!=16)break;
        uint8_t reg;uint16_t mask,value;int e=patch(c,d,&reg,&mask,&value);if(e)return e;
        e=b->allow_patch(b->context,b->expected_revision[i],reg,mask,value);if(e)return e;
        if(apply) {
            uint16_t old,check;e=b->read16(b->context,(uint8_t)(0x34+i),reg,&old);
            if(!e)e=b->write16(b->context,(uint8_t)(0x34+i),reg,(uint16_t)((old&~mask)|(value&mask)));
            if(!e)e=b->read16(b->context,(uint8_t)(0x34+i),reg,&check);
            if(!e && (check&mask)!=(value&mask))e=EIO;
            if(e)return e;s->applied[i]++;
        }
    }
    return 0;
}
int x9_tfa_apply_register_prefix(const struct x9_tfa_bus *b,const void *data,uint32_t bytes,
    const char *name,struct x9_tfa_status *s)
{
    if(!s)return EINVAL;memset(s,0,sizeof(*s));struct x9_tfa_profile plan;
    int e=valid_bus(b);if(!e)e=x9_tfa_select_profile(data,bytes,name,&plan);
    struct image c={data,bytes,54};
    /* Preflight BOTH full prefixes before the first write. Blob and policy
     * must be immutable/leased for this serialized call; never user mappings. */
    for(unsigned i=0;!e && i<2;i++) {
        e=prefix(b,&c,i,plan.device_offset[i],0,s,0);
        if(!e)e=prefix(b,&c,i,plan.profile_offset[i],1,s,0);
    }
    if(e){s->error=(uint32_t)e;return e;}
    e=x9_tfa_probe_and_mute(b,s);
    for(unsigned i=0;!e && i<2;i++) {
        e=prefix(b,&c,i,plan.device_offset[i],0,s,1);
        if(!e)e=prefix(b,&c,i,plan.profile_offset[i],1,s,1);
    }
    /* No unmute API and no rollback of trigger/MTP writes. Prefix failure
     * retains the board lease for a real hardware-reset recovery. */
    int muted=mute_both(b,s);if(!e)e=muted;s->error=(uint32_t)e;return e;
}
#endif
