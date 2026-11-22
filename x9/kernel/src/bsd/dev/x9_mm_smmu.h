/* MM SMMUv3 snapshot inspection, private stage1 construction and explicitly
 * leased runtime installation. No automatic platform registration or MMIO.
 * Captured PLJ110 SID/apertures are recorded in NOTES.md. A matching runtime
 * capture is required; DT's gran4k-only is NOT a runtime TG0 selection.
 */
#ifndef X9_MM_SMMU_H
#define X9_MM_SMMU_H
#include <stdint.h>

#define X9_MM_SMMU_HFRP_SID 0xa0u
#define X9_MM_SMMU_CODEC_SID 0xb0u
#define X9_MM_SMMU_MAX_RANGES 16u
#define X9_MM_SMMU_MAX_CHECK_BYTES (512u * 1024u * 1024u)
#define X9_MM_SMMU_MAX_DMA_SPANS 4096u /* implementation bound, NOT a hardware limit */
#define X9_MM_SMMU_READ 1u
#define X9_MM_SMMU_WRITE 2u
#define X9_MM_SMMU_UNPRIV 4u

/* Host-endian register values, copied while the domain is stable. Offsets:
 * IDR0/1/5:0/4/14, CR0/ACK:20/24, STRTAB:80/88, CMDQ:90/98/9c,
 * GERROR/N:60/64. These offsets are not pointers or an MMIO mapping.
 */
struct x9_mm_smmu_regs {
    uint32_t idr0, idr1, idr5, cr0, cr0ack, strtab_cfg;
    uint32_t cmdq_prod, cmdq_cons, gerror, gerrorn;
    uint64_t strtab_base, cmdq_base;
};
struct x9_mm_smmu_range { uint64_t start, bytes; };
struct x9_mm_smmu_reader {
    void *cookie;
    /* Complete 8/64-byte LE copies from an immutable/coherent table snapshot.
     * Callback must be bounded, return errno and never allocate, map, wake or
     * modify the device. No PA-to-pointer cast is done by this inspector.
     * Live use requires provider-owned snapshot/serialization, not merely two
     * matching samples; shared firmware can otherwise change tables mid-walk.
     */
    int (*read)(void *, uint64_t pa, uint8_t *, uint32_t bytes);
    uint32_t range_count;
    struct x9_mm_smmu_range ranges[X9_MM_SMMU_MAX_RANGES];
};
struct x9_mm_smmu_query {
    uint32_t sid, ssid, ssid_valid; /* SID is never silently reused as SSID */
    uint64_t iova;
};
enum x9_mm_smmu_stage {
    X9_MM_SMMU_REGS = 1, X9_MM_SMMU_STE, X9_MM_SMMU_CD,
    X9_MM_SMMU_WALK, X9_MM_SMMU_DONE
};
struct x9_mm_smmu_result {
    uint32_t stage, error, sid, ssid;
    uint32_t page_shift, input_bits, output_bits, asid;
    uint32_t start_level, step_count, access, command_queue_empty;
    uint64_t ste_pa, cd_pa, ttbr;
    uint64_t ste[8], cd[8];
    struct { uint64_t pa, descriptor; uint32_t level, reserved; } steps[4];
    uint64_t output_pa, span_bytes;
    uint32_t attr_index, mair_byte, shareability, reserved;
};

/* Reads at most STE/CD + two L1 descriptors + four PTEs. Supports little-endian
 * AArch64 S1, linear/two-level STE/CD tables, 4K/16K TG0 and <=48-bit PA/IOVA.
 * Rejects bypass, S2/combined, disabled/invalid/unsupported formats, faults,
 * missing tables, unrecognized queue state, out-of-aperture IOVA and AF=0.
 * Empty queues/valid translations DO NOT confer ownership or authorize maps.
 * Output PA/span/access are zero on failure; raw diagnostic trace is retained.
 * Inputs/output must be disjoint; reader ranges authorize TABLE READS only.
 */
int x9_mm_smmu_translate(const struct x9_mm_smmu_regs *,
                          const struct x9_mm_smmu_reader *,
                          const struct x9_mm_smmu_query *,
                          struct x9_mm_smmu_result *);

/* Verify an existing contiguous mapping against expected provider-owned PA
 * and required READ/WRITE/UNPRIV bits; never installs or repairs a mapping.
 * Max512MiB, at most bytes/4096+2 leaf visits. Provider splits scattered DMA
 * pages into spans; the walk budget follows the actual requested length.
 * Reports first failing offset (bytes on success), and that translation's
 * trace. Permissions are conservative (no HA/HD permission promotion). MAIR
 * is the raw CD attribute, not an assertion about MTK STE attribute overrides.
 * It does not prove target PA ownership, cache sync or TLB freshness.
 */
int x9_mm_smmu_check_mapping(const struct x9_mm_smmu_regs *,
                             const struct x9_mm_smmu_reader *,
                             const struct x9_mm_smmu_query *, uint64_t expected_pa,
                             uint32_t bytes, uint32_t required_access,
                             uint32_t *checked_bytes, struct x9_mm_smmu_result *);

struct x9_mm_smmu_dma_span {
    uint64_t expected_pa; /* actual provider-owned physical backing, NOT IOVA */
    uint32_t bytes, reserved;
};
/* Check ONE complete existing IOVA extent against an ordered scatter list.
 * Derived IOVA addresses are query.iova + cumulative bytes, so descriptions
 * cannot leave gaps, duplicate an IOVA slice or extend past total_bytes. All
 * descriptions (1..4096) are validated BEFORE the first table read: nonempty,
 * reserved0, <=48-bit PA without wrap, sum exactly total_bytes<=512MiB, and
 * no physically overlapping backing. This narrow subset rejects aliases;
 * no page-alignment/PA-contiguity is inferred from a first SG DMA address.
 * Visits <=4096+span_count leaf translations for a 4K minimum granule.
 * Each slice uses the same explicit SID/SSID and immutable register/table
 * snapshot and compares every leaf against its actual expected PA and all
 * requested permission bits (at least READ or WRITE, optionally UNPRIV).
 * checked_bytes is the prefix actually verified, failed_span is the offending
 * description/translation (span_count on success or aggregate length error).
 * Raw first-failure trace is retained; PA/span/access are cleared on error.
 *
 * Inputs and outputs must be disjoint, stable host copies; no live tables or
 * allocation, mapping, queue/TLB writes, repair or DMA is performed. Read
 * authorization is still ONLY reader.ranges for TABLES; the scatter list
 * neither authorizes PA reads/writes nor proves backing ownership, hardware
 * TLB freshness/cache completion/firmware pairing/lifetime. A future provider
 * must supply real backing and snapshot serialization; never treat caller-
 * selected expected_pa or this check's return0 as permission to submit DMA.
 */
int x9_mm_smmu_check_scatter(const struct x9_mm_smmu_regs *,
                             const struct x9_mm_smmu_reader *,
                             const struct x9_mm_smmu_query *,
                             const struct x9_mm_smmu_dma_span *, uint32_t span_count,
                             uint32_t total_bytes, uint32_t required_access,
                             uint32_t *checked_bytes, uint32_t *failed_span,
                             struct x9_mm_smmu_result *);

#define X9_MM_SMMU_STAGE1_4K36_ABI 1u
#define X9_MM_SMMU_TABLE_BYTES 4096u
/* 512MiB at an unaligned 2MiB/1GiB start: L1 + up to2 L2 + up to257 L3. */
#define X9_MM_SMMU_BUILD_MAX_TABLES 260u
struct x9_mm_smmu_table_page {
    uint64_t pa; /* real provider-owned PA, NOT IOVA; no PA-to-pointer cast */
    uint8_t *host; /* private, writable, UNINSTALLED host storage, exactly 4KiB */
};
struct x9_mm_smmu_table_build {
    uint64_t ttbr;
    uint32_t table_count, mapped_bytes, input_bits, output_bits;
};
/* Exact private table capacity for the complete same-SID aperture. No table
 * reads, allocation, hardware authority or modification of the query. */
int x9_mm_smmu_stage1_geometry(const struct x9_mm_smmu_query *, uint32_t bytes,
                               uint32_t *table_count);
/* Construct a private three-level stage1 tree (4K, input36, start level1),
 * never modify installed tables or publish a CD/STE. A 16KiB CPU allocation
 * may supply separately described 4KiB subpages; PA is independently supplied
 * and must fit the explicit hardware/context output_bits (32/36/40/42/44/48).
 * ONE contiguous IOVA extent, full scatter backing, 4KiB address/length
 * alignment, same captured SID aperture and <=512MiB bound. No aliases/gaps or
 * intersecting backing/table PAs; source descriptors, result and ALL host
 * table buffers must be stable and mutually disjoint. All validation including
 * table capacity occurs BEFORE any table byte is changed. Error clears result
 * but leaves table storage unchanged; result/source or result/table alias is
 * a caller error rejected WITHOUT changing result or storage. Invalid pointers
 * are caller errors.
 * Uses only 4K page leaves, AF1, AttrIndx0, inner-shareable, PXN/UXN. Access
 * requires READ, optionally WRITE/UNPRIV (no write-only ARM mapping). Caller
 * must configure the matching MAIR Attr0 and verify actual hardware support.
 * Success zeroes/builds only table_count buffers, leaves extra buffers alone.
 * Provider must own backing/storage for the current epoch, prove cache sync,
 * CD/STE installation, command/ACK/TLB completion and lifetime separately;
 * this builder's success is NOT an active mapping or permission to DMA.
 */
int x9_mm_smmu_build_stage1(uint32_t abi, const struct x9_mm_smmu_query *,
                            const struct x9_mm_smmu_dma_span *, uint32_t span_count,
                            uint32_t total_bytes, uint32_t access,
                            uint32_t output_bits, const struct x9_mm_smmu_table_page *,
                            uint32_t page_count, struct x9_mm_smmu_table_build *);

#define X9_MM_SMMU_CONTEXT_ABI 1u
struct x9_mm_smmu_context_request {
    uint64_t epoch, cd_pa; /* fresh reset epoch and independently owned CD PA */
    uint32_t sid, asid; /* nonzero leased ASID; never inferred from SID */
};
struct x9_mm_smmu_context {
    struct x9_mm_smmu_context_request request;
    struct x9_mm_smmu_table_build table;
    uint8_t cd[64], ste[64]; /* LE wire descriptors, NOT installed pointers */
};
/* Build a private, linear single-CD stage1 context for the preceding 4K/36
 * tree. Uses CURRENT hardware IDRs, rejects force-stall/unsupported endian,
 * granule, ASID/OAS. CD0 only, no SSID, NSEL1, no ATS/PRI/HA/HD/TBI/S2/bypass.
 * Attr0=0xff (normal WB RA/WA); table walks WB RA/WA, inner-shareable.
 * CD R/A/ASET/AA64/V/EPD1 match the captured ordinary non-HA/HD path.
 * An epoch/PA/ASID argument is NOT proof of ownership or actual reset.
 * Stable disjoint host inputs/output required; alias rejected without writes,
 * other errors clear output. No device/memory callbacks or MMIO are issued.
 */
int x9_mm_smmu_build_context(uint32_t abi, const struct x9_mm_smmu_regs *,
                             const struct x9_mm_smmu_context_request *,
                             const struct x9_mm_smmu_table_build *,
                             struct x9_mm_smmu_context *);

struct x9_mm_smmu_private_mapping {
    struct x9_mm_smmu_context context;
    struct x9_mm_smmu_query query; /* SID=context SID, SSID0/SSV0 */
    const struct x9_mm_smmu_dma_span *spans;
    const struct x9_mm_smmu_table_page *pages; /* EXACT used private tree pages */
    uint32_t span_count, page_count, total_bytes, access;
};
enum x9_mm_smmu_install_state {
    X9_MM_SMMU_UNEXPOSED = 0, X9_MM_SMMU_INSTALLED,
    X9_MM_SMMU_RETIRED, X9_MM_SMMU_QUARANTINED
};
struct x9_mm_smmu_install_result {
    uint64_t epoch, ste_pa;
    uint32_t state, error, checked_bytes, producer;
};
/* Runtime adapter, NOT an automatically registered platform provider.
 * All callbacks are mandatory, bounded, errno-returning (positive errno).
 * enter() must acquire exclusive stream/context/queue serialization, prove
 * fresh epoch, stopped endpoint, power/clock ownership, exact private CD/tree
 * and DMA backing ownership, cache policy, and table PA<->host correspondence.
 * Install CD must be fresh/zero/unreferenced; retire CD must belong solely to
 * this installed stream. No other stream may reference this CD/ASID/tree.
 * No Android mapping/READY or caller-chosen PA is sufficient. On enter error
 * it must unwind itself; leave() is called exactly once after enter success.
 * guard() revalidates the lease/epoch/stopped endpoint before every mutation
 * and completion. No concurrent queue producer or target STE/CD/table writer
 * is allowed. Other live streams are never reset/disabled/invalidated here.
 * reader provides current serialized CPU copies, not a saved Android dump;
 * its ranges authorize stream table reads. Private reads are narrowed again
 * by this implementation to the exact owned CD and used tree pages.
 * write() copies to already-owned CPU mappings; store64() is an aligned
 * atomic LE descriptor store, not a 32-bit pair. clean() completes CPU->SMMU
 * visibility; barrier() implements the real architecture device ordering.
 * publish() writes only CMDQ_PROD, compares the expected prior producer and
 * orders preceding command bytes; read_regs() independently reads live ACK,
 * queue and error registers. No callback may suppress a failed ACK/timeout.
 * clock_ns() is monotonic; pause() is bounded by the absolute deadline.
 * leave(state) releases serialization, NOT installed resources. INSTALLED
 * retains backing/table/ASID/power until RETIRED. QUARANTINED retains ALL
 * potentially exposed dependencies until separate proven platform recovery.
 * No reentry; all metadata/buffers remain stable, resident and disjoint for
 * the entire call. Caller is kernel-owned code, never an ioctl PA interface.
 */
struct x9_mm_smmu_install_io {
    void *cookie;
    struct x9_mm_smmu_reader reader;
    int (*enter)(void *, const struct x9_mm_smmu_private_mapping *);
    int (*guard)(void *, uint64_t epoch);
    void (*leave)(void *, uint32_t state);
    int (*read_regs)(void *, struct x9_mm_smmu_regs *);
    int (*write)(void *, uint64_t pa, const uint8_t *, uint32_t bytes);
    int (*store64)(void *, uint64_t pa, uint64_t host_value);
    int (*clean)(void *, uint64_t pa, uint32_t bytes);
    int (*barrier)(void *);
    int (*publish)(void *, uint32_t expected_prod, uint32_t new_prod);
    uint64_t (*clock_ns)(void *);
    int (*pause)(void *, uint64_t deadline_ns);
};
/* Real controlled install/retirement algorithms. Require enabled SMMU/CMDQ
 * and directly matching CR0ACK. Never reset the global SMMU or replace its
 * queue/stream-table registers. Install accepts ONLY a fully zero invalid
 * STE or canonical valid ABORT (word0=1, all other words0), not a live/bypass
 * context. Validates the private scatter translations before exposure and
 * again through the installed live STE after CFGI/CD/TLBI_ASID/CMD_SYNC is
 * consumed. Uses break-before-make, exact producer/wrap/consumer/error checks
 * and one shared <=1s deadline; return0 is never taken as ACK by itself.
 * Retire accepts only this exact context's STE/CD, clears V, invalidates and
 * drains before zeroing the remaining STE. Successful RETIRED is the only
 * exposed-path result permitting release. Any uncertain mutation/cache/
 * queue/epoch/timeout failure becomes QUARANTINED, never blind rollback.
 * Neither function enables a decoder or proves VCP READY/decode completion.
 */
int x9_mm_smmu_install_stage1(const struct x9_mm_smmu_private_mapping *,
                              const struct x9_mm_smmu_install_io *,
                              uint32_t deadline_ms, struct x9_mm_smmu_install_result *);
int x9_mm_smmu_retire_stage1(const struct x9_mm_smmu_private_mapping *,
                             const struct x9_mm_smmu_install_io *,
                             uint32_t deadline_ms, struct x9_mm_smmu_install_result *);
#endif
