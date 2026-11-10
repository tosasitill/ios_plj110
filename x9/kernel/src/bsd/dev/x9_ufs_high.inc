/* MT6993 late UFS scaling. Sources: captured ufshci0 DT, clk-mt6993.c,
 * clk-mux.c, clk-pll.c and ufs-mediatek.c. Only our two mux fields change;
 * PLLs are borrowed, never reprogrammed or disabled. The vendor PLL_FENC
 * prepare op only maintains a software reference; readiness is fence bit4.
 * No broad TOPCKGEN RMW, guessed APMIXED base, PHY reset or MCQ takeover.
 */
#define X9_HIGH_ERRORS 0x70884U
#define X9_PLL_RATE_MASK 0x073fffffU
static struct {
 vm_offset_t top,univ,main;
 uint32_t cfg_before,cfg_after,univ_rate,main_rate,voltage[3];
 uint32_t fenc_before,fenc_after,cksta_before,cksta_after,univ_fenc,main_fenc;
 unsigned attempted,active,clocks_changed,floor_held,stage;
 int result;
 unsigned completed,guard;
 struct x9u_hs_state hs;
 struct {
  unsigned valid,stage,guard;
  uint32_t is,hcs,hce,doorbell,ue[5],cfg,fenc,cksta,pll[4],voltage[3];
 } fault;
} xg;
static uint32_t high_rd(vm_offset_t base,unsigned off)
{return *(volatile uint32_t *)(base+off);}
static void high_wr(vm_offset_t base,unsigned off,uint32_t value)
{*(volatile uint32_t *)(base+off)=value;__asm__ volatile("dsb sy":::"memory");}
static void high_poison(void)
{
 /* Preserve the FIRST fault, not a later failed read or panic-time state.
  * All callers hold x9ufs_lock. UEC registers are read-to-clear: read them
  * only for a real UIC_ERROR and never from the formatting/query path. */
 if(!xg.fault.valid) {
  xg.fault.stage=xg.stage;xg.fault.guard=xg.guard;
  if(xu.hci) {
   xg.fault.is=read32(NULL,0x20);xg.fault.hcs=read32(NULL,0x30);
   xg.fault.hce=read32(NULL,0x34);xg.fault.doorbell=read32(NULL,0x58);
   if(xg.fault.is&4U)for(unsigned i=0;i<5;i++)xg.fault.ue[i]=read32(NULL,0x38+4*i);
  }
  if(xg.top) {
   xg.fault.cfg=high_rd(xg.top,0xf0);xg.fault.fenc=high_rd(xg.top,0x274);
   xg.fault.cksta=high_rd(xg.top,0x334);
  }
  if(xg.univ){xg.fault.pll[0]=high_rd(xg.univ,0x34);xg.fault.pll[2]=high_rd(xg.univ,0xc);}
  if(xg.main){xg.fault.pll[1]=high_rd(xg.main,0x34);xg.fault.pll[3]=high_rd(xg.main,0xc);}
  (void)sweet_x9_vcore_ufs_check(xg.fault.voltage);
  xg.fault.valid=1;
 }
 /* xl and every transient view use the same callback and DMA aperture. */
 xu.failed=true;xu.writable=false;xu.host.poisoned=1;xl.host.poisoned=1;
}
static uint64_t high_pll_hz(uint32_t con1)
{
 /* MT6993 pcwbits22/integer8, CON1 postdiv[26:24], clk26m reference.
  * Round up like __mtk_pll_recalc_rate. Never write a shared PLL PCW. */
 uint64_t vco=(26000000ULL*(con1&0x3fffffU)+0x3fffU)>>14;
 unsigned div=1U<<((con1>>24)&7);
 return (vco+div-1)/div;
}
static int high_rate_matches(uint64_t frequency_hz,uint64_t expected)
{return frequency_hz>=expected-expected/1000&&frequency_hz<=expected+expected/1000;}
static int high_clocks_ok(void)
{
 if(!xg.top||!xg.univ||!xg.main)return 0;
 uint32_t cfg=high_rd(xg.top,0xf0),fenc=high_rd(xg.top,0x274);
 return cfg!=0xffffffffU&&fenc!=0xffffffffU&&
  (cfg&0x8783U)==0x403U&& /* UFS parent4/pdn15=0, FDE parent3/pdn7=0 */
  !(high_rd(xg.top,0x334)&((1U<<5)|(1U<<6)))&&
  (fenc&0xc0U)==0xc0U&&
  (high_rd(xg.univ,0x34)&0x10U)&&(high_rd(xg.main,0x34)&0x10U)&&
  (high_rd(xg.univ,0xc)&X9_PLL_RATE_MASK)==xg.univ_rate&&
  (high_rd(xg.main,0xc)&X9_PLL_RATE_MASK)==xg.main_rate;
}
static int high_platform_check(void *context)
{
 (void)context;
 if(xu.failed)return -1;
 if(!xg.active)return 0;
 /* All native READ/WRITE/SYNC views pass here before ringing the doorbell
  * and before accepting completion. Losing the floor/clock/link stops them
  * all; never repair clock drift while a command might be in flight. */
 unsigned reason=0;
 if(!sweet_x9_vcore_ufs_ok())reason|=1;
 if(!high_clocks_ok())reason|=2;
 if(read32(NULL,0x20)&X9_HIGH_ERRORS)reason|=4;
 if(!(read32(NULL,0x34)&1))reason|=8;
 if((read32(NULL,0x30)&0xb)!=0xb)reason|=16;
 if(reason) {
  xg.guard=reason;
  xg.stage=90;high_poison();return -1;
 }
 return 0;
}
static int high_wait(vm_offset_t base,unsigned off,uint32_t mask,uint32_t want)
{
 uint64_t start=now_us(NULL);
 for(unsigned i=0;i<=300;i++) {
  if((high_rd(base,off)&mask)==want)return 0;
  if(now_us(NULL)-start>=3000)break;
  delay_us(NULL,10);
 }
 return X9U_TIMEOUT;
}
static int high_clock_prepare(void)
{
 xg.stage=3;
 xg.top=ml_io_map(0x1c210000,0x1000);
 /* Real MT6993 providers: captured DT, not the old APMIXED block. */
 xg.main=ml_io_map(0x1c213000,0x100);
 xg.univ=ml_io_map(0x1c213100,0x100);
 if(!xg.top||!xg.main||!xg.univ)return X9U_NOT_READY;
 xg.cfg_before=high_rd(xg.top,0xf0);
 uint32_t uf=high_rd(xg.univ,0x34),mf=high_rd(xg.main,0x34);
 xg.univ_fenc=uf;xg.main_fenc=mf;
 xg.fenc_before=high_rd(xg.top,0x274);xg.cksta_before=high_rd(xg.top,0x334);
 xg.univ_rate=high_rd(xg.univ,0xc)&X9_PLL_RATE_MASK;
 xg.main_rate=high_rd(xg.main,0xc)&X9_PLL_RATE_MASK;
 if(xg.cfg_before==0xffffffffU||uf==0xffffffffU||mf==0xffffffffU||
    !(uf&0x10U)||!(mf&0x10U)||
    !high_rate_matches(high_pll_hz(xg.univ_rate),2496000000ULL)||
    !high_rate_matches(high_pll_hz(xg.main_rate),2184000000ULL)||
    ((xg.cfg_before>>8)&7)>4||(xg.cfg_before&0x8000U)||
    xg.fenc_before==0xffffffffU||!(xg.fenc_before&0x40U)||
    (xg.cksta_before&0x60U))return X9U_UNSUPPORTED;
 xg.stage=4;
 if(sweet_x9_vcore_ufs_hold(xg.voltage))return X9U_NOT_READY;
 xg.floor_held=1;
 /* FDE mux may have been gated because plaintext root uses no crypto.
  * Enable only that mux (bit7); keep UFS0/other clocks untouched. */
 xg.stage=5;
 if(xg.cfg_before&0x80U){xg.clocks_changed=1;high_wr(xg.top,0xf8,0x80U);}
 int e=high_wait(xg.top,0x274,0xc0U,0xc0U);if(e)return e;
 /* Same order as _ufs_mtk_clk_scale(scale_up): VCORE, UFS, FDE.
  * SET/CLR touch only one parent field; UPDATE1 bits26/25 are W1 strobes. */
 const uint32_t mask[]={0x700U,3U},parent[]={0x400U,3U},update[]={1U<<26,1U<<25};
 const uint32_t busy[]={1U<<5,1U<<6};
 for(unsigned i=0;i<2;i++) {
  xg.stage=6+i;
  if((high_rd(xg.top,0xf0)&mask[i])!=parent[i]) {
   xg.clocks_changed=1;
   high_wr(xg.top,0xf8,mask[i]);high_wr(xg.top,0xf4,parent[i]);
   high_wr(xg.top,8,update[i]);
  }
  if((e=high_wait(xg.top,0x334,busy[i],0)))return e;
  if((high_rd(xg.top,0xf0)&mask[i])!=parent[i])return X9U_RESPONSE;
 }
 xg.cfg_after=high_rd(xg.top,0xf0);
 if(!high_clocks_ok()||!sweet_x9_vcore_ufs_ok())return X9U_RESPONSE;
 xg.active=1;xg.stage=8;return 0;
}
static int high_sample(uint8_t digest[32])
{
 /* Same four 64KiB logical windows as native_root_sample in the shim.
  * Root is mutable by now: compare fresh bytes under the I/O mutex, not
  * the old manifest digest. No filesystem request can mutate in between. */
 const uint64_t offsets[]={0,xu.bytes/4,xu.bytes/2,xu.bytes-sizeof(header_data)};
 struct sha256 hash;sha256_init(&hash);
 for(unsigned i=0;i<4;i++) {
  int e=x9u_image_read_mapped(&xu.host,2,&xu.partition,xu.extents,xu.extent_count,
   xu.bytes/4096,offsets[i],header_data,sizeof(header_data));
  if(e)return e;
  sha256_update(&hash,header_data,sizeof(header_data));
 }
 sha256_final(&hash,digest);return 0;
}
void sweet_x9_ufs_late_high(void)
{
 int requested=sweet_x9_arg("ug",0);
 if(requested!=4&&requested!=5)return;
 struct x9u_hs_state hs={0};uint32_t qualified[2][6]={{0}};
 unsigned checked=0;int e=X9U_NOT_READY;uint8_t before[32],after[32];
 lck_mtx_lock(&x9ufs_lock);
 if(xg.attempted){lck_mtx_unlock(&x9ufs_lock);return;}
 xg.attempted=1;xg.stage=1;xg.result=X9U_NOT_READY;
 if(!xu.ready||xu.failed||xu.host.poisoned)goto done;
 /* Mutex drains all native root/log/mailbox/modem views. Reject external
  * outstanding transfers or stale UICs; no forced clear/reset to proceed. */
 e=x9u_hs_prepare_high(&xu.host.io,&hs,(unsigned)requested);
 if(e) {
  if(e!=X9U_UNSUPPORTED||hs.uncertain)high_poison();
  goto done;
 }
 xg.stage=2;
 e=high_sample(before);if(e){high_poison();goto done;}
 e=high_clock_prepare();
 if(e){if(xg.clocks_changed)high_poison();goto done;}
 /* Qualify the changed host clock at the old gear before changing PA gear. */
 e=high_sample(after);
 if(e||memcmp(before,after,32)){if(!e)e=X9U_CRC;high_poison();goto done;}
 unsigned first=hs.before[1]>4?hs.before[1]:4;
 for(unsigned gear=first;gear<=hs.target_gear;gear++) {
  xg.stage=10+gear;
  if(high_platform_check(NULL)){e=X9U_NOT_READY;goto done;}
  e=x9u_hs_apply_high(&xu.host.io,&hs,gear);
  if(e){
   high_poison();
   if(hs.stage==46&&(hs.pmc_is&4U))memcpy(xg.fault.ue,hs.pmc_ue,sizeof(hs.pmc_ue));
   goto done;
  }
  e=high_sample(after);
  if(e||memcmp(before,after,32)){if(!e)e=X9U_CRC;high_poison();goto done;}
  if(checked<2)memcpy(qualified[checked++],hs.after,sizeof(hs.after));
 }
 if(high_platform_check(NULL)){e=X9U_NOT_READY;goto done;}
 xg.stage=20;e=0;
done:;
 xg.result=e;xg.hs=hs;xg.completed=1;
 if(xg.top) {
  xg.cfg_after=high_rd(xg.top,0xf0);xg.fenc_after=high_rd(xg.top,0x274);
  xg.cksta_after=high_rd(xg.top,0x334);
 }
 bool failed=xu.failed;
 unsigned stage=xg.stage;
 lck_mtx_unlock(&x9ufs_lock);
 /* No console while the UFS mutex is held. Never call intermediate G4 a
  * successful final G5 if the later transition failed. */
 printf("X9 UFS HIGH: requested=G%d negotiated=G%u localver=%u caps=%u/%u status=%d stage=%u PA_stage=%u attr=0x%x UIC=%u UPMCRS=%u changed=%u uncertain=%u poisoned=%u\n",
  requested,hs.target_gear,hs.local_version,hs.max_rx_gear,hs.peer_max_rx_gear,e,stage,
  hs.stage,hs.last_attribute,hs.uic_result,hs.upmcrs,hs.changed,hs.uncertain,failed);
 printf("X9 UFS HIGH clocks: CFG14=%08x->%08x UNIVPLL=%lluHz MAINPLL=%lluHz SW_REQ=%08x->%08x VCORE_current=%u min_requested=650000uV floor_held=%u (controller, not PMIC measurement)\n",
  xg.cfg_before,xg.cfg_after,high_pll_hz(xg.univ_rate),high_pll_hz(xg.main_rate),
  xg.voltage[0],xg.voltage[1],(xg.voltage[2]>>8)&7,xg.floor_held);
 printf("X9 UFS HIGH fences: TOP=%08x->%08x CKSTA=%08x->%08x UNIV=%08x MAIN=%08x CON1=%08x/%08x\n",
  xg.fenc_before,xg.fenc_after,xg.cksta_before,xg.cksta_after,
  xg.univ_fenc,xg.main_fenc,xg.univ_rate,xg.main_rate);
 printf("X9 UFS HIGH PMC: isolated_line_resets=%u IS=%08x UEC=%08x/%08x/%08x/%08x/%08x\n",
  hs.pmc_line_resets,hs.pmc_is,hs.pmc_ue[0],hs.pmc_ue[1],hs.pmc_ue[2],hs.pmc_ue[3],hs.pmc_ue[4]);
 for(unsigned i=0;i<checked;i++) {
  uint32_t *p=qualified[i];
  printf("X9 UFS HIGH sample: mode=0x%x TX_G%u RX_G%u TX_lanes=%u RX_lanes=%u series=%u SHA256_match=1 windows=4 bytes=262144 final_status=%d\n",
   p[0],p[1],p[2],p[3],p[4],p[5],e);
 }
 if(e)printf("X9 UFS HIGH: %s; no retry/reset/voltage rollback, DMA retained\n",
  failed?"uncertain state: all native I/O disabled":"precondition blocked: unchanged qualified link retained");
}
void sweet_x9_ufs_boot_failure(char *out,unsigned capacity)
{
 if(!out||!capacity)return;
 /* Called after load_init_program_at_path returned, before panic begins.
  * No SMC, DME command, DMA, MMIO, console, vote or filesystem operation.
  * Wait only for the same bounded transfer/transition lock used by I/O. */
 lck_mtx_lock(&x9ufs_lock);
 uint32_t ue[5];memcpy(ue,xg.fault.ue,sizeof(ue));
 snprintf(out,capacity,
  "ufs=%u/%u/%u/%u high=%u/%u/%u:%d G%u PA=%u:%x:%u:%u guard=%x IS/HCS/HCE/DB=%08x/%08x/%x/%x UEC=%08x/%08x/%08x/%08x/%08x CLK=%08x/%08x/%08x PLL=%08x/%08x/%08x/%08x DV=%08x/%08x/%08x OCS/phase=%u/%u LR=%u",
  xu.ready,xu.writable,xu.failed,xu.host.poisoned,xg.attempted,xg.completed,
  xg.fault.valid?xg.fault.stage:xg.stage,xg.result,xg.hs.after[1],
  xg.hs.stage,xg.hs.last_attribute,xg.hs.uic_result,xg.hs.upmcrs,
  xg.fault.guard,xg.fault.is,xg.fault.hcs,xg.fault.hce,xg.fault.doorbell,
  ue[0],ue[1],ue[2],ue[3],ue[4],
  xg.fault.cfg,xg.fault.fenc,xg.fault.cksta,
  xg.fault.pll[0],xg.fault.pll[1],xg.fault.pll[2],xg.fault.pll[3],
  xg.fault.voltage[0],xg.fault.voltage[1],xg.fault.voltage[2],
  xu.host.last_ocs,xu.host.failure_phase,xg.hs.pmc_line_resets);
 lck_mtx_unlock(&x9ufs_lock);
}
