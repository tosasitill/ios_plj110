/* Real VT dispatch -> clone-owned native codec. Firmware keeps references;
 * this file holds only VT frame identities and their metadata descriptions.
 * Temporal processing permits deferred display-order output. Without it we
 * synchronously drain before returning, as VT requires. A drain MUST retain
 * the codec DPB/parameter sets (see session_ops contract), not recreate IDRs.
 * No host POC sorting, software references, worker or successful ACK-as-frame.
 */
static OSStatus codec_error(int e)
{
    if (e == ENOMEM) return kVTAllocationFailedErr;
    if (e == EINVAL || e == ENOTSUP || e == E2BIG || e == EMSGSIZE)
        return kVTVideoDecoderBadDataErr;
    return kVTVideoDecoderNotAvailableNowErr;
}
static uint64_t video_clock_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static unsigned pending_index(struct state *s, uint64_t token)
{
    unsigned i = 0;
    while (i < s->pending_count && s->pending[i].token != token) i++;
    return i;
}
static struct pending_frame pending_take(struct state *s, unsigned i)
{
    struct pending_frame f = s->pending[i];
    s->pending[i] = s->pending[--s->pending_count];
    memset(&s->pending[s->pending_count], 0, sizeof(s->pending[0]));
    return f;
}
static OSStatus video_emit(struct state *s, const struct pending_frame *f,
                          OSStatus error, VTDecodeInfoFlags flags, CVPixelBufferRef pixel)
{
    if (s->invalid || !s->session) return kVTInvalidSessionErr;
    s->emitting = true;
    OSStatus e = emit(s->session, f->frame, error,
                     flags | (f->deferred ? kVTDecodeInfo_Asynchronous : 0), pixel);
    s->emitting = false;
    return e;
}
static void video_fault(struct state *s, OSStatus error)
{
    /* Once input was accepted, this decoder owns its completion, including
     * errors. Retire each token before emitting, never return an error that
     * would cause VT's duct to emit the same accepted current frame again. */
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    s->native_session = 0;
    memset(&s->hdr_sequence,0,sizeof(s->hdr_sequence));
    while (s->pending_count && !s->invalid) {
        struct pending_frame f = pending_take(s, 0);
        OSStatus e = video_emit(s, &f, error, 0, NULL);
        CFRelease(f.description);
        if (e) { video_cancel(s); break; }
    }
}
/* One RECEIVE always names its ORIGINAL token, even when a later input
 * caused display output. Attach that token's immutable CM description. */
static int video_receive(struct state *s, uint32_t wait_ms, OSStatus *error)
{
    size_t bytes = (size_t)s->config.width * s->config.height * 3 /
                   (s->config.bit_depth == 10 ? 1u : 2u);
    uint8_t *raw = malloc(bytes);
    if (!raw) { *error = kVTAllocationFailedErr; return ENOMEM; }
    struct x9_vdec_output r = { .version = X9_VDEC_VERSION, .deadline_ms = wait_ms,
        .generation = s->generation, .session = s->native_session,
        .output_address = (uint64_t)(uintptr_t)raw, .output_capacity = (uint32_t)bytes };
    if (ioctl(s->fd, X9_VDEC_SESSION_RECEIVE, &r)) {
        int e = errno; free(raw); *error = codec_error(e); return e;
    }
    unsigned i = pending_index(s, r.token);
    if (!r.token || !r.completion || r.reserved || i == s->pending_count ||
        (r.result_flags & ~X9_VDEC_OUTPUT_DROPPED) || (r.flags & ~X9_VDEC_FULL_RANGE) ||
        (!(r.result_flags & X9_VDEC_OUTPUT_DROPPED) && (r.output_bytes != bytes ||
         r.width != s->config.width || r.height != s->config.height ||
         r.format != (s->config.bit_depth == 10 ? X9_VDEC_P010 : X9_VDEC_NV12)))) {
        free(raw); *error = kVTVideoDecoderBadDataErr; return EIO;
    }
    struct pending_frame f = pending_take(s, i);
    CVPixelBufferRef pixel = NULL;
    OSStatus result = noErr;
    VTDecodeInfoFlags flags = 0;
    if ((r.result_flags & X9_VDEC_OUTPUT_DROPPED) || f.suppress) flags = kVTDecodeInfo_FrameDropped;
    else {
        struct x9_vdec_picture picture = { .width = r.width, .height = r.height,
            .format = r.format, .flags = r.flags, .output_bytes = r.output_bytes };
        result = pixel_buffer(&picture, raw, f.description, &pixel);
    }
    free(raw);
    OSStatus emitted = video_emit(s, &f, result, flags, pixel);
    if (pixel) CVPixelBufferRelease(pixel);
    CFRelease(f.description);
    *error = emitted;
    return emitted ? EIO : 0;
}
static OSStatus video_collect(struct state *s, Boolean drain)
{
    if (s->invalid || s->fd < 0) return kVTInvalidSessionErr;
    if (drain) {
        struct x9_vdec_session r = { .version = X9_VDEC_VERSION,
            .generation = s->generation, .session = s->native_session };
        if (ioctl(s->fd, X9_VDEC_SESSION_DRAIN, &r)) return codec_error(errno);
    }
    uint64_t now = video_clock_ms(), deadline = now + 5000;
    if (!now) return kVTVideoDecoderNotAvailableNowErr;
    /* At most max_pending outputs + the final ENODATA. Total RECEIVE wait
     * is bounded to5s, not5s per frame. DRAIN itself has its own5s bound. */
    for (unsigned n = 0; n <= s->max_pending && !s->invalid; n++) {
        uint32_t wait = 0;
        if (drain) {
            now = video_clock_ms();
            if (!now || now >= deadline) return kVTVideoDecoderNotAvailableNowErr;
            wait = (uint32_t)(deadline - now);
        }
        OSStatus error = noErr;
        int e = video_receive(s, wait, &error);
        if (e == ENODATA) return drain && !s->pending_count ? noErr : kVTVideoDecoderBadDataErr;
        if (e == EAGAIN) return drain ? kVTVideoDecoderNotAvailableNowErr : noErr;
        if (e) return error;
    }
    return s->invalid ? kVTInvalidSessionErr : kVTVideoDecoderBadDataErr;
}
static OSStatus decode_frame(Base object, void *frame, CMSampleBufferRef sample,
                            VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    if (info) *info = 0;
    if (!sample || !frame || CMSampleBufferGetNumSamples(sample) != 1 ||
        !CMSampleBufferDataIsReady(sample)) return kVTVideoDecoderUnsupportedDataFormatErr;
    /* Hold our CM object across synchronous callback invalidation/release. */
    CFRetain(object);
    struct state *s = storage(object);
    pthread_mutex_lock(&s->lock);
    uint8_t *input = NULL;
    CMVideoFormatDescriptionRef snapshot = NULL;
    uint64_t token = 0;
    Boolean accepted = false;
    OSStatus e = kVTVideoDecoderBadDataErr;
    if (s->invalid || s->fd < 0 || !s->hvcc) { e = kVTInvalidSessionErr; goto done; }
    if (s->emitting) {
        e = kVTVideoDecoderNotAvailableNowErr; goto done;
    }
    if (s->pending_count >= s->max_pending) {
        e = video_collect(s, false);
        if (e) { if (!s->invalid) video_fault(s, e); goto done; }
        if (s->invalid) { e = kVTInvalidSessionErr; goto done; }
        if (s->pending_count >= s->max_pending) {
            e = kVTVideoDecoderNotAvailableNowErr; goto done;
        }
    }
    struct x9_hevc_config c;
    CMVideoFormatDescriptionRef description = CMSampleBufferGetFormatDescription(sample);
    CFDataRef data = configuration(description, &c);
    if (!data || !CFEqual(data, s->hvcc)) { e = kVTVideoDecoderUnsupportedDataFormatErr; goto done; }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t bytes = block ? CMBlockBufferGetDataLength(block) : 0;
    if (!bytes || bytes > X9_HEVC_MAX_INPUT) goto done;
    input = malloc(bytes);
    if (!input) { e = kVTAllocationFailedErr; goto done; }
    e = CMBlockBufferCopyDataBytes(block, 0, bytes, input);
    if (e) goto done;
    struct x9_hevc_au_metadata metadata;
    struct x9_hevc_hdr_sequence prospective;
    int gate = x9_hevc_sample_metadata(&s->config,CFDataGetBytePtr(s->hvcc),
        (size_t)CFDataGetLength(s->hvcc),input,bytes,&metadata);
    if (!gate) gate = x9_hevc_hdr_sequence_next(&s->hdr_sequence,&metadata,&prospective);
    if (gate) { e = codec_error(gate); goto done; }
    /* Allocation and cross-source conflicts are resolved BEFORE SUBMIT. No
     * accepted token can be lost due to a later metadata allocation failure.
     * Container values stay picture-local; they never become SEI state. */
    e = x9_hevc_copy_hdr_description(description,&prospective.hdr,&snapshot);
    if (e) goto done;
    if (s->next_token == UINT64_MAX) { e = kVTVideoDecoderNotAvailableNowErr; goto done; }
    token = ++s->next_token;
    struct x9_vdec_submit r = { .version = X9_VDEC_VERSION, .generation = s->generation,
        .session = s->native_session, .input_address = (uint64_t)(uintptr_t)input,
        .input_bytes = (uint32_t)bytes, .token = token };
    if (ioctl(s->fd, X9_VDEC_SESSION_SUBMIT, &r)) {
        int error = errno;
        e = codec_error(error);
        if (error == EIO || error == ETIMEDOUT || error == ENXIO || error == ESTALE)
            video_fault(s, e);
        goto done; /* kernel/provider did NOT accept this current input */
    }
    s->hdr_sequence = prospective; /* provider accepted; not an ACK-as-frame */
    s->pending[s->pending_count++] = (struct pending_frame){ .token = token, .frame = frame,
        .description = snapshot,
        .suppress = !!(flags & kVTDecodeFrame_DoNotOutputFrame) };
    snapshot = NULL; /* ownership transferred to this ORIGINAL token */
    accepted = true;
    /* EnableAsync permits, but never requires, background decompression.
     * Only EnableTemporal permits indefinite display-order deferral. */
    e = video_collect(s, !(flags & kVTDecodeFrame_EnableTemporalProcessing));
    if (e && !s->invalid) video_fault(s, e);
    unsigned i = pending_index(s, token);
    if (i < s->pending_count) {
        s->pending[i].deferred = true;
        if (info) *info |= kVTDecodeInfo_Asynchronous;
    }
done:
    if (snapshot) CFRelease(snapshot);
    free(input);
    pthread_mutex_unlock(&s->lock);
    CFRelease(object);
    /* Accepted input has been emitted, retained, failed explicitly, or
     * canceled by invalidate. VT must not manufacture a duplicate callback. */
    return accepted ? noErr : e;
}
static OSStatus finish(Base object)
{
    CFRetain(object);
    struct state *s = storage(object);
    pthread_mutex_lock(&s->lock);
    OSStatus e = s->emitting ? kVTVideoDecoderNotAvailableNowErr : video_collect(s, true);
    if (e && !s->invalid && !s->emitting) video_fault(s, e);
    pthread_mutex_unlock(&s->lock);
    CFRelease(object);
    return e;
}
