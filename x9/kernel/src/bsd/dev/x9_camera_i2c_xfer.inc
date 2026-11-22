/* Real cold probe/standby AP FIFO path, not a provider/IRQ adapter.
 * mt6991-compatible status: COMP0, ACKERR1, HS_NACK2, ARB_LOST3,
 * RS_TRANSFER4, CONFERR9. Only a stopped, qualified COMP/NACK request
 * permits local status/FIFO/START cleanup. No reset/resume/retry after
 * arbitration/configuration/unknown status or an unretired request.
 * Software journal flags do NOT prove exclusive AP/SCP ownership or ABA.
 */
static int cam_bus_other_idle(struct cam_probe_bus *b)
{
    for(unsigned channel=1;channel<b->banks;channel++) {
        vm_offset_t c=b->base+channel*0x10000;
        if(cam_rr(c,CAM_I2C_START)&1)return EBUSY; /* other host actually running */
    }
    for(unsigned channel=0;channel<b->dma_banks;channel++)
        if(cam_rr(b->dma,channel*0x10000+0x08)&1)return EBUSY;
    return 0;
}
static int cam_bus_request_fault(struct cam_probe_bus *b,int e,uint32_t status,uint32_t start)
{
    if(!b->failed_status && status!=UINT32_MAX)b->last_status=status;
    b->failed_status=b->wedged=true;
    printf("X9 CAMERA AP REQUEST: gate=%x errno=%d status=%08x start=%08x "
        "fifo=%u request=%u bus=%s (retained, no further W1C/FIFO/START cleanup or retry)\n",
        b->gate_bit,e,status,start,b->fifo_owned,b->request_owned,
        b==&cam_journal.ldo?"WL":"sensor");
    return e;
}
/* Expected own channel status and a stopped START; other exposed channels
 * and all APDMA banks must remain idle before consuming RX or doing W1C.
 * Several CSR reads, not an atomic hardware ownership snapshot. */
static int cam_bus_terminal_check(struct cam_probe_bus *b,uint32_t status)
{
    int e=cam_bus_controls(b,false);if(e)return e;
    e=cam_bus_other_idle(b);if(e)return e;
    uint32_t start=cam_rr(b->base,CAM_I2C_START);
    if(start&~(1u<<14))return EBUSY; /* no START, resume or extra-leg configuration */
    if(cam_rr(b->base,CAM_I2C_STAT)!=status)return ESTALE;
    return 0;
}
/* AP FIFO transfers. FIFO_DATA is byte-accessed upstream even with a
 * 32-bit CSR. WRRD keeps its four-byte TX prefix and up to16 RX bytes.
 * Write-only transfers may use the whole 16-byte mt6991 FIFO: Android's
 * adaptor_i2c_wr_seq_p8 puts the 16-bit register and consecutive payload
 * bytes in one i2c_msg, and i2c-mt65xx.c:1967 selects FIFO whenever
 * msg.len <= fifo_size. No DMA, extra write leg or automatic retry. */
static int cam_bus_transfer_n(struct cam_probe_bus *b,uint8_t slave,
                            const uint8_t *tx,unsigned count,uint8_t *rx,unsigned rx_count)
{
    if(!tx || !count || (rx?count>4:count>CAM_I2C_FIFO_BYTES) ||
       rx_count>CAM_I2C_FIFO_BYTES ||
       (!!rx != !!rx_count))return EINVAL;
    if(!b->owned || cam_bus_unretired(b))return EBUSY;
    int e=cam_bus_clock(b);if(!e)e=cam_bus_idle(b);
    if(!e)e=cam_bus_controls(b,false);if(e)return e;
    uint32_t control=(b->saved[1]&~((1u<<1)|(1u<<2)|(1u<<4)|(1u<<8)|(1u<<9)))|
        (1u<<3)|(1u<<5);
    if(rx)control|=(1u<<1)|(1u<<4);
    e=cam_bus_control_write(b,CAM_I2C_CTRL,control,false);
    if(!e)e=cam_bus_control_write(b,CAM_I2C_MASK,0,false);
    if(!e)e=cam_bus_control_write(b,CAM_I2C_ADDR,(uint32_t)slave<<1,false);
    if(!e)e=cam_bus_control_write(b,CAM_I2C_LEN,count,false);
    if(!e)e=cam_bus_control_write(b,CAM_I2C_AUX,rx_count,false);
    if(!e)e=cam_bus_control_write(b,CAM_I2C_TRANS,rx?2:1,false);
    if(!e)e=cam_bus_idle(b); /* no newly active channel before FIFO clear */
    if(!e)e=cam_bus_controls(b,false); /* fresh pin/clock/CSR guard before FIFO mutation */
    if(e)return e;
    b->fifo_owned=true; /* BEFORE first possibly ambiguous FIFO mutation */
    cam_rw(b->base,CAM_I2C_FIFO_CLR,1);
    for(unsigned i=0;i<count;i++)*(volatile uint8_t *)(b->base+CAM_I2C_DATA)=tx[i];
    __asm__ volatile("dsb sy" ::: "memory");
    e=cam_bus_controls(b,false);if(!e)e=cam_bus_other_idle(b);
    if(!e && ((cam_rr(b->base,CAM_I2C_START)&1) || cam_rr(b->base,CAM_I2C_STAT)))e=EBUSY;
    if(!e && (cam_rr(b->base,CAM_I2C_FIFO)&31)!=count)e=EIO;
    if(e)return cam_bus_request_fault(b,e,UINT32_MAX,UINT32_MAX);
    /* Match i2c-mt65xx.c I2C_POLLING_TIMEOUT=50ms and its final STAT
     * sample. A thread may be descheduled between START and the first
     * poll: elapsed wall time alone must not discard a sticky terminal
     * status which the full stopped-request checks can still qualify.
     * This is an observation budget, not a physical completion timestamp.
     * Never revisit a request once the no-terminal failure is latched. */
    uint64_t interval;nanoseconds_to_absolutetime(50000000ULL,&interval);
    uint64_t began=mach_absolute_time(),deadline=began+interval,elapsed_ns;
    b->request_owned=true; /* BEFORE START, including an unconfirmed write */
    cam_rw(b->base,CAM_I2C_START,(1u<<14)|1u);
    uint32_t status=0,polls=0,start=UINT32_MAX;
    for(;;) {
        status=cam_rr(b->base,CAM_I2C_STAT);
        polls++;
        if(status)break;
        if(mach_absolute_time()>=deadline) {
            status=cam_rr(b->base,CAM_I2C_STAT); /* upstream final sample */
            polls++;
            break;
        }
        delay(5);
    }
    absolutetime_to_nanoseconds(mach_absolute_time()-began,&elapsed_ns);
    if(!status || elapsed_ns>=50000000ULL) {
        start=cam_rr(b->base,CAM_I2C_START); /* real sample, not ffffffff placeholder */
        printf("X9 CAMERA AP WAIT: gate=%x slave=%x tx=%u rx=%u polls=%u "
            "elapsed_us=%llu budget_us=50000 status=%08x start=%08x "
            "bus=%s (terminal observation only; qualification follows)\n",
            b->gate_bit,slave,count,rx_count,polls,elapsed_ns/1000,status,start,
            b==&cam_journal.ldo?"WL":"sensor");
    }
    if(!status)return cam_bus_request_fault(b,ETIMEDOUT,status,start);
    if(status&~7u)return cam_bus_request_fault(b,EIO,status,UINT32_MAX);
    start=cam_rr(b->base,CAM_I2C_START);
    if(start&~(1u<<14))return cam_bus_request_fault(b,EBUSY,status,start);
    e=cam_bus_terminal_check(b,status);
    if(e)return cam_bus_request_fault(b,e,status,start);
    int result=(status&6)?ENXIO:0; /* a qualified NACK is not an ACK */
    uint32_t pending=cam_rr(b->base,CAM_I2C_FIFO)&31;
    if((!result && pending!=rx_count) || (result && pending>count))
        return cam_bus_request_fault(b,EIO,status,start);
    uint8_t bytes[CAM_I2C_FIFO_BYTES]={0};
    if(!result)for(unsigned i=0;i<rx_count;i++)bytes[i]=*(volatile uint8_t *)(b->base+CAM_I2C_DATA);
    /* Recheck after consuming RX, before the first cleanup write. */
    e=cam_bus_terminal_check(b,status);
    if(!e) {
        pending=cam_rr(b->base,CAM_I2C_FIFO)&31;
        if((!result && pending) || (result && pending>count))e=EIO;
    }
    if(e)return cam_bus_request_fault(b,e,status,start);
    if(!b->failed_status)b->last_status=status;
    cam_rw(b->base,CAM_I2C_STAT,status); /* only qualified own COMP/NACK bits */
    /* Do not continue cleanup if W1C was ambiguous or new status/START/
     * DMA/configuration activity appeared. These checks are not atomic. */
    e=cam_bus_terminal_check(b,0);
    if(e)return cam_bus_request_fault(b,e,status,UINT32_MAX);
    cam_rw(b->base,CAM_I2C_FIFO_CLR,1);
    e=cam_bus_terminal_check(b,0);
    if(!e && (cam_rr(b->base,CAM_I2C_FIFO)&31))e=EIO;
    if(e)return cam_bus_request_fault(b,e,status,UINT32_MAX);
    cam_rw(b->base,CAM_I2C_START,0);
    e=cam_bus_controls(b,false);if(!e)e=cam_bus_idle(b);
    if(e)return cam_bus_request_fault(b,e,status,start);
    b->fifo_owned=b->request_owned=false; /* only after confirmed local idle */
    if(result)b->failed_status=true;
    if(!result && rx)memcpy(rx,bytes,rx_count); /* no output before cleanup succeeds */
    return result;
}
static int cam_bus_transfer(struct cam_probe_bus *b,uint8_t slave,
                            const uint8_t *tx,unsigned count,uint8_t *rx)
{ return cam_bus_transfer_n(b,slave,tx,count,rx,rx?1:0); }
