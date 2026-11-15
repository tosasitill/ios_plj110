/* Read-only topology production at the actual LK -> ADT handoff. Qualifies
 * full register/name/interrupt and pin tables, not merely a matching address.
 * No SMC, MMIO, IRQ init/ack, SRAM read, reset or inherited READY claim. */
#include "fdt.h"
#include "plog.h"
#include "vcp_fdt_expected.h"
#include "../kernel/src/bsd/dev/x9_vcp_handoff.h"
_Static_assert(sizeof(struct x9_vcp_handoff) == X9_VCP_HANDOFF_BYTES, "VCP handoff ABI");
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static int same(const uint8_t *p, size_t n, const char *s)
{
    size_t at = 0;
    while (at < n && s[at] && p[at] == (uint8_t)s[at]) at++;
    return at < n && !s[at] && !p[at];
}
static int exact(uintptr_t dt, const char *path, const char *key,
                   const uint8_t *want, size_t bytes)
{
    const uint8_t *p; size_t n;
    if (!fdt_property(dt, path, key, &p, &n) || n != bytes) return 0;
    for (size_t i = 0; i < n; i++) if (p[i] != want[i]) return 0;
    return 1;
}
static int cells(uintptr_t dt, const char *path, const char *key,
                   const uint32_t *want, size_t count)
{
    const uint8_t *p; size_t n;
    if (!fdt_property(dt, path, key, &p, &n) || n != count * 4) return 0;
    for (size_t i = 0; i < count; i++) if (be32(p + i * 4) != want[i]) return 0;
    return 1;
}
#define CELLS(dt,path,key,a) cells(dt,path,key,a,sizeof(a)/sizeof((a)[0]))
#define NAMES(dt,path,key,a) exact(dt,path,key,(const uint8_t *)(a),sizeof(a))
static int layout(uintptr_t dt)
{
    const char *v = "/soc/vcp@31800000", *m = "/soc/mmup@31a00000";
    const char *d = "/soc/vdec@36000000";
    const char dc[] = "mediatek,vcodec-dec", dp[] = "platform:mt6993";
    const char vc[] = "mediatek,vcp", mc[] = "mediatek,mmup";
    const char vi[] = "wdt\0reserved\0mbox0\0mbox1\0mbox2";
    const char mi[] = "mbox0\0mbox1";
    const uint32_t two[] = {2}, three[] = {3}, sram[] = {0x80000};
    const uint32_t one[] = {1}, zero[] = {0};
    const uint8_t okay[] = {'o','k','a','y',0};
    const uint8_t *p; size_t n;
    if (!CELLS(dt,"","#address-cells",two) || !CELLS(dt,"","#size-cells",two) ||
        !CELLS(dt,"/soc","#address-cells",two) || !CELLS(dt,"/soc","#size-cells",two) ||
        !fdt_property(dt,"/soc","ranges",&p,&n) || n) return 0;
    if (!NAMES(dt,v,"compatible",vc) || !NAMES(dt,m,"compatible",mc) ||
        !exact(dt,v,"status",okay,sizeof(okay)) || !exact(dt,m,"status",okay,sizeof(okay)) ||
        !CELLS(dt,v,"reg",vcp_reg) || !CELLS(dt,m,"reg",mmup_reg) ||
        !NAMES(dt,v,"reg-names",vcp_reg_names) || !NAMES(dt,m,"reg-names",mmup_reg_names) ||
        !CELLS(dt,v,"interrupts",vcp_interrupts) || !CELLS(dt,m,"interrupts",mmup_interrupts) ||
        !NAMES(dt,v,"interrupt-names",vi) || !NAMES(dt,m,"interrupt-names",mi) ||
        !CELLS(dt,v,"core-nums",two) || !CELLS(dt,v,"vcp-sram-size",sram) ||
        !CELLS(dt,v,"vcp-mem-tbl",x9_vcp_global_mem_table) ||
        !CELLS(dt,v,"vcp-secure-dump",zero) ||
        !CELLS(dt,v,"twohart",one) || !CELLS(dt,m,"twohart",zero) ||
        !CELLS(dt,v,"mbox-count",three) || !CELLS(dt,m,"mbox-count",two)) return 0;
    return NAMES(dt,d,"compatible",dc) && NAMES(dt,d,"mediatek,platform",dp) &&
        CELLS(dt,d,"mediatek,ipm",two) && CELLS(dt,d,"vdec-power-in-vcp",one) &&
        CELLS(dt,d,"vp-mode-buf-size-multi",one) &&
        CELLS(dt,v,"send-table",vcp_send) &&
        CELLS(dt,v,"recv-table",vcp_recv) &&
        CELLS(dt,m,"send-table",mmup_send) &&
        CELLS(dt,m,"recv-table",mmup_recv);
}
/* Validate the WHOLE ADT before writing. Property order is irrelevant; a
 * duplicate name/placeholder or a placeholder outside /chosen is invalid. */
struct walk { uint8_t *p; size_t size, at, found; unsigned nodes, matches, chosens; };
static int node(struct walk *w, unsigned depth)
{
    uint32_t props, children;
    unsigned named = 0, chosen = 0, holders = 0;
    size_t found = 0;
    if (depth > 32 || ++w->nodes > 1024 || w->at > w->size || w->size - w->at < 8)
        return -1;
    props = le32(w->p + w->at); children = le32(w->p + w->at + 4); w->at += 8;
    if (props > 1024 || children > 1024) return -1;
    for (unsigned i = 0; i < props; i++) {
        uint8_t *key; uint32_t bytes; size_t padded;
        if (w->size - w->at < 36) return -1;
        key = w->p + w->at; bytes = le32(key + 32) & 0x7fffffff; w->at += 36;
        padded = ((size_t)bytes + 3) & ~(size_t)3;
        if (padded > w->size - w->at) return -1;
        if (same(key,32,"name")) {
            if (named++) return -1;
            chosen = depth == 1 && same(w->p + w->at,bytes,"chosen");
        }
        if (same(key,32,"x9-vcp-handoff")) {
            if (holders++ || bytes != X9_VCP_HANDOFF_BYTES) return -1;
            found = w->at;
        }
        w->at += padded;
    }
    if (chosen && ++w->chosens != 1) return -1;
    if (holders) {
        if (!chosen || ++w->matches != 1) return -1;
        w->found = found;
    }
    for (unsigned i = 0; i < children; i++) if (node(w,depth+1)) return -1;
    return 0;
}
int fdt_vcp_patch_adt(uintptr_t dt, void *adt, size_t size)
{
    struct walk w = {.p=(uint8_t *)adt,.size=size};
    struct x9_vcp_handoff h = {0};
    uint8_t *p;
    if (!adt || size > 1024 * 1024 || node(&w,0) || w.at != size ||
        w.matches != 1 || w.chosens != 1)
        return -1;
    p = w.p + w.found;
    for (unsigned i = 0; i < sizeof(h); i++) p[i] = 0;
    if (!layout(dt)) {
        plog_str("X9 VCP FDT: topology/global layout mismatch, mailbox transport unavailable\n");
        return 0;
    }
    h.version = X9_VCP_HANDOFF_VERSION; h.topology = 1; h.cores = 2; h.sram_bytes = 0x80000;
    for (unsigned i = 0; i < X9_VCP_MBOX_COUNT; i++) {
        h.mbox[i].base = 0x31b80000 + i * 0x10000;
        h.mbox[i].set = h.mbox[i].base + 0x100;
        h.mbox[i].clear = h.mbox[i].base + 0x10c;
        h.mbox[i].irq = 593 + i;
    }
    h.cfg_base[0] = 0x31a0a000; h.cfg_base[1] = 0x31a0d000;
    h.twohart[0] = 1; h.twohart[1] = 0;
    h.global_layout = X9_VCP_GLOBAL_LAYOUT_CAPTURE1;
    h.vp_multiplier = 1;
    if (!x9_vcp_handoff_valid(&h)) return -1;
    /* arm64 is little-endian; copy bytes without requiring aligned ADT. */
    for (unsigned i = 0; i < sizeof(h); i++) p[i] = ((const uint8_t *)&h)[i];
    plog_str("X9 VCP FDT: current mailbox/global memory layout qualified (not READY)\n");
    return 1;
}
