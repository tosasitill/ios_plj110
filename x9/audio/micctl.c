/* Explicit local capture diagnostic: no recording unless 'record' is asked. */
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
static uint64_t ns(void)
{
    mach_timebase_info_data_t t;mach_timebase_info(&t);
    uint64_t now=mach_absolute_time();return now/t.denom*t.numer+(now%t.denom)*t.numer/t.denom;
}
int main(int argc,char **argv)
{
    bool record=argc==4 && !strcmp(argv[1],"record");
    if(!record && !(argc==2 && !strcmp(argv[1],"status"))) {
        fprintf(stderr,"usage: micctl status | micctl record <seconds:1..30> <new.pcm>\n");return 2;
    }
    int fd=open(record?X9_MIC_DEVICE:X9_MIC_STATUS_DEVICE,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){perror("open microphone");return 1;}
    struct x9_mic_status s={0};
    if(ioctl(fd,X9_MIC_GET_STATUS,&s)<0){perror("status");close(fd);return 1;}
    printf("v%u mode=%u flags=%#x sources=%#x %uHz/%uch/%ubit errno=%u generation=%llu captured=%llu read=%llu dropped=%llu queue=%u\n",
        s.version,s.mode,s.flags,s.supported_sources,s.rate,s.channels,s.sample_bits,s.last_errno,
        s.generation,s.captured_frames,s.read_frames,s.dropped_frames,s.queued_frames);
    if(!record){close(fd);return 0;}
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
