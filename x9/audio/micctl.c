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
    if(!record && !power && !calibrate && !(argc==2 && !strcmp(argv[1],"status"))) {
        fprintf(stderr,"usage: micctl status | micctl power | micctl calibrate | micctl record <seconds:1..30> <new.pcm>\n");return 2;
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
        if(power || calibrate) {
            if(geteuid()!=0 || s.version!=1 || s.mode!=2) {
                fprintf(stderr,"power/calibrate requires root and x9_mic=2; no hardware request issued\n");
                close(fd);return 1;
            }
            printf("explicit cold %s, shared one attempt per boot; no ADC/DMA or recording\n",
                calibrate?"power + AFE/MTKAIF phase calibration":"power/ID/VAUD18 probe (no AFE)");
            if(ioctl(fd,calibrate?X9_MIC_CALIBRATE:X9_MIC_POWER_PROBE)<0){perror("cold microphone diagnostic");result=1;}
            /* Even on failure retrieve the decisive stage and retained journal. */
        }
        result|=hardware_status(fd);
        close(fd);return result;
    }
    char *end;long seconds=strtol(argv[2],&end,10);
    if(*end || seconds<1 || seconds>30){close(fd);return 2;}
    if(s.version!=1 || s.mode!=2 || !(s.flags&X9_MIC_READY) || (s.flags&X9_MIC_FAULT)) {
        fprintf(stderr,"physical microphone backend is not ready; no file created\n");close(fd);return 1;
    }
    /* Refuse to overwrite an existing recording or symlink. */
    int out=open(argv[3],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(out<0){perror("new PCM file");close(fd);return 1;}
    struct x9_mic_config c={.version=1,.rate=X9_MIC_RATE,.channels=1,.sample_bits=16,.source=X9_MIC_SOURCE_INTERNAL};
    int result=0;uint64_t total=0;
    if(ioctl(fd,X9_MIC_START,&c)<0){perror("start");result=1;}
    uint64_t deadline=ns()+(uint64_t)seconds*1000000000,first_deadline=ns()+1000000000;
    while(!result && ns()<deadline) {
        int16_t pcm[512];ssize_t n=read(fd,pcm,sizeof(pcm));
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            if(!total && ns()>=first_deadline){fprintf(stderr,"no real PCM within 1 second\n");result=1;break;}
            usleep(5000);continue;
        }
        if(n<=0 || (n&1)){perror("read PCM");result=1;break;}
        for(ssize_t sent=0;sent<n;) {
            ssize_t wrote=write(out,(char *)pcm+sent,(size_t)(n-sent));
            if(wrote<0 && errno==EINTR)continue;
            if(wrote<=0){perror("write PCM");result=1;break;}sent+=wrote;
        }
        total+=(uint64_t)n;
    }
    if(ioctl(fd,X9_MIC_GET_STATUS,&s)<0){perror("final status");result=1;}
    if((s.flags&X9_MIC_FAULT) || s.read_frames!=total/2) {
        fprintf(stderr,"capture fault or frame accounting mismatch\n");result=1;
    }
    if(ioctl(fd,X9_MIC_STOP)<0){perror("stop");result=1;}
    close(fd);if(close(out)<0){perror("close PCM");result=1;}
    if(!total){unlink(argv[3]);result=1;}
    printf("real PCM bytes=%llu captured=%llu read=%llu dropped=%llu; %s (raw S16_LE/mono/48000, never zero-padded)\n",
        total,s.captured_frames,s.read_frames,s.dropped_frames,result?"incomplete":"captured; acoustic verification still required");
    return result;
}
