/* Included by sweet_x9_vdec.c: clone-fd ownership and native codec session
 * orchestration. No MMIO, firmware boot, reference decoding or software DPB. */
#define VDEC_OPEN_FILES 64u
struct host_session {
    dev_t dev;
    int opened, draining, held;
    uint64_t id, generation, tokens[X9_VDEC_MAX_PENDING], held_token;
    uint32_t pending, config_bytes, held_flags;
    uint8_t *config_data;
    struct x9_hevc_config config;
    void *native;
    struct x9_vdec_frame frame;
};
static struct host_session host_sessions[VDEC_OPEN_FILES];
static uint32_t next_minor;
static uint64_t next_session;
static int vdec_clone(__unused dev_t d, int action)
{
    /* Clone lookup can be followed by stat or failed open, with no d_close.
     * Allocate no resources here. Never reuse dev_t while a cloned vnode may
     * still exist; reserve session storage only in the successful d_open. */
    if (action != DEVFS_CLONE_ALLOC) return -1;
    lck_mtx_lock(&vdec_lock);
    int n = next_minor < 0xffffff ? (int)++next_minor : -1;
    lck_mtx_unlock(&vdec_lock);
    return n;
}
static struct host_session *session_find(dev_t d)
{
    for (unsigned i = 0; i < VDEC_OPEN_FILES; i++)
        if (host_sessions[i].opened && host_sessions[i].dev == d) return &host_sessions[i];
    return NULL;
}
static int session_open(dev_t d)
{
    if (session_find(d)) return EBUSY;
    for (unsigned i = 0; i < VDEC_OPEN_FILES; i++) if (!host_sessions[i].opened) {
        host_sessions[i].opened = 1; host_sessions[i].dev = d; return 0;
    }
    return EMFILE;
}
static void session_clear(struct host_session *s)
{
    dev_t d = s->dev; int opened = s->opened;
    if (s->held && s->frame.cookie) platform.release(&s->frame);
    if (s->native) session_ops.destroy(s->native);
    if (s->config_data) kfree_data(s->config_data, s->config_bytes);
    memset(s, 0, sizeof(*s)); s->dev = d; s->opened = opened;
}
static void session_fault(int e)
{
    if (e == EIO || e == ETIMEDOUT) {
        status.ready = 0; status.profiles = 0; status.generation++;
        /* destroy owns cancellation/quarantine. No physical reset here. */
    }
}
static int session_create(struct host_session *s, struct x9_vdec_session *r)
{
    if (r->version != X9_VDEC_VERSION || r->session ||
        !span(r->config_address, r->config_bytes) || r->config_bytes > X9_HEVC_MAX_CONFIG) return EINVAL;
    if (s->native) return EBUSY;
    if (!status.ready || !platform.sessions) return ENXIO;
    if (r->generation != status.generation) return ESTALE;
    unsigned active = 0;
    for (unsigned i = 0; i < VDEC_OPEN_FILES; i++) active += host_sessions[i].native != NULL;
    if (active >= X9_VDEC_MAX_SESSIONS) return EBUSY;
    int e = 0;
    uint8_t *bs = NULL;
    size_t bs_bytes = 0;
    s->config_bytes = r->config_bytes;
    s->config_data = kalloc_data(s->config_bytes, Z_WAITOK);
    if (!s->config_data) { e = ENOMEM; goto done; }
    e = copyin((user_addr_t)r->config_address, s->config_data, s->config_bytes);
    if (!e) e = x9_hevc_config_parse(s->config_data, s->config_bytes, &s->config);
    if (e) goto done;
    if (!(status.profiles & (1u << s->config.decoder_profile)) ||
        s->config.coded_width > status.max_width || s->config.coded_height > status.max_height ||
        s->config.max_dpb > session_ops.max_pending) { e = ENOTSUP; goto done; }
    e = x9_hevc_parameter_sets_annexb(&s->config, s->config_data, NULL, 0, &bs_bytes);
    if (e) goto done;
    bs = kalloc_data(bs_bytes, Z_WAITOK);
    if (!bs) { e = ENOMEM; goto done; }
    e = x9_hevc_parameter_sets_annexb(&s->config, s->config_data, bs, bs_bytes, &bs_bytes);
    if (!e) e = session_ops.create(&s->config, bs, (uint32_t)bs_bytes, 5000, &s->native);
    if (!e && !s->native) e = EIO;
    if (!e) {
        if (next_session == UINT64_MAX) { e = EOVERFLOW; goto done; }
        s->id = ++next_session; s->generation = status.generation; r->session = s->id;
    }
done:
    if (bs) kfree_data(bs, bs_bytes);
    if (e) { session_fault(e); session_clear(s); }
    return e;
}
static int session_submit(struct host_session *s, const struct x9_vdec_submit *r)
{
    if (r->version != X9_VDEC_VERSION || !r->token ||
        !span(r->input_address, r->input_bytes) || r->input_bytes > X9_HEVC_MAX_INPUT) return EINVAL;
    if (s->draining) return EBUSY;
    if (s->pending == session_ops.max_pending) return EAGAIN;
    for (unsigned i = 0; i < s->pending; i++) if (s->tokens[i] == r->token) return EEXIST;
    uint8_t *input = kalloc_data(r->input_bytes, Z_WAITOK), *bs = NULL;
    size_t bs_bytes = 0;
    int e = input ? 0 : ENOMEM;
    if (!e) e = copyin((user_addr_t)r->input_address, input, r->input_bytes);
    if (!e) e = x9_hevc_au_annexb(&s->config, s->config_data, input, r->input_bytes, NULL, 0, &bs_bytes);
    if (e) goto done;
    bs = kalloc_data(bs_bytes, Z_WAITOK);
    if (!bs) { e = ENOMEM; goto done; }
    e = x9_hevc_au_annexb(&s->config, s->config_data, input, r->input_bytes, bs, bs_bytes, &bs_bytes);
    if (!e) e = session_ops.submit(s->native, &s->config, bs, (uint32_t)bs_bytes, r->token, 5000);
    if (!e) s->tokens[s->pending++] = r->token;
done:
    if (bs) kfree_data(bs, bs_bytes);
    if (input) kfree_data(input, r->input_bytes);
    return e;
}
static int session_receive(struct host_session *s, struct x9_vdec_output *r)
{
    struct x9_vdec_output in = { .version = X9_VDEC_VERSION, .deadline_ms = r->deadline_ms,
        .generation = r->generation, .session = r->session, .output_address = r->output_address,
        .output_capacity = r->output_capacity };
    if (memcmp(r, &in, sizeof(in)) || r->deadline_ms > 5000 ||
        !span(r->output_address, r->output_capacity) || r->output_capacity > X9_VDEC_MAX_OUTPUT) return EINVAL;
    uint32_t row = s->config.width * (s->config.bit_depth == 10 ? 2 : 1);
    uint32_t bytes = row * s->config.height * 3 / 2;
    if (r->output_capacity < bytes) return EMSGSIZE;
    int e = 0;
    if (!s->held) {
        memset(&s->frame, 0, sizeof(s->frame)); s->held_token = 0; s->held_flags = 0;
        e = session_ops.receive(s->native, r->deadline_ms, &s->frame, &s->held_token, &s->held_flags);
        if (e) {
            if (s->frame.cookie) { platform.release(&s->frame); memset(&s->frame, 0, sizeof(s->frame)); }
            if (e == ENODATA) {
                if (!s->draining || s->pending) return EIO;
                s->draining = 0;
            }
            return e;
        }
        s->held = 1;
    }
    unsigned index = 0;
    while (index < s->pending && s->tokens[index] != s->held_token) index++;
    if (index == s->pending || !s->held_token ||
        (s->held_flags & ~X9_VDEC_OUTPUT_DROPPED) || !s->frame.cookie || !s->frame.completion) return EIO;
    if (!(s->held_flags & X9_VDEC_OUTPUT_DROPPED)) {
        e = frame_valid(&s->config, &s->frame);
        if (e) return e;
        uint64_t offset = 0;
        for (unsigned plane = 0; plane < 2 && !e; plane++) {
            uint32_t top = s->config.crop_y >> plane, rows = s->config.height >> plane;
            for (uint32_t y = 0; y < rows && !e; y++) {
                const uint8_t *src = s->frame.plane[plane].data +
                    (uint64_t)(top + y) * s->frame.plane[plane].stride +
                    s->config.crop_x * (s->config.bit_depth == 10 ? 2 : 1);
                e = copyout(src, (user_addr_t)(r->output_address + offset), row); offset += row;
            }
        }
        if (e) return e; /* keep CPU-owned frame/token for a safe copyout retry */
        r->output_bytes = bytes; r->width = s->config.width; r->height = s->config.height;
        r->format = s->frame.format; r->flags = s->frame.flags;
    }
    r->token = s->held_token; r->completion = s->frame.completion; r->result_flags = s->held_flags;
    platform.release(&s->frame); memset(&s->frame, 0, sizeof(s->frame)); s->held = 0;
    s->tokens[index] = s->tokens[--s->pending]; status.completed++;
    return 0;
}
static int session_ioctl(dev_t d, u_long cmd, caddr_t data)
{
    struct host_session *s = session_find(d);
    if (!s) return EBADF;
    int e = 0;
    if (cmd == X9_VDEC_SESSION_CREATE) e = session_create(s, (struct x9_vdec_session *)data);
    else {
        /* All extension request structures deliberately share this prefix. */
        struct x9_vdec_session *r = (struct x9_vdec_session *)data;
        if (r->version != X9_VDEC_VERSION || !s->native || r->session != s->id) return EINVAL;
        if (cmd == X9_VDEC_SESSION_DESTROY) {
            if (r->config_bytes || r->config_address) return EINVAL;
            /* Destroy is valid after generation loss: close must still retire. */
            session_clear(s); return 0;
        }
        if (!status.ready) return ENXIO;
        if (r->generation != status.generation || r->generation != s->generation) return ESTALE;
        if (cmd == X9_VDEC_SESSION_SUBMIT) {
            status.requests++; e = session_submit(s, (const struct x9_vdec_submit *)data);
        } else if (cmd == X9_VDEC_SESSION_RECEIVE) e = session_receive(s, (struct x9_vdec_output *)data);
        else if (cmd == X9_VDEC_SESSION_DRAIN) {
            if (r->config_bytes || r->config_address) return EINVAL;
            if (!s->draining) {
                e = session_ops.drain(s->native, 5000);
                if (!e) s->draining = 1;
            }
        } else e = ENOTTY;
    }
    session_fault(e);
    if (e && e != EAGAIN && e != ENODATA) { status.last_errno = e; status.rejected++; }
    return e;
}
