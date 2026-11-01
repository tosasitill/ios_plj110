/* Host-only hostile-stream exercise, ASan/UBSan. No kernel or device I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "all_resource.h"
static uint32_t state=0x200;
static uint32_t rnd(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
int main(int argc,char **argv)
{
 if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 3;
 uint8_t original[X9DC_INPUT],input[X9DC_INPUT];uint32_t head[3];unsigned calls=0;
 void *scratch=malloc(X9D_CODEC_SCRATCH);
 while(fread(head,sizeof(head),1,f)==1) {
  unsigned type=head[0],expected=head[1],length=head[2];
  if(!length||length>X9DC_INPUT||expected>X9DC_BLOCK||fread(original,1,length,f)!=length)return 4;
  uint8_t *out=malloc(expected?expected:1);
  if(x9d_codec_decode(type,original,length,out,expected,scratch))return 5;
  free(out);
  for(unsigned i=0;i<3000;i++) {
   memcpy(input,original,length);size_t n=length,e=expected;
   switch(i%4) {
    case 0:input[rnd()%length]^=(uint8_t)(1U<<(rnd()%8));break;
    case 1:n=rnd()%(length+1);break;
    case 2:e=rnd()%(X9DC_BLOCK+1);break;
    default:for(unsigned k=0;k<8;k++)input[rnd()%length]=(uint8_t)rnd();break;
   }
   out=malloc(e?e:1);(void)x9d_codec_decode(type,input,n,out,e,scratch);free(out);calls++;
  }
 }
 fclose(f);free(scratch);printf("ASan/UBSan: %u malformed/truncated/wrong-size stream cases completed.\n",calls);return 0;
}
