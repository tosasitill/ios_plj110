/* Synthetic regression source. Compile only under the current project rule;
 * no captured PDU, device access, deletion or automatic test execution. */
#include <cassert>
#include <cstdint>
#include <cstring>
#include "../../kernel/src/bsd/dev/x9_sms_storage.h"
static unsigned tlv(uint8_t *p,unsigned at,unsigned tag,const uint8_t *v,unsigned n){
 p[at]=(uint8_t)tag;p[at+1]=(uint8_t)n;p[at+2]=(uint8_t)(n>>8);
 memcpy(p+at+3,v,n);return at+3+n;
}
static unsigned finish(uint8_t *p,unsigned n){p[5]=(uint8_t)(n-7);p[6]=(uint8_t)((n-7)>>8);return n;}
int main(){
 uint8_t p[64]={0,1,0,0x31};uint8_t storage=1,tag=1,mode=1;
 struct x9ms_storage_request r={};
 unsigned n=tlv(p,7,1,&storage,1);n=tlv(p,n,0x11,&mode,1);n=tlv(p,n,0x10,&tag,1);finish(p,n);
 assert(n==19&&x9ms_storage_decode(p,n,&r)==X9MS_OK&&r.list&&r.storage==1&&r.has_tag&&r.tag==1);
 for(tag=0;tag<4;tag++){p[18]=tag;assert(x9ms_storage_decode(p,n,&r)==X9MS_OK&&r.tag==tag);}
 p[18]=1;p[11]=0x12;assert(x9ms_storage_decode(p,n,&r)==X9MS_UNSUPPORTED);p[11]=0x11;
 p[14]=0;assert(x9ms_storage_decode(p,n,&r)==X9MS_UNSUPPORTED);p[14]=1;
 p[15]=0x11;assert(x9ms_storage_decode(p,n,&r)==X9MS_ARG);p[15]=0x10;
 for(unsigned k=0;k<n;k++)assert(x9ms_storage_decode(p,k,&r)!=X9MS_OK);
 struct x9ms_storage_request sentinel={123,1,1,1,1,3};r=sentinel;
 p[18]=4;assert(x9ms_storage_decode(p,n,&r)==X9MS_ARG&&!memcmp(&r,&sentinel,sizeof(r)));p[18]=1;
 uint8_t indexed[6]={0,0x34,0x12,0,0,0};memset(p,0,sizeof(p));p[3]=0x22;
 n=tlv(p,7,1,indexed,5);n=tlv(p,n,0x10,&mode,1);finish(p,n);
 assert(x9ms_storage_decode(p,n,&r)==X9MS_OK&&!r.list&&!r.modify&&r.index==0x1234);
 uint8_t ims=0;n=tlv(p,n,0x11,&ims,1);finish(p,n);
 assert(x9ms_storage_decode(p,n,&r)==X9MS_OK);p[n-1]=1;
 assert(x9ms_storage_decode(p,n,&r)==X9MS_UNSUPPORTED);
 memset(p,0,sizeof(p));p[3]=0x23;n=tlv(p,7,1,indexed,6);n=tlv(p,n,0x10,&mode,1);finish(p,n);
 assert(x9ms_storage_decode(p,n,&r)==X9MS_OK&&r.modify&&r.index==0x1234);
 p[15]=1;assert(x9ms_storage_decode(p,n,&r)==X9MS_UNSUPPORTED);p[15]=0;
 p[11]=p[12]=0;assert(x9ms_storage_decode(p,n,&r)==X9MS_ARG);
 return 0;
}
