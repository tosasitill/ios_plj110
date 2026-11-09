/* X9 modem stage 3: AP-side services the MD needs before HS2 -> READY.
 * Included by x9_modem.inc (after the ring reader, before x9md_listen).
 *
 *  - ring TX            ccci_ringbuf write half + CCIF doorbell
 *  - RPC (ch32 -> ch33) port_rpc.c kernel ops: GPIO/ADC, EINT attrs, DTSI props,
 *                       user-space ops (ccci_rpcd) answered with their failure/empty form
 *  - FSD (ch14 -> ch15) ccci_mdinit file service: read-only NV partitions + RAM writes
 *  - sysmsg (ch2 -> ch3) battery info + echo test
 *
 * Source contracts: eccci/port/port_rpc.c, port_sysmsg.c, ccci_mdinit decompile
 * (~/ghidra-md/ccci_mdinit.c).
 */

#define X9MD_CH_SYS_RX 2
#define X9MD_CH_SYS_TX 3
#define X9MD_CH_FS_RX 14
#define X9MD_CH_FS_TX 15
#define X9MD_CH_RPC_RX 32
#define X9MD_CH_RPC_TX 33
#define X9MD_PKT_MAX 0x1000

static int x9md_assert_bit=1;           /* RPC/FS assert_bit only while waiting for HS2 */
static uint16_t x9md_txseq[256];
static unsigned x9md_tx_packets,x9md_tx_kicks,x9md_tx_busy;
static unsigned x9md_log_budget=1500;
#define X9MD_LOG(...) do{if(x9md_log_budget){x9md_log_budget--;printf(__VA_ARGS__);}}while(0)

/* ---- ring TX ---- */
static void x9md_tx_copy(uint32_t buf,uint32_t len,uint32_t pos,const uint8_t *src,uint32_t n){
 for(uint32_t i=0;i<n;i++){*(volatile uint8_t *)(x9md_nc+buf+pos)=src[i];if(++pos>=len)pos=0;}
}
/* One CCCI packet on tx queue qi: header (d0,d1,channel|seq,rsv) + payload.
 * d1 is ccci_header.data[1]: total length for RPC/FS, message id for sysmsg. */
static int x9md_tx(unsigned qi,unsigned ch,uint32_t d0,uint32_t d1,uint32_t rsv,const uint8_t *pl,uint32_t n){
 struct x9md_q *q=&x9md_q[qi];
 uint32_t len=q->tx,buf=q->ctl+24+q->rx,pkg=16+n,ccif=(8+pkg+8+7)&~7u;
 if(!len||ccif+8>len)return -1;
 uint32_t rd=0,wr=0;
 for(unsigned tries=0;;tries++){
  rd=x9md_m32(q->ctl+12);wr=x9md_m32(q->ctl+16);
  if(rd>=len||wr>=len)return -2;
  uint32_t used=wr>=rd?wr-rd:wr+len-rd;
  if(len-used>ccif+8)break;
  if(tries>2000){printf("X9 MODEM: TX q%u full (rd=%x wr=%x need %x)\n",qi,rd,wr,ccif);return -3;}
  x9md_sleep_ms(1);
 }
 uint32_t hdr[4];
 hdr[0]=d0;hdr[1]=d1;
 hdr[2]=(ch&0xffffu)|((uint32_t)(x9md_txseq[ch&255]++&0x7fff)<<16)|((x9md_assert_bit&&(ch==X9MD_CH_RPC_TX||ch==X9MD_CH_FS_TX))?0x80000000u:0);
 hdr[3]=rsv;
 uint32_t magic[2]={0xaabbaabbu,pkg},foot[2]={0xccddeeffu,0xccddeeffu};
 uint32_t p=wr;
 x9md_tx_copy(buf,len,p,(uint8_t *)magic,8);p=(p+8)%len;
 x9md_tx_copy(buf,len,p,(uint8_t *)hdr,16);p=(p+16)%len;
 if(n)x9md_tx_copy(buf,len,p,pl,n);
 x9md_tx_copy(buf,len,(wr+ccif-8)%len,(uint8_t *)foot,8);
 __asm__ volatile("dmb sy":::"memory");
 x9md_w32(q->ctl+16,(wr+ccif)%len);
 __asm__ volatile("dmb sy":::"memory");
 /* OPPO MT6993 md_ccif_send / md_ccif_op_send_skb: the packet is committed before
  * notification. BUSY means the previous doorbell is pending: return success without
  * waiting or ringing again. Our single RX owner must remain able to drain other
  * queues (141.txt: repeated ~4 s waits precede ccismcore_ccci.c:1873, before HS2).
  * Source: kernel_device_modules-6.12/drivers/misc/mediatek/eccci/hif/ccci_hif_ccif.c,
  * oppo-source modules/devicetree commit 4d505a4292dab3176a45fec66dc85debc362e24c. */
 uint32_t busy=x9md_rd(x9md_ap,X9MD_BUSY);
 x9md_tx_packets++;
 if(busy&(1u<<qi)){
  x9md_tx_busy++;
  if(x9md_tx_busy<=4)printf("X9 MODEM: TX q%u busy %08x, packet queued without wait (ch%u rd=%x wr=%x)\n",
   qi,busy,ch,rd,(wr+ccif)%len);
 }else{
  x9md_wr(x9md_ap,X9MD_BUSY,1u<<qi);x9md_wr(x9md_ap,X9MD_TCHNUM,qi);x9md_tx_kicks++;
 }
 return 0;
}

static void x9md_tx_report(const char *when){
 struct x9md_q *q=&x9md_q[4];
 printf("X9 MODEM: TX stats %s packets=%u kicks=%u busy_nowait=%u busy=%08x q4_tx=%x/%x/%x q4_rx=%x/%x/%x\n",
  when,x9md_tx_packets,x9md_tx_kicks,x9md_tx_busy,x9md_rd(x9md_ap,X9MD_BUSY),
  x9md_m32(q->ctl+12),x9md_m32(q->ctl+16),q->tx,x9md_m32(q->ctl),x9md_m32(q->ctl+4),q->rx);
}

/* ---- little-endian helpers over unaligned byte buffers ---- */
static uint32_t x9md_ld32(const uint8_t *p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}
static void x9md_st32(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

/* LV list: [para_num][len,data(4-aligned)]... */
#define X9MD_MAXLV 6
struct x9md_lv{uint32_t len;const uint8_t *p;};
static int x9md_lv_parse(const uint8_t *b,uint32_t n,uint32_t *para,struct x9md_lv *lv){
 if(n<4)return -1;
 *para=x9md_ld32(b);
 if(*para>X9MD_MAXLV)return -1;
 uint32_t o=4;
 for(uint32_t i=0;i<*para;i++){
  if(o+4>n)return -1;
  lv[i].len=x9md_ld32(b+o);o+=4;
  if(lv[i].len>n||o+lv[i].len>n)return -1;
  lv[i].p=b+o;o+=(lv[i].len+3)&~3u;
 }
 return 0;
}
struct x9md_rep{uint8_t *b;uint32_t n,cap,para;};
/* reply body starts at para_num */
static void x9md_rep_init(struct x9md_rep *r,uint8_t *b,uint32_t cap){r->b=b;r->cap=cap;r->n=4;r->para=0;}
static void x9md_rep_lv(struct x9md_rep *r,const void *d,uint32_t len){
 uint32_t al=(len+3)&~3u;
 if(r->n+4+al>r->cap)return;
 x9md_st32(r->b+r->n,len);r->n+=4;
 if(d)memcpy(r->b+r->n,d,len);else memset(r->b+r->n,0,len);
 if(al>len)memset(r->b+r->n+len,0,al-len);
 r->n+=al;r->para++;
}
static void x9md_rep_u32(struct x9md_rep *r,uint32_t v){uint8_t t[4];x9md_st32(t,v);x9md_rep_lv(r,t,4);}
static void x9md_rep_done(struct x9md_rep *r){x9md_st32(r->b,r->para);}

/* ---- AMMS DRDI: md1drdi image from the modem partition -> SMEM_USER_MD_DRDI window ---- */
extern int sweet_x9_ufs_read_part(const char *name,uint64_t offset,void *data,size_t length);
extern const void *sweet_x9_ufs_load_part(const char *name,uint64_t *bytes);
#define X9MD_DRDI_ID 41
static uint64_t x9md_drdi_base,x9md_drdi_size;
static int x9md_drdi_state;     /* 0 not tried, 1 found, -1 failed */
static int x9md_drdi_done;      /* ccci_rpcd DAT_00110018: -1 unset, 1 copy committed */
static uint32_t x9md_drdi_sets;
static const char *x9md_drdi_part="modem_a";

static int x9md_ufs_read(const char *part,uint64_t off,void *buf,size_t n){
 int e=ENXIO;
 for(unsigned t=0;t<300&&(e=sweet_x9_ufs_read_part(part,off,buf,n))==ENXIO;t++)x9md_sleep_ms(100);   /* root controller comes up later */
 return e;
}
static int x9md_drdi_ufs(uint64_t off,void *buf,size_t n){return x9md_ufs_read(x9md_drdi_part,off,buf,n);}
/* MTK image chain: 0x200-byte headers {magic 0x58881688, dsize, name[32], ... ext hdr_size @0x28}, next = align16(hdr+hdr_size+dsize) */
static int x9md_drdi_locate(void){
 if(x9md_drdi_state)return x9md_drdi_state>0?0:-1;
 uint8_t h[0x200];uint64_t off=0;
 x9md_drdi_state=-1;
 for(unsigned i=0;i<16;i++){
  int e=x9md_drdi_ufs(off,h,sizeof(h));
  if(e){printf("X9 MODEM: DRDI read %s +%llx failed %d\n",x9md_drdi_part,off,e);return -1;}
  if(x9md_ld32(h)!=0x58881688u){printf("X9 MODEM: DRDI no image header at %llx (%08x)\n",off,x9md_ld32(h));return -1;}
  uint32_t dsize=x9md_ld32(h+4),hs=x9md_ld32(h+0x28);
  if(hs<0x40||hs>0x1000)hs=0x200;
  X9MD_LOG("X9 MODEM: DRDI img '%.32s' hdr %llx dsize %x\n",(const char *)h+8,off,dsize);
  if(!strncmp((const char *)h+8,"md1drdi",32)){
   x9md_drdi_base=off+hs;x9md_drdi_size=dsize;x9md_drdi_state=1;
   printf("X9 MODEM: DRDI md1drdi in %s at +%llx size %llx\n",x9md_drdi_part,x9md_drdi_base,x9md_drdi_size);
   return 0;
  }
  off=(off+hs+dsize+15)&~15ull;
 }
 return -1;
}
/* copy one {src,dst,len} set into the DRDI window */
static int x9md_drdi_copy_set(uint32_t src,uint32_t dst,uint32_t len){
 static uint8_t buf[0x10000];
 struct x9md_reg *r=&x9md_regs[X9MD_DRDI_ID];
 if(!r->in_lk||r->size<0x10000||len>0x10000||(uint64_t)dst+len>0x10000)return -1;
 if((uint64_t)src+len>x9md_drdi_size)return -1;
 int e=x9md_drdi_ufs(x9md_drdi_base+src,buf,len);
 if(e){printf("X9 MODEM: DRDI read src %x len %x failed %d\n",src,len,e);return -1;}
 for(uint32_t i=0;i<len;i++)*(volatile uint8_t *)(x9md_nc+r->ap_off+dst+i)=buf[i];
 __asm__ volatile("dsb sy":::"memory");
 return 0;
}

/* ---- device facts (live.dts, PLJ110) ---- */
static vm_offset_t x9md_gpio;     /* GPIO base 0x1002d000, DIN at +0x200 + (pin/32)*0x10 */
struct x9md_gpio_ent{const char *md_name;uint16_t pin;};
static const struct x9md_gpio_ent x9md_gpios[]={
 {"GPIO_FDD_Band_Support_Detection_1",0x84},{"GPIO_FDD_Band_Support_Detection_2",0x85},
 {"GPIO_FDD_Band_Support_Detection_3",0x86},{"GPIO_FDD_Band_Support_Detection_4",0x87},
 {"GPIO_SIM1_HOT_PLUG",0xe0},{"GPIO_SIM2_HOT_PLUG",0xe1},
 {"GPIO_SIM1_SCLK",0x5a},{"GPIO_SIM1_SRST",0x5b},{"GPIO_SIM1_SIO",0x5c},
 {"GPIO_SIM2_SCLK",0x5d},{"GPIO_SIM2_SRST",0x5e},{"GPIO_SIM2_SIO",0x5f},
};
#define X9MD_ADC_NUM 0x31fu
#define X9MD_ADC_VAL 1u
static uint32_t x9md_gpio_val(uint32_t pin){
 if(!x9md_gpio||pin>=0x100)return 0;
 return (x9md_rd(x9md_gpio,0x200+(pin/32)*0x10)>>(pin%32))&1;
}
static int x9md_gpio_lookup(const uint8_t *name,uint32_t maxlen){
 uint32_t l=0;while(l<maxlen&&name[l])l++;
 if(!l)return -1;
 for(unsigned i=0;i<sizeof(x9md_gpios)/sizeof(x9md_gpios[0]);i++)
  if(!strncmp((const char *)name,x9md_gpios[i].md_name,l))return x9md_gpios[i].pin;
 return -1;
}

/* IPC_RPC_GET_GPIO_ADC_OP: v1 (157 B, 3 pins) / v2 (425 B, 10 pins) */
static void x9md_rpc_gpio_adc(struct x9md_rep *r,const struct x9md_lv *in){
 const uint8_t *p=in->p;
 uint32_t namesz=34,adcn_off,cnt_off,pins,req,valid,names_off,nums_off;
 if(in->len==425){pins=10;req=p[0]|(p[1]<<8);valid=p[2]|(p[3]<<8);names_off=4;nums_off=4+10*34;adcn_off=nums_off+40+33;}
 else if(in->len==157){pins=3;req=p[0];valid=p[1];names_off=2;nums_off=2+3*34;adcn_off=nums_off+12+33;}
 else{x9md_rep_u32(r,(uint32_t)-2);x9md_rep_u32(r,(uint32_t)-2);return;}
 cnt_off=adcn_off+4;
 uint8_t out[10*4*2+8];uint32_t n=pins*8+8;
 memset(out,0x0f,sizeof(out));
 for(uint32_t i=0;i<pins;i++){
  if(!(valid&(1u<<i)))continue;
  int pin=-1;
  if(req&1){pin=x9md_gpio_lookup(p+names_off+i*namesz,namesz);if(pin>=0)x9md_st32(out+i*4,pin);}
  if(req&2){
   int use=(req&1)?pin:(int)x9md_ld32(p+nums_off+i*4);
   if(use>=0)x9md_st32(out+pins*4+i*4,x9md_gpio_val(use));
  }
 }
 uint32_t count=x9md_ld32(p+cnt_off);
 if(req&0x10)x9md_st32(out+pins*8,X9MD_ADC_NUM);
 if(req&0x20)x9md_st32(out+pins*8+4,count*X9MD_ADC_VAL);
 X9MD_LOG("X9 MODEM: RPC GPIO_ADC len %u req=%x valid=%x -> pins %08x %08x %08x %08x val %08x %08x %08x %08x\n",in->len,req,valid,
  x9md_ld32(out),x9md_ld32(out+4),x9md_ld32(out+8),x9md_ld32(out+12),
  x9md_ld32(out+pins*4),x9md_ld32(out+pins*4+4),x9md_ld32(out+pins*4+8),x9md_ld32(out+pins*4+12));
 x9md_rep_u32(r,0);
 x9md_rep_lv(r,out,n);
}

/* '-' and '_' are interchangeable (DT node names vs MD names) */
static int x9md_norm_eq(const uint8_t *s,uint32_t len,const char *want){
 uint32_t i=0;
 for(;i<len&&s[i]&&want[i];i++){char a=s[i],b=want[i];if(a=='-')a='_';if(b=='-')b='_';if(a!=b)return 0;}
 return i&&(i==len||!s[i])&&!want[i];
}

static void x9md_rpc_dispatch(uint32_t op,uint32_t para,const struct x9md_lv *lv,struct x9md_rep *r){
 switch(op){
 case 0x4009:   /* GET_GPIO_ADC */
  if(para!=1){x9md_rep_u32(r,(uint32_t)-2);x9md_rep_u32(r,(uint32_t)-2);}
  else x9md_rpc_gpio_adc(r,&lv[0]);
  break;
 case 0x4001: case 0x4002: case 0x4003: { /* TDD_EINT_NUM / GPIO_NUM / ADC_NUM: [name][..] -> [num][num] */
  int num=-5;
  if(para>=2&&lv[0].len>=1){
   if(op==0x4002){int pin=x9md_gpio_lookup(lv[0].p,lv[0].len);if(pin>=0)num=pin;}
   else if(op==0x4003)num=X9MD_ADC_NUM;
   X9MD_LOG("X9 MODEM: RPC %04x '%.30s' -> %d\n",op,(const char *)lv[0].p,num);
  }
  x9md_rep_u32(r,(uint32_t)num);x9md_rep_u32(r,(uint32_t)num);
  break;}
 case 0x4005: { /* GET_EINT_ATTR: [name][?][type]; reply [ret][value] */
  int ret=-14,val=0;
  if(para>=3&&para<=6&&lv[0].len>=1&&lv[2].len>=4){
   unsigned sim=0;uint32_t type=x9md_ld32(lv[2].p);
   if(x9md_norm_eq(lv[0].p,lv[0].len,"MD1_SIM1_HOT_PLUG_EINT"))sim=1;
   else if(x9md_norm_eq(lv[0].p,lv[0].len,"MD1_SIM2_HOT_PLUG_EINT"))sim=2;
   if(sim&&type<7){
    ret=0;
    switch(type){
    case 0:val=sim-1;break;   /* interrupts[0] */
    case 1:val=10;break;      /* debounce 0x2710 us / 1000 */
    case 6:val=sim;break;     /* src-pin */
    default:val=0;break;      /* POLA/SENS from interrupts[1]=8 (LEVEL_LOW) -> 0; socket 0; dedicated 0 */
    }
   }
   X9MD_LOG("X9 MODEM: RPC EINT_ATTR '%.24s' type %u -> ret %d val %d\n",(const char *)lv[0].p,type,ret,val);
  }
  x9md_rep_u32(r,(uint32_t)ret);x9md_rep_u32(r,(uint32_t)(ret?ret:val));
  break;}
 case 0x4006: case 0x4007: {   /* GPIO_VAL / ADC_VAL */
  uint32_t v=0;
  if(para==1&&lv[0].len>=4)v=(op==0x4006)?x9md_gpio_val(x9md_ld32(lv[0].p)):X9MD_ADC_VAL;
  x9md_rep_u32(r,v);x9md_rep_u32(r,v);
  break;}
 case 0x400e: { /* DTSI_QUERY: ccci_rpc_md_dtsi_input {u8 req,u8 index,char strName[64]} */
  uint8_t out[68];memset(out,0x0f,sizeof(out));
  if(para==1&&lv[0].len>=66&&lv[0].p[0]==1){
   const char *nm=(const char *)lv[0].p+2;
   if(!strncmp(nm,"mediatek,md_product_name_model_id",64))x9md_st32(out,0x5e17);
   else if(!strncmp(nm,"mediatek,md_drdi_rf_set_idx",64))x9md_st32(out,0x0f0f0f0f);
   X9MD_LOG("X9 MODEM: RPC DTSI '%.40s' -> %08x\n",nm,x9md_ld32(out));
  }
  x9md_rep_u32(r,0);x9md_rep_lv(r,out,68);
  break;}
 case 0x400f:   /* QUERY_AP_SYS_PROPERTY (ccci_rpcd): [strlen][value+NUL] */
  { const char *val="";const char *k=para>=1?(const char *)lv[0].p:"";
    /* real values from the Android getprop capture (2026-11-09): svn/svn.c are empty */
    if(!strncmp(k,"ro.build.version.ota",lv[0].len))val="PLJ110_11.C.61_1610_202610051600";
    if(para>=1)X9MD_LOG("X9 MODEM: RPC SYSPROP '%.60s' (len %u) -> '%s'\n",k,lv[0].len,val);
    x9md_rep_u32(r,(uint32_t)strlen(val));x9md_rep_lv(r,val,strlen(val)+1); }
  break;
 case 0x4010:   /* SAR_TABLE_IDX_QUERY */
  x9md_rep_u32(r,0);x9md_rep_u32(r,0);break;
 case 0x4014: { /* AMMS_DRDI_CONTROL (ccci_rpc_amms_drdi_ctl, ccci_rpcd): in 0xbc B, out [ret][8 B] */
  uint8_t o[8]={0};int ret=-1;
  if(para==1&&lv[0].len==0xbc){
   const uint8_t *q=lv[0].p;
   if(q[0]==1)for(unsigned i=0;i<0xbc;i+=32)
    X9MD_LOG("X9 MODEM: AMMS +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n",i,x9md_ld32(q+i),x9md_ld32(q+i+4),x9md_ld32(q+i+8),x9md_ld32(q+i+12),
     x9md_ld32(q+i+16),x9md_ld32(q+i+20),i+28<0xbc?x9md_ld32(q+i+24):0,i+32<=0xbc?x9md_ld32(q+i+28):0);
   o[1]=q[1];
   if(q[0]==1){            /* init: version 3, num sets in q[5], {start,size} pairs from +8 */
    o[4]=3;
    if(q[4]==3&&q[5]<0x10&&x9md_drdi_locate()==0){
     int bad=0;
     for(unsigned i=0;i<q[5];i++)if((uint64_t)x9md_ld32(q+8+i*8)+x9md_ld32(q+12+i*8)>x9md_drdi_size)bad=1;
     if(bad){o[0]=0xfe;o[6]=0xff;}
     else{ret=0;o[0]=0;o[5]=0;o[6]=0;x9md_drdi_sets=q[5];x9md_drdi_done=-1;}
    }else{o[0]=0xff;o[6]=0xff;}
   }else if(q[0]==2){      /* copy sets {src,dst,len} from +8, stride 12 (ccci_rpc_amms_drdi_ctl) */
    o[4]=(q[4]==1)?0:0xff;
    if(x9md_drdi_done==1){ret=0;o[0]=0;}
    else{
     int bad=0;
     for(uint32_t i=0;i<x9md_drdi_sets&&i<10;i++){
      uint32_t src=x9md_ld32(q+8+i*12),dst=x9md_ld32(q+12+i*12),len=x9md_ld32(q+16+i*12);
      if(len&&x9md_drdi_copy_set(src,dst,len)){bad=1;X9MD_LOG("X9 MODEM: AMMS copy set %u src %x dst %x len %x FAILED\n",i,src,dst,len);break;}
      if(len&&q[1]<6)X9MD_LOG("X9 MODEM: AMMS copy set %u src %x dst %x len %x ok\n",i,src,dst,len);
     }
     if(bad){x9md_drdi_done=-1;o[0]=0xff;}
     else{ret=0;o[0]=0;x9md_drdi_done=(q[4]==1);}
    }
   }else o[0]=0xff;
   if(q[0]!=2||q[1]<4)X9MD_LOG("X9 MODEM: AMMS cmd %u seq %u ver %u num %u -> ret %d\n",q[0],q[1],q[4],q[5],ret);
  }
  x9md_rep_u32(r,(uint32_t)ret);x9md_rep_lv(r,o,8);break;}
 case 0x4015:   /* SAVE_MD_CAPID */
  x9md_rep_u32(r,0);x9md_rep_u32(r,0);break;
 default:       /* port_rpc.c default branch: FS_NO_OP */
  x9md_rep_u32(r,(uint32_t)-1);break;
 }
}

static uint8_t x9md_rpc_out[0x900];
static void x9md_rpc_handle(const uint8_t *pkt,uint32_t len){
 if(len<24)return;
 uint32_t d0=x9md_ld32(pkt),rsv=x9md_ld32(pkt+12),op=x9md_ld32(pkt+16);
 struct x9md_lv lv[X9MD_MAXLV];uint32_t para=0;
 struct x9md_rep r;
 x9md_rep_init(&r,x9md_rpc_out+4,sizeof(x9md_rpc_out)-4);
 if(x9md_lv_parse(pkt+20,len-20,&para,lv)<0){
  X9MD_LOG("X9 MODEM: RPC op %04x bad LV layout (len %u)\n",op,len);
  x9md_rep_u32(&r,(uint32_t)-2);
 }else x9md_rpc_dispatch(op,para,lv,&r);
 x9md_rep_done(&r);
 x9md_st32(x9md_rpc_out,op|0xffff0000u);
 int rc=x9md_tx(1,X9MD_CH_RPC_TX,d0,16+4+r.n,rsv,x9md_rpc_out,4+r.n);
 X9MD_LOG("X9 MODEM: RPC op %04x para %u -> reply %u B para %u tx=%d\n",op,para,4+r.n,r.para,rc);
}

#include "x9_ext4ro.inc"
#include "../../../generated/x9_mdota.inc"     /* generated: vendor /vendor/etc/mdota packed by x9/tools/md_ota_pack.py (T: drive) */

/* T: = /vendor/etc/mdota (MCF OTA configs): read-only, embedded in the kernel image. Pack: 'MDOT', u32 n, n x (u16 len, path, u32 size, u32 off). */
static int x9fs_emb_find(const char *sub,uint32_t *off,uint32_t *size,int *isdir){
 const uint8_t *p=x9_mdota_pack;size_t total=(size_t)(x9_mdota_pack_end-x9_mdota_pack);
 if(total<8||memcmp(p,"MDOT",4))return -1;
 uint32_t n=x9md_ld32(p+4);size_t o=8,sl=strlen(sub);
 while(sl&&sub[sl-1]=='/')sl--;
 int dir=0;
 if(!sl)dir=1;
 for(uint32_t i=0;i<n&&o+2<=total;i++){
  uint32_t pl=p[o]|(p[o+1]<<8);o+=2;
  if(o+pl+8>total)return -1;
  const char *name=(const char *)p+o;
  if(pl==sl&&!memcmp(name,sub,sl)){*off=x9md_ld32(p+o+pl+4);*size=x9md_ld32(p+o+pl);*isdir=0;return 0;}
  if(pl>sl&&name[sl]=='/'&&!memcmp(name,sub,sl))dir=1;
  o+=pl+8;
 }
 if(dir){*off=0;*size=0;*isdir=1;return 0;}
 return -1;
}

/* ---- FSD: RAM overlay over the Android NV partitions (read-only ext4) ---- */
#define X9FS_NODES 1024
#define X9FS_POOL (128u<<20)   /* allocated at preload; 138.txt: the MD rewrites ~40 x 230 KB FD* files when MCF applies a config */
#define X9FS_HANDLES 129
struct x9fs_node{char path[160];uint8_t used,dir,deleted,backed;uint32_t off,size,cap,ext_ino;};
static struct x9fs_node x9fs_n[X9FS_NODES];
static uint8_t *x9fs_pool;
extern void *sweet_x9_big_alloc(size_t bytes);
static uint32_t x9fs_pool_used;
struct x9fs_h{uint8_t used;int node;uint32_t pos;};
static struct x9fs_h x9fs_h[X9FS_HANDLES];

/* Unbudgeted, rare diagnostics: distinguish stale small-pool images, pool exhaustion,
 * node exhaustion and handle exhaustion without changing the file service semantics. */
static void x9fs_usage(const char *why){
 unsigned nodes=0,handles=0;uint64_t live=0,capacity=0;
 for(unsigned i=0;i<X9FS_NODES;i++)if(x9fs_n[i].used){
  nodes++;
  if(!x9fs_n[i].deleted&&!x9fs_n[i].backed){live+=x9fs_n[i].size;capacity+=x9fs_n[i].cap;}
 }
 for(unsigned i=1;i<X9FS_HANDLES;i++)if(x9fs_h[i].used)handles++;
 printf("X9 MODEM: FS usage %s pool=%u/%u live=%llu capacity=%llu nodes=%u/%u handles=%u/%u\n",
  why,x9fs_pool_used,X9FS_POOL,live,capacity,nodes,X9FS_NODES,handles,X9FS_HANDLES-1);
}

#define FS_ENOENT -9
#define FS_EBADH -10
#define FS_EMFILE -5
#define FS_EACCES -16
#define FS_EOTHER -18
#define FS_EPATH -19

/* UCS-2LE name -> '/'-separated ASCII, trailing '/' stripped */
static int x9fs_path(const struct x9md_lv *lv,char *out,unsigned cap){
 unsigned n=0;
 out[0]=0;
 for(uint32_t i=0;i+1<lv->len;i+=2){
  uint32_t c=lv->p[i]|(lv->p[i+1]<<8);
  if(!c)break;
  if(c=='\\')c='/';
  if(c>126||c<32)c='?';
  if(n+1>=cap)return FS_EPATH;
  out[n++]=(char)c;
 }
 while(n>1&&out[n-1]=='/')n--;
 out[n]=0;
 return n?0:FS_EPATH;
}
/* drive letter -> Android partition (ccci_mdinit): Z nvdata/md, V nvdata/md_cmn, X protect_f(protect1)/md,
 * Y protect_s(protect2)/md, S nvcfg, O md_sec */
struct x9fs_img{const char *part;const uint8_t *mem;uint64_t bytes;};
struct x9fs_drive{char letter;const char *part;const char *prefix;struct x9e4 m;int state;struct x9fs_img img;};
static struct x9fs_drive x9fs_drives[]={
 {'Z',"nvdata","/md"},{'V',"nvdata","/md_cmn"},{'X',"protect1","/md"},{'Y',"protect2","/md"},{'S',"nvcfg",""},{'O',"md_sec",""},
};
static int x9fs_ext_rd(void *ctx,uint64_t off,void *buf,size_t n){
 struct x9fs_img *im=ctx;
 if(!im->mem){   /* wait (bounded) for the controller, then pull the whole partition into RAM */
  for(unsigned t=0;t<300&&!im->mem;t++){
   im->mem=sweet_x9_ufs_load_part(im->part,&im->bytes);
   if(!im->mem)x9md_sleep_ms(100);
  }
 }
 if(im->mem){
  if(off>im->bytes||n>im->bytes-off)return -1;
  memcpy(buf,im->mem+off,n);return 0;
 }
 return x9md_ufs_read(im->part,off,buf,n);
}
static struct x9fs_drive *x9fs_drive_for(const char *p,char *sub,size_t cap){
 if(!p[0]||p[1]!=':')return NULL;
 char l=p[0]>='a'&&p[0]<='z'?p[0]-32:p[0];
 for(unsigned i=0;i<sizeof(x9fs_drives)/sizeof(x9fs_drives[0]);i++){
  struct x9fs_drive *d=&x9fs_drives[i];
  if(d->letter!=l)continue;
  if(!d->state){
   d->img.part=d->part;d->m.rd=x9fs_ext_rd;d->m.ctx=&d->img;
   int e=x9e4_mount(&d->m);
   d->state=e?-1:1;
   printf("X9 MODEM: FS drive %c: %s mount %d\n",d->letter,d->part,e);
  }
  if(d->state<0)return NULL;
  if(strlcpy(sub,d->prefix,cap)>=cap||strlcat(sub,p+2,cap)>=cap)return NULL;
  if(!sub[0])strlcpy(sub,"/",cap);
  return d;
 }
 return NULL;
}
/* mount every drive now (pulls each partition into RAM) so the first MD file request is fast */
static void x9fs_preload(void){
 char sub[200];
 if(!x9fs_pool){x9fs_pool=sweet_x9_big_alloc(X9FS_POOL);printf("X9 MODEM: FS RAM pool %u MiB at %p\n",X9FS_POOL>>20,(void *)x9fs_pool);}
 uint64_t t0=mach_absolute_time(),ns;
 for(unsigned i=0;i<sizeof(x9fs_drives)/sizeof(x9fs_drives[0]);i++){
  char root[3]={x9fs_drives[i].letter,':',0};
  x9fs_drive_for(root,sub,sizeof(sub));
 }
 absolutetime_to_nanoseconds(mach_absolute_time()-t0,&ns);
 printf("X9 MODEM: FS preload done in %u ms\n",(unsigned)(ns/1000000));
 x9fs_usage("preload");
}
static int x9fs_ext_stat(const char *p,struct x9e4_inode *in,uint32_t *ino,struct x9fs_drive **dp){
 if((p[0]=='T'||p[0]=='t')&&p[1]==':'){   /* embedded vendor mdota */
  uint32_t off,size;int dir;
  const char *sub=p+2;while(*sub=='/')sub++;
  if(x9fs_emb_find(sub,&off,&size,&dir))return -1;
  memset(in,0,sizeof(*in));in->mode=dir?0x41ed:0x81a4;in->size=size;
  if(ino)*ino=off;                       /* offset into the pack (0 for directories) */
  if(dp)*dp=NULL;
  return 0;
 }
 char sub[200];struct x9fs_drive *d=x9fs_drive_for(p,sub,sizeof(sub));
 if(!d)return -1;
 if(x9e4_lookup(&d->m,sub,in,ino))return -1;
 if(dp)*dp=d;
 return 0;
}
static int x9fs_tomb(const char *p){
 for(int i=0;i<X9FS_NODES;i++)if(x9fs_n[i].used&&x9fs_n[i].deleted&&!strcmp(x9fs_n[i].path,p))return i;
 return -1;
}
/* live RAM node (tombstones are not live) */
static int x9fs_find(const char *p){
 for(int i=0;i<X9FS_NODES;i++)if(x9fs_n[i].used&&!x9fs_n[i].deleted&&!strcmp(x9fs_n[i].path,p))return i;
 return -1;
}
static int x9fs_isdir(const char *p){
 size_t l=strlen(p);
 if(l&&l<=2&&p[l-1]==':')return 1;
 int i=x9fs_find(p);
 if(i>=0)return x9fs_n[i].dir;
 for(int j=0;j<X9FS_NODES;j++)
  if(x9fs_n[j].used&&!x9fs_n[j].deleted&&!strncmp(x9fs_n[j].path,p,l)&&x9fs_n[j].path[l]=='/')return 1;
 if(x9fs_tomb(p)>=0)return 0;
 struct x9e4_inode in;
 if(!x9fs_ext_stat(p,&in,NULL,NULL))return (in.mode&0xf000)==0x4000;
 return 0;
}
static int x9fs_new(const char *p,int dir){
 if(strlen(p)>=sizeof(x9fs_n[0].path))return -1;
 for(int i=0;i<X9FS_NODES;i++)if(!x9fs_n[i].used){
  memset(&x9fs_n[i],0,sizeof(x9fs_n[i]));
  strlcpy(x9fs_n[i].path,p,sizeof(x9fs_n[i].path));x9fs_n[i].used=1;x9fs_n[i].dir=dir;return i;
 }
 return -1;
}
/* RAM node for an existing ext4 file (data stays on the partition until written) */
static int x9fs_adopt(const char *p){
 if(x9fs_tomb(p)>=0)return -1;
 struct x9e4_inode in;uint32_t ino;
 if(x9fs_ext_stat(p,&in,&ino,NULL)||(in.mode&0xf000)!=0x8000)return -1;
 int i=x9fs_new(p,0);if(i<0)return -2;
 x9fs_n[i].backed=1;x9fs_n[i].ext_ino=ino;x9fs_n[i].size=(uint32_t)in.size;
 return i;
}
static int x9fs_node_read(struct x9fs_node *nd,uint32_t pos,uint8_t *dst,uint32_t n){
 if(!nd->backed){memcpy(dst,x9fs_pool+nd->off+pos,n);return 0;}
 if((nd->path[0]=='T'||nd->path[0]=='t')&&nd->path[1]==':'){
  if((uint64_t)nd->ext_ino+pos+n>(uint64_t)(x9_mdota_pack_end-x9_mdota_pack))return -1;
  memcpy(dst,x9_mdota_pack+nd->ext_ino+pos,n);return 0;
 }
 char sub[200];struct x9fs_drive *d=x9fs_drive_for(nd->path,sub,sizeof(sub));
 struct x9e4_inode in;
 if(!d||x9fs_ext_stat(nd->path,&in,NULL,NULL))return -1;
 return x9e4_read(&d->m,&in,pos,dst,n)==(int64_t)n?0:-1;
}
static int x9fs_grow(struct x9fs_node *n,uint32_t want){
 if(want<=n->cap)return 0;
 uint32_t nc=n->cap*2;if(nc<want)nc=want;if(nc<512)nc=512;nc=(nc+15)&~15u;
 if(!x9fs_pool||x9fs_pool_used+nc>X9FS_POOL){
  printf("X9 MODEM: FS grow refused '%s' want=%u size=%u cap=%u alloc=%u pool=%u/%u present=%d\n",
   n->path,want,n->size,n->cap,nc,x9fs_pool_used,X9FS_POOL,x9fs_pool!=NULL);
  return -1;
 }
 memcpy(x9fs_pool+x9fs_pool_used,x9fs_pool+n->off,n->size);
 n->off=x9fs_pool_used;n->cap=nc;x9fs_pool_used+=nc;
 return 0;
}
static int x9fs_materialize(struct x9fs_node *nd){
 if(!nd->backed)return 0;
 uint32_t sz=nd->size;
 nd->backed=0;                      /* grow() copies from the pool */
 uint32_t old_off=nd->off,old_cap=nd->cap;(void)old_off;(void)old_cap;
 nd->cap=0;nd->size=0;
 if(x9fs_grow(nd,sz?sz:1)){nd->backed=1;nd->size=sz;return -1;}
 nd->size=sz;
 static uint8_t tmp[0x4000];
 for(uint32_t o=0;o<sz;){
  uint32_t c=sz-o;if(c>sizeof(tmp))c=sizeof(tmp);
  nd->backed=1;
  int e=x9fs_node_read(nd,o,tmp,c);
  nd->backed=0;
  if(e){printf("X9 MODEM: FS materialize '%s' read failed at %u\n",nd->path,o);return -1;}
  memcpy(x9fs_pool+nd->off+o,tmp,c);o+=c;
 }
 return 0;
}
static int x9fs_hnew(int node){
 for(int i=1;i<X9FS_HANDLES;i++)if(!x9fs_h[i].used){x9fs_h[i].used=1;x9fs_h[i].node=node;x9fs_h[i].pos=0;return i;}
 return FS_EMFILE;
}
static struct x9fs_h *x9fs_hget(uint32_t h){return(h>0&&h<X9FS_HANDLES&&x9fs_h[h].used)?&x9fs_h[h]:NULL;}

static int x9fs_open(const char *p,uint32_t flag){
 int i=x9fs_find(p);
 if(i<0){
  i=x9fs_adopt(p);
  if(i==-2)return FS_EMFILE;
  if(i<0){
   if(x9fs_isdir(p)){
    /* open(dir, O_RDONLY) succeeds on Android; the MD opens NVRAM dirs before deciding whether to mkdir
     * (mkdir on an existing dir returns -18 and makes dev_fs.c:331 assert, 125.txt) */
    if(flag&0x30000)return FS_EACCES;
    i=x9fs_new(p,1);if(i<0)return FS_EMFILE;
   }else{
    if(!(flag&0x30000))return FS_ENOENT;
    i=x9fs_new(p,0);if(i<0)return FS_EMFILE;
   }
  }
 }
 if(x9fs_n[i].dir&&(flag&0x30000))return FS_EACCES;
 if(flag&0x20000){x9fs_n[i].size=0;x9fs_n[i].backed=0;}
 return x9fs_hnew(i);
}

static uint8_t x9fs_in[5][0x40d0];
static uint32_t x9fs_in_len[5];
static uint8_t x9fs_out[0x4100];
static unsigned x9fs_nops;

/* Reply fragments: header + op_id + body slice (<= 0xd80 B each), bit31 = more */
static void x9fs_send(uint32_t d0,uint32_t rsv,uint32_t op,const uint8_t *body,uint32_t n){
 uint32_t off=0;
 static uint8_t frag[4+0xd80];
 d0&=~0xc0000000u;
 do{
  uint32_t c=n-off;if(c>0xd80)c=0xd80;
  x9md_st32(frag,op);memcpy(frag+4,body+off,c);
  int more=(off+c<n);
  if(x9md_tx(4,X9MD_CH_FS_TX,more?(d0|0x80000000u):d0,16+4+c,rsv,frag,4+c)){printf("X9 MODEM: FS tx failed at %u/%u\n",off,n);return;}
  off+=c;
 }while(off<n);
}

#define LVU(i) ((para>(i)&&lv[i].len>=4)?x9md_ld32(lv[i].p):0)
static void x9fs_handle(uint32_t d0,uint32_t rsv,const uint8_t *msg,uint32_t mlen){
 /* msg = op_id, para_num, LVs */
 if(mlen<8)return;
 uint32_t op=x9md_ld32(msg);
 struct x9md_lv lv[X9MD_MAXLV];uint32_t para=0;
 struct x9md_rep r;
 char path[160],path2[160];
 x9md_rep_init(&r,x9fs_out+4,sizeof(x9fs_out)-4);
 path[0]=path2[0]=0;
 int ret=FS_EOTHER;uint32_t u1=0;const char *name="?";
 if(x9md_lv_parse(msg+4,mlen-4,&para,lv)<0){name="BADLV";ret=FS_EPATH;x9md_rep_u32(&r,(uint32_t)ret);goto done;}
 switch(op){
 case 0x1001: case 0x1011:
  name=op==0x1001?"Open":"OpenHint";
  if(para<2||x9fs_path(&lv[0],path,sizeof(path)))ret=FS_EPATH;
  else ret=x9fs_open(path,LVU(1));
  X9MD_LOG("X9 MODEM: FS %s '%s' flag=%x -> %d\n",name,path,LVU(1),ret);
  x9md_rep_u32(&r,(uint32_t)ret);
  if(op==0x1011)x9md_rep_lv(&r,para>2?lv[2].p:NULL,8);
  break;
 case 0x1002: { name="Seek";
  struct x9fs_h *h=x9fs_hget(LVU(0));
  if(!h)ret=FS_EBADH;
  else{int32_t off=(int32_t)LVU(1);uint32_t wh=LVU(2);
   int64_t np=(wh==0?0:wh==1?(int64_t)h->pos:(int64_t)x9fs_n[h->node].size)+off;
   if(np<0)ret=FS_EOTHER;else{h->pos=(uint32_t)np;ret=(int)np;}}
  x9md_rep_u32(&r,(uint32_t)ret);break;}
 case 0x1003: { name="Read";
  struct x9fs_h *h=x9fs_hget(LVU(0));uint32_t cnt=LVU(1),got=0;
  if(cnt>0x4000)cnt=0x4000;
  if(!h){ret=FS_EBADH;x9md_rep_u32(&r,(uint32_t)ret);x9md_rep_u32(&r,0);x9md_rep_lv(&r,NULL,0);}
  else{struct x9fs_node *nd=&x9fs_n[h->node];ret=0;
   static uint8_t rbuf[0x4000];
   if(h->pos<nd->size){got=nd->size-h->pos;if(got>cnt)got=cnt;}
   if(got&&x9fs_node_read(nd,h->pos,rbuf,got)){got=0;ret=FS_EOTHER;}
   x9md_rep_u32(&r,(uint32_t)ret);x9md_rep_u32(&r,got);x9md_rep_lv(&r,rbuf,got);
   h->pos+=got;u1=got;}
  break;}
 case 0x1004: { name="Write";
  struct x9fs_h *h=x9fs_hget(LVU(0));uint32_t w=0;
  if(!h)ret=FS_EBADH;
  else if(para<3)ret=FS_EPATH;
  else{struct x9fs_node *nd=&x9fs_n[h->node];w=LVU(2);if(w>lv[1].len)w=lv[1].len;
   if(x9fs_materialize(nd)||x9fs_grow(nd,h->pos+w))ret=FS_EMFILE;
   else{
    if(h->pos>nd->size)memset(x9fs_pool+nd->off+nd->size,0,h->pos-nd->size);
    memcpy(x9fs_pool+nd->off+h->pos,lv[1].p,w);h->pos+=w;if(h->pos>nd->size)nd->size=h->pos;ret=0;u1=w;}}
  x9md_rep_u32(&r,(uint32_t)ret);x9md_rep_u32(&r,ret?0:w);break;}
 case 0x1005: { name="Close";
  struct x9fs_h *h=x9fs_hget(LVU(0));
  if(h){h->used=0;ret=0;}else ret=FS_EBADH;
  x9md_rep_u32(&r,(uint32_t)ret);break;}
 case 0x1006: name="CloseAll";memset(x9fs_h,0,sizeof(x9fs_h));ret=0;x9md_rep_u32(&r,0);break;
 case 0x1007: name="CreateDir";
  if(para<1||x9fs_path(&lv[0],path,sizeof(path)))ret=FS_EPATH;
  else if(x9fs_isdir(path))ret=FS_EOTHER;
  else ret=x9fs_new(path,1)>=0?0:FS_EMFILE;
  X9MD_LOG("X9 MODEM: FS CreateDir '%s' -> %d\n",path,ret);
  x9md_rep_u32(&r,(uint32_t)ret);break;
 case 0x1008: name="RemoveDir";
  if(para<1||x9fs_path(&lv[0],path,sizeof(path)))ret=FS_EPATH;
  else{int i=x9fs_find(path);
   if(i>=0&&x9fs_n[i].dir){size_t l=strlen(path);ret=0;
    for(int j=0;j<X9FS_NODES;j++)if(x9fs_n[j].used&&!strncmp(x9fs_n[j].path,path,l)&&x9fs_n[j].path[l]=='/')ret=FS_EOTHER;
    if(!ret)x9fs_n[i].used=0;}
   else ret=FS_ENOENT;}
  x9md_rep_u32(&r,(uint32_t)ret);break;
 case 0x1009: { name="GetFileSize";
  struct x9fs_h *h=x9fs_hget(LVU(0));
  if(h){ret=0;u1=x9fs_n[h->node].size;}else ret=FS_EBADH;
  x9md_rep_u32(&r,(uint32_t)ret);x9md_rep_u32(&r,u1);break;}
 case 0x100f: case 0x1018: name="Delete";
  if(para<1||x9fs_path(&lv[0],path,sizeof(path)))ret=FS_EPATH;
  else{
   int i=x9fs_find(path);struct x9e4_inode in;
   int ext=x9fs_tomb(path)<0&&!x9fs_ext_stat(path,&in,NULL,NULL)&&(in.mode&0xf000)==0x8000;
   if(i>=0&&x9fs_n[i].dir)ret=FS_ENOENT;
   else if(i>=0||ext){
    if(i>=0)x9fs_n[i].used=0;
    if(ext){int t=x9fs_new(path,0);if(t>=0)x9fs_n[t].deleted=1;}
    ret=0;
   }else ret=FS_ENOENT;}
  X9MD_LOG("X9 MODEM: FS Delete '%s' -> %d\n",path,ret);
  x9md_rep_u32(&r,(uint32_t)ret);break;
 case 0x100c: name="Move";
  /* FS_CCCI_Move(src,dst,flag): flag bit0 = copy to dst, bit1 = remove src afterwards; neither -> -2.
   * The MD backs up X: -> Y: with copy semantics and re-reads X: afterwards (127.txt: a plain rename
   * made X:/MC01_007 vanish -> -9 -> lid_error_handle.c:276 0x1001). */
  { uint32_t flag=LVU(2);
  if(para<3||x9fs_path(&lv[0],path,sizeof(path))||x9fs_path(&lv[1],path2,sizeof(path2)))ret=FS_EPATH;
  else if(!(flag&3)||(flag&0xfffefffcu))ret=-2;
  else{
   int i=x9fs_find(path);
   if(i<0)i=x9fs_adopt(path);
   if(i<0||x9fs_n[i].dir)ret=FS_ENOENT;
   else if(strlen(path2)>=sizeof(x9fs_n[0].path))ret=FS_EPATH;
   else{
    ret=0;
    if(flag&1){   /* copy src -> dst (RAM copy, never written back to the partition) */
     int k=x9fs_find(path2);
     if(k<0)k=x9fs_new(path2,0);
     if(k<0)ret=FS_EMFILE;
     else if(k==i){ret=0;}
     else if(x9fs_n[k].dir)ret=FS_EACCES;
     else{
      struct x9fs_node *d=&x9fs_n[k],*sn=&x9fs_n[i];
      uint32_t sz=sn->size;
      d->backed=0;d->size=0;
      if(x9fs_grow(d,sz?sz:1))ret=FS_EMFILE;
      else{
       static uint8_t mtmp[0x4000];
       for(uint32_t o=0;o<sz&&!ret;){
        uint32_t c=sz-o;if(c>sizeof(mtmp))c=sizeof(mtmp);
        if(x9fs_node_read(sn,o,mtmp,c))ret=FS_EOTHER;
        else{memcpy(x9fs_pool+d->off+o,mtmp,c);o+=c;}
       }
       if(!ret)d->size=sz;
      }
      int t=x9fs_tomb(path2);if(t>=0)x9fs_n[t].used=0;
     }
    }
    if(!ret&&(flag&2)){   /* remove source */
     struct x9e4_inode in;
     int ext=x9fs_tomb(path)<0&&!x9fs_ext_stat(path,&in,NULL,NULL);
     if(flag&1){x9fs_n[i].used=0;if(ext){int t=x9fs_new(path,0);if(t>=0)x9fs_n[t].deleted=1;}}
     else{   /* plain move == rename */
      if(x9fs_materialize(&x9fs_n[i]))ret=FS_EMFILE;
      else{
       int k=x9fs_find(path2);if(k>=0&&k!=i)x9fs_n[k].used=0;
       strlcpy(x9fs_n[i].path,path2,sizeof(x9fs_n[i].path));
       int t=x9fs_tomb(path2);if(t>=0)x9fs_n[t].used=0;
       if(ext){int t2=x9fs_new(path,0);if(t2>=0)x9fs_n[t2].deleted=1;}
      }
     }
    }
   }
  }
  X9MD_LOG("X9 MODEM: FS Move '%s' -> '%s' flag=%x = %d\n",path,path2,flag,ret);
  x9md_rep_u32(&r,(uint32_t)ret);}
  break;
 case 0x100b: name="Rename";
  if(para<2||x9fs_path(&lv[0],path,sizeof(path))||x9fs_path(&lv[1],path2,sizeof(path2)))ret=FS_EPATH;
  else{int i=x9fs_find(path);
   if(i<0){i=x9fs_adopt(path);if(i>=0&&x9fs_materialize(&x9fs_n[i])){x9fs_n[i].used=0;i=-1;}}
   if(i<0)ret=FS_ENOENT;
   else if(strlen(path2)>=sizeof(x9fs_n[0].path))ret=FS_EPATH;
   else{
    struct x9e4_inode in;
    int ext_old=x9fs_tomb(path)<0&&!x9fs_ext_stat(path,&in,NULL,NULL);
    int k=x9fs_find(path2);if(k>=0&&k!=i)x9fs_n[k].used=0;
    strlcpy(x9fs_n[i].path,path2,sizeof(x9fs_n[i].path));
    int t=x9fs_tomb(path2);if(t>=0)x9fs_n[t].used=0;
    if(ext_old){int t2=x9fs_new(path,0);if(t2>=0)x9fs_n[t2].deleted=1;}
    ret=0;}}
  X9MD_LOG("X9 MODEM: FS Rename '%s' -> '%s' = %d\n",path,path2,ret);
  x9md_rep_u32(&r,(uint32_t)ret);break;
 case 0x100a: name="GetFolderSize";ret=0;x9md_rep_u32(&r,0);break;
 case 0x100d: name="Count";ret=0;x9md_rep_u32(&r,0);break;
 case 0x100e: { name="GetDiskInfo";
  uint8_t d[0x54];memset(d,0,sizeof(d));
  x9md_st32(d+0x30,0x200);x9md_st32(d+0x34,4096>>9);x9md_st32(d+0x38,0x10000);x9md_st32(d+0x3c,0x8000);x9md_st32(d+0x40,0x200);
  ret=0;x9md_rep_u32(&r,0);x9md_rep_lv(&r,d,sizeof(d));break;}
 case 0x1010: name="GetAttributes";
  if(para<1||x9fs_path(&lv[0],path,sizeof(path)))ret=FS_EPATH;
  else if(x9fs_isdir(path))ret=0x10;
  else if(x9fs_find(path)>=0)ret=0;
  else{struct x9e4_inode in;ret=(x9fs_tomb(path)<0&&!x9fs_ext_stat(path,&in,NULL,NULL))?0:FS_ENOENT;}
  X9MD_LOG("X9 MODEM: FS GetAttributes '%s' -> %d\n",path,ret);
  x9md_rep_u32(&r,(uint32_t)ret);break;
 case 0x1012: case 0x1013:
  name=op==0x1012?"FindFirst":"FindNext";
  if(op==0x1012&&para>0&&!x9fs_path(&lv[0],path,sizeof(path)))X9MD_LOG("X9 MODEM: FS FindFirst '%s' -> none\n",path);
  ret=FS_ENOENT;x9md_rep_u32(&r,(uint32_t)ret);x9md_rep_lv(&r,NULL,0x34);x9md_rep_lv(&r,NULL,2);break;
 case 0x1014: name="FindClose";ret=0;x9md_rep_u32(&r,0);break;
 case 0x1021: name="Restore";
  /* FS_CCCI_Restore -> NVM_RestoreFromBinRegion_OneFile(path): copy one LID back from the factory BIN region
   * (nvdata/AllFile). Not implemented; report success when the file exists so the MD re-reads it. */
  if(para<1||x9fs_path(&lv[0],path,sizeof(path)))ret=FS_EPATH;
  else{struct x9e4_inode in;ret=(x9fs_find(path)>=0||(x9fs_tomb(path)<0&&!x9fs_ext_stat(path,&in,NULL,NULL)))?0:FS_EOTHER;}
  X9MD_LOG("X9 MODEM: FS Restore '%s' -> %d\n",path,ret);
  x9md_rep_u32(&r,(uint32_t)ret);break;
 case 0x1015: case 0x1019: case 0x101c: name="LockFAT/ClearDisk/SetDisk";ret=0;x9md_rep_u32(&r,0);break;
 case 0x1016: name="UnlockAll";ret=1;x9md_rep_u32(&r,1);break;
 case 0x1017: name="Shutdown";ret=0;memset(x9fs_h,0,sizeof(x9fs_h));break;   /* no LV */
 case 0x101a: name="GetDrive";ret='Z';x9md_rep_u32(&r,'Z');break;
 case 0x101b: name="GetClusterSize";ret=4096;x9md_rep_u32(&r,4096);break;
 case 0x1023: name="BinRegion";ret=-2;x9md_rep_u32(&r,(uint32_t)-2);break;
 case 0x1022: { name="CMPT_Read";
  /* in LV1 (u32): [0] opid_map, [3] open flag, [5] offset, [6] whence, [8] length; map bits: 0 open,1 size,2 seek,3 read,4 close */
  uint32_t m=0,err=0,size=0,got=0;const uint8_t *src=NULL;int h=-1;
  if(para<2||lv[1].len<36||x9fs_path(&lv[0],path,sizeof(path))){ret=FS_EPATH;err=(uint32_t)ret;}
  else{
   uint32_t map=x9md_ld32(lv[1].p),fl=x9md_ld32(lv[1].p+12),off=x9md_ld32(lv[1].p+20),wh=x9md_ld32(lv[1].p+24),cnt=x9md_ld32(lv[1].p+32);
   struct x9fs_h *hp=NULL;
   if(map&1){h=x9fs_open(path,fl);if(h<0){ret=h;err=(uint32_t)h;goto cmpt_r_done;}m=1;}
   hp=x9fs_hget(h);
   if(!hp){ret=FS_EBADH;err=(uint32_t)ret;goto cmpt_r_done;}
   if(map&2){size=x9fs_n[hp->node].size;m|=2;}
   if(map&4){int64_t np=(wh==0?0:wh==1?(int64_t)hp->pos:(int64_t)x9fs_n[hp->node].size)+(int32_t)off;if(np<0){ret=FS_EOTHER;err=(uint32_t)ret;goto cmpt_r_done;}hp->pos=(uint32_t)np;m|=4;}
   if(map&8){struct x9fs_node *nd=&x9fs_n[hp->node];static uint8_t cbuf[0x4000];
    if(cnt>0x4000)X9MD_LOG("X9 MODEM: FS CMPT_Read '%s' request %u clamped to 0x4000\n",path,cnt);
    if(cnt>0x4000)cnt=0x4000;
    if(hp->pos<nd->size){got=nd->size-hp->pos;if(got>cnt)got=cnt;}
    if(got&&x9fs_node_read(nd,hp->pos,cbuf,got)){got=0;ret=FS_EOTHER;err=(uint32_t)ret;goto cmpt_r_done;}
    src=cbuf;hp->pos+=got;m|=8;}
   if(map&0x10){hp->used=0;m|=0x10;}
   ret=0;
   X9MD_LOG("X9 MODEM: FS CMPT_Read '%s' map=%x flag=%x off=%u wh=%u cnt=%u -> mask %x size %u got %u\n",path,map,fl,off,wh,cnt,m,size,got);
  }
cmpt_r_done:
  {uint8_t mo[8];x9md_st32(mo,m);x9md_st32(mo+4,err);
   x9md_rep_lv(&r,mo,8);x9md_rep_u32(&r,size);x9md_rep_u32(&r,got);x9md_rep_lv(&r,src,got);}
  if(ret)X9MD_LOG("X9 MODEM: FS CMPT_Read '%s' failed %d\n",path,ret);
  break;}
 case 0x1024: { name="CMPT_Write";
  /* in LV1 (u32): [0] opid_map, [3] flag, [4] offset, [5] whence, [7] length, [8] handle; LV2 = data; bits 0 open,2 seek,4 close,5 write */
  uint32_t m=0,err=0,wr=0;int h=-1;
  if(para<3||lv[1].len<36||x9fs_path(&lv[0],path,sizeof(path))){ret=FS_EPATH;err=(uint32_t)ret;}
  else{
   uint32_t map=x9md_ld32(lv[1].p),fl=x9md_ld32(lv[1].p+12),off=x9md_ld32(lv[1].p+16),wh=x9md_ld32(lv[1].p+20),len=x9md_ld32(lv[1].p+28);
   int hin=(int)x9md_ld32(lv[1].p+32);
   struct x9fs_h *hp=NULL;
   if(hin<0){if(!(map&1)){ret=FS_EBADH;err=(uint32_t)ret;goto cmpt_w_done;}h=x9fs_open(path,fl);if(h<0){ret=h;err=(uint32_t)h;goto cmpt_w_done;}m=1;}
   else h=hin;
   hp=x9fs_hget(h);
   if(!hp){ret=FS_EBADH;err=(uint32_t)ret;goto cmpt_w_done;}
   if(map&4){int64_t np=(wh==0?0:wh==1?(int64_t)hp->pos:(int64_t)x9fs_n[hp->node].size)+(int32_t)off;if(np<0){ret=FS_EOTHER;err=(uint32_t)ret;x9fs_h[h].used=0;goto cmpt_w_done;}hp->pos=(uint32_t)np;m|=4;}
   if(map&0x20){
    struct x9fs_node *nd=&x9fs_n[hp->node];uint32_t w=len;if(w>lv[2].len)w=lv[2].len;
    if(x9fs_materialize(nd)||x9fs_grow(nd,hp->pos+w)){ret=FS_EMFILE;err=(uint32_t)ret;hp->used=0;goto cmpt_w_done;}
    if(hp->pos>nd->size)memset(x9fs_pool+nd->off+nd->size,0,hp->pos-nd->size);
    memcpy(x9fs_pool+nd->off+hp->pos,lv[2].p,w);hp->pos+=w;if(hp->pos>nd->size)nd->size=hp->pos;wr=w;m|=0x20;}
   if(map&0x10){hp->used=0;m|=0x10;}
   ret=0;
   X9MD_LOG("X9 MODEM: FS CMPT_Write '%s' map=%x flag=%x off=%u wh=%u len=%u hin=%d -> mask %x wrote %u\n",path,map,fl,off,wh,len,hin,m,wr);
  }
cmpt_w_done:
  {uint8_t mo[8];x9md_st32(mo,m);x9md_st32(mo+4,err);
   x9md_rep_lv(&r,mo,8);x9md_rep_u32(&r,h<0?0:(uint32_t)h);x9md_rep_u32(&r,wr);}
  if(ret){
   printf("X9 MODEM: FS CMPT_Write '%s' failed %d map=%x mask=%x handle=%d\n",path,ret,
    para>=2&&lv[1].len>=4?x9md_ld32(lv[1].p):0,m,h);
   if(ret==FS_EMFILE)x9fs_usage("CMPT_Write failure");
  }
  break;}
 case 0x1026: { name="CMPT_Read_EX";
  /* in LV1 (u32): [0] opid_map, [3] open flag, [5] offset, [6] whence, [8] length, [10] handle (when map bit0 clear);
   * reply: [mask,err 8B][size][bytes read][data][handle or -1] */
  uint32_t m=0,err=0,size=0,got=0;const uint8_t *src=NULL;int h=-1;uint32_t outh=0xffffffffu;
  if(para<2||lv[1].len<44||x9fs_path(&lv[0],path,sizeof(path))){ret=FS_EPATH;err=(uint32_t)ret;}
  else{
   uint32_t map=x9md_ld32(lv[1].p),fl=x9md_ld32(lv[1].p+12),off=x9md_ld32(lv[1].p+20),wh=x9md_ld32(lv[1].p+24),cnt=x9md_ld32(lv[1].p+32);
   int hin=(int)x9md_ld32(lv[1].p+40);
   struct x9fs_h *hp=NULL;
   if(map&1){h=x9fs_open(path,fl);if(h<0){ret=h;err=(uint32_t)h;goto cmpt_x_done;}m=1;}
   else h=hin;
   hp=x9fs_hget(h);
   if(!hp){ret=FS_EBADH;err=(uint32_t)ret;goto cmpt_x_done;}
   if(map&2){size=x9fs_n[hp->node].size;m|=2;}
   if(map&4){int64_t np=(wh==0?0:wh==1?(int64_t)hp->pos:(int64_t)x9fs_n[hp->node].size)+(int32_t)off;if(np<0){ret=FS_EOTHER;err=(uint32_t)ret;x9fs_h[h].used=0;goto cmpt_x_done;}hp->pos=(uint32_t)np;m|=4;}
   if(map&8){struct x9fs_node *nd=&x9fs_n[hp->node];static uint8_t xbuf[0x4000];
    if(cnt>0x4000)X9MD_LOG("X9 MODEM: FS CMPT_Read_EX '%s' request %u clamped to 0x4000\n",path,cnt);
    if(cnt>0x4000)cnt=0x4000;
    if(hp->pos<nd->size){got=nd->size-hp->pos;if(got>cnt)got=cnt;}
    if(got&&x9fs_node_read(nd,hp->pos,xbuf,got)){got=0;ret=FS_EOTHER;err=(uint32_t)ret;hp->used=0;goto cmpt_x_done;}
    src=xbuf;hp->pos+=got;m|=8;}
   if(map&0x10){hp->used=0;m|=0x10;}
   outh=(uint32_t)h;ret=0;
   X9MD_LOG("X9 MODEM: FS CMPT_Read_EX '%s' map=%x flag=%x off=%u wh=%u cnt=%u hin=%d -> mask %x size %u got %u h %d\n",path,map,fl,off,wh,cnt,hin,m,size,got,h);
  }
cmpt_x_done:
  {uint8_t mo[8];x9md_st32(mo,m);x9md_st32(mo+4,err);
   x9md_rep_lv(&r,mo,8);x9md_rep_u32(&r,size);x9md_rep_u32(&r,got);x9md_rep_lv(&r,src,got);x9md_rep_u32(&r,outh);}
  if(ret)X9MD_LOG("X9 MODEM: FS CMPT_Read_EX '%s' failed %d\n",path,ret);
  break;}
 case 0x1025: name="GetFileDetail";ret=FS_ENOENT;x9md_rep_u32(&r,(uint32_t)ret);x9md_rep_lv(&r,NULL,0x18);break;
 default:
  ret=FS_EOTHER;x9md_rep_u32(&r,(uint32_t)ret);break;
 }
done:
 x9md_rep_done(&r);
 x9fs_nops++;
 if(x9fs_nops<1500)
  X9MD_LOG("X9 MODEM: FS #%u op %04x %s para %u ret %d u1=%u -> %u B\n",x9fs_nops,op,name,para,ret,u1,r.n);
 x9fs_send(d0,rsv,op|0xffff0000u,x9fs_out+4,r.n);
}
#undef LVU

/* MD -> AP fragment from FS_RX: first fragment carries op_id + para_num + LVs,
 * continuation fragments repeat header + op_id and continue the byte stream. */
static void x9fs_rx(const uint8_t *pkt,uint32_t len){
 if(len<20)return;
 uint32_t d0=x9md_ld32(pkt),rsv=x9md_ld32(pkt+12),idx=rsv;
 if(idx>4)idx=0;
 uint8_t *buf=x9fs_in[idx];
 uint32_t *bl=&x9fs_in_len[idx];
 if(!*bl){
  if(len-16>sizeof(x9fs_in[0]))return;
  memcpy(buf,pkt+16,len-16);*bl=len-16;                 /* op_id + body */
 }else{
  if(*bl+len-20>sizeof(x9fs_in[0])){printf("X9 MODEM: FS reassembly overflow idx %u\n",idx);*bl=0;return;}
  memcpy(buf+*bl,pkt+20,len-20);*bl+=len-20;
 }
 if(d0&0x80000000u)return;                              /* more fragments follow */
 uint32_t n=*bl;*bl=0;
 x9fs_handle(d0,rsv,buf,n);
}

/* ---- system messages (ch2 -> ch3) ---- */
static void x9md_sys_rx(const uint8_t *pkt,uint32_t len){
 if(len<16)return;
 uint32_t id=x9md_ld32(pkt+4),rsv=x9md_ld32(pkt+12);
 uint32_t reply_id=0,reply=0;
 if(id==0x105){reply_id=0x105;reply=4000;}                 /* MD_GET_BATTERY_INFO: mV */
 else if(id==0x114){reply_id=0x115;reply=rsv;}             /* TEST_MSG_ID_MD2AP -> AP2MD */
 const char *nm="?";
 switch(id){
 case 0x102:nm="MD_TX_POWER";break;case 0x103:nm="MD_RF_MAX_TEMPERATURE_SUB6";break;case 0x104:nm="MD_RF_ALL_TEMPERATURE_MMW";break;
 case 0x105:nm="MD_GET_BATTERY_INFO";break;case 0x107:nm="MD_SIM_TYPE";break;case 0x110:nm="MD_SW_MD1_TX_POWER_REQ";break;
 case 0x112:nm="MD_THROTTLING";break;case 0x114:nm="TEST_MSG_ID_MD2AP";break;case 0x116:nm="TEST_MSG_ID_L1CORE_MD2AP";break;
 case 0x118:nm="SIM_LOCK_RANDOM_PATTERN";break;case 0x11a:nm="CCISM_SHM_INIT_ACK";break;case 0x11b:nm="CCISM_SHM_INIT_DONE";break;
 case 0x11e:nm="LWA_CONTROL_MSG";break;case 0x120:nm="MD_DISPLAY_DYNAMIC_MIPI";break;case 0x121:nm="MD_RF_HOPPING_NOTIFY";break;
 case 0x12a:nm="MD_NR_BAND_ACTIVATE_INFO";break;case 0x12b:nm="MD_DRAM_SLC";break;case 0x1000:nm="MD_WDT_MONITOR";break;
 }
 X9MD_LOG("X9 MODEM: SYSMSG id %x (%s) rsv %08x%s\n",id,nm,rsv,reply_id?" (answered)":"");
 /* proxy_send_msg_to_md: data0 = CCCI_MAGIC_NUM, data1 = msg id, reserved = value */
 if(reply_id)x9md_tx(0,X9MD_CH_SYS_TX,0xffffffffu,reply_id,reply,NULL,0);
}

/* Android reads every SIM pad as pinmux mode 1 (MD SIM function; GPIO90-95 SCLK/SRST/SIO of SIM1/2, GPIO224/225 hot-plug EINT),
 * nobody claims them in the DT, so the boot firmware... but on our boot 90-95 and 225 are still mode 0 (136.txt). Set them. */
static void x9md_sim_pin_dump(void);
/* mt6993 GPIO_MODE: 4 pins per 32-bit register at 0x300+(pin/4)*0x10, 8-bit stride, 5-bit field (pinctrl-mt6993.c
 * mt6993_pin_mode_range); the driver is FLAG_RACE_FREE_ACCESS: write ((1<<4)|mode)<<shift to reg+0xC (MWR). */
static void x9md_sim_pinmux(void){
 static const unsigned pins[]={90,91,92,93,94,95,224,225};
 if(!x9md_gpio)return;
 for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++){
  unsigned p=pins[i],off=0x300+(p/4)*0x10,sh=(p%4)*8;
  x9md_wr(x9md_gpio,off+0xc,(0x10u|1u)<<sh);
 }
 x9md_sim_pin_dump();
}
/* ---- READY-state AP duties (stage 4) ---- */
/* AT channel = ttyC0: MD->AP ch 10 (UART2_RX) on rx queue 5, AP->MD ch 12 (UART2_TX) on tx queue 5
 * (port_cfg.c md_ccci_ports: CCCI_UART2_TX/RX, DATA_AT_CMD_Q). Frame: data0=0, data1=16+len, channel, reserved=0. */
#define X9MD_CH_AT_RX 10
#define X9MD_CH_AT_TX 12
#define X9MD_Q_AT 5
/* ---- stage 5 AT engine: one command at a time, advance on a final result code ---- */
static char x9at_buf[2048],x9at_line[256];
static unsigned x9at_len,x9at_line_len;
static int x9at_pending,x9at_done,x9at_overflow,x9at_line_overflow;
static void x9at_reset(void){
 x9at_len=x9at_line_len=0;x9at_done=x9at_overflow=x9at_line_overflow=0;
 bzero(x9at_buf,sizeof(x9at_buf));bzero(x9at_line,sizeof(x9at_line));
}
static int x9md_at_send(const char *cmd);
#include "x9_at_bridge.inc"
#include "x9_mipc_bridge.inc"
static void x9at_feed(const uint8_t *pkt,uint32_t len){
 if(!x9at_pending||x9at_done)return;
 for(uint32_t i=16;i<len;i++){
  char c=(char)pkt[i];
  if(x9at_len+1<sizeof(x9at_buf))x9at_buf[x9at_len++]=c;
  else x9at_overflow=1;
  if(c=='\n'){
   if(x9at_line_len&&x9at_line[x9at_line_len-1]=='\r')x9at_line_len--;
   x9at_line[x9at_line_len]=0;
   if(!x9at_line_overflow){
    if(!strcmp(x9at_line,"OK"))x9at_done=1;
    else if(!strcmp(x9at_line,"ERROR")||!strcmp(x9at_line,"NO CARRIER")||
      !strncmp(x9at_line,"+CME ERROR:",11)||!strncmp(x9at_line,"+CMS ERROR:",11))x9at_done=-1;
   }
   x9at_line_len=0;x9at_line_overflow=0;
   if(x9at_done)break;
  }else if(x9at_line_len+1<sizeof(x9at_line))x9at_line[x9at_line_len++]=c;
  else x9at_line_overflow=1;
 }
 x9at_buf[x9at_len]=0;
}
static void x9md_at_rx(const uint8_t *pkt,uint32_t len){
 char t[200];unsigned n=0;
 x9at_feed(pkt,len);
 /* SIM files, IMSI/IMEI/APN and authentication material must not enter USB/pstore.
  * Redact all ttyC0 bodies while the bridge is enabled, including echo and URCs. */
 if(x9_at_enabled())return;
 for(uint32_t i=16;i<len&&n+4<sizeof(t);i++){
  uint8_t c=pkt[i];
  if(c=='\r'){t[n++]='\\';t[n++]='r';}
  else if(c=='\n'){t[n++]='\\';t[n++]='n';}
  else if(c>=32&&c<127)t[n++]=c;
  else{t[n++]='.';}
 }
 t[n]=0;
 printf("X9 MODEM: AT RX (%u B): %s\n",len-16,t);
}
static int x9md_at_send(const char *cmd){
 uint32_t n=(uint32_t)strlen(cmd);
 int rc=x9md_tx(X9MD_Q_AT,X9MD_CH_AT_TX,0,16+n,0,(const uint8_t *)cmd,n);
 if(!x9_at_enabled())printf("X9 MODEM: AT TX '%.40s' rc=%d\n",cmd,rc);
 return rc;
}
/* read-only probe sequence, one command per second after READY (x9_md_at=1) */
static const char *const x9md_at_seq[]={"AT\r","ATI\r","AT+CGMR\r","AT+CFUN?\r","AT+CPIN?\r","AT+CREG?\r","AT+COPS?\r","AT+CSQ\r",
 /* level 2 (x9_md_at=2): bring the SIM stack up without turning the radio on (CFUN=4 = flight mode) */
 "AT+CFUN=4\r","AT+CFUN?\r","AT+CPIN?\r","AT+ESIMS?\r","AT+EUSIM?\r","AT+CREG?\r","AT+CFUN?\r"};
/* per-channel receive statistics (4.7: which MD ports are alive) */
static unsigned x9md_chstat[256];
static void x9md_chstat_note(unsigned ch){if(ch<256)x9md_chstat[ch]++;}
static void x9md_chstat_dump(void){
 char b[240];unsigned n=0;
 for(unsigned c=0;c<256&&n+16<sizeof(b);c++)if(x9md_chstat[c])n+=snprintf(b+n,sizeof(b)-n,"ch%u=%u ",c,x9md_chstat[c]);
 printf("X9 MODEM: RX per channel: %s\n",n?b:"(none)");
}
/* 4.8: SIM pins. The SIM interface lives in the MD; Android's DT has no pinctrl state for GPIO90-95 (so the boot
 * firmware leaves them in the MD SIM function) and no AP-visible vsim regulator. Log the pinmux we inherited. */
static void x9md_sim_pin_dump(void){
 static const unsigned pins[]={0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,0xe0,0xe1};
 if(!x9md_gpio)return;
 char b[200];unsigned n=0;
 for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++){
  unsigned p=pins[i];
  uint32_t mode=(x9md_rd(x9md_gpio,0x300+(p/4)*0x10)>>((p%4)*8))&0x1f;
  uint32_t dir=(x9md_rd(x9md_gpio,0x0+(p/32)*0x10)>>(p%32))&1;
  n+=snprintf(b+n,sizeof(b)-n,"%u:m%u/d%u/i%u ",p,mode,dir,x9md_gpio_val(p));
 }
 printf("X9 MODEM: SIM pins (pin:mode/dir/in): %s\n",b);
}
/* ccci_fsm_poller.c: AP sends an empty system-style message on CCCI_STATUS_TX (ch 68, queue 0) every 75 s and expects a
 * CCCI_STATUS_RX (ch 67) reply within 15 s. Android only runs it while mdlogger is a critical user; behind x9_md_poll=1 here. */
static unsigned x9md_poll_sent;
static void x9md_poll_tick(unsigned ms){
 if(!sweet_x9_arg("x9_md_poll",0))return;
 if(ms>=10000&&ms/75000+1>x9md_poll_sent){
  x9md_poll_sent=ms/75000+1;
  int rc=x9md_tx(0,68,0xffffffffu,0,0,NULL,0);
  X9MD_LOG("X9 MODEM: poll MD status TX rc=%d\n",rc);
 }
}
/* ccci_mdinit monitor_time_update_thread -> send_new_time_to_new_md(): ASCII "%010u,"x8 on ccci_0_202 (TIME_TX = ch 203, queue 1),
 * HIRES layout: sec lo/hi, tz minutes west (as unsigned), dst, usec lo/hi, arch counter lo/hi. Android repeats it every ~30 s. */
static void x9md_send_time(void){
 clock_sec_t sec_=0;clock_usec_t usec_=0;
 clock_get_calendar_microtime(&sec_,&usec_);
 uint32_t sec=(uint32_t)sec_,usec=(uint32_t)usec_;
 uint64_t cnt=x9md_cntvct();
 char b[96];
 int n=snprintf(b,sizeof(b),"%010u,%010u,%010u,%010u,%010u,%010u,%010u,%010u",
  sec,0u,(unsigned)(-480),0u,usec,0u,(unsigned)cnt,(unsigned)(cnt>>32));
 int rc=x9md_tx(1,203,0,16+n+1,0,(const uint8_t *)b,n+1);
 X9MD_LOG("X9 MODEM: CTime TX '%s' rc=%d\n",b,rc);
}
/* ccci_fsm_scp_c.c: after READY the AP sends CCISM_SHM_INIT (0x119) on the system channel (SYSTEM_TX = ch 3, queue 0);
 * the MD answers CCISM_SHM_INIT_ACK (0x11a) / DONE (0x11b). Android waits for the SCP first; we have no SCP firmware. */
static void x9md_send_ccism_init(void){
 int rc=x9md_tx(0,X9MD_CH_SYS_TX,0xffffffffu,0x119,0,NULL,0);
 X9MD_LOG("X9 MODEM: SYSMSG TX CCISM_SHM_INIT (0x119) rc=%d\n",rc);
}
struct x9at_step{const char *cmd;unsigned wait_ms;unsigned timeout_ms;};
/* level 3 = what a minimal RIL does to take the SIM stack up and register: echo off, verbose errors, SIM state in flight
 * mode, registration URCs on, radio on, then poll state. Every step waits for OK/ERROR (or its own timeout). */
static const struct x9at_step x9at_script[]={
 {"ATE0\r",0,5000},{"AT+CMEE=1\r",0,5000},{"AT+CFUN=4\r",300,30000},{"AT+ESIMS?\r",1000,5000},{"AT+CPIN?\r",300,10000},
 {"AT+EUSIM?\r",300,5000},{"AT+CREG=2\r",0,5000},{"AT+CEREG=2\r",0,5000},{"AT+C5GREG=2\r",0,5000},
 {"AT+CFUN=1\r",500,60000},{"AT+CPIN?\r",5000,10000},{"AT+CFUN?\r",0,5000},{"AT+COPS?\r",5000,10000},{"AT+CREG?\r",0,5000},
 {"AT+CEREG?\r",0,5000},{"AT+C5GREG?\r",0,5000},{"AT+CSQ\r",0,5000},{"AT+COPS?\r",15000,10000},{"AT+CREG?\r",0,5000},
 {"AT+CEREG?\r",0,5000},{"AT+C5GREG?\r",0,5000},{"AT+CGATT?\r",0,10000},{"AT+CSQ\r",0,5000},
};
static void x9at_engine(unsigned ms){
 static unsigned idx,sent_ms,free_ms,started;
 if(!started){started=1;free_ms=ms;}
 if(x9at_bridge_tick(ms,idx>=10))return;
 if(x9at_pending){
  if(x9at_done){
   X9MD_LOG("X9 MODEM: AT step %u '%.20s' -> %s after %u ms\n",idx,x9at_script[idx].cmd,x9at_done>0?"OK":"ERROR",ms-sent_ms);
   x9at_pending=0;x9at_done=0;idx++;free_ms=ms;
  }else if(ms-sent_ms>x9at_script[idx].timeout_ms){
   X9MD_LOG("X9 MODEM: AT step %u '%.20s' TIMEOUT\n",idx,x9at_script[idx].cmd);
   x9at_pending=0;idx++;free_ms=ms;
   if(x9_at_enabled())x9at_bridge_poison();
  }
  return;
 }
 if(idx>=sizeof(x9at_script)/sizeof(x9at_script[0])){
  static int fin;if(!fin){fin=1;printf("X9 MODEM: AT script finished\n");}
  return;
 }
 if(ms-free_ms<x9at_script[idx].wait_ms+200)return;
 x9at_reset();x9at_pending=1;sent_ms=ms;
 if(x9md_at_send(x9at_script[idx].cmd)<0){x9at_pending=0;if(x9_at_enabled())x9at_bridge_poison();}
}
static void x9md_ready_tick(unsigned ms_since_ready){
 x9mi_tick();
 static unsigned next,ctime_next,init_done,pins_done;
 if(!pins_done){pins_done=1;x9md_sim_pin_dump();}
 x9md_poll_tick(ms_since_ready);
 if(ms_since_ready%10000<2)x9md_chstat_dump();
 if(!init_done&&ms_since_ready>=300){init_done=1;x9md_send_ccism_init();}
 if(ms_since_ready>=1000&&ms_since_ready/30000>=ctime_next){ctime_next=ms_since_ready/30000+1;x9md_send_time();}
 int lvl=sweet_x9_arg("x9_md_at",0);
 /* The bridge requires echo-off/verbose finals and the serial radio bring-up. */
 if(x9_at_enabled())lvl=3;
 if(!lvl)return;
 if(ms_since_ready<2000)return;
 if(lvl>=3){x9at_engine(ms_since_ready);return;}
 unsigned idx=(ms_since_ready-2000)/1000,cnt=lvl>=2?sizeof(x9md_at_seq)/sizeof(x9md_at_seq[0]):8;
 if(idx>=cnt||idx<next)return;
 next=idx+1;
 x9md_at_send(x9md_at_seq[idx]);
}

/* Dispatch one MD packet. qi = ring it arrived on. */
static void x9md_s3_rx(unsigned qi,unsigned chn,const uint8_t *pkt,uint32_t len){
 (void)qi;
 x9md_chstat_note(chn);
 if(chn==X9MD_CH_RPC_RX)x9md_rpc_handle(pkt,len);
 else if(chn==X9MD_CH_FS_RX)x9fs_rx(pkt,len);
 else if(chn==X9MD_CH_SYS_RX)x9md_sys_rx(pkt,len);
 else if(chn==X9MD_CH_AT_RX)x9md_at_rx(pkt,len);
 else if(chn==216){
  if(qi!=5||len<16||len>X9MD_PKT_MAX)x9mi_online(0);
  else x9mi_rx(pkt+16,len-16);
 }
}
