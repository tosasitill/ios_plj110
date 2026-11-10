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
    if (r || n != sizeof(s) || s.version != 1) { fprintf(stderr,"board status/ABI failed: 0x%x\n",r); return 1; }
    printf("ADSP board flags=0x%x errno=%u restore=%u owned=0x%x AP=0x%x all=0x%x done=0x%x busy=0x%x\n"
           "  pending=0x%x sleep=0x%x DPSW=%x/%x vlogic=%x sram=%x\n"
           "  SMC op=%u a0=%llx a1=%llx a2=%llx a3=%llx calls=%llu resets=%llu timeouts=%llu\n",
           s.flags,s.error,s.restore_error,s.owned_vote,s.ap_vote,s.all_vote,s.pd_done,s.pd_busy,
           s.pending,s.sleep,s.dpsw_req,s.dpsw_ack,s.vlogic_ack,s.sram_ack,s.smc_op,
           (unsigned long long)s.smc_result[0],(unsigned long long)s.smc_result[1],
           (unsigned long long)s.smc_result[2],(unsigned long long)s.smc_result[3],
           (unsigned long long)s.smc_calls,(unsigned long long)s.resets,(unsigned long long)s.timeouts);
    puts("Cached snapshot only. Owned AO may remain pinned; no PA unmute or speaker-ready implied.");
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
    if(e)fprintf(stderr,"container rejected: errno=%d (requires exact dual-I2C6 profile, size, CRC)\n",e);
    else for(unsigned i=0;i<2;i++)printf("PA6-%02x %s profile=%s CRC=%08x register-prefix=%u opaque=%u\n",
        0x34+i,p.device_name[i],p.profile_name[i],p.crc,p.prefix_items[i],p.opaque_items[i]);
    memset(data,0,(size_t)length);free(data);
    if(!e)puts("Local parser only; no chip revision/loaded-profile proof, no hardware writes.");
    return e?1:0;
}
static void print_status(const X9AudioControl *s) {
    printf("X9 audio ABI=%u state=0x%x blockers=0x%x errno=%u rate=%u channels=%u\n"
           "hardware played=%llu queued=%llu underruns=%llu bad_producer=%llu generation=%u\n",
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
    uint64_t end = 0, last_played = 0, last_progress = mach_absolute_time();
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    int eof = 0;
    while (!interrupted) {
        X9AudioControl s;
        if (status(c, &s) || !(s.state & X9AudioRunning)) goto done;
        if (s.out_frames != last_played) { last_played = s.out_frames; last_progress = mach_absolute_time(); }
        if ((mach_absolute_time() - last_progress) * tb.numer / tb.denom > 5000000000ULL) {
            fprintf(stderr, "no hardware cursor progress for 5s; stop\n"); goto done;
        }
        if (eof && s.out_frames >= end) { print_status(&s); rc = 0; break; }
        if (!eof && end >= s.out_queued && end - s.out_queued <= X9_AUDIO_RING - X9_AUDIO_PERIOD) {
            int16_t pcm[X9_AUDIO_PERIOD * X9_AUDIO_CHANNELS] = {0};
            size_t n = fread(pcm, 1, sizeof(pcm), f);
            if (ferror(f) || n % 4) { fprintf(stderr, "read error or partial stereo frame\n"); goto done; }
            if (!n) { eof = 1; continue; }
            for (unsigned i = 0; i < X9_AUDIO_PERIOD; i++) {
                unsigned slot = (unsigned)((end + i) % X9_AUDIO_RING) * 2;
                ring[slot] = pcm[i * 2]; ring[slot + 1] = pcm[i * 2 + 1];
            }
            end += X9_AUDIO_PERIOD; __atomic_store_n(&p->written, end, __ATOMIC_RELEASE);
            if (n < sizeof(pcm)) eof = 1;
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
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "amp-inspect")) return inspect_container(argv[2], argv[3]);
    int booting = argc == 2 && !strcmp(argv[1], "dsp-boot");
    int stopping = argc == 2 && !strcmp(argv[1], "dsp-stop");
    int board = argc == 2 && !strcmp(argv[1], "board-status");
    int playing = argc == 3 && !strcmp(argv[1], "play");
    int dsp = argc == 2 && !strcmp(argv[1], "dsp-status");
    if (!playing && !dsp && !board && !booting && !stopping && !(argc == 2 && !strcmp(argv[1], "status"))) {
        fprintf(stderr, "usage: audioctl status | dsp-status | board-status | dsp-boot | dsp-stop |\n  play <S16LE stereo 48000Hz raw PCM> | amp-inspect <local.cnt> <exact-profile>\n"); return 2;
    }
    FILE *f = playing ? fopen(argv[2], "rb") : NULL;
    if (playing && !f) { perror(argv[2]); return 1; }
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(X9_AUDIO_CLASS));
    if (!service) { fprintf(stderr, "X9Audio absent (requires x9_audio=1)\n"); if (f) fclose(f); return 1; }
    io_connect_t c = 0; kern_return_t r = IOServiceOpen(service, mach_task_self(), (booting || stopping) ? 2 : playing ? 1 : 0, &c);
    IOObjectRelease(service);
    if (r) { fprintf(stderr, "open failed: 0x%x\n", r); if (f) fclose(f); return 1; }
    X9AudioControl s; int rc = status(c, &s);
    if (!rc) {
        print_status(&s);
        if (dsp) rc = dsp_status(c);
        if (board) rc = board_status(c);
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
