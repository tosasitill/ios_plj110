/* Bounded Apple device-tree traversal and opt-in resident root layout. */
#include "resident.h"
#include "loader.h"

static uint32_t u32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static int same(const uint8_t *s, size_t n, const char *want)
{
    for (size_t i=0;i<n;i++) {
        if (s[i]!=(uint8_t)want[i]) return 0;
        if (!want[i]) return 1;
    }
    return 0;
}
struct walk { uint8_t *data; size_t size,at,found; unsigned nodes,matches; };
static int node(struct walk *w, unsigned depth, int chosen)
{
    if (depth>32 || ++w->nodes>1024 || w->size-w->at<8) return -1;
    uint32_t props=u32(w->data+w->at),children=u32(w->data+w->at+4);
    w->at+=8;
    if (props>1024 || children>1024) return -1;
    int is_chosen=0,is_map=0;
    for (uint32_t i=0;i<props;i++) {
        if (w->size-w->at<36) return -1;
        uint8_t *key=w->data+w->at;
        uint32_t size=u32(key+32)&0x7fffffffU;
        w->at+=36;
        size_t padded=((size_t)size+3)&~(size_t)3;
        if (padded>w->size-w->at) return -1;
        if (same(key,32,"name")) {
            is_chosen=depth==1 && same(w->data+w->at,size,"chosen");
            is_map=depth==2 && chosen && same(w->data+w->at,size,"memory-map");
        }
        if (same(key,32,"RAMDisk") && is_map) {
            if (size!=16) return -1;
            w->found=w->at;w->matches++;
        }
        w->at+=padded;
    }
    for (uint32_t i=0;i<children;i++) if (node(w,depth+1,is_chosen)) return -1;
    return 0;
}
int resident_patch_adt(void *data, size_t size, uint64_t base, uint64_t length)
{
    struct walk w={data,size,0,0,0,0};
    if (node(&w,0,0) || w.at!=size || w.matches!=1) return -1;
    for (unsigned i=0;i<8;i++) {
        w.data[w.found+i]=(uint8_t)(base>>(8*i));
        w.data[w.found+8+i]=(uint8_t)(length>>(8*i));
    }
    return 0;
}
static const char *layout(const void *data, size_t size, uint64_t *base, uint64_t *capacity, int storage)
{
    uint64_t adt,args;
    const char *error=payload_validate(data,size,&adt,&args);
    if (error) return error;
    const struct payload *p=data;
    /* Root is placed after kernel BSS, before copied ADT/boot_args. Reserve
     * 256MiB of uncommitted RAM so a full upload cannot consume all VM memory. */
    uint64_t offset=p->kernel_end-p->virt_base;
    uint64_t reserve=0x10000000UL+p->adt_size+0x8000;
    int profile=storage ? (p->phys_base==0x140000000UL && p->mem_size==0x20000000UL) :
                          (p->phys_base==0x200000000UL && p->mem_size>=0x100000000UL);
    if (!profile || offset>p->mem_size || reserve>p->mem_size-offset)
        return "resident root outside selected captured RAM profile";
    *base=p->phys_base+offset;
    *capacity=(p->mem_size-offset-reserve)&~0x3fffUL;
    return 0;
}

const char *resident_layout(const void *data,size_t size,uint64_t *base,uint64_t *capacity)
{return layout(data,size,base,capacity,0);}
const char *resident_layout_ufs(const void *data,size_t size,uint64_t *base,uint64_t *capacity)
{return layout(data,size,base,capacity,1);}
