/* Explicit diagnostics: power never records; only 'record' starts PCM. */
#include "../kernel/src/bsd/dev/x9_microphone.h"
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
_Static_assert(sizeof(struct x9_mic_board_status)==96,"board ABI size");
_Static_assert(sizeof(struct x9_mic_afe_status)==192,"AFE ABI size");
_Static_assert(sizeof(struct x9_mic_analog_status)==128,"analog ABI size");
_Static_assert(sizeof(struct x9_mic_digital_status)==192,"digital ABI size");
_Static_assert(sizeof(struct x9_mic_dma_status)==192,"DMA ABI size");
_Static_assert(sizeof(struct x9_mic_signal_status)==192,"signal ABI size");
static int hardware_status(int fd)
{
    struct x9_mic_hardware_status h={0};
    if(ioctl(fd,X9_MIC_GET_HARDWARE,&h)==0) {
        printf("hardware v%u stage=%u flags=%#x errno=%u restore_errno=%u swcid=%#x ready=%#x phases=%u chosen=%d/%d cycles=%d/%d monitor=%#x reg=%#x calibration_ns=%llu vaud18=%#x\n",
            h.version,h.stage,h.flags,h.last_errno,h.restore_errno,h.swcid,h.internal_ready,
            h.scanned_phases,h.chosen_phase[0],h.chosen_phase[1],h.cycle[0],h.cycle[1],
            h.monitor,h.last_register,h.calibration_ns,h.vaud18_monitor);
    } else if(errno!=ENOTTY){perror("hardware status");return 1;}
    struct x9_mic_board_status b={0};
    if(ioctl(fd,X9_MIC_GET_BOARD,&b)==0) {
        printf("board v%u stage=%u flags=%#x errno=%u restore_errno=%u gpio9=%#x/%#x sleep=%#x/%#x stat=%#x/%#x gate=%#x/%#x bus=%#x start=%#x fifo=%#x reg=%#x xo=%#x/%#x nominal_i2c_hz=%u\n",
            b.version,b.stage,b.flags,b.last_errno,b.restore_errno,b.gpio9_before,b.gpio9_after,
            b.sleep_before,b.sleep_after,b.sleep_stat,b.sleep_stat2,b.gate_before,b.gate_after,
            b.bus_status,b.bus_start,b.bus_fifo,b.last_register,b.xo_before,b.xo_after,b.timing_hz);
    } else if(errno!=ENOTTY){perror("board status");return 1;}
    struct x9_mic_afe_status a={0};
    if(ioctl(fd,X9_MIC_GET_AFE,&a)==0) {
        printf("afe v%u stage=%u flags=%#x errno=%u restore_errno=%u AP=%#x/%#x all=%#x/%#x ACK=%#x/%#x DONE=%#x VLP=%#x/%#x FENC=%#x/%#x top0=%#x/%#x top4=%#x/%#x engine=%#x/%#x mon=%#x spm=%#x/%#x cbip=%#x/%#x reg=%#x\n",
            a.version,a.stage,a.flags,a.last_errno,a.restore_errno,a.ap_vote_before,a.ap_vote_after,
            a.all_vote_before,a.all_vote_after,a.ack_before,a.ack_after,a.done,a.vlp_before,a.vlp_after,
            a.fenc_before,a.fenc_after,a.top0_before,a.top0_after,a.top4_before,a.top4_after,
            a.engine_before,a.engine_after,a.engine_monitor,a.spm_before,a.spm_after,
            a.cbip_before,a.cbip_after,a.last_register);
        if(a.flags&X9_MIC_AFE_ATTEMPTED) {
            printf("afe foreign AUDIO XPU votes=%#x/%#x\n",a.foreign_vote_before,a.foreign_vote_after);
            printf("afe smc INIT=%lld SIDEBANDS=%lld SRAM_REQ=%lld REL=%lld (INT64_MIN=not issued; INIT/SIDEBANDS have no inverse)\n",
                (long long)a.smc_result[0],(long long)a.smc_result[1],
                (long long)a.smc_result[2],(long long)a.smc_result[3]);
            const unsigned pins[]={98,100,101,102,103};
            for(unsigned i=0;i<5;i++)printf("afe pin%u=%#x/%#x\n",pins[i],a.pins_before[i],a.pins_after[i]);
        }
    } else if(errno!=ENOTTY){perror("AFE status");return 1;}
    struct x9_mic_analog_status q={0};
    if(ioctl(fd,X9_MIC_GET_ANALOG,&q)==0) {
        printf("analog v%u stage=%u flags=%#x errno=%u restore_errno=%u HWCID=%#x/%#x AIN%u/ADC%u type=%u bias=%umV gain_x2=%u rc_sw=%u rc_monitor=%#x journal=%u/%u reg=%#x elapsed_ns=%llu\n",
            q.version,q.stage,q.flags,q.last_errno,q.restore_errno,q.hwcid_h,q.hwcid_l,
            q.ain,q.adc,q.mic_type,q.bias_mv,q.gain_x2,q.rc_sw,q.rc_monitor,
            q.journal_restored,q.journal_count,q.last_register,q.elapsed_ns);
        printf("analog active PGA/ADC/bias0/bias2/DCC=%#x/%#x/%#x/%#x/%#x CLKSQ/CKPDN=%#x/%#x; after=%#x/%#x/%#x/%#x/%#x (codec pin names, not physical microphone or PCM proof)\n",
            q.pga_active,q.adc_active,q.bias0_active,q.bias2_active,q.dcc_active,
            q.clksq_active,q.clocks_active,q.pga_after,q.adc_after,q.bias0_after,q.bias2_after,q.dcc_after);
    } else if(errno!=ENOTTY){perror("analog status");return 1;}
    struct x9_mic_digital_status g={0};
    if(ioctl(fd,X9_MIC_GET_DIGITAL,&g)==0) {
        printf("digital v%u stage=%u flags=%#x errno=%u restore_errno=%u nominal_rate=%u delay=%u/%u journal codec=%u/%u afe=%u/%u reg=%#x elapsed_ns=%llu\n",
            g.version,g.stage,g.flags,g.last_errno,g.restore_errno,g.nominal_rate,g.delay_data,g.delay_cycle,
            g.codec_journal_restored,g.codec_journal_count,g.afe_journal_restored,g.afe_journal_count,
            g.last_register,g.elapsed_ns);
        printf("digital codec SRC/TOP/TX0/TX1/PAD/MODE4=%#x/%#x/%#x/%#x/%#x/%#x MUX=%#x/%#x; after=%#x/%#x/%#x/%#x/%#x/%#x\n",
            g.codec_src,g.codec_top,g.codec_tx[0],g.codec_tx[1],g.codec_pad,g.codec_mode4,g.codec_mux[0],g.codec_mux[1],
            g.codec_after_src,g.codec_after_top,g.codec_after_tx[0],g.codec_after_tx[1],g.codec_after_pad,g.codec_after_mode4);
        printf("digital afe UL/RX0/RX1/PAD/CLK/GATE=%#x/%#x/%#x/%#x/%#x/%#x MON=%#x/%#x; after=%#x/%#x/%#x/%#x/%#x/%#x (configuration readback only, not PCM/sound proof)\n",
            g.afe_ul,g.afe_rx[0],g.afe_rx[1],g.afe_pad,g.afe_clock,g.afe_gate,g.afe_monitor[0],g.afe_monitor[1],
            g.afe_after_ul,g.afe_after_rx[0],g.afe_after_rx[1],g.afe_after_pad,g.afe_after_clock,g.afe_after_gate);
    } else if(errno!=ENOTTY){perror("digital status");return 1;}
    struct x9_mic_dma_status d={0};
    if(ioctl(fd,X9_MIC_GET_DMA,&d)==0) {
        printf("dma v%u stage=%u flags=%#x errno=%u restore_errno=%u eligible=%u blockers=%#x active_memifs=%#x CPU=%#x channel=%u ring_bytes=%u CON=%#x/%#x MON=%#x/%#x CUR=%#llx/%#llx journal=%u/%u pending=%#x observed=%llu dropped=%llu reg=%#x restore_reg=%#x\n",
            d.version,d.stage,d.flags,d.last_errno,d.restore_errno,d.eligible,d.blockers,d.active_memifs,
            d.memif_con0,d.adc_channel,d.ring_bytes,d.con0_before,d.con0_after,d.monitor_before,d.monitor_after,
            d.cur_before,d.cur_after,d.journal_restored,d.journal_count,d.pending_flags,
            d.observed_frames,d.dropped_frames,d.last_register,d.restore_register);
        if(d.flags&X9_MIC_DMA_REGISTERS_VALID)for(unsigned c=0;c<2;c++)
            printf("dma CONN%u words0,1,2,4,5,6,7=%#x/%#x/%#x/%#x/%#x/%#x/%#x\n",
                20+c,d.route[c][0],d.route[c][1],d.route[c][2],d.route[c][3],d.route[c][4],d.route[c][5],d.route[c][6]);
        if(d.blockers)printf("dma blockers: MAP=1 OWNER=2 COPY=4 QUIESCE=8 SOURCE_ID=16; eligible/idle is not allocated DMA, PCM or acoustic proof\n");
    } else if(errno!=ENOTTY){perror("DMA status");return 1;}
    struct x9_mic_signal_status v={0};
    if(ioctl(fd,X9_MIC_GET_SIGNAL,&v)==0) {
        printf("shadow v%u flags=%#x errno=%u restore_errno=%u reads=%u/%u interval_us=%u elapsed_ns=%llu UL=%#x sync=%#x/%#x -> %#x/%#x rejected_read=%u reg=%#x (non-atomic MMIO reads, no ready/frame/rate/PCM/acoustic proof)\n",
            v.version,v.flags,v.last_errno,v.restore_errno,v.reads,v.target_reads,v.interval_us,
            v.elapsed_ns,v.ul_control,v.sync_first[0],v.sync_first[1],v.sync_last[0],v.sync_last[1],
            v.rejected_read,v.last_register);
        if(v.reads)for(unsigned i=0;i<2;i++) {
            const struct x9_mic_signal_channel *c=&v.channel[i];
            printf("shadow CH%u raw=%#x/%#x signed24 first/last=%d/%d min/max=%d/%d changes=%u zeros=%u sum=%lld sum_squares=%llu (zero/constant is not a failed-mic verdict)\n",
                i+1,c->raw_first,c->raw_last,c->first,c->last,c->minimum,c->maximum,
                c->changes,c->zero_reads,(long long)c->sum,(unsigned long long)c->sum_squares);
        }
    } else if(errno!=ENOTTY){perror("shadow status");return 1;}
    return 0;
}
static uint64_t ns(void)
{
    mach_timebase_info_data_t t;mach_timebase_info(&t);
    uint64_t now=mach_absolute_time();return now/t.denom*t.numer+(now%t.denom)*t.numer/t.denom;
}
int main(int argc,char **argv)
{
    bool record=argc==4 && !strcmp(argv[1],"record");
    bool power=argc==2 && !strcmp(argv[1],"power");
    bool calibrate=argc==2 && !strcmp(argv[1],"calibrate");
    bool analog=argc==2 && !strcmp(argv[1],"analog-l");
    bool digital=argc==2 && !strcmp(argv[1],"digital-l");
    if(!record && !power && !calibrate && !analog && !digital && !(argc==2 && !strcmp(argv[1],"status"))) {
        fprintf(stderr,"usage: micctl status | micctl power | micctl calibrate | micctl analog-l | micctl digital-l | micctl record <seconds:1..30> <new.pcm>\n");return 2;
    }
    int fd=open(record?X9_MIC_DEVICE:X9_MIC_STATUS_DEVICE,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){perror("open microphone");return 1;}
    struct x9_mic_status s={0};
    if(ioctl(fd,X9_MIC_GET_STATUS,&s)<0){perror("status");close(fd);return 1;}
    printf("v%u mode=%u flags=%#x sources=%#x %uHz/%uch/%ubit errno=%u generation=%llu captured=%llu read=%llu dropped=%llu queue=%u\n",
        s.version,s.mode,s.flags,s.supported_sources,s.rate,s.channels,s.sample_bits,s.last_errno,
        s.generation,s.captured_frames,s.read_frames,s.dropped_frames,s.queued_frames);
    if(!record) {
        int result=0;
        if(power || calibrate || analog || digital) {
            if(geteuid()!=0 || s.version!=1 || s.mode!=2) {
                fprintf(stderr,"cold diagnostics require root and mode2 (x9_mic or ADT); no hardware request issued\n");
                close(fd);return 1;
            }
            printf("explicit cold %s, shared one attempt per boot; no PCM/DMA or recording\n",
                digital?"power + phasecal + ADC_L + codec SRC/TX + AFE RX diagnostic":
                analog?"power + phasecal + AIN0/PGA_L/ADC_L DCC diagnostic":
                calibrate?"power + AFE/MTKAIF phase calibration":"power/ID/VAUD18 probe (no AFE)");
            if(ioctl(fd,digital?X9_MIC_DIGITAL_L:analog?X9_MIC_ANALOG_L:calibrate?X9_MIC_CALIBRATE:X9_MIC_POWER_PROBE)<0){perror("cold microphone diagnostic");result=1;}
            /* Even on failure retrieve the decisive stage and retained journal. */
        }
        result|=hardware_status(fd);
        close(fd);return result;
    }
    char *end;errno=0;long seconds=strtol(argv[2],&end,10);
    if(errno || end==argv[2] || *end || seconds<1 || seconds>30){close(fd);return 2;}
    if(s.version!=1 || s.mode!=2 || !(s.flags&X9_MIC_READY) || (s.flags&X9_MIC_FAULT)) {
        fprintf(stderr,"physical microphone backend is not ready; no file created\n");close(fd);return 1;
    }
    /* Refuse to overwrite an existing recording or symlink. */
    int out=open(argv[3],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(out<0){perror("new PCM file");close(fd);return 1;}
    struct x9_mic_config c={.version=1,.rate=X9_MIC_RATE,.channels=1,.sample_bits=16,.source=X9_MIC_SOURCE_INTERNAL};
    int result=0;uint64_t total=0,generation=0;
    uint64_t target=(uint64_t)seconds*X9_MIC_RATE*X9_MIC_FRAME_BYTES;
    bool started=false;
    if(ioctl(fd,X9_MIC_START,&c)<0){perror("start");result=1;}
    else {
        started=true;
        if(ioctl(fd,X9_MIC_GET_STATUS,&s)<0){perror("started status");result=1;}
        else if(s.version!=1 ||
            (s.flags&(X9_MIC_READY|X9_MIC_RUNNING|X9_MIC_FAULT))!=(X9_MIC_READY|X9_MIC_RUNNING) ||
            s.source!=X9_MIC_SOURCE_INTERNAL || s.read_frames) {
            fprintf(stderr,"started capture state is not valid\n");result=1;
        } else generation=s.generation;
    }
    /* Capture an exact frame count, not whatever a wall-clock loop received.
     * Cold initialization is outside this deadline. Allow <=1s for first
     * real bytes, then <=250ms without progress; no silence/short success. */
    uint64_t now=ns(),deadline=now+((uint64_t)seconds+1)*1000000000;
    uint64_t first_deadline=now+1000000000,last_progress=now;
    while(!result && total<target) {
        now=ns();
        if(now>=deadline || (!total && now>=first_deadline) ||
           (total && now-last_progress>=250000000)) {
            fprintf(stderr,"real PCM deadline/stall: bytes=%llu expected=%llu\n",total,target);
            result=1;break;
        }
        int16_t pcm[512];size_t want=(size_t)(target-total);
        if(want>sizeof(pcm))want=sizeof(pcm);
        ssize_t n=read(fd,pcm,want);
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {usleep(5000);continue;}
        if(n<=0 || (n&1) || (size_t)n>want) {
            if(n<0)perror("read PCM");else fprintf(stderr,"invalid/ended PCM read: %zd\n",n);
            result=1;break;
        }
        last_progress=ns();
        for(ssize_t sent=0;sent<n;) {
            ssize_t wrote=write(out,(char *)pcm+sent,(size_t)(n-sent));
            if(wrote<0 && errno==EINTR)continue;
            if(wrote<=0){perror("write PCM");result=1;break;}
            sent+=wrote;total+=(uint64_t)wrote;
        }
        memset(pcm,0,sizeof(pcm));
    }
    if(started) {
        if(ioctl(fd,X9_MIC_GET_STATUS,&s)<0){perror("final running status");result=1;}
        else if(s.version!=1 || s.generation!=generation ||
            (s.flags&(X9_MIC_READY|X9_MIC_RUNNING|X9_MIC_FAULT))!=(X9_MIC_READY|X9_MIC_RUNNING) ||
            s.read_frames!=total/2 || s.captured_frames<s.read_frames || s.dropped_frames || total!=target) {
            fprintf(stderr,"capture fault, discontinuity or incomplete frame accounting\n");result=1;
        }
    }
    bool stopped=ioctl(fd,X9_MIC_STOP)==0;
    if(!stopped){perror("stop");result=1;}
    if(started && stopped) {
        struct x9_mic_status after={0};
        if(ioctl(fd,X9_MIC_GET_STATUS,&after)<0){perror("stopped status");result=1;}
        else if(after.version!=1 || after.generation!=generation+1 ||
            (after.flags&(X9_MIC_RUNNING|X9_MIC_FAULT)) || after.last_errno || after.queued_frames ||
            after.read_frames!=s.read_frames || after.captured_frames!=s.captured_frames ||
            after.dropped_frames!=s.dropped_frames) {
            fprintf(stderr,"kernel STOP did not confirm a clean final state\n");result=1;
        }
    }
    /* Cache snapshots after explicit STOP: never another hardware request.
     * Preserve all decisive power/calibration/DMA evidence even on failure. */
    result|=hardware_status(fd);
    if(close(fd)<0){perror("close microphone");result=1;}
    if(close(out)<0){perror("close PCM");result=1;}
    if(!total){unlink(argv[3]);result=1;}
    printf("real PCM bytes=%llu expected=%llu captured=%llu read=%llu dropped=%llu; %s (raw S16_LE/mono/48000, never zero-padded)\n",
        total,target,s.captured_frames,s.read_frames,s.dropped_frames,
        result?"incomplete":"captured and stopped; acoustic verification still required");
    return result;
}
