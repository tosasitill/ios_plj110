/* ARM SMMUv3 existing-state inspection for captured X9 SOC-SMMU SID0x63.
 * A DMA address equal to PA is usable only with observed bypass. Translation,
 * invalid stream entries, global abort or changing control state fail closed.
 */
#include "ufs_smmu.h"

int x9u_smmu_identity(void *ctx,unsigned sid,struct x9u_smmu_state *s,
                       uint32_t (*read)(void *,unsigned),
                       int (*table64)(void *,uint64_t,uint64_t *))
{
 if(!s||!read||!table64||sid>65535)return -1;
 *s=(struct x9u_smmu_state){0};s->sid=sid;
 s->idr0=read(ctx,0);s->cr0=read(ctx,0x20);s->cr0ack=read(ctx,0x24);s->gbpa=read(ctx,0x44);
 if(!s->idr0||s->idr0==0xffffffffU||s->cr0!=s->cr0ack||(s->gbpa&(1U<<31)))return -2;
 if(!(s->cr0&1)) {
  if(s->gbpa&(1U<<20))return -3;
  s->identity=1;return 0;
 }
 s->strtab_base=(uint64_t)read(ctx,0x84)<<32|read(ctx,0x80);
 s->strtab_config=read(ctx,0x88);
 unsigned log=s->strtab_config&63,format=(s->strtab_config>>16)&3,split=(s->strtab_config>>6)&31;
 if(!log||log>16||sid>=(1U<<log))return -4;
 uint64_t base=s->strtab_base&0x000fffffffffffC0ULL,ste;
 if(!base)return -5;
 if(!format) {
  uint64_t bytes=(1ULL<<log)*64;
  if(base&(bytes-1))return -6;
  ste=base+(uint64_t)sid*64;
 } else if(format==1) {
  if(split!=8||split>log)return -7;
  if(table64(ctx,base+((uint64_t)(sid>>split))*8,&s->l1))return -8;
  unsigned span=s->l1&31;
  if(!span||span>split+1||(sid&255)>=(1U<<(span-1)))return -9;
  uint64_t l2=s->l1&0x000fffffffffffC0ULL;
  if(!l2||(l2&((1ULL<<(span+5))-1)))return -10;
  ste=l2+(uint64_t)(sid&255)*64;
 } else return -11;
 if(table64(ctx,ste,&s->ste0))return -12;
 /* V=1 and CFG=4 are architectural bypass; CFG=0 is abort, not bypass. */
 if(!(s->ste0&1)||((s->ste0>>1)&7)!=4)return -13;
 if(read(ctx,0x20)!=s->cr0||read(ctx,0x24)!=s->cr0ack||read(ctx,0x44)!=s->gbpa||
    read(ctx,0x80)!=(uint32_t)s->strtab_base||read(ctx,0x84)!=(uint32_t)(s->strtab_base>>32)||
    read(ctx,0x88)!=s->strtab_config)return -14;
 s->identity=1;return 0;
}
