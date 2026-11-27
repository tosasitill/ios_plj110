/* Read-only topology production at the actual LK -> ADT handoff. Qualifies
 * full register/name/interrupt and pin tables, not merely a matching address.
 * No SMC, MMIO, IRQ init/ack, SRAM read, reset or inherited READY claim.
 *
 * Stall root cause (2026-11-23 revert of the feat/hevc-hw merge): the first
 * version of layout() called fdt_property() once per field (~35 calls);
 * every call re-validates the whole LK FDT up to END. In the pre-XNU shim
 * (MMU and D-cache off, no watchdog service) those per-field full-tree
 * walks stalled the boot, the same failure that had stopped the audio
 * qualifiers at BOOT130. layout() therefore uses ONE bounded fdt_collect
 * pass -- the fdt_collect.c pattern built for audio: the same complete-tree
 * validation run once, watchdog kicked every 2048 tokens, fail-closed
 * after a 2000 ms budget -- and reads every field from the collection. */
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
/* The five nodes layout() qualifies, captured in one collection pass. */
enum { N_ROOT, N_SOC, N_VCP, N_MMUP, N_VDEC, N_NODES };
static const char vcp_paths[N_NODES][FDT_CAPTURE_PATH_BYTES] = {
    "", "/soc", "/soc/vcp@31800000", "/soc/mmup@31a00000", "/soc/vdec@36000000",
};
/* ~6 KiB: static storage, never the pre-XNU stack (see fdt.h). */
static struct fdt_collect vcp_collect;
static int exact(unsigned node, const char *key,
                 const uint8_t *want, size_t bytes)
{
    const uint8_t *p; size_t n;
    if (fdt_collect_get(&vcp_collect, node, key, &p, &n) != 1 || n != bytes)
        return 0;
    for (size_t i = 0; i < n; i++) if (p[i] != want[i]) return 0;
    return 1;
}
static int cells(unsigned node, const char *key,
                 const uint32_t *want, size_t count)
{
    const uint8_t *p; size_t n;
    if (fdt_collect_get(&vcp_collect, node, key, &p, &n) != 1 ||
        n != count * 4) return 0;
    for (size_t i = 0; i < count; i++) if (be32(p + i * 4) != want[i]) return 0;
    return 1;
}
#define CELLS(node,key,a) cells(node,key,a,sizeof(a)/sizeof((a)[0]))
#define NAMES(node,key,a) exact(node,key,(const uint8_t *)(a),sizeof(a))
static int layout(uintptr_t dt)
{
    const char dc[] = "mediatek,vcodec-dec", dp[] = "platform:mt6993";
    const char vc[] = "mediatek,vcp", mc[] = "mediatek,mmup";
    const char vi[] = "wdt\0reserved\0mbox0\0mbox1\0mbox2";
    const char mi[] = "mbox0\0mbox1";
    const uint32_t two[] = {2}, three[] = {3}, sram[] = {0x80000};
    const uint32_t one[] = {1}, zero[] = {0};
    const uint8_t okay[] = {'o','k','a','y',0};
    const uint8_t *p; size_t n;
    /* ONE bounded pass: whole-tree validation, watchdog serviced, hard
     * time budget. Any refusal (invalid tree, a wanted node missing or
     * duplicated, property capacity, over budget) is a plain mismatch:
     * fail closed, print, and let the boot continue without VCP. */
    if (fdt_collect(&vcp_collect, dt, vcp_paths, N_NODES,
                    (1u << N_NODES) - 1) != FDT_PROPERTY_PRESENT) {
        plog_str("X9 VCP FDT: collection refused or over budget, transport unavailable\n");
        return 0;
    }
    plog_hex("X9 VCP FDT watchdog kicks", vcp_collect.kicks);
    plog_hex("X9 VCP FDT properties", vcp_collect.count);
    if (!CELLS(N_ROOT,"#address-cells",two) || !CELLS(N_ROOT,"#size-cells",two) ||
        !CELLS(N_SOC,"#address-cells",two) || !CELLS(N_SOC,"#size-cells",two) ||
        fdt_collect_get(&vcp_collect,N_SOC,"ranges",&p,&n) != 1 || n) return 0;
    if (!NAMES(N_VCP,"compatible",vc) || !NAMES(N_MMUP,"compatible",mc) ||
        !exact(N_VCP,"status",okay,sizeof(okay)) || !exact(N_MMUP,"status",okay,sizeof(okay)) ||
        !CELLS(N_VCP,"reg",vcp_reg) || !CELLS(N_MMUP,"reg",mmup_reg) ||
        !NAMES(N_VCP,"reg-names",vcp_reg_names) || !NAMES(N_MMUP,"reg-names",mmup_reg_names) ||
        !CELLS(N_VCP,"interrupts",vcp_interrupts) || !CELLS(N_MMUP,"interrupts",mmup_interrupts) ||
        !NAMES(N_VCP,"interrupt-names",vi) || !NAMES(N_MMUP,"interrupt-names",mi) ||
        !CELLS(N_VCP,"core-nums",two) || !CELLS(N_VCP,"vcp-sram-size",sram) ||
        !CELLS(N_VCP,"vcp-mem-tbl",x9_vcp_global_mem_table) ||
        !CELLS(N_VCP,"vcp-secure-dump",zero) ||
        !CELLS(N_VCP,"twohart",one) || !CELLS(N_MMUP,"twohart",zero) ||
        !CELLS(N_VCP,"mbox-count",three) || !CELLS(N_MMUP,"mbox-count",two)) return 0;
    return NAMES(N_VDEC,"compatible",dc) && NAMES(N_VDEC,"mediatek,platform",dp) &&
        CELLS(N_VDEC,"mediatek,ipm",two) && CELLS(N_VDEC,"vdec-power-in-vcp",one) &&
        CELLS(N_VDEC,"vp-mode-buf-size-multi",one) &&
        CELLS(N_VCP,"send-table",vcp_send) &&
        CELLS(N_VCP,"recv-table",vcp_recv) &&
        CELLS(N_MMUP,"send-table",mmup_send) &&
        CELLS(N_MMUP,"recv-table",mmup_recv);
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
