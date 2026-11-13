/* Bounded multi-node FDT collection for the pre-XNU audio handoffs.
 *
 * The old audio qualifiers called fdt_property_lookup()/fdt_node_schema()/
 * fdt_phandle_unique() once per field; each call re-validated the whole LK
 * FDT up to END. One ADSP+MIC handoff cost ~120 complete-tree walks with the
 * MMU and D-cache off and no watchdog service (BOOT130 stopped there).
 *
 * Here the SAME validation rules run exactly twice per boot:
 *   pass 1 (fdt_collect)          validate + record every property and the
 *                                 direct child count of each wanted node;
 *   pass 2 (fdt_collect_phandles) validate + check that each wanted phandle
 *                                 value occurs exactly once, at its owner.
 * Nothing is published before a valid END and trailing padding. Records are
 * offsets into this caller's read-only FDT, consumed synchronously and never
 * reused across another boot or a mutation. Each pass services the watchdog
 * every FDT_COLLECT_KICK tokens and fails closed after FDT_COLLECT_BUDGET_MS.
 */
#include "fdt.h"
#include "hw.h"

#define FDT_COLLECT_KICK 2048u
#define FDT_COLLECT_BUDGET_MS 2000u
#define NONE 0xffu

static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int eq(const char *a,const char *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static int overlap(size_t a,size_t n,size_t b,size_t m)
{return n&&m&&a<b+m&&b<a+n;}

struct pass {
    uint32_t handles[FDT_COLLECT_MAX_HANDLES];
    uint8_t owners[FDT_COLLECT_MAX_HANDLES];
    unsigned nh,count[FDT_COLLECT_MAX_HANDLES],wrong[FDT_COLLECT_MAX_HANDLES];
};

static int walk(struct fdt_collect *c,struct pass *ph)
{
    const uint8_t *b=(const uint8_t *)c->fdt;
    uint64_t freq=counter_frequency(),began=rsr("cntpct_el0");
    uint64_t budget=freq/1000*FDT_COLLECT_BUDGET_MS;
    if(be(b)!=0xd00dfeed)return FDT_PROPERTY_INVALID;
    uint32_t total=be(b+4),start=be(b+8),strings=be(b+12),reserve=be(b+16);
    uint32_t version=be(b+20),last=be(b+24),ss=be(b+32),sz=be(b+36);
    if(total<40||total>0x400000||version!=17||last>17||last>version||
       start<40||(start&3)||start>total||sz>total-start||(sz&3)||
       strings<40||strings>total||ss>total-strings||
       reserve<40||(reserve&7)||reserve>total||overlap(start,sz,strings,ss))
        return FDT_PROPERTY_INVALID;
    size_t r=reserve;
    for(;;) {
        if(total-r<16||overlap(r,16,start,sz)||overlap(r,16,strings,ss))return FDT_PROPERTY_INVALID;
        unsigned nonzero=0;
        for(unsigned i=0;i<16;i++)nonzero|=b[r+i];
        r+=16;
        if(!nonzero)break;
    }
    size_t at=start,end=(size_t)start+sz,used=0,parents[32];
    unsigned depth=0,nodes=0,roots=0,recorded=0,tokens=0;
    unsigned child[32]={0},node_at[32],first_rec[32];
    uint32_t seen=0;
    uint16_t children[FDT_COLLECT_MAX_NODES]={0};
    char path[256]={0};
    while(end-at>=4) {
        if(++tokens%FDT_COLLECT_KICK==0) {
            wdt_kick();c->kicks++;
            if(rsr("cntpct_el0")-began>budget)return FDT_PROPERTY_INVALID;
        }
        unsigned t=be(b+at);at+=4;
        if(t==1) {
            if(depth==32||++nodes>8192)return FDT_PROPERTY_INVALID;
            size_t first=at;
            while(at<end&&b[at]){if(b[at]=='/')return FDT_PROPERTY_INVALID;at++;}
            if(at==end)return FDT_PROPERTY_INVALID;
            parents[depth]=used;child[depth]=0;
            if(depth) {
                child[depth-1]=1;
                if(node_at[depth-1]!=NONE)children[node_at[depth-1]]++;
                size_t n=at-first;
                if(!n||used+1+n>=sizeof(path))return FDT_PROPERTY_INVALID;
                path[used++]='/';
                for(size_t i=0;i<n;i++)path[used++]=(char)b[first+i];
                path[used]=0;
            } else if(at!=first||++roots!=1)return FDT_PROPERTY_INVALID;
            node_at[depth]=NONE;first_rec[depth]=recorded;
            for(unsigned i=0;i<c->path_count;i++)if(eq(path,c->paths[i])) {
                if(seen&(1u<<i))return FDT_PROPERTY_INVALID;
                seen|=1u<<i;node_at[depth]=i;
            }
            depth++;at=(at+4)&~(size_t)3;
        } else if(t==2) {
            if(!depth)return FDT_PROPERTY_INVALID;
            used=parents[--depth];path[used]=0;
        } else if(t==3) {
            if(!depth||child[depth-1]||end-at<8)return FDT_PROPERTY_INVALID;
            uint32_t n=be(b+at),offset=be(b+at+4);at+=8;
            if(n>end-at||offset>=ss)return FDT_PROPERTY_INVALID;
            const char *key=(const char *)b+strings+offset;
            size_t limit=ss-offset,i=0;
            /* Bounded names for every property, never truncated into a key. */
            while(i<limit&&key[i]){if(i>=FDT_CAPTURE_KEY_BYTES-1)return FDT_PROPERTY_INVALID;i++;}
            if(!i||i==limit)return FDT_PROPERTY_INVALID;
            unsigned owner=node_at[depth-1];
            if(ph) {
                if(eq(key,"phandle")||eq(key,"linux,phandle")) {
                    if(n!=4)return FDT_PROPERTY_INVALID;
                    uint32_t v=be(b+at);
                    for(unsigned k=0;k<ph->nh;k++)if(ph->handles[k]==v) {
                        ph->count[k]++;
                        if(owner!=ph->owners[k])ph->wrong[k]=1;
                    }
                }
            } else if(owner!=NONE) {
                if(recorded==FDT_COLLECT_MAX_PROPERTIES)return FDT_PROPERTY_INVALID;
                /* A node's properties precede its children, so its records
                 * are contiguous: duplicate keys are checked locally. */
                for(unsigned k=first_rec[depth-1];k<recorded;k++)
                    if(eq((const char *)b+strings+c->prop[k].key,key))return FDT_PROPERTY_INVALID;
                c->prop[recorded++]=(struct fdt_collected){(uint16_t)owner,0,offset,(uint32_t)at,n};
            }
            at=(at+n+3)&~(size_t)3;
        } else if(t==4)continue;
        else if(t==9&&!depth&&roots==1) {
            while(end-at>=4) {
                unsigned tail=be(b+at);at+=4;
                if(tail&&tail!=4)return FDT_PROPERTY_INVALID;
            }
            if(at!=end)return FDT_PROPERTY_INVALID;
            if((seen&c->required)!=c->required)return FDT_PROPERTY_NODE_MISSING;
            c->ticks+=rsr("cntpct_el0")-began;c->passes++;
            if(!ph) {
                c->strings=strings;c->present=seen;c->count=recorded;
                for(unsigned k=0;k<c->path_count;k++)c->children[k]=children[k];
            }
            return FDT_PROPERTY_PRESENT;
        } else return FDT_PROPERTY_INVALID;
        if(at>end)return FDT_PROPERTY_INVALID;
    }
    return FDT_PROPERTY_INVALID;
}

int fdt_collect(struct fdt_collect *c,uintptr_t fdt,const char (*paths)[FDT_CAPTURE_PATH_BYTES],
                unsigned count,uint32_t required)
{
    if(!c)return FDT_PROPERTY_INVALID;
    c->fdt=fdt;c->paths=paths;c->path_count=count;c->required=required;
    c->present=0;c->count=0;c->strings=0;c->ticks=0;c->kicks=0;c->passes=0;
    if(!fdt||(fdt&7)||!paths||!count||count>FDT_COLLECT_MAX_NODES||
       (count<32&&(required>>count)))return FDT_PROPERTY_INVALID;
    for(unsigned i=0;i<count;i++) {
        unsigned n=0;while(n<FDT_CAPTURE_PATH_BYTES&&paths[i][n])n++;
        if(n==FDT_CAPTURE_PATH_BYTES||(n&&paths[i][0]!='/'))return FDT_PROPERTY_INVALID;
        for(unsigned j=0;j<i;j++)if(eq(paths[i],paths[j]))return FDT_PROPERTY_INVALID;
    }
    return walk(c,0);
}

int fdt_collect_phandles(struct fdt_collect *c,const uint32_t *handles,const uint8_t *owners,
                         unsigned count,uint32_t *ok)
{
    if(ok)*ok=0;
    if(!c||!c->passes||!ok||count>FDT_COLLECT_MAX_HANDLES||(count&&(!handles||!owners)))
        return FDT_PROPERTY_INVALID;
    struct pass p={{0},{0},count,{0},{0}};
    for(unsigned i=0;i<count;i++) {
        if(!handles[i]||handles[i]==UINT32_MAX||owners[i]>=c->path_count||
           !(c->present&(1u<<owners[i])))return FDT_PROPERTY_INVALID;
        p.handles[i]=handles[i];p.owners[i]=owners[i];
    }
    int r=walk(c,&p);
    if(r!=FDT_PROPERTY_PRESENT)return r;
    uint32_t mask=0;
    for(unsigned i=0;i<count;i++)if(p.count[i]==1&&!p.wrong[i])mask|=1u<<i;
    *ok=mask;return FDT_PROPERTY_PRESENT;
}

int fdt_collect_get(const struct fdt_collect *c,unsigned node,const char *key,const uint8_t **value,size_t *bytes)
{
    *value=0;*bytes=0;
    if(!c||node>=c->path_count||!(c->present&(1u<<node)))return -1;
    const uint8_t *b=(const uint8_t *)c->fdt;
    for(unsigned i=0;i<c->count;i++)
        if(c->prop[i].node==node&&eq((const char *)b+c->strings+c->prop[i].key,key)) {
            *value=b+c->prop[i].value;*bytes=c->prop[i].bytes;return 1;
        }
    return 0;
}

int fdt_collect_schema(const struct fdt_collect *c,unsigned node,const char (*keys)[32],unsigned count,
                       struct fdt_node_property *props,uint32_t *mask,unsigned *children)
{
    if(!keys||!count||count>32||!props||!mask||!children)return FDT_PROPERTY_INVALID;
    *mask=0;*children=0;
    for(unsigned i=0;i<count;i++)props[i]=(struct fdt_node_property){0};
    if(!c||node>=c->path_count)return FDT_PROPERTY_INVALID;
    if(!(c->present&(1u<<node)))return FDT_PROPERTY_NODE_MISSING;
    const uint8_t *b=(const uint8_t *)c->fdt;
    uint32_t seen=0;
    for(unsigned i=0;i<c->count;i++)if(c->prop[i].node==node) {
        const char *key=(const char *)b+c->strings+c->prop[i].key;
        unsigned k=0;while(k<count&&!eq(key,keys[k]))k++;
        if(k==count)return FDT_PROPERTY_INVALID;   /* unknown key: strict schema */
        seen|=1u<<k;props[k]=(struct fdt_node_property){b+c->prop[i].value,c->prop[i].bytes};
    }
    *mask=seen;*children=c->children[node];return FDT_PROPERTY_PRESENT;
}
