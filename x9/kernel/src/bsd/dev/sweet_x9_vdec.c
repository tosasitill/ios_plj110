/* Safe host front end only. MT6993 rv_v2 VCP boot/IPI/MM-SMMU provider is
 * not present yet. Without it ready/profiles stay zero and no job is sent. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/file_internal.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <miscfs/devfs/devfs.h>
#include "x9_vdec.h"
#include "../../../../hevc/hevc_bitstream.c"
extern int sweet_early_is_x9(void);
extern int sweet_x9_arg(const char *, int);
LCK_GRP_DECLARE(vdec_group, "x9-vdec");
LCK_MTX_DECLARE(vdec_lock, &vdec_group);
static struct x9_vdec_status status = { .version = X9_VDEC_VERSION };
static struct x9_vdec_platform platform;
static struct x9_vdec_session_ops session_ops;
#include "x9_vdec_dma.inc"
#include "x9_vdec_memory.inc"
#include "x9_vdec_shared.inc"
#include "x9_vdec_control.inc"

int x9_vdec_register_platform(const struct x9_vdec_platform *p)
{
    if (!p || p->version != X9_VDEC_VERSION || !p->decode || !p->release ||
        p->prerequisites != X9_VDEC_REQUIRED ||
        !(p->profiles & X9_VDEC_MAIN) ||
        (p->profiles & ~(X9_VDEC_MAIN | X9_VDEC_MAIN10)) ||
        !p->max_width || !p->max_height || p->max_width > X9_HEVC_MAX_DIM ||
        p->max_height > X9_HEVC_MAX_DIM) return EINVAL;
    if (p->sessions && (p->sessions->capabilities != X9_VDEC_SESSION_REQUIRED ||
        !p->sessions->max_pending || p->sessions->max_pending > X9_VDEC_MAX_PENDING ||
        !p->sessions->create || !p->sessions->submit || !p->sessions->receive ||
        !p->sessions->drain || !p->sessions->destroy)) return EINVAL;
    lck_mtx_lock(&vdec_lock);
    int e = status.mode != 2 ? EPERM : platform.version ? EBUSY : 0;
    if (!e) {
        platform = *p;
        if (p->sessions) { session_ops = *p->sessions; platform.sessions = &session_ops; }
        status.prerequisites = p->prerequisites; status.profiles = p->profiles;
        status.max_width = p->max_width; status.max_height = p->max_height;
        status.generation++; status.ready = 1; status.last_errno = 0;
    }
    lck_mtx_unlock(&vdec_lock);
    return e;
}
static int span(uint64_t address, uint32_t bytes)
{
    return address && bytes && address <= UINT64_MAX - bytes;
}
static int frame_valid(const struct x9_hevc_config *c, const struct x9_vdec_frame *f)
{
    unsigned bpp = c->bit_depth == 10 ? 2 : 1;
    if (!f->completion || !f->cookie || (f->flags & ~X9_VDEC_FULL_RANGE) ||
        f->format != (bpp == 2 ? X9_VDEC_P010 : X9_VDEC_NV12) ||
        f->coded_width < c->coded_width || f->coded_height < c->coded_height ||
        f->coded_width > X9_HEVC_MAX_DIM || f->coded_height > X9_HEVC_MAX_DIM ||
        (f->coded_width & 1) || (f->coded_height & 1) ||
        f->width != c->width || f->height != c->height ||
        f->crop_x != c->crop_x || f->crop_y != c->crop_y) return EIO;
    for (unsigned i = 0; i < 2; i++) {
        const struct x9_vdec_plane *p = &f->plane[i];
        uint32_t rows = f->coded_height >> i;
        if (!p->data || p->reserved || p->stride < f->coded_width * bpp ||
            p->stride > X9_HEVC_MAX_DIM * 2 + 4096 ||
            p->bytes < (uint64_t)p->stride * rows ||
            p->bytes > X9_VDEC_MAX_OUTPUT ||
            (uintptr_t)p->data > UINTPTR_MAX - p->bytes) return EIO;
    }
    return 0;
}
static int decode(struct x9_vdec_picture *r)
{
    struct x9_vdec_picture in = {
        .version = X9_VDEC_VERSION, .generation = r->generation,
        .config_address = r->config_address, .input_address = r->input_address,
        .output_address = r->output_address, .config_bytes = r->config_bytes,
        .input_bytes = r->input_bytes, .output_capacity = r->output_capacity
    };
    if (memcmp(r, &in, sizeof(in)) || !span(in.config_address, in.config_bytes) ||
        !span(in.input_address, in.input_bytes) || !span(in.output_address, in.output_capacity) ||
        in.config_bytes > X9_HEVC_MAX_CONFIG || in.input_bytes > X9_HEVC_MAX_INPUT ||
        in.output_capacity > X9_VDEC_MAX_OUTPUT) return EINVAL;
    uint8_t *cfg = NULL, *input = NULL, *bs = NULL;
    size_t bs_bytes = 0;
    struct x9_vdec_frame frame = {0};
    struct x9_hevc_config c;
    int e = 0;
    status.requests++;
    if (!status.ready) { e = ENXIO; goto done; }
    if (in.generation != status.generation) { e = ESTALE; goto done; }
    cfg = kalloc_data(in.config_bytes, Z_WAITOK);
    input = kalloc_data(in.input_bytes, Z_WAITOK);
    if (!cfg || !input) { e = ENOMEM; goto done; }
    /* Snapshot before parsing or output, so overlapping user spans cannot
     * change already validated headers/lengths. Never hand user VAs to VCP. */
    e = copyin((user_addr_t)in.config_address, cfg, in.config_bytes);
    if (!e) e = copyin((user_addr_t)in.input_address, input, in.input_bytes);
    if (!e) e = x9_hevc_config_parse(cfg, in.config_bytes, &c);
    if (e) goto done;
    if (!(status.profiles & (1u << c.decoder_profile)) ||
        c.coded_width > status.max_width || c.coded_height > status.max_height) {
        e = ENOTSUP; goto done;
    }
    uint32_t row = c.width * (c.bit_depth == 10 ? 2 : 1);
    uint32_t out_bytes = row * c.height * 3 / 2;
    if (in.output_capacity < out_bytes) { e = EMSGSIZE; goto done; }
    e = x9_hevc_idr_annexb(&c, cfg, input, in.input_bytes, NULL, 0, &bs_bytes);
    if (e) goto done;
    bs = kalloc_data(bs_bytes, Z_WAITOK);
    if (!bs) { e = ENOMEM; goto done; }
    e = x9_hevc_idr_annexb(&c, cfg, input, in.input_bytes, bs, bs_bytes, &bs_bytes);
    if (e) goto done;
    e = platform.decode(&c, bs, (uint32_t)bs_bytes, 5000, &frame);
    /* A START ACK alone is NOT a decoded frame. Reject P01H/HYFBC, incomplete
     * pictures and mismatched crop before any byte is copied to the client. */
    if (!e) e = frame_valid(&c, &frame);
    if (!e) {
        uint64_t offset = 0;
        for (unsigned plane = 0; plane < 2 && !e; plane++) {
            const struct x9_vdec_plane *p = &frame.plane[plane];
            uint32_t top = c.crop_y >> plane, rows = c.height >> plane;
            for (uint32_t y = 0; y < rows && !e; y++) {
                const uint8_t *src = p->data + (uint64_t)(top + y) * p->stride +
                                     c.crop_x * (c.bit_depth == 10 ? 2 : 1);
                e = copyout(src, (user_addr_t)(in.output_address + offset), row);
                offset += row;
            }
        }
        if (!e) {
            r->output_bytes = out_bytes; r->width = c.width; r->height = c.height;
            r->format = frame.format; r->flags = frame.flags;
            r->completion = frame.completion; status.completed++;
        }
    }
    if (frame.cookie) platform.release(&frame);
    if (e == ETIMEDOUT || e == EIO) {
        status.ready = 0; status.profiles = 0; status.generation++;
        /* Platform owns quarantine; do NOT free physical DMA or reset VCP. */
    }
done:
    if (bs) kfree_data(bs, bs_bytes);
    if (input) kfree_data(input, in.input_bytes);
    if (cfg) kfree_data(cfg, in.config_bytes);
    status.last_errno = e;
    if (e) status.rejected++;
    return e;
}
#include "../../../../hevc/vdec_sessions.inc"
static int vdec_open(dev_t d, int flags, __unused int t, __unused struct proc *p)
{
    uid_t uid = kauth_cred_getuid(kauth_cred_get());
    if ((flags & FWRITE) && (status.mode != 2 || (uid && uid != 501))) return EPERM;
    lck_mtx_lock(&vdec_lock);
    int e = session_open(d);
    lck_mtx_unlock(&vdec_lock);
    return e;
}
static int vdec_close(dev_t d, __unused int f, __unused int t, __unused struct proc *p)
{
    lck_mtx_lock(&vdec_lock);
    struct host_session *s = session_find(d);
    if (s) { session_clear(s); memset(s, 0, sizeof(*s)); }
    lck_mtx_unlock(&vdec_lock);
    return 0;
}
static int vdec_ioctl(dev_t d, u_long cmd, caddr_t data, int flags, __unused struct proc *p)
{
    if (cmd != X9_VDEC_GET_STATUS && cmd != X9_VDEC_GET_CAPABILITIES &&
        cmd != X9_VDEC_DECODE_IDR && cmd != X9_VDEC_SESSION_CREATE &&
        cmd != X9_VDEC_SESSION_SUBMIT && cmd != X9_VDEC_SESSION_RECEIVE &&
        cmd != X9_VDEC_SESSION_DRAIN && cmd != X9_VDEC_SESSION_DESTROY) return ENOTTY;
    if (cmd != X9_VDEC_GET_STATUS && cmd != X9_VDEC_GET_CAPABILITIES) {
        uid_t uid = kauth_cred_getuid(kauth_cred_get());
        if (status.mode != 2 || !(flags & FWRITE) || (uid && uid != 501)) return EPERM;
    }
    /* Native operations are bounded. Clone-minor ownership follows the vnode,
     * not PID/uid: fork/dup keep one shared codec lifetime, last close drains. */
    lck_mtx_lock(&vdec_lock);
    int e = 0;
    if (cmd == X9_VDEC_GET_STATUS) memcpy(data, &status, sizeof(status));
    else if (cmd == X9_VDEC_GET_CAPABILITIES) {
        struct x9_vdec_capabilities c = { .version = X9_VDEC_VERSION,
            .generation = status.generation };
        if (status.ready && platform.sessions) {
            c.capabilities = session_ops.capabilities; c.max_pending = session_ops.max_pending;
        }
        memcpy(data, &c, sizeof(c));
    } else if (cmd == X9_VDEC_DECODE_IDR) {
        struct x9_vdec_picture picture;
        memcpy(&picture, data, sizeof(picture));
        e = decode(&picture);
        if (!e) memcpy(data, &picture, sizeof(picture));
    } else e = session_ioctl(d, cmd, data);
    lck_mtx_unlock(&vdec_lock);
    return e;
}
static struct cdevsw vdec_cdev = {
    .d_open = vdec_open, .d_close = vdec_close,
    .d_read = eno_rdwrt, .d_write = eno_rdwrt, .d_ioctl = vdec_ioctl,
    .d_stop = eno_stop, .d_reset = eno_reset, .d_ttys = NULL,
    .d_select = eno_select, .d_mmap = eno_mmap, .d_strategy = eno_strat,
    .d_type = 0
};
void sweet_x9_vdec_init(void)
{
    if (!sweet_early_is_x9()) return;
    int mode = sweet_x9_arg("x9_vdec", 0);
    if (mode != 1 && mode != 2) return;
    status.mode = (uint32_t)mode;
    int major = cdevsw_add(-1, &vdec_cdev);
    if (major < 0) { status.last_errno = ENXIO; return; }
    if (!devfs_make_node_clone(makedev(major, 0), DEVFS_CHAR, 501, 501, 0600,
                              vdec_clone, "x9-vdec")) {
        (void)cdevsw_remove(major, &vdec_cdev); status.last_errno = ENOMEM;
    }
    printf("X9 VDEC: mode=%d ready=0; VCP rv_v2/IPI/MM-SMMU provider absent, no hardware writes\n", mode);
}
