#include "x9_mm_smmu.h"
#include <sys/errno.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

#define BIT(n) (UINT64_C(1) << (n))
#define ADDRESS_MASK UINT64_C(0x000ffffffffffff0)
#define TABLE_MASK (ADDRESS_MASK & ~UINT64_C(63))
#define PTE_ADDRESS_MASK UINT64_C(0x0000fffffffff000)
#define PA_LIMIT BIT(48)

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void store64(uint8_t *p, uint64_t value)
{
    unsigned i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (i * 8));
}

static int host_intersects(const void *a, uint64_t as, const void *b, uint64_t bs)
{
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    /* Reject wrapping host spans too; no pointer is dereferenced here. */
    if (!a || !b || as > UINTPTR_MAX - av || bs > UINTPTR_MAX - bv) return 1;
    return av < bv + bs && bv < av + as;
}

static unsigned pa_bits(unsigned encoding)
{
    static const unsigned widths[] = {32, 36, 40, 42, 44, 48, 52};
    return encoding < 7 ? widths[encoding] : 0;
}

static int aperture(uint32_t sid, uint64_t iova, uint64_t bytes)
{
    uint64_t end;
    if (!bytes || iova > UINT64_MAX - bytes) return ERANGE;
    end = iova + bytes;
    if (sid == X9_MM_SMMU_HFRP_SID)
        return iova >= UINT64_C(0x140000000) && end <= UINT64_C(0x1c0000000)
            ? 0 : ERANGE;
    if (sid == X9_MM_SMMU_CODEC_SID)
        return end <= UINT64_C(0xfc910000) ||
            (iova >= UINT64_C(0xfffff000) && end <= UINT64_C(0x400000000))
            ? 0 : ERANGE;
    return ENOTSUP;
}

int x9_mm_smmu_stage1_geometry(const struct x9_mm_smmu_query *q, uint32_t bytes,
                               uint32_t *tables)
{
    uint64_t end;
    int e;
    if (!tables || (q && host_intersects(tables, sizeof(*tables), q, sizeof(*q))))
        return EINVAL;
    *tables = 0;
    if (!q || !bytes || bytes > X9_MM_SMMU_MAX_CHECK_BYTES ||
        (bytes & 4095) || (q->iova & 4095) || q->ssid_valid > 1 ||
        (!q->ssid_valid && q->ssid)) return EINVAL;
    e = aperture(q->sid, q->iova, bytes);
    if (e) return e;
    end = q->iova + bytes;
    if (end > BIT(36)) return ERANGE;
    *tables = 1 + (uint32_t)(((end - 1) >> 30) - (q->iova >> 30) + 1) +
        (uint32_t)(((end - 1) >> 21) - (q->iova >> 21) + 1);
    if (*tables > X9_MM_SMMU_BUILD_MAX_TABLES) { *tables = 0; return E2BIG; }
    return 0;
}

int x9_mm_smmu_build_stage1(uint32_t abi, const struct x9_mm_smmu_query *query,
                            const struct x9_mm_smmu_dma_span *spans, uint32_t span_count,
                            uint32_t total_bytes, uint32_t access,
                            uint32_t output_bits, const struct x9_mm_smmu_table_page *pages,
                            uint32_t page_count, struct x9_mm_smmu_table_build *out)
{
    uint64_t limit, sum = 0, end, base2, base3, attributes, iova;
    uint64_t desc_bytes, page_desc_bytes, offset, block;
    uint32_t i, j, n2, n3, needed, parent, leaf, used = 0;
    const struct x9_mm_smmu_dma_span *s;
    const struct x9_mm_smmu_table_page *p;
    int e;
    if (!out) return EINVAL;
    /* An aliased result cannot be zeroed without modifying source/storage.
     * Reject that caller error before writing ANY byte. */
    if (query && host_intersects(out, sizeof(*out), query, sizeof(*query)))
        return EINVAL;
    if (spans && span_count <= X9_MM_SMMU_MAX_DMA_SPANS &&
        host_intersects(out, sizeof(*out), spans, (uint64_t)span_count * sizeof(*spans)))
        return EINVAL;
    if (pages && page_count <= X9_MM_SMMU_BUILD_MAX_TABLES) {
        if (host_intersects(out, sizeof(*out), pages, (uint64_t)page_count * sizeof(*pages)))
            return EINVAL;
        for (i = 0; i < page_count; i++)
            if (pages[i].host && host_intersects(out, sizeof(*out), pages[i].host, 4096))
                return EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (abi != X9_MM_SMMU_STAGE1_4K36_ABI) return ENOTSUP;
    if (!query || !spans || !pages || !span_count ||
        span_count > X9_MM_SMMU_MAX_DMA_SPANS || !page_count ||
        page_count > X9_MM_SMMU_BUILD_MAX_TABLES ||
        !total_bytes || total_bytes > X9_MM_SMMU_MAX_CHECK_BYTES ||
        (total_bytes & 4095) || (query->iova & 4095) ||
        !(access & X9_MM_SMMU_READ) ||
        (access & ~(X9_MM_SMMU_READ | X9_MM_SMMU_WRITE | X9_MM_SMMU_UNPRIV)) ||
        query->ssid_valid > 1 || (!query->ssid_valid && query->ssid)) return EINVAL;
    if (output_bits != 32 && output_bits != 36 && output_bits != 40 &&
        output_bits != 42 && output_bits != 44 && output_bits != 48) return ENOTSUP;
    e = x9_mm_smmu_stage1_geometry(query, total_bytes, &needed);
    if (e) return e;
    end = query->iova + total_bytes; /* no wrap after aperture */
    if (end > BIT(36)) return ERANGE;
    limit = BIT(output_bits);
    base2 = query->iova >> 30;
    base3 = query->iova >> 21;
    n2 = (uint32_t)(((end - 1) >> 30) - base2 + 1);
    n3 = (uint32_t)(((end - 1) >> 21) - base3 + 1);
    if (page_count < needed) return ENOSPC;
    desc_bytes = (uint64_t)span_count * sizeof(*spans);
    page_desc_bytes = (uint64_t)page_count * sizeof(*pages);
    for (i = 0; i < span_count; i++) {
        s = &spans[i];
        if (s->reserved || !s->bytes || (s->bytes & 4095) ||
            (s->expected_pa & 4095) || s->expected_pa >= limit ||
            s->bytes > limit - s->expected_pa || s->bytes > total_bytes - sum)
            return ERANGE;
        for (j = 0; j < i; j++)
            if (s->expected_pa < spans[j].expected_pa + spans[j].bytes &&
                spans[j].expected_pa < s->expected_pa + s->bytes) return EINVAL;
        sum += s->bytes;
    }
    if (sum != total_bytes) return EINVAL;
    for (i = 0; i < page_count; i++) {
        p = &pages[i];
        if (!p->pa || (p->pa & 4095) || p->pa >= limit ||
            4096 > limit - p->pa || !p->host || ((uintptr_t)p->host & 7) ||
            host_intersects(p->host, 4096, query, sizeof(*query)) ||
            host_intersects(p->host, 4096, spans, desc_bytes) ||
            host_intersects(p->host, 4096, pages, page_desc_bytes) ||
            host_intersects(p->host, 4096, out, sizeof(*out))) return EINVAL;
        for (j = 0; j < span_count; j++)
            if (p->pa < spans[j].expected_pa + spans[j].bytes &&
                spans[j].expected_pa < p->pa + 4096) return EINVAL;
        for (j = 0; j < i; j++)
            if (p->pa == pages[j].pa ||
                host_intersects(p->host, 4096, pages[j].host, 4096)) return EINVAL;
    }
    /* Nothing above touches table storage. Every following index is bounded
     * by the validated full extent and precomputed tree geometry. */
    for (i = 0; i < needed; i++) memset(pages[i].host, 0, 4096);
    for (i = 0; i < n2; i++)
        store64(pages[0].host + (base2 + i) * 8, pages[1 + i].pa | 3);
    for (i = 0; i < n3; i++) {
        block = base3 + i;
        parent = (uint32_t)((block >> 9) - base2);
        store64(pages[1 + parent].host + (block & 511) * 8,
                pages[1 + n2 + i].pa | 3);
    }
    attributes = 3 | BIT(10) | (UINT64_C(3) << 8) | BIT(53) | BIT(54);
    if (!(access & X9_MM_SMMU_WRITE)) attributes |= BIT(7);
    if (access & X9_MM_SMMU_UNPRIV) attributes |= BIT(6);
    for (i = 0; i < span_count; i++) {
        for (offset = 0; offset < spans[i].bytes; offset += 4096) {
            iova = query->iova + used;
            leaf = 1 + n2 + (uint32_t)((iova >> 21) - base3);
            store64(pages[leaf].host + ((iova >> 12) & 511) * 8,
                    (spans[i].expected_pa + offset) | attributes);
            used += 4096;
        }
    }
    out->ttbr = pages[0].pa;
    out->table_count = needed;
    out->mapped_bytes = used;
    out->input_bits = 36;
    out->output_bits = output_bits;
    return 0;
}

static int read_words(const struct x9_mm_smmu_reader *r, uint64_t pa,
                       uint64_t *out, uint32_t count)
{
    uint8_t raw[64];
    uint32_t bytes;
    int allowed = 0, e;
    if (!count || count > 8) return EINVAL;
    bytes = count * 8;
    if (!pa || (pa & 7) || pa >= PA_LIMIT ||
        bytes > PA_LIMIT - pa) return ERANGE;
    for (unsigned i = 0; i < r->range_count; i++) {
        const struct x9_mm_smmu_range *a = &r->ranges[i];
        if (pa >= a->start && pa - a->start < a->bytes &&
            bytes <= a->bytes - (pa - a->start)) allowed = 1;
    }
    if (!allowed) return EACCES;
    memset(raw, 0, sizeof(raw));
    e = r->read(r->cookie, pa, raw, bytes);
    if (e) return e;
    for (unsigned i = 0; i < count; i++) out[i] = le64(raw + i * 8);
    return 0;
}

static int regs_valid(const struct x9_mm_smmu_regs *r,
                       const struct x9_mm_smmu_reader *reader,
                       struct x9_mm_smmu_result *out)
{
    unsigned ttf = (r->idr0 >> 2) & 3, endian = (r->idr0 >> 21) & 3;
    if (!reader->read || !reader->range_count ||
        reader->range_count > X9_MM_SMMU_MAX_RANGES) return EINVAL;
    for (unsigned i = 0; i < reader->range_count; i++) {
        const struct x9_mm_smmu_range *a = &reader->ranges[i];
        if (!a->bytes || a->start >= PA_LIMIT || a->bytes > PA_LIMIT - a->start)
            return ERANGE;
    }
    if (!(r->idr0 & 2) || ttf < 2 || ((r->idr0 >> 27) & 3) > 1 ||
        (endian != 0 && endian != 2) ||
        !pa_bits(r->idr5 & 7)) return ENOTSUP;
    if ((r->cr0 ^ r->cr0ack) & 31) return EBUSY;
    if (!(r->cr0 & 1)) return ENXIO;
    if ((r->gerror ^ r->gerrorn) & 0x1fd) return EIO;
    if (r->cr0 & 8) {
        unsigned qbits = r->cmdq_base & 31, maxbits = (r->idr1 >> 21) & 31;
        uint64_t base = r->cmdq_base & (ADDRESS_MASK & ~UINT64_C(31));
        uint32_t mask;
        if (!qbits || qbits > maxbits || qbits > 19 || !base ||
            base >= BIT(pa_bits(r->idr5 & 7)) ||
            BIT(qbits + 4) > BIT(pa_bits(r->idr5 & 7)) - base ||
            (base & ((BIT(qbits + 4) - 1) | 31))) return ENOTSUP;
        /* CONS error bits and unexpected producer/consumer bits are not a
         * wrapped empty queue. Both wrap bit and index must agree. */
        mask = (uint32_t)(BIT(qbits + 1) - 1);
        if ((r->cmdq_cons | r->cmdq_prod) & ~mask) return EIO;
        out->command_queue_empty = r->cmdq_cons == r->cmdq_prod;
    }
    return 0;
}

static int locate_ste(const struct x9_mm_smmu_regs *r,
                            const struct x9_mm_smmu_reader *reader,
                            const struct x9_mm_smmu_query *q,
                            struct x9_mm_smmu_result *out)
{
    unsigned sidbits = r->idr1 & 63, tablebits = r->strtab_cfg & 63;
    unsigned format = (r->strtab_cfg >> 16) & 3;
    unsigned oas = pa_bits(r->idr5 & 7);
    uint64_t base = r->strtab_base & TABLE_MASK, descriptor;
    int e;
    out->stage = X9_MM_SMMU_STE;
    if (!sidbits || sidbits > 32 || !tablebits || tablebits > sidbits ||
        tablebits > 32 || q->sid >= BIT(tablebits) || !base || base >= BIT(oas))
        return ERANGE;
    if (format == 0) {
        out->ste_pa = base + (uint64_t)q->sid * 64;
    } else if (format == 1) {
        unsigned split = (r->strtab_cfg >> 6) & 31;
        unsigned l1bits;
        uint64_t l1_pa;
        if (((r->idr0 >> 27) & 3) != 1 || split < 6 || split > tablebits ||
            split > 10 || tablebits - split > 17)
            return ENOTSUP;
        l1bits = tablebits - split;
        if (base & (BIT(l1bits + 3) - 1)) return EINVAL;
        l1_pa = base + (uint64_t)(q->sid >> split) * 8;
        if (l1_pa >= BIT(oas) || 8 > BIT(oas) - l1_pa) return ERANGE;
        e = read_words(reader, l1_pa, &descriptor, 1);
        if (e) return e;
        /* SPAN=split+1 covers the whole split-sized L2 table. Smaller
         * spans/implementation-specific variants require separate evidence. */
        if ((descriptor & 31) != split + 1) return ENOTSUP;
        base = descriptor & TABLE_MASK;
        if (!base || base >= BIT(oas) || (base & (BIT(split + 6) - 1)))
            return EINVAL;
        out->ste_pa = base + (q->sid & (BIT(split) - 1)) * 64;
    } else return ENOTSUP;
    if (out->ste_pa >= BIT(oas) || 64 > BIT(oas) - out->ste_pa) return ERANGE;
    e = read_words(reader, out->ste_pa, out->ste, 8);
    return e;
}

static int inspect_context(const struct x9_mm_smmu_regs *r,
                            const struct x9_mm_smmu_reader *reader,
                            const struct x9_mm_smmu_query *q,
                            struct x9_mm_smmu_result *out)
{
    unsigned cdmax, ssidbits, tg, stride, levels, topbits, format;
    unsigned oas = pa_bits(r->idr5 & 7);
    uint64_t cd, base, descriptor;
    int e = locate_ste(r, reader, q, out);
    if (e) return e;
    if (!(out->ste[0] & 1)) return ENOENT;
    if (((out->ste[0] >> 1) & 7) != 5 || ((out->ste[1] >> 30) & 3))
        return ENOTSUP; /* includes MTK dynamic bypass even with a ctxptr */
    out->stage = X9_MM_SMMU_CD;
    cdmax = (unsigned)(out->ste[0] >> 59);
    ssidbits = (r->idr1 >> 6) & 31;
    if (cdmax > ssidbits || cdmax > 20 || q->ssid_valid > 1) return ERANGE;
    if (!q->ssid_valid && (q->ssid || (out->ste[1] & 3) != 2)) return EACCES;
    if (q->ssid >= BIT(cdmax)) return ERANGE;
    out->ssid = q->ssid;
    base = out->ste[0] & TABLE_MASK;
    format = (unsigned)(out->ste[0] >> 4) & 3;
    if (!base || base >= BIT(oas)) return ERANGE;
    if (format == 0) {
        out->cd_pa = base + (uint64_t)q->ssid * 64;
    } else if (format == 2) {
        uint64_t l1_pa;
        if (!(r->idr0 & (1u << 19)) || cdmax <= 10) return ENOTSUP;
        if (base & (BIT(cdmax - 10 + 3) - 1)) return EINVAL;
        l1_pa = base + (uint64_t)(q->ssid >> 10) * 8;
        if (l1_pa >= BIT(oas) || 8 > BIT(oas) - l1_pa) return ERANGE;
        e = read_words(reader, l1_pa, &descriptor, 1);
        if (e) return e;
        if (!(descriptor & 1)) return ENOENT;
        base = descriptor & (ADDRESS_MASK & ~UINT64_C(4095));
        if (!base || base >= BIT(oas) || (base & 65535)) return EINVAL;
        out->cd_pa = base + (q->ssid & 1023u) * 64;
    } else return ENOTSUP;
    if (out->cd_pa >= BIT(oas) || 64 > BIT(oas) - out->cd_pa) return ERANGE;
    e = read_words(reader, out->cd_pa, out->cd, 8);
    if (e) return e;
    cd = out->cd[0];
    if (!(cd & BIT(31))) return ENOENT;
    if (!(cd & BIT(41)) || (cd & (BIT(15) | BIT(38))) || !(cd & BIT(30)))
        return ENOTSUP; /* endian, TBI and TTBR1 are not silently guessed */
    if (cd & BIT(14)) return EACCES;
    if (((cd >> 12) & 3) == 1) return EINVAL;
    tg = (unsigned)(cd >> 6) & 3;
    if (tg == 0 && (r->idr5 & 16)) out->page_shift = 12;
    else if (tg == 2 && (r->idr5 & 32)) out->page_shift = 14;
    else return ENOTSUP;
    out->input_bits = 64 - (unsigned)(cd & 63);
    out->output_bits = pa_bits((unsigned)(cd >> 32) & 7);
    out->asid = (unsigned)(cd >> 48);
    /* A 52-bit-capable SMMU may still run a <=48-bit context. Do not reject
     * that hardware, but never decode an actual IPS=52/LPA2 page table here. */
    if (out->input_bits < 25 || out->input_bits > 48 || !out->output_bits ||
        out->output_bits > 48 ||
        out->output_bits > oas || (!(r->idr0 & 4096) && out->asid > 255))
        return ENOTSUP;
    stride = out->page_shift - 3;
    levels = (out->input_bits - out->page_shift + stride - 1) / stride;
    if (!levels || levels > 4) return ENOTSUP;
    out->start_level = 4 - levels;
    topbits = out->input_bits - out->page_shift - stride * (levels - 1);
    out->ttbr = out->cd[1] & ADDRESS_MASK;
    if (!out->ttbr || out->ttbr >= BIT(out->output_bits) ||
        (out->ttbr & (BIT(topbits + 3) - 1))) return EINVAL;
    return 0;
}

static int walk(const struct x9_mm_smmu_reader *reader,
                 const struct x9_mm_smmu_query *q, struct x9_mm_smmu_result *out)
{
    uint64_t table = out->ttbr, parents[4] = {0};
    unsigned stride = out->page_shift - 3;
    unsigned deny_write = 0, deny_unpriv = 0;
    out->stage = X9_MM_SMMU_WALK;
    if (q->iova >= BIT(out->input_bits)) return ERANGE;
    for (unsigned level = out->start_level; level < 4; level++) {
        unsigned shift = out->page_shift + stride * (3 - level);
        unsigned indexbits = level == out->start_level ? out->input_bits - shift : stride;
        uint64_t index = (q->iova >> shift) & (BIT(indexbits) - 1), descriptor;
        uint64_t address, block_bytes, offset, end;
        unsigned step, ap;
        int e;
        for (unsigned i = 0; i < out->step_count; i++)
            if (table == parents[i]) return ELOOP;
        parents[out->step_count] = table;
        step = out->step_count++;
        out->steps[step].pa = table + index * 8;
        out->steps[step].level = level;
        if (out->steps[step].pa >= BIT(out->output_bits) ||
            8 > BIT(out->output_bits) - out->steps[step].pa) return ERANGE;
        e = read_words(reader, out->steps[step].pa, &descriptor, 1);
        if (e) return e;
        out->steps[step].descriptor = descriptor;
        if (!(descriptor & 1)) return ENOENT;
        /* DBM at bit51 is an attribute, NOT PA. 52-bit/LPA2 descriptors
         * require a separate decoder; do not round reserved 16K low bits. */
        if ((descriptor & (BIT(51) - BIT(48))) ||
            (descriptor & PTE_ADDRESS_MASK & (BIT(out->page_shift) - 1)))
            return ENOTSUP;
        address = descriptor & PTE_ADDRESS_MASK & ~(BIT(out->page_shift) - 1);
        if (address >= BIT(out->output_bits)) return ERANGE;
        if (level < 3 && (descriptor & 2)) {
            if (!address) return EINVAL;
            /* Hierarchical APTable permissions apply even if the leaf is RW. */
            deny_unpriv |= !!(descriptor & BIT(61));
            deny_write |= !!(descriptor & BIT(62));
            table = address;
            continue;
        }
        if ((level == 3 && !(descriptor & 2)) || level == 0) return EINVAL;
        block_bytes = BIT(shift);
        if (address & (block_bytes - 1)) return EINVAL;
        if (!(descriptor & BIT(10))) return EACCES; /* no HW access-flag update */
        offset = q->iova & (block_bytes - 1);
        if (block_bytes > BIT(out->output_bits) - address) return ERANGE;
        ap = (unsigned)(descriptor >> 6) & 3;
        out->access = X9_MM_SMMU_READ;
        if (!(ap & 2) && !deny_write) out->access |= X9_MM_SMMU_WRITE;
        if ((ap & 1) && !deny_unpriv) out->access |= X9_MM_SMMU_UNPRIV;
        out->attr_index = (unsigned)(descriptor >> 2) & 7;
        out->mair_byte = (unsigned)(out->cd[3] >> (out->attr_index * 8)) & 255;
        out->shareability = (unsigned)(descriptor >> 8) & 3;
        if (out->shareability == 1) return EINVAL;
        out->output_pa = address + offset;
        out->span_bytes = block_bytes - offset;
        end = q->sid == X9_MM_SMMU_HFRP_SID ? UINT64_C(0x1c0000000) :
            q->iova < UINT64_C(0xfc910000) ? UINT64_C(0xfc910000) : UINT64_C(0x400000000);
        if (out->span_bytes > end - q->iova) out->span_bytes = end - q->iova;
        out->stage = X9_MM_SMMU_DONE;
        return 0;
    }
    return EINVAL;
}

static int failed(struct x9_mm_smmu_result *out, int e)
{
    out->error = (uint32_t)e;
    if (e) { out->output_pa = 0; out->span_bytes = 0; out->access = 0; }
    return e;
}

int x9_mm_smmu_translate(const struct x9_mm_smmu_regs *r,
                          const struct x9_mm_smmu_reader *reader,
                          const struct x9_mm_smmu_query *q,
                          struct x9_mm_smmu_result *out)
{
    int e;
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    out->stage = X9_MM_SMMU_REGS;
    if (!r || !reader || !q) return failed(out, EINVAL);
    out->sid = q->sid;
    out->ssid = q->ssid;
    e = aperture(q->sid, q->iova, 1);
    if (!e) e = regs_valid(r, reader, out);
    if (!e) e = inspect_context(r, reader, q, out);
    if (!e) e = walk(reader, q, out);
    return failed(out, e);
}

int x9_mm_smmu_check_mapping(const struct x9_mm_smmu_regs *r,
                             const struct x9_mm_smmu_reader *reader,
                             const struct x9_mm_smmu_query *q, uint64_t pa,
                             uint32_t bytes, uint32_t access,
                             uint32_t *checked, struct x9_mm_smmu_result *out)
{
    int e;
    struct x9_mm_smmu_query query;
    if (!out || !checked) return EINVAL;
    memset(out, 0, sizeof(*out));
    out->stage = X9_MM_SMMU_REGS;
    *checked = 0;
    if (!q || !bytes || bytes > X9_MM_SMMU_MAX_CHECK_BYTES ||
        !access || (access & ~7u) || pa >= PA_LIMIT || bytes > PA_LIMIT - pa)
        return failed(out, EINVAL);
    e = aperture(q->sid, q->iova, bytes);
    if (e) return failed(out, e);
    query = *q;
    /* Include both partial ends without retaining the old 16MiB walk ceiling. */
    uint32_t visits = bytes / 4096 + 2;
    for (unsigned n = 0; *checked < bytes && n < visits; n++) {
        uint64_t remaining;
        query.iova = q->iova + *checked;
        e = x9_mm_smmu_translate(r, reader, &query, out);
        if (e) return e;
        if (out->output_pa != pa + *checked) return failed(out, EADDRNOTAVAIL);
        if ((out->access & access) != access) return failed(out, EACCES);
        remaining = bytes - *checked;
        *checked += (uint32_t)(out->span_bytes < remaining ? out->span_bytes : remaining);
    }
    return failed(out, *checked == bytes ? 0 : E2BIG);
}

int x9_mm_smmu_check_scatter(const struct x9_mm_smmu_regs *r,
                             const struct x9_mm_smmu_reader *reader,
                             const struct x9_mm_smmu_query *q,
                             const struct x9_mm_smmu_dma_span *spans,
                             uint32_t count, uint32_t bytes, uint32_t access,
                             uint32_t *checked, uint32_t *failed_span,
                             struct x9_mm_smmu_result *out)
{
    struct x9_mm_smmu_query query;
    uint32_t total = 0, local_checked, i, j;
    int e;
    if (!out || !checked || !failed_span) return EINVAL;
    memset(out, 0, sizeof(*out));
    out->stage = X9_MM_SMMU_REGS;
    *checked = 0;
    *failed_span = 0;
    if (!r || !reader || !q || !spans || !count ||
        count > X9_MM_SMMU_MAX_DMA_SPANS || !bytes ||
        bytes > X9_MM_SMMU_MAX_CHECK_BYTES || (access & ~7u) ||
        !(access & (X9_MM_SMMU_READ | X9_MM_SMMU_WRITE)))
        return failed(out, EINVAL);
    e = aperture(q->sid, q->iova, bytes);
    if (e) return failed(out, e);
    /* No table callbacks until the ENTIRE description has passed validation.
     * Keep immutable inputs in place rather than a multi-KiB kernel copy. */
    for (i = 0; i < count; i++) {
        const struct x9_mm_smmu_dma_span *s = &spans[i];
        *failed_span = i;
        if (s->reserved || !s->bytes || s->expected_pa >= PA_LIMIT ||
            s->bytes > PA_LIMIT - s->expected_pa)
            return failed(out, EINVAL);
        if (s->bytes > bytes - total) return failed(out, EOVERFLOW);
        for (j = 0; j < i; j++) {
            const struct x9_mm_smmu_dma_span *previous = &spans[j];
            if (s->expected_pa < previous->expected_pa + previous->bytes &&
                previous->expected_pa < s->expected_pa + s->bytes)
                return failed(out, EINVAL);
        }
        total += s->bytes;
    }
    *failed_span = count;
    if (total != bytes) return failed(out, EINVAL);
    query = *q;
    for (i = 0; i < count; i++) {
        *failed_span = i;
        query.iova = q->iova + *checked;
        e = x9_mm_smmu_check_mapping(r, reader, &query, spans[i].expected_pa,
                                    spans[i].bytes, access, &local_checked, out);
        *checked += local_checked;
        if (e) return e;
    }
    *failed_span = count;
    return 0;
}

int x9_mm_smmu_build_context(uint32_t abi, const struct x9_mm_smmu_regs *r,
                             const struct x9_mm_smmu_context_request *request,
                             const struct x9_mm_smmu_table_build *table,
                             struct x9_mm_smmu_context *out)
{
    unsigned endian, stall, oas, ips = 0, sidbits;
    uint64_t cd0, ste1;
    if (!out) return EINVAL;
    if ((r && host_intersects(out, sizeof(*out), r, sizeof(*r))) ||
        (request && host_intersects(out, sizeof(*out), request, sizeof(*request))) ||
        (table && host_intersects(out, sizeof(*out), table, sizeof(*table)))) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (abi != X9_MM_SMMU_CONTEXT_ABI) return ENOTSUP;
    if (!r || !request || !table || !request->epoch || !request->asid ||
        request->asid > 65535 || !request->cd_pa || (request->cd_pa & 63) ||
        !table->ttbr || (table->ttbr & 4095) || !table->table_count ||
        table->table_count > X9_MM_SMMU_BUILD_MAX_TABLES ||
        !table->mapped_bytes || (table->mapped_bytes & 4095) ||
        table->mapped_bytes > X9_MM_SMMU_MAX_CHECK_BYTES) return EINVAL;
    endian = (r->idr0 >> 21) & 3;
    stall = (r->idr0 >> 24) & 3;
    oas = pa_bits(r->idr5 & 7);
    sidbits = r->idr1 & 63;
    if (!(r->idr0 & 2) || ((r->idr0 >> 2) & 3) < 2 ||
        (endian != 0 && endian != 2) || stall > 1 || !(r->idr5 & 16) ||
        !oas || table->input_bits != 36 ||
        (!(r->idr0 & 4096) && request->asid > 255)) return ENOTSUP;
    if ((request->sid != X9_MM_SMMU_CODEC_SID &&
         request->sid != X9_MM_SMMU_HFRP_SID) || !sidbits || sidbits > 32 ||
        request->sid >= BIT(sidbits)) return ERANGE;
    while (ips < 6 && pa_bits(ips) != table->output_bits) ips++;
    if (ips == 6 || table->output_bits > oas) return ENOTSUP;
    if (request->cd_pa >= PA_LIMIT || request->cd_pa >= BIT(oas) ||
        64 > BIT(oas) - request->cd_pa ||
        table->ttbr >= BIT(table->output_bits) ||
        4096 > BIT(table->output_bits) - table->ttbr ||
        (request->cd_pa < table->ttbr + 4096 &&
         table->ttbr < request->cd_pa + 64)) return ERANGE;
    cd0 = 28 | BIT(8) | BIT(10) | (UINT64_C(3) << 12) |
        BIT(30) | BIT(31) | ((uint64_t)ips << 32) |
        BIT(41) | BIT(45) | BIT(46) | BIT(47) | ((uint64_t)request->asid << 48);
    ste1 = 2 | BIT(2) | BIT(4) | (UINT64_C(3) << 6);
    if (stall == 0) ste1 |= BIT(27); /* reject stalls; no PRI/RESUME consumer */
    out->request = *request;
    out->table = *table;
    store64(out->cd, cd0);
    store64(out->cd + 8, table->ttbr);
    store64(out->cd + 24, 255); /* matching AttrIndx0 in the private tree */
    store64(out->ste, request->cd_pa | 11); /* V + S1_TRANS, linear CD0 */
    store64(out->ste + 8, ste1);
    return 0;
}

/* A private view substitutes ONLY this stream's STE, CD and explicitly owned
 * tree pages. Post-install view instead reads those exact addresses from the
 * serialized provider, refusing walks into arbitrary inherited tables. */
struct install_run;
static int run_guard(struct install_run *);
struct install_view {
    const struct x9_mm_smmu_private_mapping *map;
    const struct x9_mm_smmu_install_io *io;
    uint64_t ste_pa;
    unsigned private_bytes;
    struct install_run *run;
};

static int view_read(void *cookie, uint64_t pa, uint8_t *dst, uint32_t bytes)
{
    const struct install_view *v = cookie;
    const struct x9_mm_smmu_private_mapping *m = v->map;
    const uint8_t *source = NULL;
    uint64_t base = 0, size = 0;
    int e = run_guard(v->run);
    if (e) return e;
    if (pa >= v->ste_pa && pa - v->ste_pa < 64) {
        base = v->ste_pa; size = 64; source = m->context.ste;
    } else if (pa >= m->context.request.cd_pa &&
               pa - m->context.request.cd_pa < 64) {
        base = m->context.request.cd_pa; size = 64; source = m->context.cd;
    } else {
        for (uint32_t i = 0; i < m->page_count; i++)
            if (pa >= m->pages[i].pa && pa - m->pages[i].pa < 4096) {
                base = m->pages[i].pa; size = 4096; source = m->pages[i].host;
                break;
            }
    }
    if (!source || bytes > size - (pa - base)) return EACCES;
    if (v->private_bytes) memcpy(dst, source + (pa - base), bytes);
    else {
        e = v->io->reader.read(v->io->reader.cookie, pa, dst, bytes);
        if (e) return e;
        /* A coherent copy is insufficient if any table was changed by a
         * concurrent owner. Exact equality is required, not just a valid PA. */
        if (memcmp(dst, source + (pa - base), bytes)) return ESTALE;
    }
    return 0;
}

static int check_private(struct install_view *view,
                           const struct x9_mm_smmu_regs *r, uint32_t *checked)
{
    struct x9_mm_smmu_reader reader = { .cookie = view, .read = view_read };
    struct x9_mm_smmu_regs linear = *r;
    struct x9_mm_smmu_result trace;
    uint32_t failed_span;
    uint64_t low = PA_LIMIT, high = 0;
    /* locate_ste computes the real STE address in an artificial LINEAR table;
     * this avoids granting the walker permission to inherited L1/CD tables. */
    linear.strtab_base = view->ste_pa - (uint64_t)view->map->query.sid * 64;
    linear.strtab_cfg = r->idr1 & 63;
    reader.ranges[0].start = view->ste_pa;
    reader.ranges[0].bytes = 64;
    reader.ranges[1].start = view->map->context.request.cd_pa;
    reader.ranges[1].bytes = 64;
    reader.range_count = 2;
    /* Used tree pages plus STE/CD may exceed the inspector range bound;
     * the callback enforces exact addresses, so one encompassing read range
     * is used for the pages, without permitting reads outside those pages. */
    for (uint32_t i = 0; i < view->map->page_count; i++) {
        uint64_t p = view->map->pages[i].pa;
        if (p < low) low = p;
        if (p + 4096 > high) high = p + 4096;
    }
    reader.ranges[2].start = low;
    reader.ranges[2].bytes = high - low;
    reader.range_count = 3;
    return x9_mm_smmu_check_scatter(&linear, &reader, &view->map->query,
        view->map->spans, view->map->span_count, view->map->total_bytes,
        view->map->access, checked, &failed_span, &trace);
}

static int intersects_pa(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a < b + bn && b < a + an; /* callers validate nonwrapping spans */
}

static int tree_valid(const struct x9_mm_smmu_private_mapping *m)
{
    uint64_t first = m->query.iova, end = first + m->total_bytes;
    uint64_t base2 = first >> 30, base3 = first >> 21;
    uint32_t n2 = (uint32_t)(((end - 1) >> 30) - base2 + 1);
    uint32_t n3 = (uint32_t)(((end - 1) >> 21) - base3 + 1);
    uint64_t attr = 3 | BIT(10) | (UINT64_C(3) << 8) | BIT(53) | BIT(54);
    uint64_t limit;
    uint32_t span = 0;
    uint64_t span_offset = 0;
    if (1 + n2 + n3 != m->page_count || m->context.table.output_bits > 48 ||
        m->context.table.output_bits < 32) return EINVAL;
    limit = BIT(m->context.table.output_bits);
    if (!(m->access & X9_MM_SMMU_WRITE)) attr |= BIT(7);
    if (m->access & X9_MM_SMMU_UNPRIV) attr |= BIT(6);
    for (uint32_t p = 0; p < m->page_count; p++) {
        if (m->pages[p].pa >= limit || 4096 > limit - m->pages[p].pa) return ERANGE;
        for (uint32_t index = 0; index < 512; index++) {
            uint64_t expected = 0;
            if (!p) {
                if (index >= base2 && index - base2 < n2)
                    expected = m->pages[1 + index - base2].pa | 3;
            } else if (p <= n2) {
                uint64_t block = ((base2 + p - 1) << 9) + index;
                if (block >= base3 && block - base3 < n3)
                    expected = m->pages[1 + n2 + block - base3].pa | 3;
            } else {
                uint64_t va = ((base3 + p - 1 - n2) << 21) + (uint64_t)index * 4096;
                if (va >= first && va < end) {
                    /* L3 pages and entries are visited in increasing IOVA
                     * order. Retain one scatter cursor: do not rescan all
                     * earlier spans for each of up to131072 leaf entries. */
                    if (span >= m->span_count ||
                        span_offset >= m->spans[span].bytes) return EINVAL;
                    uint64_t pa = m->spans[span].expected_pa + span_offset;
                    if (pa >= limit || 4096 > limit - pa) return ERANGE;
                    expected = pa | attr;
                    span_offset += 4096;
                    if (span_offset == m->spans[span].bytes) {
                        span++; span_offset = 0;
                    }
                }
            }
            /* Verify ALL entries including unused ones. A correct requested
             * translation alone must not hide mappings outside the lease. */
            if (le64(m->pages[p].host + (uint64_t)index * 8) != expected) return EINVAL;
        }
    }
    return span == m->span_count && !span_offset ? 0 : EINVAL;
}

static int mapping_valid(const struct x9_mm_smmu_private_mapping *m,
                          const struct x9_mm_smmu_install_io *io,
                          const struct x9_mm_smmu_install_result *out)
{
    uint64_t cd = m->context.request.cd_pa, sum = 0;
    uint64_t sn, pn;
    int e;
    if (!m->spans || !m->pages || !m->span_count ||
        m->span_count > X9_MM_SMMU_MAX_DMA_SPANS || !m->page_count ||
        m->page_count > X9_MM_SMMU_BUILD_MAX_TABLES ||
        m->page_count != m->context.table.table_count ||
        m->pages[0].pa != m->context.table.ttbr ||
        m->total_bytes != m->context.table.mapped_bytes ||
        !m->total_bytes || m->total_bytes > X9_MM_SMMU_MAX_CHECK_BYTES ||
        (m->total_bytes & 4095) || (m->query.iova & 4095) ||
        m->query.sid != m->context.request.sid || m->query.ssid || m->query.ssid_valid ||
        !(m->access & X9_MM_SMMU_READ) || (m->access & ~7u)) return EINVAL;
    sn = (uint64_t)m->span_count * sizeof(*m->spans);
    pn = (uint64_t)m->page_count * sizeof(*m->pages);
    if (host_intersects(m, sizeof(*m), io, sizeof(*io)) ||
        host_intersects(m, sizeof(*m), m->spans, sn) ||
        host_intersects(m, sizeof(*m), m->pages, pn) ||
        host_intersects(io, sizeof(*io), m->spans, sn) ||
        host_intersects(io, sizeof(*io), m->pages, pn) ||
        host_intersects(m->pages, pn, m->spans, sn)) return EINVAL;
    e = aperture(m->query.sid, m->query.iova, m->total_bytes);
    if (e) return e;
    if (!cd || (cd & 63) || cd >= PA_LIMIT || 64 > PA_LIMIT - cd) return EINVAL;
    for (uint32_t i = 0; i < m->span_count; i++) {
        const struct x9_mm_smmu_dma_span *s = &m->spans[i];
        if (s->reserved || !s->bytes || (s->bytes & 4095) ||
            (s->expected_pa & 4095) || s->expected_pa >= PA_LIMIT ||
            s->bytes > PA_LIMIT - s->expected_pa || s->bytes > m->total_bytes - sum ||
            intersects_pa(cd, 64, s->expected_pa, s->bytes)) return EINVAL;
        for (uint32_t j = 0; j < i; j++)
            if (intersects_pa(s->expected_pa, s->bytes,
                              m->spans[j].expected_pa, m->spans[j].bytes)) return EINVAL;
        sum += s->bytes;
    }
    if (sum != m->total_bytes) return EINVAL;
    for (uint32_t i = 0; i < m->page_count; i++) {
        const struct x9_mm_smmu_table_page *p = &m->pages[i];
        if (!p->host || ((uintptr_t)p->host & 7) || !p->pa || (p->pa & 4095) ||
            p->pa >= PA_LIMIT || 4096 > PA_LIMIT - p->pa ||
            intersects_pa(cd, 64, p->pa, 4096) ||
            host_intersects(p->host, 4096, m, sizeof(*m)) ||
            host_intersects(p->host, 4096, io, sizeof(*io)) ||
            host_intersects(p->host, 4096, out, sizeof(*out)) ||
            host_intersects(p->host, 4096, m->pages, m->page_count * sizeof(*m->pages)) ||
            host_intersects(p->host, 4096, m->spans, m->span_count * sizeof(*m->spans)))
            return EINVAL;
        for (uint32_t j = 0; j < m->span_count; j++)
            if (intersects_pa(p->pa, 4096, m->spans[j].expected_pa, m->spans[j].bytes))
                return EINVAL;
        for (uint32_t j = 0; j < i; j++)
            if (p->pa == m->pages[j].pa ||
                host_intersects(p->host, 4096, m->pages[j].host, 4096)) return EINVAL;
    }
    return tree_valid(m);
}

struct install_run {
    const struct x9_mm_smmu_private_mapping *map;
    const struct x9_mm_smmu_install_io *io;
    struct x9_mm_smmu_regs initial;
    uint64_t deadline, last_clock;
    uint32_t producer, uncertain;
};

static int run_guard(struct install_run *run)
{
    uint64_t now = run->io->clock_ns(run->io->cookie);
    if (now < run->last_clock) return EIO;
    run->last_clock = now;
    if (now >= run->deadline) return ETIMEDOUT;
    return run->io->guard(run->io->cookie, run->map->context.request.epoch);
}

static int live_regs(struct install_run *run, struct x9_mm_smmu_regs *r)
{
    struct x9_mm_smmu_result trace = {0};
    int e = run_guard(run);
    if (!e) e = run->io->read_regs(run->io->cookie, r);
    if (e) return e;
    if (r->idr0 != run->initial.idr0 || r->idr1 != run->initial.idr1 ||
        r->idr5 != run->initial.idr5 || r->cr0 != run->initial.cr0 ||
        r->cr0ack != run->initial.cr0ack || r->strtab_cfg != run->initial.strtab_cfg ||
        r->strtab_base != run->initial.strtab_base || r->cmdq_base != run->initial.cmdq_base)
        return ESTALE;
    e = regs_valid(r, &run->io->reader, &trace);
    if (!e && (!(r->cr0 & 8) || r->gerror != r->gerrorn ||
               r->cmdq_prod != run->producer)) e = EIO;
    return e;
}

static int guarded_clean(struct install_run *run, uint64_t pa, uint32_t bytes)
{
    int e = run_guard(run);
    if (!e) e = run->io->clean(run->io->cookie, pa, bytes);
    return e;
}

static int guarded_write(struct install_run *run, uint64_t pa,
                           const uint8_t *src, uint32_t bytes)
{
    int e = run_guard(run);
    if (!e) e = run->io->write(run->io->cookie, pa, src, bytes);
    return e;
}

static int guarded_store(struct install_run *run, uint64_t pa, uint64_t value)
{
    int e = run_guard(run);
    if (!e) { run->uncertain = 1; e = run->io->store64(run->io->cookie, pa, value); }
    return e;
}

static int guarded_barrier(struct install_run *run)
{
    int e = run_guard(run);
    if (!e) e = run->io->barrier(run->io->cookie);
    return e;
}

static int invalidate_context(struct install_run *run)
{
    struct x9_mm_smmu_regs r;
    uint8_t command[16];
    uint64_t words[4] = {
        UINT64_C(3) | ((uint64_t)run->map->query.sid << 32),
        UINT64_C(6) | ((uint64_t)run->map->query.sid << 32),
        UINT64_C(0x11) | ((uint64_t)run->map->context.request.asid << 48),
        UINT64_C(0x46) | (UINT64_C(2) << 12) | (UINT64_C(3) << 22) |
                        (UINT64_C(15) << 24)
    };
    unsigned qbits;
    uint32_t indexmask, ringmask, next, distance = 0, progressed;
    uint64_t base;
    int e = live_regs(run, &r);
    if (e) return e;
    if (r.cmdq_cons != r.cmdq_prod) return EBUSY;
    qbits = r.cmdq_base & 31;
    if (qbits < 2) return ENOTSUP; /* bounded batch needs four free entries */
    indexmask = (uint32_t)(BIT(qbits) - 1);
    ringmask = (uint32_t)(BIT(qbits + 1) - 1);
    base = r.cmdq_base & (ADDRESS_MASK & ~UINT64_C(31));
    next = run->producer;
    for (unsigned i = 0; i < 4 && !e; i++) {
        uint64_t pa = base + (uint64_t)(next & indexmask) * 16;
        store64(command, words[i]); store64(command + 8, i == 0 ? 1 : 0);
        /* Queue bytes may be fetched speculatively even before publication;
         * retain dependencies if any write/clean result is uncertain. */
        run->uncertain = 1;
        e = guarded_write(run, pa, command, 16);
        if (!e) e = guarded_clean(run, pa, 16);
        next = (next + 1) & ringmask;
    }
    if (!e) e = guarded_barrier(run);
    if (!e) e = run_guard(run);
    if (!e) e = run->io->publish(run->io->cookie, run->producer, next);
    if (e) return e;
    run->producer = next;
    /* Exact consumer target after CMD_SYNC, not an IPI/SMC return0. With the
     * exclusive producer lease no other client can move PROD beyond target. */
    for (;;) {
        e = live_regs(run, &r);
        if (e) return e;
        progressed = (r.cmdq_cons - ((next - 4) & ringmask)) & ringmask;
        if (progressed > 4 || progressed < distance) return EIO;
        distance = progressed;
        if (r.cmdq_cons == next) return guarded_barrier(run);
        e = run->io->pause(run->io->cookie, run->deadline);
        if (e) return e;
        if (run->io->clock_ns(run->io->cookie) <= run->last_clock) return EIO;
    }
}

static int protected_objects(const struct x9_mm_smmu_private_mapping *m,
                               const struct x9_mm_smmu_regs *r, uint64_t ste,
                               const struct x9_mm_smmu_reader *reader)
{
    unsigned qbits = r->cmdq_base & 31;
    uint64_t queue = r->cmdq_base & (ADDRESS_MASK & ~UINT64_C(31));
    uint64_t bytes = BIT(qbits + 4), cd = m->context.request.cd_pa;
    if (ste < (uint64_t)m->query.sid * 64 ||
        intersects_pa(ste, 64, queue, bytes) ||
        intersects_pa(ste, 64, cd, 64) || intersects_pa(queue, bytes, cd, 64)) return EINVAL;
    for (uint32_t i = 0; i < m->page_count; i++)
        if (intersects_pa(ste, 64, m->pages[i].pa, 4096) ||
            intersects_pa(queue, bytes, m->pages[i].pa, 4096)) return EINVAL;
    for (uint32_t i = 0; i < m->span_count; i++)
        if (intersects_pa(ste, 64, m->spans[i].expected_pa, m->spans[i].bytes) ||
            intersects_pa(queue, bytes, m->spans[i].expected_pa, m->spans[i].bytes))
            return EINVAL;
    /* Authorize reads using narrow stream-table allocation ranges, then
     * exclude ALL of them from private storage/backing/command writes. It is
     * not enough to protect only the selected 64-byte STE. */
    for (uint32_t i = 0; i < reader->range_count; i++) {
        const struct x9_mm_smmu_range *a = &reader->ranges[i];
        if (intersects_pa(a->start, a->bytes, cd, 64) ||
            intersects_pa(a->start, a->bytes, queue, bytes)) return EINVAL;
        for (uint32_t p = 0; p < m->page_count; p++)
            if (intersects_pa(a->start, a->bytes, m->pages[p].pa, 4096)) return EINVAL;
        for (uint32_t s = 0; s < m->span_count; s++)
            if (intersects_pa(a->start, a->bytes,
                              m->spans[s].expected_pa, m->spans[s].bytes)) return EINVAL;
    }
    return 0;
}

static int install_operation(const struct x9_mm_smmu_private_mapping *m,
                               const struct x9_mm_smmu_install_io *io,
                               uint32_t deadline_ms, struct x9_mm_smmu_install_result *out,
                               unsigned retire)
{
    struct install_run run = { .map = m, .io = io };
    struct x9_mm_smmu_result trace = {0};
    struct x9_mm_smmu_context expected;
    struct x9_mm_smmu_regs r;
    struct install_view view = { .map = m, .io = io, .private_bytes = !retire, .run = &run };
    uint8_t zero[64] = {0};
    uint64_t raw[8];
    uint64_t budget = (uint64_t)deadline_ms * 1000000;
    int e;
    if (!out) return EINVAL;
    /* Validate every output alias BEFORE clearing it or acquiring hardware. */
    if ((m && host_intersects(out, sizeof(*out), m, sizeof(*m))) ||
        (io && host_intersects(out, sizeof(*out), io, sizeof(*io)))) return EINVAL;
    if (m && m->spans && m->span_count <= X9_MM_SMMU_MAX_DMA_SPANS &&
        host_intersects(out, sizeof(*out), m->spans, m->span_count * sizeof(*m->spans)))
        return EINVAL;
    if (m && m->pages && m->page_count <= X9_MM_SMMU_BUILD_MAX_TABLES) {
        if (host_intersects(out, sizeof(*out), m->pages, m->page_count * sizeof(*m->pages)))
            return EINVAL;
        for (uint32_t i = 0; i < m->page_count; i++)
            if (m->pages[i].host && host_intersects(out, sizeof(*out), m->pages[i].host, 4096))
                return EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (retire) out->state = X9_MM_SMMU_QUARANTINED;
    if (!m || !io || !io->enter || !io->guard || !io->leave || !io->read_regs ||
        !io->write || !io->store64 || !io->clean || !io->barrier || !io->publish ||
        !io->clock_ns || !io->pause || !io->reader.read ||
        !deadline_ms || deadline_ms > 1000) {
        out->error = EINVAL; return EINVAL;
    }
    out->epoch = m->context.request.epoch;
    e = mapping_valid(m, io, out);
    if (e) { out->error = (uint32_t)e; return e; }
    run.last_clock = io->clock_ns(io->cookie);
    if (run.last_clock > UINT64_MAX - budget) { out->error = EOVERFLOW; return EOVERFLOW; }
    run.deadline = run.last_clock + budget;
    e = io->enter(io->cookie, m);
    if (e) { out->error = (uint32_t)e; return e; }
    /* Retirement begins with exposed resources, including errors before the
     * first write. Never advertise UNEXPOSED/RETIRED on an unverified lease. */
    run.uncertain = retire;
    e = run_guard(&run);
    if (!e) e = io->read_regs(io->cookie, &run.initial);
    if (!e) e = regs_valid(&run.initial, &io->reader, &trace);
    if (!e && (!(run.initial.cr0 & 8) || !trace.command_queue_empty ||
               run.initial.gerror != run.initial.gerrorn)) e = EBUSY;
    if (!e) e = x9_mm_smmu_build_context(X9_MM_SMMU_CONTEXT_ABI, &run.initial,
                                          &m->context.request, &m->context.table, &expected);
    if (!e && memcmp(&expected, &m->context, sizeof(expected))) e = EINVAL;
    if (!e) e = locate_ste(&run.initial, &io->reader, &m->query, &trace);
    view.ste_pa = out->ste_pa = trace.ste_pa;
    if (!e) e = protected_objects(m, &run.initial, view.ste_pa, &io->reader);
    if (!e) {
        if (retire) {
            for (unsigned i = 0; i < 8; i++)
                if (trace.ste[i] != le64(m->context.ste + 8 * i)) { e = ESTALE; break; }
        } else {
            if (trace.ste[0] > 1) e = EBUSY;
            for (unsigned i = 1; i < 8 && !e; i++) if (trace.ste[i]) e = EBUSY;
        }
    }
    if (!e) e = run_guard(&run);
    if (!e) e = check_private(&view, &run.initial, &out->checked_bytes);
    run.producer = run.initial.cmdq_prod;
    if (e) goto leave;
    if (!retire) {
        /* A newly leased CD must be unreferenced and zero; do not overwrite
         * a still-valid CD from a previous boot/stream. Provider ownership is
         * independently checked by enter, not inferred from these bytes. */
        e = run_guard(&run);
        if (!e) e = io->reader.read(io->reader.cookie, m->context.request.cd_pa, zero, 64);
        if (!e) for (unsigned i = 0; i < 64; i++) if (zero[i]) { e = EBUSY; break; }
        for (uint32_t i = 0; i < m->page_count && !e; i++)
            e = guarded_clean(&run, m->pages[i].pa, 4096);
        if (!e) e = guarded_write(&run, m->context.request.cd_pa + 8, m->context.cd + 8, 56);
        if (!e) e = guarded_clean(&run, m->context.request.cd_pa + 8, 56);
        if (!e) e = guarded_barrier(&run);
        /* CD is unexposed; do not mark resources exposed solely for this
         * atomic write, but an error still retains them conservatively. */
        if (!e) e = guarded_store(&run, m->context.request.cd_pa, le64(m->context.cd));
        if (!e) e = guarded_clean(&run, m->context.request.cd_pa, 8);
        if (!e) e = guarded_barrier(&run);
    }
    /* Re-read target immediately before break-before-make; lease forbids
     * another owner writing it. Detect changed bytes rather than overwrite. */
    if (!e) e = run_guard(&run);
    if (!e) e = read_words(&io->reader, view.ste_pa, raw, 8);
    if (!e && memcmp(raw, trace.ste, sizeof(raw))) e = ESTALE;
    if (!e) e = guarded_store(&run, view.ste_pa, trace.ste[0] & ~UINT64_C(1));
    if (!e) e = guarded_clean(&run, view.ste_pa, 8);
    if (!e) e = guarded_barrier(&run);
    if (!e) e = invalidate_context(&run);
    if (!e && retire) {
        e = guarded_write(&run, view.ste_pa + 8, zero + 8, 56);
        if (!e) e = guarded_store(&run, view.ste_pa, 0);
        if (!e) e = guarded_clean(&run, view.ste_pa, 64);
        if (!e) e = guarded_barrier(&run);
        if (!e) e = invalidate_context(&run);
        if (!e) e = guarded_store(&run, m->context.request.cd_pa,
                                    le64(m->context.cd) & ~BIT(31));
        if (!e) e = guarded_clean(&run, m->context.request.cd_pa, 8);
        if (!e) e = guarded_barrier(&run);
        if (!e) e = read_words(&io->reader, view.ste_pa, raw, 8);
        if (!e) for (unsigned i = 0; i < 8; i++) if (raw[i]) { e = EIO; break; }
    } else if (!e) {
        e = guarded_write(&run, view.ste_pa + 8, m->context.ste + 8, 56);
        if (!e) e = guarded_clean(&run, view.ste_pa + 8, 56);
        if (!e) e = guarded_barrier(&run);
        if (!e) e = guarded_store(&run, view.ste_pa, le64(m->context.ste));
        if (!e) e = guarded_clean(&run, view.ste_pa, 8);
        if (!e) e = guarded_barrier(&run);
        if (!e) e = invalidate_context(&run);
        view.private_bytes = 0;
        if (!e) e = live_regs(&run, &r);
        if (!e) e = locate_ste(&r, &io->reader, &m->query, &trace);
        if (!e && trace.ste_pa != view.ste_pa) e = ESTALE;
        if (!e) e = check_private(&view, &r, &out->checked_bytes);
    }
    if (!e) e = live_regs(&run, &r);
    if (!e && r.cmdq_cons != run.producer) e = EBUSY;
    if (!e) e = run_guard(&run);
leave:
    out->producer = run.producer;
    out->error = (uint32_t)e;
    out->state = e ? (run.uncertain ? X9_MM_SMMU_QUARANTINED : X9_MM_SMMU_UNEXPOSED) :
        retire ? X9_MM_SMMU_RETIRED : X9_MM_SMMU_INSTALLED;
    io->leave(io->cookie, out->state);
    return e;
}

int x9_mm_smmu_install_stage1(const struct x9_mm_smmu_private_mapping *m,
                              const struct x9_mm_smmu_install_io *io,
                              uint32_t deadline_ms, struct x9_mm_smmu_install_result *out)
{
    return install_operation(m, io, deadline_ms, out, 0);
}

int x9_mm_smmu_retire_stage1(const struct x9_mm_smmu_private_mapping *m,
                             const struct x9_mm_smmu_install_io *io,
                             uint32_t deadline_ms, struct x9_mm_smmu_install_result *out)
{
    return install_operation(m, io, deadline_ms, out, 1);
}
