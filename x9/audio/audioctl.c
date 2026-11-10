/* Own-code audio status and raw S16LE/stereo/48k playback tool. No install. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "../kernel/src/bsd/dev/x9_audio.h"
#include "../kernel/src/bsd/dev/x9_tfa98xx.h"
#include "../kernel/src/bsd/dev/x9_speaker_afe.h"
#include "../kernel/src/bsd/dev/x9_tfa_plj110.h"
#include "../kernel/src/bsd/dev/x9_audio_resources.h"
#include "../kernel/src/bsd/dev/x9_speaker_mt6993.h"
#include "../kernel/src/bsd/dev/x9_speaker_pll.h"
static volatile sig_atomic_t interrupted;
static void on_signal(int n) { (void)n; interrupted = 1; }
static int status(io_connect_t c, X9AudioControl *s) {
    size_t n = sizeof(*s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioStatus, NULL, 0, s, &n);
    if (r || n != sizeof(*s) || s->magic != X9_AUDIO_MAGIC || s->version != X9_AUDIO_VERSION) {
        fprintf(stderr, "status/ABI failed: 0x%x\n", r); return -1;
    }
    return 0;
}
static kern_return_t call(io_connect_t c, unsigned sel)
{ return IOConnectCallScalarMethod(c, sel, NULL, 0, NULL, NULL); }
static int dsp_status(io_connect_t c) {
    const char *states[] = { "unconfigured", "offline", "booting", "online", "quarantined" };
    for (uint64_t core = 0; core < 2; core++) {
        X9AudioDSPStatus s = {0}; size_t n = sizeof(s);
        kern_return_t r = IOConnectCallMethod(c, X9AudioDSPStatusSelector, &core, 1,
            NULL, 0, NULL, NULL, &s, &n);
        if (r || n != sizeof(s) || s.version != 1 || s.core != core || s.state > X9DSPQuarantined) {
            fprintf(stderr, "DSP status/ABI failed: 0x%x\n", r); return 1;
        }
        printf("ADSP%llu %s errno=%u reset-generation=%u pending=%u last-ipi=%u last-message=%u\n"
               "  tx=%llu rx=%llu ready=%llu ack=%llu stray=%llu malformed=%llu\n"
               "  timeout=%llu wake-error=%llu event-overflow=%llu\n",
               (unsigned long long)core, states[s.state], s.error, s.generation, s.event_pending,
               s.last_ipi, s.last_message, (unsigned long long)s.sent, (unsigned long long)s.received,
               (unsigned long long)s.ready_count, (unsigned long long)s.matched_acks,
               (unsigned long long)s.stray_acks, (unsigned long long)s.malformed,
               (unsigned long long)s.timeouts, (unsigned long long)s.wake_errors,
               (unsigned long long)s.event_overflows);
    }
    puts("DSP online is not speaker-ready; board boot/stop require root and x9_audio=2.");
    return 0;
}
static int board_status(io_connect_t c) {
    X9AudioBoardStatus s = {0}; size_t n = sizeof(s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioBoardStatusSelector, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.version != X9_AUDIO_BOARD_STATUS_VERSION) { fprintf(stderr,"board status/ABI failed: 0x%x\n",r); return 1; }
    printf("ADSP board flags=0x%x errno=%u restore=%u rollback=%u registers-valid=%u owned=0x%x AP=0x%x all=0x%x done=0x%x busy=0x%x\n"
           "  pending=0x%x sleep=0x%x DPSW=%x/%x vlogic=%x sram=%x\n"
           "  SMC op=%u a0=%llx a1=%llx a2=%llx a3=%llx calls=%llu resets=%llu timeouts=%llu\n",
           s.flags,s.error,s.restore_error,s.rollback_error,!!(s.flags&X9BoardRegistersValid),
           s.owned_vote,s.ap_vote,s.all_vote,s.pd_done,s.pd_busy,
           s.pending,s.sleep,s.dpsw_req,s.dpsw_ack,s.vlogic_ack,s.sram_ack,s.smc_op,
           (unsigned long long)s.smc_result[0],(unsigned long long)s.smc_result[1],
           (unsigned long long)s.smc_result[2],(unsigned long long)s.smc_result[3],
           (unsigned long long)s.smc_calls,(unsigned long long)s.resets,(unsigned long long)s.timeouts);
    puts("Cached snapshot only. Owned AO may remain pinned; no PA unmute or speaker-ready implied.");
    return 0;
}
static int afe_status(io_connect_t c) {
    struct x9_speaker_afe_status s = {0}; size_t n = sizeof(s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioAFEStatusSelector, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.version != X9_SPEAKER_AFE_STATUS_VERSION) {
        fprintf(stderr,"AFE status/ABI failed: 0x%x\n",r); return 1;
    }
    printf("AFE registered=%u leased=%u prepared=%u running=%u poisoned=%u errno=%u restore=%u\n"
           "  dirty=0x%x uncertain=0x%x partial-copy=%u mute=%u disable=%u quiesce=%u\n"
           "  played=%llu submitted=%llu cursor=%u cached-DL24=%x cached-OUT4=%x\n"
           "  low-water-stops=%llu late-polls=%llu\n",
           s.registered,s.leased,s.prepared,s.running,s.poisoned,s.error,s.restore_error,
           s.dirty_mask,s.uncertain_mask,s.copy_pending,s.mute_error,s.disable_error,s.quiesce_error,
           (unsigned long long)s.played,(unsigned long long)s.submitted,s.cursor,s.dl24,s.out4,
           (unsigned long long)s.low_water_stops,(unsigned long long)s.late_polls);
    puts("Cached only; absent provider is expected. Watchdog scheduling is not a hardware no-replay guarantee.");
    return 0;
}
static int stream_status(io_connect_t c) {
    _Static_assert(sizeof(X9AudioStreamStatus) == 48, "stream status v2 size");
    X9AudioStreamStatus s = {0}; size_t n = sizeof(s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioStreamStatusSelector, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.version != X9_AUDIO_STREAM_STATUS_VERSION) { fprintf(stderr,"stream status/ABI failed: 0x%x\n",r); return 1; }
    printf("Physical backend lease=%u last-stop-errno=%u (Running=0 alone does not prove DMA/PA stopped)\n",
           s.backend_held,s.stop_error);
    printf("  physical prefill=%u committed=%u played=%llu queued=%llu anchor=%llu cursor-retries=%u\n",
           s.prefill_frames,s.prefill_committed,(unsigned long long)s.physical_played,
           (unsigned long long)s.physical_queued,(unsigned long long)s.physical_anchor,s.position_retries);
    puts("Raw cursor includes hidden prefill; PCM played/queued exclude it. Prefill is not audible-output proof.");
    return 0;
}
static int resource_status(io_connect_t c) {
    _Static_assert(sizeof(X9AudioResourceStatus) == 64, "resource status v1 size");
    X9AudioResourceStatus s = {0}; size_t n = sizeof(s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioResourceStatusSelector, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.version != X9_AUDIO_RESOURCE_STATUS_VERSION ||
        s.owner > X9AudioResourceSpeaker || s.last_owner > X9AudioResourceSpeaker) {
        fprintf(stderr,"resource status/ABI failed: 0x%x\n",r); return 1;
    }
    const char *owners[] = { "none", "ADSP", "microphone", "speaker AFE" };
    printf("AP audio resource owner=%s ticket=%llu retained-errno=%u last-owner=%s last-errno=%u release-errno=%u\n"
           "  claims=%llu releases=%llu refusals=%u rejected-releases=%llu\n",
           owners[s.owner],(unsigned long long)s.ticket,s.held_error,owners[s.last_owner],s.last_error,s.release_error,
           (unsigned long long)s.claims,(unsigned long long)s.releases,s.refusals,(unsigned long long)s.rejected_releases);
    puts("Software AP transition lease only; not a clock/rail/DMA readiness proof. No user claim/release or MMIO.");
    return 0;
}
static int amp_status(io_connect_t c) {
    X9AudioAmpStatus s = {0}; size_t n = sizeof(s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioAmpStatusSelector, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.version != X9_TFA_BOARD_STATUS_VERSION) {
        fprintf(stderr,"PA status/ABI failed: 0x%x\n",r); return 1;
    }
    printf("PA cached flags=0x%x stage=%u errno=%u restore=%u dirty=0x%x\n"
           "  reg03 left=%04x right=%04x read-mask=%x known-family-mask=%x\n"
           "  PMIF=%x reset-before DIR/DO/DI/MODE/SW/AD=%x/%x/%x/%x/%x/%x\n"
           "  LEFT supply-before selector/EN/DA/HW=%x/%x/%x/%x\n"
           "  reset-after=%x/%x/%x/%x/%x/%x LEFT supply-after=%x/%x/%x/%x\n"
           "  gate=%x DMA-gate=%x DMA-EN=%x pins=%x/%x parent=%u con1=%x nominal-Hz=%u probe-Hz=%u\n"
           "  DMA-parent=%u con1=%x last-slave=%x request STAT/START/FIFO=%x/%x/%x\n",
           s.flags,s.stage,s.error,s.restore_error,s.dirty_mask,
           s.revision[0],s.revision[1],s.revision_read,s.known_family,
           s.pmif_status,s.reset[0],s.reset[1],s.reset[2],s.reset[3],s.reset[4],s.reset[5],
           s.supply[0],s.supply[1],s.supply[2],s.supply[3],
           s.reset_after[0],s.reset_after[1],s.reset_after[2],s.reset_after[3],s.reset_after[4],s.reset_after[5],
           s.supply_after[0],s.supply_after[1],s.supply_after[2],s.supply_after[3],
           s.gate,s.dma_gate,s.dma_enable,s.pin[0],s.pin[1],s.parent,s.pll_con1,s.nominal_parent_hz,s.timing_hz,
           s.dma_parent,s.dma_pll_con1,s.last_slave,s.request_status,s.request_start,s.request_fifo);
    for(unsigned i=0;i<2;i++) printf("  bank%u START/MASK/STAT/FIFO=%x/%x/%x/%x\n",i,
        s.bank_start[i],s.bank_mask[i],s.bank_status[i],s.bank_fifo[i]);
    puts("Cached observations only; right supply unknown, no PA value writes/mute/reset/calibration or speaker-ready proof.");
    return 0;
}
static int inspect_container(const char *path,const char *profile) {
    FILE *f=fopen(path,"rb");if(!f){perror(path);return 1;}
    if(fseek(f,0,SEEK_END)){fclose(f);return 1;}
    long length=ftell(f);
    if(length<54 || length>X9_TFA_CONTAINER_MAX || fseek(f,0,SEEK_SET)){fclose(f);return 1;}
    void *data=malloc((size_t)length);if(!data){fclose(f);return 1;}
    size_t n=fread(data,1,(size_t)length,f);int read_error=ferror(f);fclose(f);
    struct x9_tfa_profile p;int e=n==(size_t)length && !read_error?
        x9_tfa_select_profile(data,(uint32_t)length,profile,&p):1;
    if(e)fprintf(stderr,"container rejected: errno=%d (requires exact dual-I2C-kind/bus0 profile, size, CRC)\n",e);
    else for(unsigned i=0;i<2;i++)printf("PA6-%02x %s profile=%s CRC=%08x register-prefix=%u opaque=%u\n",
        0x34+i,p.device_name[i],p.profile_name[i],p.crc,p.prefix_items[i],p.opaque_items[i]);
    memset(data,0,(size_t)length);free(data);
    if(!e)puts("Local parser only; no chip revision/loaded-profile proof, no hardware writes.");
    return e?1:0;
}
static void print_status(const X9AudioControl *s) {
    printf("X9 audio ABI=%u state=0x%x blockers=0x%x errno=%u rate=%u channels=%u\n"
           "PCM played=%llu queued=%llu underruns=%llu bad_producer=%llu generation=%u\n",
           s->version, s->state, s->blockers, s->error, s->rate, s->channels,
           (unsigned long long)s->out_frames, (unsigned long long)s->out_queued,
           (unsigned long long)s->underruns, (unsigned long long)s->bad_producer, s->generation);
    const char *names[] = { "power/clocks", "ADSP firmware/READY", "mailbox/IPI", "EMI/cache/shared DMA",
                           "AFE", "physical amplifier+calibration", "speaker routing", "physical backend" };
    for (unsigned i = 0; i < 8; i++) if (s->blockers & (1u << i)) printf("pending: %s\n", names[i]);
}
static int play(io_connect_t c, FILE *f) {
    mach_vm_address_t maps[3] = {0}; mach_vm_size_t sizes[3] = {0};
    const uint64_t min[] = { sizeof(X9AudioControl), sizeof(X9AudioProducer), X9_AUDIO_RING_BYTES };
    int rc = 1, started = 0;
    for (unsigned i = 0; i < 3; i++) {
        kern_return_t r = IOConnectMapMemory64(c, i, mach_task_self(), &maps[i], &sizes[i],
                                              kIOMapAnywhere | (i == 0 ? kIOMapReadOnly : 0));
        if (r || sizes[i] < min[i]) { fprintf(stderr, "map %u failed: 0x%x\n", i, r); goto done; }
    }
    kern_return_t r = call(c, X9AudioStart);
    if (r) { fprintf(stderr, "physical playback not started: 0x%x\n", r); goto done; }
    started = 1;
    X9AudioProducer *p = (void *)(uintptr_t)maps[1]; int16_t *ring = (void *)(uintptr_t)maps[2];
    uint64_t end = 0, content_end = 0, tail_left = X9_AUDIO_PREFILL_MIN;
    uint64_t last_played = 0, last_progress = mach_absolute_time();
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    int eof = 0;
    while (!interrupted) {
        X9AudioControl s;
        if (status(c, &s) || !(s.state & X9AudioRunning)) goto done;
        if (s.out_frames != last_played) { last_played = s.out_frames; last_progress = mach_absolute_time(); }
        if ((mach_absolute_time() - last_progress) * tb.numer / tb.denom > 5000000000ULL) {
            fprintf(stderr, "no hardware cursor progress for 5s; stop\n"); goto done;
        }
        /* Stop once REAL content is played, not after exhausting the DMA
         * queue: AFE must mute at its 480-frame low-water margin. Keep two
         * zero periods behind EOF so the last data is not cut off early. */
        if (eof && s.out_frames >= content_end) { print_status(&s); rc = 0; break; }
        if ((!eof || tail_left) && end >= s.out_queued && end - s.out_queued <= X9_AUDIO_RING - X9_AUDIO_PERIOD) {
            int16_t pcm[X9_AUDIO_PERIOD * X9_AUDIO_CHANNELS] = {0};
            if (!eof) {
                size_t n = fread(pcm, 1, sizeof(pcm), f);
                if (ferror(f) || n % 4) { fprintf(stderr, "read error or partial stereo frame\n"); goto done; }
                if (!n) { eof = 1; continue; }
                if (end > UINT64_MAX - X9_AUDIO_PERIOD) { fprintf(stderr, "PCM timeline overflow\n"); goto done; }
                content_end = end + n / 4;
                if (n < sizeof(pcm)) eof = 1;
            } else tail_left -= X9_AUDIO_PERIOD;
            if (end > UINT64_MAX - X9_AUDIO_PREFILL_MAX - X9_AUDIO_PERIOD) {
                fprintf(stderr, "physical timeline overflow\n"); goto done;
            }
            for (unsigned i = 0; i < X9_AUDIO_PERIOD; i++) {
                unsigned slot = (unsigned)((end + i) % X9_AUDIO_RING) * 2;
                ring[slot] = pcm[i * 2]; ring[slot + 1] = pcm[i * 2 + 1];
            }
            end += X9_AUDIO_PERIOD; __atomic_store_n(&p->written, end, __ATOMIC_RELEASE);
        } else usleep(2000);
    }
done:
    if (started) {
        kern_return_t stopped = call(c, X9AudioStop);
        if (stopped) { fprintf(stderr, "stop failed: 0x%x\n", stopped); rc = 1; }
    }
    for (unsigned i = 0; i < 3; i++) if (maps[i]) IOConnectUnmapMemory64(c, i, mach_task_self(), maps[i]);
    return rc;
}
static int clock_status(io_connect_t c) {
    _Static_assert(sizeof(X9AudioClockStatus) == 216, "clock status v1 size");
    X9AudioClockStatus s; size_t n = sizeof(s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioClockStatusSelector, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.version != X9_AUDIO_CLOCK_STATUS_VERSION) {
        fprintf(stderr,"clock status/ABI failed: 0x%x\n",r); return 1;
    }
    printf("cold register-clock flags=%x stage=%u errno=%u restore-errno=%u held=%u ticket=%llu\n"
           "  votes owned/borrowed/parked=%x/%x/%x dirty/uncertain=%x/%x last=%x:%x\n"
           "  AP before/after=%x/%x aggregate=%x/%x ack=%x/%x done=%x foreign=%x/%x\n"
           "  VLP before/after=%x/%x FENC=%x/%x TOP before/configured/after=%x/%x/%x\n",
           s.flags,s.stage,s.error,s.restore_error,(s.flags&X9ClockHeld)!=0,(unsigned long long)s.ticket,
           s.owned_vote,s.borrowed_vote,s.parked_vote,s.dirty_mask,s.uncertain_mask,s.last_register,s.last_value,
           s.ap_before,s.ap_after,s.all_before,s.all_after,s.ack_before,s.ack_after,s.done,
           s.foreign_before,s.foreign_after,s.vlp_before,s.vlp_after,s.fenc_before,s.fenc_after,
           s.top_before,s.top_configured,s.top_after);
    for (unsigned i=0;i<4;i++)printf("  OUT4[%u] before/configured/after=%x/%x/%x\n",
        i,s.out_before[i],s.out_configured[i],s.out_after[i]);
    printf("  APLL2 AO-only CON0/CON1/CON3/FENC/TUNER/PCW0/PCW1=%x/%x/%x/%x/%x/%x/%x\n"
           "  AUDIO SMC INIT/SIDEBANDS=%llx/%llx; FENC/nominal profile is NOT PLL lock or speaker Ready\n",
           s.apll[0],s.apll[1],s.apll[2],s.apll[3],s.apll[4],s.apll[5],s.apll[6],
           (unsigned long long)s.smc_result[0],(unsigned long long)s.smc_result[1]);
    return 0;
}
static int pll_status(io_connect_t c) {
    _Static_assert(sizeof(X9AudioPLLStatus) == 264, "PLL status v1 size");
    X9AudioPLLStatus s; size_t n = sizeof(s);
    kern_return_t r = IOConnectCallStructMethod(c, X9AudioPLLStatusSelector, NULL, 0, &s, &n);
    if (r || n != sizeof(s) || s.version != X9_AUDIO_PLL_STATUS_VERSION) {
        fprintf(stderr,"PLL status/ABI failed: 0x%x\n",r); return 1;
    }
    printf("APLL2 reference-relative cycle flags=%x stage=%u errno=%u restore-errno=%u held=%u ticket=%llu\n"
           "  dirty/uncertain=%x/%x mux-owned=%x last=%x:%x meter-HW=%x\n",
           s.flags,s.stage,s.error,s.restore_error,(s.flags&X9PLLHeld)!=0,(unsigned long long)s.ticket,
           s.dirty_mask,s.uncertain_mask,s.mux_owned,s.last_register,s.last_value,s.meter_hw);
    const char *regs[]={"CON0","CON1","CON3","FENC","TUNER","PCW0","PCW1"};
    for(unsigned i=0;i<7;i++)printf("  APLL2 %s before/measured/after=%x/%x/%x\n",regs[i],
        s.pll_before[i],s.pll_active[i],s.pll_after[i]);
    printf("  consumers VLP-CFG7/FENC0=%x/%x TOP-CFG9/FENC1=%x/%x MM-CFG3/FENC0=%x/%x\n",
        s.consumers[0],s.consumers[1],s.consumers[2],s.consumers[3],s.consumers[4],s.consumers[5]);
    for(unsigned i=0;i<4;i++)printf("  meter %s before/after=%x/%x\n",
        i==0?"VLP-selector":i==1?"CON0":i==2?"CON1":"CON2",s.meter_before[i],s.meter_after[i]);
    const unsigned ids[]={33,34,30,34,30,34,30,34};
    for(unsigned i=0;i<8;i++)printf("  sample[%u] VLP-id=%u count=%u nominal-reference-Hz=%u\n",
        i,ids[i],s.counts[i],s.hz[i]);
    puts("Cached only. Rates use unmeasured nominal 26MHz reference; 1% sanity/2-count spread is not ppm accuracy, PLL lock, I2S waveform or speaker Ready.");
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "amp-inspect")) return inspect_container(argv[2], argv[3]);
    int amp = argc == 2 && !strcmp(argv[1], "amp-status");
    int probing = argc == 2 && !strcmp(argv[1], "amp-probe");
    int booting = argc == 2 && !strcmp(argv[1], "dsp-boot");
    int stopping = argc == 2 && !strcmp(argv[1], "dsp-stop");
    int board = argc == 2 && !strcmp(argv[1], "board-status");
    int afe = argc == 2 && !strcmp(argv[1], "afe-status");
    int resources = argc == 2 && !strcmp(argv[1], "resource-status");
    int clocks = argc == 2 && !strcmp(argv[1], "clock-status");
    int clock_probe = argc == 2 && !strcmp(argv[1], "clock-probe");
    int pll = argc == 2 && !strcmp(argv[1], "pll-status");
    int pll_probe = argc == 2 && !strcmp(argv[1], "pll-probe");
    int stream = argc == 2 && !strcmp(argv[1], "stream-status");
    int stream_stop = argc == 2 && !strcmp(argv[1], "stream-stop");
    int playing = argc == 3 && !strcmp(argv[1], "play");
    int dsp = argc == 2 && !strcmp(argv[1], "dsp-status");
    if (!amp && !probing && !playing && !dsp && !board && !afe && !resources && !clocks && !clock_probe && !pll && !pll_probe && !stream && !stream_stop && !booting && !stopping && !(argc == 2 && !strcmp(argv[1], "status"))) {
        fprintf(stderr, "usage: audioctl amp-status | amp-probe | status | resource-status | clock-status | clock-probe | pll-status | pll-probe | stream-status | stream-stop | afe-status | dsp-status | board-status | dsp-boot | dsp-stop |\n  play <S16LE stereo 48000Hz raw PCM> | amp-inspect <local.cnt> <exact-profile>\n"); return 2;
    }
    FILE *f = playing ? fopen(argv[2], "rb") : NULL;
    if (playing && !f) { perror(argv[2]); return 1; }
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(X9_AUDIO_CLASS));
    if (!service) { fprintf(stderr, "X9Audio absent (requires x9_audio=1 or 2)\n"); if (f) fclose(f); return 1; }
    io_connect_t c = 0; kern_return_t r = IOServiceOpen(service, mach_task_self(), (probing || clock_probe || pll_probe || booting || stopping || stream_stop) ? 2 : playing ? 1 : 0, &c);
    IOObjectRelease(service);
    if (r) { fprintf(stderr, "open failed: 0x%x\n", r); if (f) fclose(f); return 1; }
    X9AudioControl s; int rc = status(c, &s);
    if (!rc) {
        print_status(&s);
        if (amp) rc = amp_status(c);
        if (probing) {
            kern_return_t op = call(c, X9AudioAmpProbeSelector);
            if (op) fprintf(stderr,"PA read-only probe failed: 0x%x; once per boot, no retry/reset loop\n",op);
            int ar = amp_status(c); rc = op || ar;
        }
        if (dsp) rc = dsp_status(c);
        if (board) rc = board_status(c);
        if (afe) rc = afe_status(c);
        if (resources) rc = resource_status(c);
        if (clocks) rc = clock_status(c);
        if (clock_probe) {
            kern_return_t op = call(c, X9AudioClockProbeSelector);
            if (op) fprintf(stderr,"cold register-clock cycle failed: 0x%x; no retry/reset loop\n",op);
            int cr = clock_status(c); rc = op || cr;
        }
        if (pll) rc = pll_status(c);
        if (pll_probe) {
            kern_return_t op = call(c, X9AudioPLLProbeSelector);
            if (op) fprintf(stderr,"APLL2/meter cycle failed: 0x%x; shared clock-probe attempt, no retry/reset loop\n",op);
            int pr = pll_status(c), cr = clock_status(c); rc = op || pr || cr;
        }
        if (stream) rc = stream_status(c);
        if (stream_stop) {
            kern_return_t op = call(c, X9AudioStop);
            if (op) fprintf(stderr,"physical stop still failed: 0x%x; resources retained\n",op);
            int sr = stream_status(c); rc = op || sr;
        }
        if (booting || stopping) {
            kern_return_t op=call(c,booting?X9AudioDSPBootSelector:X9AudioDSPStopSelector);
            if(op)fprintf(stderr,"board operation failed: 0x%x; no retry/reset loop\n",op);
            int br=board_status(c), dr=dsp_status(c);rc=op || br || dr;
        }
        if (playing) {
            signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
            rc = play(c, f);
        }
    }
    IOServiceClose(c); if (f) fclose(f); return rc ? 1 : 0;
}
