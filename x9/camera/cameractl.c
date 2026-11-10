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
#include "camera_stop.h"
_Static_assert(sizeof(struct x9_camera_mm_status)==576,"camera MM ABI");
_Static_assert(sizeof(struct x9_camera_receiver)==1192,"camera receiver ABI");
_Static_assert(sizeof(struct x9_camera_config)==720,"camera config ABI");
_Static_assert(sizeof(struct x9_camera_status)==88,"camera status ABI");
static void print_stop(const struct x9_camera_stop *r, const char *kind)
{
    printf("%s stop-receipt version=%u sensor=%u flags=%x errno=%u generation=%llu/%llu attempted=%x/%x readers=%u/%u state=%x/%x (IDLE is NOT DMA evidence)\n",
        kind,r->version,r->sensor,r->flags,r->last_errno,
        (unsigned long long)r->generation_before,(unsigned long long)r->generation_after,
        r->owned_before,r->owned_after,r->readers_before,r->readers_after,r->state_before,r->state_after);
}
static int finish(int fd, int result, bool physical)
{
    if(physical) {
        struct x9_camera_stop receipt;
        int e=x9_camera_stop_once(fd,&receipt);
        print_stop(&receipt,"explicit");
        if(e){fprintf(stderr,"capture retirement: %s (%d)\n",strerror(e),e);result=1;}
    }
    if(close(fd)!=0){perror("camera close (not retried)");result=1;}
    return result;
}
static void print_config(const struct x9_camera_config *r)
{
    printf("sensor-standby sensor=%u flags=%x stage=%u errno=%u restore=%u blockers=%x pairs=%u/%u writes=%u/%u index=%u reg=%04x expected=%02x observed=%08x read=%u (temporary, uncalibrated, NOT capture ready)\n",
        r->sensor,r->flags,r->stage,r->last_errno,r->restore_errno,r->blockers,
        r->init_pairs,r->preview_pairs,r->written,r->attempted,r->table_index,
        r->reg,r->expected,r->observed,r->read_count);
    if(!(r->flags&X9_CAM_CFG_ATTEMPTED))return;
    if(r->sensor==X9_CAMERA_FRONT)
        printf("front fab-read/exit-ACK=%u B4-variant=%u (raw bytes/port-exit error in kernel X9 CAMERA FAB log; NOT QSC/LRC)\n",
            !!(r->flags&X9_CAM_CFG_FAB_READ),!!(r->flags&X9_CAM_CFG_FAB_B4));
    printf("standby cold stage=%u chip=%04x flags=%x blockers=%x errno=%u restore=%u AP timing CSR/clock evidence: use kernel X9 CAMERA COLD/CLOCK log\n",
        r->probe.stage,r->probe.chip_id,r->probe.flags,r->probe.blockers,
        r->probe.last_errno,r->probe.restore_errno);
    for(unsigned i=0;i<X9_CAM_CFG_SAMPLES && r->sample[i].reg;i++) {
        const struct x9_camera_config_sample *s=&r->sample[i];
        if(i<r->read_count)printf("standby reg=%04x expected=%02x observed=%02x %s\n",
            s->reg,s->expected,s->observed,s->observed==s->expected?"match":"MISMATCH");
        else printf("standby reg=%04x expected=%02x unread\n",s->reg,s->expected);
    }
}
static void print_mm(const struct x9_camera_mm_status *r)
{
    static const char *const names[X9_CAM_MM_SLOTS]={"AO","infra0","VMM","CAM_VCORE",
        "CAM_MAIN","CAM_MRAW","CAM-AL","CAMTM","SENINF","CAM-CG","CAMSV-CG"};
    printf("MM register-clocks sensor=%u flags=%x stage/slot=%u/%u errno=%u restore=%u blockers=%x retained=%x cfg=%x/%x/%x (NOT capture ready)\n",
        r->sensor,r->flags,r->stage,r->slot,r->last_errno,r->restore_errno,r->blockers,r->retained,
        r->cfg[0],r->cfg[1],r->cfg[2]);
    for(unsigned i=0;i<X9_CAM_MM_SLOTS;i++)if(r->sample[i].flags) {
        const struct x9_camera_mm_sample *s=&r->sample[i];
        printf("MM %u/%s mask=%08x flags=%x AP=%08x/%08x/%08x global=%08x/%08x/%08x effect=%08x/%08x/%08x STA=%08x\n",
            i,names[i],s->mask,s->flags,s->before[0],s->active[0],s->after[0],
            s->before[1],s->active[1],s->after[1],s->before[2],s->active[2],s->after[2],s->sta);
    }
}
static void print_receiver(const struct x9_camera_receiver *r)
{
    static const unsigned out_off[X9_CAM_RX_OUT_WORDS]={0,4,8,0xc,0x10,0x14,0x18,
        0x20,0x2c,0x34,0x38,0x100,0x104,0x108};
    static const unsigned sv_off[X9_CAM_RX_SV_WORDS]={0x40,0x44,0x140,0x148,
        0x540,0x580,0x5c0,0x600,0x640,0x680,0x6c0,0x700,0x1200,0x1204,0x1210,0x2100,0x2140};
    printf("receiver sensor=%u flags=%x stage=%u errno=%u restore=%u retained=%x busy=%x changed=%x blockers=%x (read-only, NOT ownership/drain)\n",
        r->sensor,r->flags,r->stage,r->last_errno,r->mm.restore_errno,r->mm.retained,
        r->busy,r->changed,r->blockers);
    if(!(r->flags&X9_CAM_RX_ATTEMPTED))return;
    print_mm(&r->mm);
    printf("MAC=%llx out=%llx/%llx CAMSV=%llx MAC ctrl/version/phy/prbs=%08x/%08x/%08x/%08x -> %08x/%08x/%08x/%08x\n",
        (unsigned long long)r->mac_base,(unsigned long long)r->out_base,
        (unsigned long long)r->out_inner_base,(unsigned long long)r->sv_base,
        r->before.mac[0],r->before.mac[1],r->before.mac[2],r->before.mac[3],
        r->after.mac[0],r->after.mac[1],r->after.mac[2],r->after.mac[3]);
    printf("async CFG/SPLIT/AFIFO=%08x/%08x/%08x -> %08x/%08x/%08x\n",
        r->before.async_cfg,r->before.async_split,r->before.afifo,
        r->after.async_cfg,r->after.async_split,r->after.afifo);
    for(unsigned bank=0;bank<2;bank++) {
        const char *name=bank?"inner":"outer";
        for(unsigned i=0;i<X9_CAM_RX_OUT_WORDS;i++)
            printf("outmux0 %s +%04x=%08x -> %08x\n",name,out_off[i],r->before.out[bank][i],r->after.out[bank][i]);
        for(unsigned i=0;i<X9_CAM_RX_SV_WORDS;i++)
            printf("CAMSV0 %s +%04x=%08x -> %08x\n",name,sv_off[i],r->before.sv[bank][i],r->after.sv[bank][i]);
    }
}
int main(int argc,char **argv)
{
    bool probe=argc==3 && !strcmp(argv[1],"probe");
    bool pipeline=argc==3 && !strcmp(argv[1],"pipeline");
    bool resources=argc==2 && !strcmp(argv[1],"resources");
    bool clocks=argc==3 && !strcmp(argv[1],"clocks");
    bool receiver=argc==3 && !strcmp(argv[1],"receiver");
    bool standby=argc==3 && !strcmp(argv[1],"standby");
    const char *which=(probe||pipeline||clocks||receiver||standby)?argv[2]:argc==2?argv[1]:"status";
    unsigned sensor=!strcmp(which,"back")?1:!strcmp(which,"front")?2:0;
    if(argc>3 || (argc==3 && !probe && !pipeline && !clocks && !receiver && !standby) || ((probe||pipeline||clocks||receiver||standby) && !sensor) ||
       (argc>=2 && !sensor && !resources && strcmp(which,"status"))) {
        fprintf(stderr,"usage: cameractl [status|back|front|probe back|probe front|standby back|standby front|pipeline back|pipeline front|resources|clocks back|clocks front|receiver back|receiver front]\n");return 2;
    }
    int fd=open(X9_CAMERA_DEVICE,((sensor && !pipeline)||resources?O_RDWR:O_RDONLY)|O_CLOEXEC);
    if(fd<0){perror(X9_CAMERA_DEVICE);return 1;}
    bool physical=sensor && !probe && !pipeline && !clocks && !receiver && !standby;
    struct x9_camera_status s={0};
    if(ioctl(fd,X9_CAMERA_GET_STATUS,&s)){perror("status");return finish(fd,1,physical);}
    printf("version=%u mode=%u flags=%x supported=%x sensor=%u errno=%u frames=%llu rejected=%llu chips=%x/%x probe_mask=%x probe_flags=%x cold_journal=%x restore_step=%u\n",
           s.version,s.mode,s.flags,s.supported_mask,s.sensor,s.last_errno,
           (unsigned long long)s.frames,(unsigned long long)s.rejected,s.chip_id[0],s.chip_id[1],s.probe_mask,s.probe_flags,
           s.cold_retained,s.cold_restore_step);
    if(!sensor) {
        struct x9_camera_stop receipt={0};
        if(ioctl(fd,X9_CAMERA_GET_STOP,&receipt)!=0) {
            if(errno!=ENOTTY){perror("historical stop receipt");return finish(fd,1,false);}
            printf("historical stop receipt unavailable (not retirement proof)\n");
        } else print_stop(&receipt,"historical-only, no MMIO/no reissue");
    }
    if(standby || !sensor) {
        struct x9_camera_config r={.version=1,.sensor=standby?sensor:0};
        if(ioctl(fd,standby?X9_CAMERA_CONFIG_STANDBY:X9_CAMERA_GET_CONFIG,&r)) {
            if(standby || errno!=ENOTTY){perror("standby ioctl");return finish(fd,1,physical);}
        } else {
            print_config(&r);
            if(standby){return finish(fd,r.last_errno || r.restore_errno?1:0,physical);}
        }
    }
    if(receiver || !sensor) {
        struct x9_camera_receiver r={.version=1,.sensor=receiver?sensor:0};
        if(ioctl(fd,receiver?X9_CAMERA_PROBE_RECEIVER:X9_CAMERA_GET_RECEIVER,&r)) {
            if(receiver || errno!=ENOTTY){perror("receiver ioctl");return finish(fd,1,physical);}
        } else {
            print_receiver(&r);
            if(receiver){return finish(fd,r.last_errno?1:0,physical);}
        }
    }
    if(clocks || !sensor) {
        struct x9_camera_mm_status r={.version=1,.sensor=clocks?sensor:0};
        if(ioctl(fd,clocks?X9_CAMERA_PROBE_MM_CLOCKS:X9_CAMERA_GET_MM_CLOCKS,&r)) {
            if(clocks || errno!=ENOTTY){perror("MM clocks ioctl");return finish(fd,1,physical);}
        } else {
            print_mm(&r);
            if(clocks){return finish(fd,r.last_errno?1:0,physical);}
        }
    }
    if(resources || !sensor) {
        struct x9_camera_resources r={.version=1};
        if(ioctl(fd,resources?X9_CAMERA_PROBE_RESOURCES:X9_CAMERA_GET_RESOURCES,&r)) {
            if(resources || errno!=ENOTTY){perror("resources ioctl");return finish(fd,1,physical);}
        } else {
            printf("shared receive-power flags=%x stage=%u errno=%u restore=%u blockers=%x smc_get/put=%08x/%08x (NOT capture ready)\n",
                r.flags,r.stage,r.last_errno,r.restore_errno,r.blockers,r.smc_get,r.smc_put);
            printf("CSI_BS_RX before/after AP=%08x/%08x global=%08x/%08x STA=%08x/%08x DONE=%08x/%08x PM_ACK=%08x/%08x ALL_STA=%08x/%08x FSM=%08x/%08x\n",
                r.csi_before[0],r.csi_after[0],r.csi_before[1],r.csi_after[1],
                r.csi_before[2],r.csi_after[2],r.csi_before[3],r.csi_after[3],
                r.csi_before[4],r.csi_after[4],r.csi_before[5],r.csi_after[5],r.hwccf_before,r.hwccf_after);
            printf("MM infra PM_ACK before/active/after=%08x/%08x/%08x required=%08x\n",
                r.mm_ack_before,r.mm_ack_active,r.mm_ack_after,r.mm_ack_required);
            printf("MM-SMMUv3 IDR0/1/3/5=%08x/%08x/%08x/%08x CR0/ACK=%08x/%08x CR1/2=%08x/%08x GBPA=%08x GERROR/N=%08x/%08x IRQ/ACK=%08x/%08x\n",
                r.idr[0],r.idr[1],r.idr[3],r.idr[5],r.cr0,r.cr0ack,r.cr1,r.cr2,
                r.gbpa,r.gerror,r.gerrorn,r.irq_ctrl,r.irq_ack);
            printf("unclaimed STRTAB=%llx cfg=%08x CMDQ=%llx prod/cons=%08x/%08x EVTQ=%llx prod/cons=%08x/%08x (no table/queue writes, no mapping)\n",
                (unsigned long long)r.strtab_base,r.strtab_cfg,(unsigned long long)r.cmdq_base,r.cmdq_prod,r.cmdq_cons,
                (unsigned long long)r.evtq_base,r.evtq_prod,r.evtq_cons);
            if(resources){return finish(fd,r.last_errno?1:0,physical);}
        }
    }
    if(pipeline) {
        struct x9_camera_pipeline r={.version=1,.sensor=sensor};
        if(ioctl(fd,X9_CAMERA_GET_PIPELINE,&r)){perror("pipeline ioctl");return finish(fd,1,physical);}
        printf("source-only pipeline sensor=%u flags=%x blockers=%x CSI%u %s/%u async=%u vc=%u dt=%02x %ux%u stride=%u bytes=%u\n",
            r.sensor,r.flags,r.blockers,r.csi_port,r.cphy?"C-PHY":"D-PHY",r.lanes,r.async,
            r.vc,r.dt,r.width,r.height,r.stride,r.bytes);
        printf("candidate outmux=%u CAMSV=%u CSI=%llx CAMSV=%llx MM-SMMUv3=%llx sid=%x dma_bits=%u dma_limit=%llx\n",
            r.outmux_candidate,r.camsv_candidate,(unsigned long long)r.csi_base,
            (unsigned long long)r.camsv_base,(unsigned long long)r.smmu_base,r.smmu_sid,r.dma_bits,
            (unsigned long long)r.dma_limit);
        printf("planned VCDT=%08x size=%08x grab=%08x/%08x RAW10=%08x WDMA-basic=%08x (no MMIO, no reservation, NOT capture ready)\n",
            r.vcdt,r.expected_size,r.grab_pixels,r.grab_lines,r.format,r.dma_basic);
        return finish(fd,0,physical);
    }
    if(probe || !sensor) {
        struct x9_camera_probe r={.version=1,.sensor=probe?sensor:0};
        if(ioctl(fd,probe?X9_CAMERA_PROBE_SENSOR:X9_CAMERA_GET_PROBE,&r)) {
            /* Older phase1 kernels have no probe ABI; status remains useful. */
            if(probe || errno!=ENOTTY){perror("probe ioctl");return finish(fd,1,physical);}
        } else {
            printf("sensor-probe sensor=%u stage=%u flags=%x blockers=%x chip=%04x errno=%u restore=%u wl=%02x/%02x mclk=%08x/%08x rails=%06x/%06x i2c=%x/%x (NOT capture ready)\n",
                r.sensor,r.stage,r.flags,r.blockers,r.chip_id,r.last_errno,r.restore_errno,
                r.wl_id,r.wl_enable,r.mclk_cfg,r.mclk_status,r.dovdd,r.afvdd,r.i2c_status[0],r.i2c_status[1]);
            printf("clock ap_vote_before=%08x done=%08x univpll_con1=%08x fence=%08x (configured nominal, NOT measured Hz)\n",
                r.hwv_vote,r.hwv_done,r.pll_con1,r.pll_fence);
            if(probe){return finish(fd,r.last_errno?1:0,physical);}
        }
    }
    if(!sensor){return finish(fd,0,physical);}
    struct hakios_camera_command c={.command=HAKIOS_CAMERA_SENSOR,.a=sensor,.b=~0u};
    if(write(fd,&c,sizeof(c))!=sizeof(c)){perror("start");return finish(fd,1,physical);}
    struct hakios_camera_frame h;
    if(read(fd,&h,sizeof(h))!=sizeof(h)){perror("frame header");return finish(fd,1,physical);}
    if(memcmp(h.magic,HAKIOS_CAMERA_MAGIC,8) || h.header_bytes!=sizeof(h) ||
       !h.height || !h.stride || (uint64_t)h.stride*h.height>X9_CAMERA_MAX_BYTES){return finish(fd,1,physical);}
    size_t bytes=sizeof(h)+(size_t)h.stride*h.height;void *buffer=malloc(bytes);
    if(!buffer){return finish(fd,1,physical);}
    ssize_t n=read(fd,buffer,bytes);
    if(n==(ssize_t)bytes){h=*(struct hakios_camera_frame*)buffer;
        printf("physical RAW10 sensor=%u %ux%u stride=%u sequence=%llu time=%llu\n",h.sensor,h.width,h.height,h.stride,
               (unsigned long long)h.sequence,(unsigned long long)h.time);}
    else perror("frame");
    free(buffer);return finish(fd,n==(ssize_t)bytes?0:1,physical);
}
