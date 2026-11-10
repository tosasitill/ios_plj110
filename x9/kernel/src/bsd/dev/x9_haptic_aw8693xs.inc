/* Native PLJ110 I2C5/AW86937S or AW86938S transport.
 * References: captured /soc/i2c@12350000/aw_haptic@5A;
 * OPPO clk-mt6993{,-peri}.c, pinctrl-mt6993.c, i2c-mt65xx.c,
 * aw8693xs.c / haptic_hv_reg.h. No QUP GPIO bitbang, APDMA, IRQ
 * registration, efuse write, analog calibration copied from another device,
 * Android firmware files or undocumented Apple selector dispatch.
 * Caller holds haptic_lock. Mapping/clock votes survive sessions, while
 * every transfer requalifies pins/clocks/CSRs. Unknown transactions are
 * latched, never reset/retried; dedicated reset can still stop the LRA.
 */
enum { H_DATA=0,H_MASK=8,H_STAT=12,H_CTRL=16,H_LEN=20,H_TRANS=24,
    H_DELAY=28,H_TIMING=32,H_START=36,H_EXT=40,H_LOW=44,H_HS=48,
    H_IO=52,H_CLEAR=56,H_SDA=60,H_AUX=68,H_DIV=72,H_MULTI=140,
    H_MIS=144,H_ADDR=148,H_FIFO=244 };
static const unsigned h_regs[]={H_CTRL,H_LEN,H_TRANS,H_DELAY,H_AUX,H_ADDR,
    H_TIMING,H_EXT,H_LOW,H_HS,H_IO,H_SDA,H_DIV,H_MIS};
static struct {
    vm_offset_t bus,dma,gpio,pads,gate,perao,top,mainpll,univpll;
    uint32_t expected[14],parent,con1,parent_byte,pad_rsel;
    bool mapped,controls,reset_owned,stuck,configured,active;
} hb;
static uint32_t hrr(vm_offset_t b,unsigned r)
{uint32_t v=*(volatile uint32_t *)(b+r);__asm__ volatile("dmb oshld":::"memory");return v;}
static void hwr(vm_offset_t b,unsigned r,uint32_t v)
{*(volatile uint32_t *)(b+r)=v;__asm__ volatile("dsb sy":::"memory");}
static int hwait(vm_offset_t b,unsigned r,uint32_t mask,uint32_t value,unsigned us)
{
    uint64_t end=now_ns()+(uint64_t)us*1000;
    for(unsigned i=0;i<us;i++) {if((hrr(b,r)&mask)==value)return 0;
        if(now_ns()>=end)break;delay(1);}
    return ETIMEDOUT;
}
static uint32_t hmode(unsigned pin)
{return (hrr(hb.gpio,0x300+(pin/4)*16)>>((pin%4)*8))&31;}
static int hmode_route(unsigned pin,unsigned mode)
{
    unsigned r=0x300+(pin/4)*16,shift=(pin%4)*8;
    /* The 5th bit enables modification, not a halfword/global mask. */
    hwr(hb.gpio,r+12,(16u|mode)<<shift);
    return hmode(pin)==mode?0:EIO;
}
static int hreset(bool high)
{
    if(!hb.reset_owned || hmode(30))return EBUSY;
    if(!(hrr(hb.gpio,0)&(1u<<30)))return EBUSY;
    hwr(hb.gpio,0x100+(high?4:8),1u<<30);
    return ((hrr(hb.gpio,0x100)>>30)&1)==(unsigned)high?0:EIO;
}
static int hbus_clock(uint64_t *hz)
{
    uint32_t cfg=(hrr(hb.top,0x110)>>16)&255;
    if(cfg!=hb.parent_byte || cfg&0x80 ||
       !(hrr(hb.top,0x278)&(1u<<29)) || (hrr(hb.top,0x338)&(1u<<27)) ||
       (hrr(hb.gate,0xe00)&1))return EBUSY;
    uint32_t parent=cfg&3;*hz=26000000;
    if(parent) {
        vm_offset_t pll=parent==2?hb.mainpll:hb.univpll;
        uint32_t con=hrr(pll,12);
        if(!(hrr(pll,0x34)&16) || con!=hb.con1)return EBUSY;
        uint64_t divisor=16384ULL*(1u<<((con>>24)&7))*(parent==1?20:parent==2?16:10);
        *hz=((uint64_t)(con&0x3fffff)*26000000+divisor-1)/divisor;
    }
    if(*hz<1000000 || *hz>500000000)return ENOTSUP;
    if(hmode(178)!=1 || hmode(179)!=1 || hmode(30))return EBUSY;
    uint32_t mask=3u<<17;
    if((hrr(hb.pads,0x60)&mask)!=mask || (hrr(hb.pads,0xa0)&mask)!=mask ||
       (hrr(hb.pads,0x90)&mask) ||
       (hrr(hb.pads,0xc0)&(63u<<18))!=hb.pad_rsel)return EBUSY;
    return 0;
}
static int hcontrols(void)
{
    if(hb.stuck)return EIO;
    uint64_t hz;int e=hbus_clock(&hz);if(e)return e;
    if(hb.controls)for(unsigned i=0;i<14;i++)
        if(hrr(hb.bus,h_regs[i])!=hb.expected[i])return EBUSY;
    return 0;
}
static int hidle(bool own)
{
    int e=hcontrols();if(e)return e;
    /* DT exposes eight channel banks, no polling of unrelated buses. */
    for(unsigned i=0;i<8;i++) {
        vm_offset_t b=hb.bus+i*0x10000;
        if(i==0 && own)continue;
        if(hrr(b,H_START) || hrr(b,H_MASK) || hrr(b,H_STAT) ||
           (hrr(b,H_FIFO)&31) || hrr(b,H_MULTI) ||
           (hrr(b,H_CTRL)&(1u|4u|128u|256u|512u)))return EBUSY;
    }
    /* Never enable the shared APDMA clock just to read it. A gated DMA
     * block is not a proof that its shadow registers are zero. Require
     * FIFO/non-DMA controls on all banks; when the gate is live, also check
     * this controller's dedicated DT DMA channel. No global DMA claim. */
    if(!(hrr(hb.perao,0x14)&(1u<<26)) && (hrr(hb.dma,8)&1))return EBUSY;
    return 0;
}
static int hput(unsigned reg,uint32_t v)
{
    int e=hcontrols();if(e)return e;
    for(unsigned i=0;i<14;i++)if(h_regs[i]==reg) {
        hb.expected[i]=v;hwr(hb.bus,reg,v);
        return hrr(hb.bus,reg)==v?0:EIO;
    }
    return EINVAL;
}
static bool hfactor(unsigned n,unsigned *sample,unsigned *step)
{
    for(unsigned a=1;a<=8;a++)if(n%a==0 && n/a>=1 && n/a<=64)
        {*sample=a;*step=n/a;return true;}
    return false;
}
static int htiming(uint64_t hz)
{
    /* Bounded Fast-mode <=390kHz, tHIGH>=0.6us / tLOW>=1.3us;
     * separately encoded MT6991 high/low sample*step and LS EXT. */
    uint32_t timing=0,low=0,divreg=0,ext=0;bool found=false;
    for(unsigned div=1;div<=32 && !found;div++) {
        unsigned total=(unsigned)((hz+div*390000ULL-1)/(div*390000ULL))+1;
        ext=(uint32_t)((hz*13+div*10000000ULL-1)/(div*10000000ULL));
        if(ext>255)continue;
        for(unsigned high=2;high<total-1;high++) {
            unsigned a,b,c,d,l=total-high;
            if((uint64_t)div*(high-1)*10000000<hz*6 ||
               (uint64_t)div*(l-1)*10000000<hz*13 ||
               !hfactor(high,&a,&b) || !hfactor(l,&c,&d))continue;
            timing=((a-1)<<8)|(b-1);low=((c-1)<<6)|(d-1);
            divreg=((div-1)<<8)|(div-1);found=true;break;
        }
    }
    if(!found)return ENOTSUP;
    const unsigned regs[]={H_TIMING,H_LOW,H_DIV,H_EXT,H_HS,H_IO,H_SDA,H_MIS,H_DELAY};
    const uint32_t values[]={timing,low,divreg,(ext<<8)|1,0,3,0,0,0};
    for(unsigned i=0;i<9;i++) {int e=hput(regs[i],values[i]);if(e)return e;}
    return 0;
}
static int htransfer(const uint8_t *tx,unsigned n,uint8_t *rx)
{
    if(!tx || !n || n>16)return EINVAL;
    int e=hidle(false);if(e)return e;
    uint32_t ctrl=(hb.expected[0]&~(1u|2u|4u|16u|128u|256u|512u))|8u|32u;
    if(rx)ctrl|=2u|16u;
    if((e=hput(H_CTRL,ctrl)) || (e=hput(H_ADDR,0xb4)) ||
       (e=hput(H_LEN,n)) || (e=hput(H_AUX,rx?1:0)) ||
       (e=hput(H_TRANS,rx?2:1)) || (e=hidle(false)))return e;
    hwr(hb.bus,H_CLEAR,1);
    for(unsigned i=0;i<n;i++)*(volatile uint8_t *)(hb.bus+H_DATA)=tx[i];
    __asm__ volatile("dsb sy":::"memory");
    if((e=hcontrols()) || (e=hidle(true)) ||
       hrr(hb.bus,H_START) || hrr(hb.bus,H_STAT) || (hrr(hb.bus,H_FIFO)&31)!=n)
        {hb.stuck=true;return e?e:EBUSY;}
    uint64_t end=now_ns()+3000000;uint32_t status=0;
    hwr(hb.bus,H_START,(1u<<14)|1u);
    for(unsigned i=0;i<1000 && now_ns()<end;i++) {
        status=hrr(hb.bus,H_STAT);if(status)break;delay(3);
    }
    if(!status || now_ns()>=end || (status&~7u) ||
       (hrr(hb.bus,H_START)&~(1u<<14)) || (e=hidle(true)))
        {hb.stuck=true;return e?e:status?EIO:ETIMEDOUT;}
    int result=status&6?ENXIO:0;
    uint32_t pending=hrr(hb.bus,H_FIFO)&31;uint8_t byte=0;
    if((!result && pending!=(rx?1u:0u)) || (result && pending>n))
        {hb.stuck=true;return EIO;}
    if(!result && rx)byte=*(volatile uint8_t *)(hb.bus+H_DATA);
    if((e=hidle(true)) || hrr(hb.bus,H_STAT)!=status ||
       (hrr(hb.bus,H_START)&~(1u<<14))) {hb.stuck=true;return e?e:EBUSY;}
    hwr(hb.bus,H_STAT,status);hwr(hb.bus,H_CLEAR,1);hwr(hb.bus,H_START,0);
    if(hrr(hb.bus,H_STAT) || hrr(hb.bus,H_START) || (hrr(hb.bus,H_FIFO)&31))
        {hb.stuck=true;return EIO;}
    if(!result && rx)*rx=byte;return result;
}
static int hget(uint8_t reg,uint8_t *out){return htransfer(&reg,1,out);}
static int hset(uint8_t reg,uint8_t value)
{
    uint8_t tx[]={reg,value},back;int e=htransfer(tx,2,NULL);
    if(!e)e=hget(reg,&back);return e?e:back==value?0:EIO;
}
static int hbits(uint8_t reg,uint8_t mask,uint8_t value)
{
    uint8_t v;int e=hget(reg,&v);if(e)return e;
    return hset(reg,(v&~mask)|(value&mask));
}
static int hcommand(uint8_t value)
{uint8_t tx[]={9,value};return htransfer(tx,2,NULL); /* command bits self-clear */}
static int hstate(void)
{
    uint8_t state,fault;int e=hget(0x3f,&state);if(!e)e=hget(1,&fault);
    if(!e){hs.hw_state=state;hs.hw_fault=fault&0xe6;}
    return e?e:hs.hw_fault?EIO:0;
}
static int hoff(void)
{
    if(!hb.configured && !hb.active)return 0;
    int e=hb.stuck?EIO:hcommand(2);
    uint64_t end=now_ns()+25000000;
    if(!e)do {e=hstate();if(e || !hs.hw_state)break;delay(100);}while(now_ns()<end);
    if(!e && hs.hw_state)e=ETIMEDOUT;
    if(e) {
        /* An uncertain I2C operation cannot prevent electrical shutdown.
         * Pull only our confirmed dedicated GPIO reset low, not bus reset.
         * This deconfigures the chip and readiness remains latched off. */
        int reset=hreset(false);
        hs.flags|=X9_HAPTIC_FAULT;hs.flags&=~X9_HAPTIC_READY;
        if(reset)hs.flags|=X9_HAPTIC_OFF_FAILED;
        else {hb.active=hb.configured=false;hs.hw_state=0;}
        printf("X9 HAPTIC: stop errno=%d dedicated-reset errno=%d; no bus retry\n",e,reset);
        return e;
    }
    hb.active=false;return 0;
}
static int hfeed(const int8_t *data,unsigned count)
{
    if(!count || count>15)return EINVAL;
    uint8_t flags;int e=hget(1,&flags);if(e)return e;
    hs.hw_fault=flags&0xe6;if(hs.hw_fault)return EIO;
    if(flags&8)return EAGAIN;
    uint8_t tx[16];tx[0]=0x32;memcpy(tx+1,data,count);
    return htransfer(tx,count+1,NULL); /* only COMP proves bus submission */
}
static int hgo(void)
{
    int e=hstate();if(e)return e;
    if(hs.hw_state)return EBUSY;
    hb.active=true; /* BEFORE a possibly landed GO */
    e=hcommand(1);uint64_t end=now_ns()+5000000;
    if(!e)do {e=hstate();if(e || hs.hw_state==8)break;delay(50);}while(now_ns()<end);
    return e?e:hs.hw_state==8?0:ETIMEDOUT;
}
static int hconfigure(void)
{
    uint8_t hi,lo,q;int e=hget(0x57,&hi);if(!e)e=hget(0x58,&lo);
    if(e)return e;hs.chip_id=(uint32_t)hi<<8|lo;
    if(hs.chip_id!=0x9371 && hs.chip_id!=0x9381)return ENODEV;
    if((e=hget(0x67,&q)))return e;if(!(q&128))return ENODEV;
    if((e=hstate()))return e;if(hs.hw_state)return EBUSY;
    /* Readback each documented configuration. No trigger input: IRQ29
     * stays untouched; triggers off, broadcast uses our sole slave address.
     * 12k signed RTP uses Android's observed base2048/AE1024/AF1536
     * FIFO partition, no RAM waveform fetch.
     * Leave factory oscillator trim selected; Android's 0xEA is F0 trim,
     * NOT its oscillator calibration and must not be blindly copied. */
    const uint8_t regs[]={3,7,0x2d,0x2e,0x2f,0x30,0x31,0x33,0x34,0x35,0x36,0x37,0x38,0x47,
        0x4a,0x1a,0x21,0x55,0x56,0x5a,0x5b};
    const uint8_t vals[]={255,0x60,8,0,0x46,0,0,0,0,0,0,0,0,0x5a,
        0xbf,8,6,0x4a,0x24,0xa7,0x44};
    /* pro1=7V/4A, pro2=6V/2.5A; UVLO=2.9/2.6/2.3V;
     * DT vmax0x11, software signed peak96, hardware gain0x60. */
    if((e=hbits(0x45,4,4)))return e;delay(600); /* RAMINIT while programming FIFO */
    for(unsigned i=0;i<sizeof(regs);i++)if((e=hset(regs[i],vals[i])))return e;
    if((e=hset(0x4b,0x32)) || (e=hbits(0x48,0x80,0)) ||
       (e=hbits(0x4c,0x7f,0x34)) || (e=hbits(0x4d,0xf0,0x20)) ||
       (e=hbits(0x45,0x3c,0)) ||
       (e=hbits(0x46,0xe3,0xc1)) || /* INT pin, 12k, vendor gain bypass, DLL off */
       (e=hbits(6,0x3f,0x31)) || (e=hbits(8,0x47,0x05)) ||
       (e=hbits(0x52,1,0)) || /* I2S disabled */
       (e=hbits(0x54,0x7f,0x5a)) || /* VBAT protect + half gain */
       (e=hset(0x5d,0x7d)))return e;
    /* Analog defaults and oscillator source are behind the chip's lock. */
    e=hset(0x7c,0x14);if(!e)e=hset(0x7f,0x41);
    if(!e)e=hbits(0x60,0x20,0);int lock=hset(0x5d,0);
    if(e || lock)return e?e:lock;
    if((e=hstate()))return e;if(hs.hw_state)return EBUSY;
    hb.configured=true;hs.gain=0x60;
    printf("X9 HAPTIC: chip=%04x RTP=12000 fifo=2048 f0=%u mHz gain=60 native I2C5 ready\n",
        hs.chip_id,hs.f0_millihz);
    return 0;
}
static int hinit_fail(int e,unsigned stage)
{
    /* Only already-mapped GPIO/clock AO state, never an off I2C/DMA bank.
     * errno alone cannot distinguish a foreign pin, parent or transaction. */
    printf("X9 HAPTIC: init rejected stage=%u errno=%d modes=%u/%u/%u east=%08x gate=%08x; no READY\n",
        stage,e,hmode(30),hmode(178),hmode(179),hrr(hb.top,0x110),hrr(hb.gate,0xe00));
    return e;
}
static int hboard_init(void)
{
    if(hb.mapped)return EALREADY;
    hb.mapped=true;
    hb.bus=ml_io_map(0x12350000,0x80000);hb.dma=ml_io_map(0x16400000,0x10000);
    hb.gpio=ml_io_map(0x1002d000,0x1000);hb.pads=ml_io_map(0x12300000,0x1000);
    hb.gate=ml_io_map(0x12650000,0x1000);hb.perao=ml_io_map(0x16640000,0x1000);
    hb.top=ml_io_map(0x1c210000,0x1000);hb.mainpll=ml_io_map(0x1c213000,0x1000);
    hb.univpll=ml_io_map(0x1c213100,0x1000);
    if(!hb.bus || !hb.dma || !hb.gpio || !hb.pads || !hb.gate || !hb.perao ||
       !hb.top || !hb.mainpll || !hb.univpll)return ENOMEM;
    /* Captured LK gpio-init-default leaves GPIO30 in JTDO_SEL1 (mode7).
     * OPPO devm_gpio_request_one(GPIOF_OUT_INIT_LOW) selects GPIO mode0
     * before hw_reset. This dedicated DT reset must not require LK to have
     * done the Linux GPIO request for us. Refuse modem BPI/JTAG6/other modes. */
    uint32_t reset_mode=hmode(30);
    if((reset_mode!=0 && reset_mode!=7) || hmode(178)>1 || hmode(179)>1)
        return hinit_fail(EBUSY,1);
    uint32_t cfg=(hrr(hb.top,0x110)>>16)&255;
    if((hrr(hb.top,0x338)&(1u<<27)) || cfg&0x7c)return hinit_fail(EBUSY,2);
    if(cfg&128) {
        /* All east leaves must be gated and the parent fenced off before
         * changing an unused parent to 26M. No PLL wake/programming is
         * needed for the FIFO transport; future east consumers can decode
         * this actual source just like the vendor clock framework does. */
        if((hrr(hb.gate,0xe00)&63)!=63 || (hrr(hb.top,0x278)&(1u<<29)))return hinit_fail(EBUSY,3);
        if(cfg&3) {
            hwr(hb.top,0x118,3u<<16);hwr(hb.top,0xc,1u<<4);
            int e=hwait(hb.top,0x338,1u<<27,0,1000);if(e)return hinit_fail(e,4);
            if(((hrr(hb.top,0x110)>>16)&255)!=(cfg&~3u) ||
               (hrr(hb.gate,0xe00)&63)!=63 || (hrr(hb.top,0x278)&(1u<<29)))return hinit_fail(EBUSY,4);
            cfg&=~3u;
        }
    }
    hb.parent=cfg&3;
    if(hb.parent) {
        vm_offset_t pll=hb.parent==2?hb.mainpll:hb.univpll;
        if(!(hrr(pll,0x34)&16))return hinit_fail(ENXIO,5);hb.con1=hrr(pll,12);
    }
    if(cfg&128) {
        if((hrr(hb.gate,0xe00)&63)!=63 || (hrr(hb.top,0x278)&(1u<<29)))return hinit_fail(EBUSY,6);
        hwr(hb.top,0x118,1u<<23); /* only east parent's PDN */
        int e=hwait(hb.top,0x278,1u<<29,1u<<29,1000);if(e)return hinit_fail(e,6);
        cfg&=127;
    }
    hb.parent_byte=cfg;
    hwr(hb.gate,0xe04,1);int e=hwait(hb.gate,0xe00,1,0,1000);if(e)return hinit_fail(e,7);
    /* GPIO30 is the dedicated DT haptic reset, including a warm LK entry.
     * Like OPPO hw_reset, reset it low 5ms then high; no bus reset. DOUT low
     * BEFORE DIR output and MODE0; expose a known-low latch when moving
     * only the dedicated reset from captured LK JTDO7 to GPIO0. */
    if(hmode(30)!=reset_mode)return hinit_fail(EBUSY,8);
    hwr(hb.gpio,8,1u<<30);hwr(hb.gpio,0x108,1u<<30);hwr(hb.gpio,4,1u<<30);
    if(hmode(30)!=reset_mode || !(hrr(hb.gpio,0)&(1u<<30)) ||
       (hrr(hb.gpio,0x100)&(1u<<30)))return hinit_fail(EIO,8);
    if(reset_mode && (e=hmode_route(30,0)))return hinit_fail(e,8);
    if(hmode(30))return hinit_fail(EIO,8);
    hb.reset_owned=true;
    printf("X9 HAPTIC: dedicated reset GPIO30 mode=%u->0 low latch verified; IRQ29 untouched\n",reset_mode);
    /* DT pull207 = MTK RSEL 7. Preserve every unrelated pad field. */
    uint32_t mask=3u<<17;
    hwr(hb.pads,0x98,mask);hwr(hb.pads,0xa4,mask);hwr(hb.pads,0x64,mask);
    hb.pad_rsel=63u<<18;hwr(hb.pads,0xc4,hb.pad_rsel);
    if((e=hmode_route(178,1)) || (e=hmode_route(179,1)))return hinit_fail(e,9);
    uint64_t hz;if((e=hbus_clock(&hz)))return hinit_fail(e,10);
    if((e=hwait(hb.gpio,0x250,3u<<18,3u<<18,1000)))return hinit_fail(e,11); /* DI178/179 */
    if((e=hidle(false)))return hinit_fail(e,12);
    for(unsigned i=0;i<14;i++)hb.expected[i]=hrr(hb.bus,h_regs[i]);hb.controls=true;
    if((e=htiming(hz)))return hinit_fail(e,13);
    /* OPPO hw_reset waits 8000..8500us after releasing the reset, not the
     * previous 2500us. Do not probe the chip before its documented settle. */
    delay(5000);if((e=hreset(true)))return hinit_fail(e,14);delay(8500);
    e=hconfigure();if(e){if(hreset(false))hs.flags|=X9_HAPTIC_OFF_FAILED;hb.configured=false;}
    return e?hinit_fail(e,15):0;
}
