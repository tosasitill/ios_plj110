/* MT6993 captured I2C0 + mt6991-i2c FIFO v2; external gauge at 0x55.
 * Preserve LK timing/pinmux. No gauge unseal, reset, learning or charger writes.
 * Hardware timeout poisons the transport until next boot, never force-recover it.
 */
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSAtomic.h>
extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static IOLock * volatile pmifLock;
extern "C" int X9PMIFAcquire(void) {
    IOLock *l = pmifLock;
    if (!l) {
        l = IOLockAlloc(); if (!l) return -1;
        if (!OSCompareAndSwapPtr(NULL, l, (void * volatile *)&pmifLock)) IOLockFree(l);
        l = pmifLock;
    }
    IOLockLock(l); return 0;
}
extern "C" void X9PMIFRelease(void) { IOLockUnlock(pmifLock); }
static vm_offset_t gaugeBus, gaugePMIF;
static bool busPoisoned;
static unsigned gaugeType, sampleCount;
static uint32_t gr(vm_offset_t b, unsigned o) { return *(volatile uint32_t *)(b+o); }
static void gw(vm_offset_t b, unsigned o, uint32_t v) {
    *(volatile uint32_t *)(b+o)=v; __asm__ volatile("dsb sy" ::: "memory");
}
static bool gx(const uint8_t *tx, unsigned n, uint8_t *data, unsigned count) {
    if (busPoisoned || (gr(gaugeBus,0x24)&1)) return false;
    unsigned ctl=gr(gaugeBus,0x10)&~(2u|4u|16u|256u|512u);
    if(count)ctl|=2u|16u;
    gw(gaugeBus,0x10,ctl|8u|32u);
    gw(gaugeBus,0x94,0xaa); gw(gaugeBus,8,0);
    gw(gaugeBus,0xc,0x21f); gw(gaugeBus,0x38,1);
    gw(gaugeBus,0x14,n); gw(gaugeBus,0x44,count);
    gw(gaugeBus,0x18,count?2:1);
    for(unsigned i=0;i<n;i++) *(volatile uint8_t *)gaugeBus=tx[i];
    gw(gaugeBus,0x24,1u|(1u<<14));
    unsigned status=0;
    for(unsigned i=0;i<2000;i++) {
        status=gr(gaugeBus,0xc); if(status&0x20f)break; IODelay(10);
    }
    if (!(status&1) || (status&0x20e)) {
        IOLog("X9 BATTERY: I2C failed status=%x; sampling stopped\n",status);
        busPoisoned=true;return false;
    }
    for(unsigned i=0;i<count;i++)data[i]=*(volatile uint8_t *)gaugeBus;
    gw(gaugeBus,0xc,status);return true;
}
static bool gaugeWord(uint8_t reg,uint16_t *v) { uint8_t b[2]; if(!gx(&reg,1,b,2))return false; *v=b[0]|((unsigned)b[1]<<8);return true;}
static bool pwait(unsigned expected) {
    for(unsigned i=0;i<1000;i++) {
        unsigned s=gr(gaugePMIF,0x8a8);
        if(s&(1u<<18))return false;
        if(((s>>1)&7)==expected)return true;
        IODelay(1);
    }
    return false;
}
static bool chargerByte(unsigned reg,uint8_t *v) {
    if(!pwait(0))return false;
    gw(gaugePMIF,0x880,(3u<<30)|(14u<<24)|reg);
    if(!pwait(6))return false;
    *v=gr(gaugePMIF,0x894);gw(gaugePMIF,0x8a4,1);return pwait(0);
}
static bool gaugePrepare(void) {
    gaugeBus=ml_io_map(0x12950000,0x1000);
    gaugePMIF=ml_io_map(0x1c018000,0x2000);
    vm_offset_t gate=ml_io_map(0x129d0000,0x1000);
    vm_offset_t gpio=ml_io_map(0x1002d000,0x1000);
    if(!gaugeBus||!gaugePMIF||!gate||!gpio)return false;
    /* GPIO112/113 are mode1 in the captured pinctrl. Do not steal other pins. */
    if((gr(gpio,0x4c0)&0x1f1f)!=0x0101) {
        IOLog("X9 BATTERY: inherited I2C0 pins unavailable\n");return false;
    }
    gw(gate,0xe04,1); /* clk-mt6993-peri: only IMPS_I2C0 gate */
    if((gr(gate,0xe00)&1)||!gr(gaugeBus,0x20)||(gr(gaugeBus,0x24)&1)) {
        IOLog("X9 BATTERY: inherited I2C0 clock/timing/bus not ready\n");return false;
    }
    /* Captured DT support_extern_cmd: stock OPlus writes MAC device-type
     * request then reads back echoed command + type after 5ms. */
    uint8_t idcmd[]={0x3e,1,0}, reg=0x3e, response[4];
    if(!gx(idcmd,3,NULL,0))return false;
    IOSleep(5);
    if(!gx(&reg,1,response,4))return false;
    if(response[0]!=1 || response[1]!=0) {
        IOLog("X9 BATTERY: external ID response invalid %02x %02x %02x %02x\n",response[0],response[1],response[2],response[3]);return false;
    }
    uint16_t id=response[2]|((unsigned)response[3]<<8);
    IOLog("X9 BATTERY: external gauge type=0x%04x\n",id);
    /* OPlus selects BQ27541 registers by default for this DT; NFG8011B
     * additionally uses register 0x0c for current. */
    if(!id || id==0xffff)return false;
    gaugeType=id;return true;
}
static void gaugePoll(void) {
    uint16_t soc,voltage,current,temp,rm,fcc,cycles;
    bool compact=gaugeType==0x0421;
    unsigned ir=(gaugeType==0x4231)?0x0c:(compact?0x10:0x14);
    if(!gaugeWord(compact?0x1c:0x2c,&soc)||
       !gaugeWord(compact?4:8,&voltage)||!gaugeWord(ir,&current)||
       !gaugeWord(compact?2:6,&temp)||!gaugeWord(compact?0x0c:0x10,&rm)||
       !gaugeWord(compact?0x2c:0x12,&fcc)||!gaugeWord(compact?0x1e:0x2a,&cycles)) {
        X9BatteryInvalidate();return;
    }
    uint8_t detect,enable,state,detect2;
    if(X9PMIFAcquire())return;
    bool ok=chargerByte(0x78,&detect)&&chargerByte(0x114,&enable)&&
        chargerByte(0x121,&state)&&chargerByte(0x78,&detect2);
    X9PMIFRelease();
    if(!ok||((detect^detect2)&4)) {X9BatteryInvalidate();return;}
    unsigned st=state&15;
    if(st>8&&st!=15){X9BatteryInvalidate();return;}
    bool ext=(detect&4)&&st!=15;
    X9BatterySample b={};b.size=sizeof(b);b.version=1;
    b.percent=soc;b.voltage_mv=voltage;b.current_ma=(int16_t)current;
    b.temperature_centi=(int)temp*10-27315;b.present=1;
    b.external=ext;b.charge_capable=ext&&(enable&16)&&st<=7;
    b.status=!ext?2:st==7?4:st<=6&&(enable&16)?1:3;
    b.remaining_mah=rm;b.full_mah=fcc;b.cycles=cycles;
    if(X9BatteryReport(&b,sizeof(b))) {X9BatteryInvalidate();return;}
    if((sampleCount++%6)==0) IOLog("X9 BATTERY: SOC=%d%% %dmV %dmA temp=%d external=%d status=%d\n",
        b.percent,b.voltage_mv,b.current_ma,b.temperature_centi,b.external,b.status);
}
class X9GaugeWorker : public OSObject {
    OSDeclareDefaultStructors(X9GaugeWorker);
public:
    bool init(void) override { return OSObject::init(); }
    IOWorkLoop *loop; IOTimerEventSource *timer;
    static void tick(OSObject *,IOTimerEventSource *t) {
        gaugePoll(); if(!busPoisoned)t->setTimeoutMS(10000);
    }
};
OSDefineMetaClassAndStructors(X9GaugeWorker,OSObject);
extern "C" bool X9BatteryStart(void) {
    static bool attempted;
    if(attempted)return false;attempted=true;
    if(!gaugePrepare())return false;
    X9GaugeWorker *w=OSTypeAlloc(X9GaugeWorker);
    if(!w||!w->init()){OSSafeReleaseNULL(w);return false;}
    w->loop=IOWorkLoop::workLoop();w->timer=IOTimerEventSource::timerEventSource(w,X9GaugeWorker::tick);
    if(!w->loop||!w->timer||w->loop->addEventSource(w->timer)!=kIOReturnSuccess) {
        OSSafeReleaseNULL(w->timer);OSSafeReleaseNULL(w->loop);w->release();return false;
    }
    w->timer->setTimeoutMS(1000);
    return true; /* Built-in kernel worker remains retained for kernel lifetime. */
}
