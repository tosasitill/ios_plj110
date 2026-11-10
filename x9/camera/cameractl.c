/* Read-only status by default; explicit back/front asks for real frames. */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "hakios.camera.h"
#include "../kernel/src/bsd/dev/x9_camera.h"
int main(int argc,char **argv)
{
    bool probe=argc==3 && !strcmp(argv[1],"probe");
    bool pipeline=argc==3 && !strcmp(argv[1],"pipeline");
    const char *which=(probe||pipeline)?argv[2]:argc==2?argv[1]:"status";
    unsigned sensor=!strcmp(which,"back")?1:!strcmp(which,"front")?2:0;
    if(argc>3 || (argc==3 && !probe && !pipeline) || ((probe||pipeline) && !sensor) ||
       (argc>=2 && !sensor && strcmp(which,"status"))) {
        fprintf(stderr,"usage: cameractl [status|back|front|probe back|probe front|pipeline back|pipeline front]\n");return 2;
    }
    int fd=open(X9_CAMERA_DEVICE,(sensor && !pipeline?O_RDWR:O_RDONLY)|O_CLOEXEC);
    if(fd<0){perror(X9_CAMERA_DEVICE);return 1;}
    struct x9_camera_status s={0};
    if(ioctl(fd,X9_CAMERA_GET_STATUS,&s)){perror("status");close(fd);return 1;}
    printf("version=%u mode=%u flags=%x supported=%x sensor=%u errno=%u frames=%llu rejected=%llu chips=%x/%x probe_mask=%x probe_flags=%x\n",
           s.version,s.mode,s.flags,s.supported_mask,s.sensor,s.last_errno,
           (unsigned long long)s.frames,(unsigned long long)s.rejected,s.chip_id[0],s.chip_id[1],s.probe_mask,s.probe_flags);
    if(pipeline) {
        struct x9_camera_pipeline r={.version=1,.sensor=sensor};
        if(ioctl(fd,X9_CAMERA_GET_PIPELINE,&r)){perror("pipeline ioctl");close(fd);return 1;}
        printf("source-only pipeline sensor=%u flags=%x blockers=%x CSI%u %s/%u async=%u vc=%u dt=%02x %ux%u stride=%u bytes=%u\n",
            r.sensor,r.flags,r.blockers,r.csi_port,r.cphy?"C-PHY":"D-PHY",r.lanes,r.async,
            r.vc,r.dt,r.width,r.height,r.stride,r.bytes);
        printf("candidate outmux=%u CAMSV=%u CSI=%llx CAMSV=%llx MM-SMMUv3=%llx sid=%x dma_bits=%u dma_limit=%llx\n",
            r.outmux_candidate,r.camsv_candidate,(unsigned long long)r.csi_base,
            (unsigned long long)r.camsv_base,(unsigned long long)r.smmu_base,r.smmu_sid,r.dma_bits,
            (unsigned long long)r.dma_limit);
        printf("planned VCDT=%08x size=%08x grab=%08x/%08x RAW10=%08x WDMA-basic=%08x (no MMIO, no reservation, NOT capture ready)\n",
            r.vcdt,r.expected_size,r.grab_pixels,r.grab_lines,r.format,r.dma_basic);
        close(fd);return 0;
    }
    if(probe || !sensor) {
        struct x9_camera_probe r={.version=1,.sensor=probe?sensor:0};
        if(ioctl(fd,probe?X9_CAMERA_PROBE_SENSOR:X9_CAMERA_GET_PROBE,&r)) {
            /* Older phase1 kernels have no probe ABI; status remains useful. */
            if(probe || errno!=ENOTTY){perror("probe ioctl");close(fd);return 1;}
        } else {
            printf("sensor-probe sensor=%u stage=%u flags=%x blockers=%x chip=%04x errno=%u restore=%u wl=%02x/%02x mclk=%08x/%08x rails=%06x/%06x i2c=%x/%x (NOT capture ready)\n",
                r.sensor,r.stage,r.flags,r.blockers,r.chip_id,r.last_errno,r.restore_errno,
                r.wl_id,r.wl_enable,r.mclk_cfg,r.mclk_status,r.dovdd,r.afvdd,r.i2c_status[0],r.i2c_status[1]);
            printf("clock ap_vote_before=%08x done=%08x univpll_con1=%08x fence=%08x (configured nominal, NOT measured Hz)\n",
                r.hwv_vote,r.hwv_done,r.pll_con1,r.pll_fence);
            if(probe){close(fd);return r.last_errno?1:0;}
        }
    }
    if(!sensor){close(fd);return 0;}
    struct hakios_camera_command c={.command=HAKIOS_CAMERA_SENSOR,.a=sensor,.b=~0u};
    if(write(fd,&c,sizeof(c))!=sizeof(c)){perror("start");close(fd);return 1;}
    struct hakios_camera_frame h;
    if(read(fd,&h,sizeof(h))!=sizeof(h)){perror("frame header");close(fd);return 1;}
    if(memcmp(h.magic,HAKIOS_CAMERA_MAGIC,8) || h.header_bytes!=sizeof(h) ||
       !h.height || !h.stride || (uint64_t)h.stride*h.height>X9_CAMERA_MAX_BYTES){close(fd);return 1;}
    size_t bytes=sizeof(h)+(size_t)h.stride*h.height;void *buffer=malloc(bytes);
    if(!buffer){close(fd);return 1;}
    ssize_t n=read(fd,buffer,bytes);
    if(n==(ssize_t)bytes){h=*(struct hakios_camera_frame*)buffer;
        printf("physical RAW10 sensor=%u %ux%u stride=%u sequence=%llu time=%llu\n",h.sensor,h.width,h.height,h.stride,
               (unsigned long long)h.sequence,(unsigned long long)h.time);}
    else perror("frame");
    free(buffer);close(fd);return n==(ssize_t)bytes?0:1;
}
