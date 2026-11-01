/* Bounded ZBM09 decoder. Independent parser for the LZBITMAP stream format.
 * Input: magic, 24-bit frame sizes/offsets, literal/distance/bitmap streams,
 * packed nibble tokens with RLE, and a 17-byte dictionary. No allocation.
 */
#include <stddef.h>
#include <stdint.h>
static uint32_t u24(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16;}
static int nibble(const uint8_t *p,size_t count,size_t *at,unsigned *value)
{
 if(*at>=count*2)return -1;
 *value=(p[*at/2]>>((*at&1)*4))&15;(*at)++;return 0;
}
int x9d_lzbitmap(const uint8_t *src,size_t size,uint8_t *dst,size_t expected)
{
 if(!src||!dst||size<10||src[0]!='Z'||src[1]!='B'||src[2]!='M'||src[3]!=9)return -1;
 size_t cursor=4,produced=0;
 while(cursor<=size&&size-cursor>=6) {
  const uint8_t *frame=src+cursor;uint32_t packed=u24(frame),plain=u24(frame+3);
  if(packed==6)return plain==0&&produced==expected?0:-1;
  if(packed<6||packed>size-cursor||!plain||plain>expected-produced)return -1;
  if(packed==(uint64_t)plain+6) {
   for(uint32_t i=0;i<plain;i++)dst[produced++]=frame[6+i];
   cursor+=packed;continue;
  }
  if(packed<32)return -1;
  uint32_t distances=u24(frame+6),bitmaps=u24(frame+9),tokens=u24(frame+12),dict=packed-17;
  if(distances<15||bitmaps<distances||tokens<bitmaps||dict<tokens)return -1;
  uint8_t masks[15]={0},kinds[15]={0,1,2};
  for(unsigned i=3;i<15;i++) {
   unsigned bit=(i-3)*10,byte=bit/8,shift=bit%8;
   uint32_t word=frame[dict+byte]|(uint32_t)frame[dict+byte+1]<<8;
   if(shift>6)word|=(uint32_t)frame[dict+byte+2]<<16;
   word>>=shift;masks[i]=(uint8_t)word;kinds[i]=(uint8_t)((word>>8)&3);
   if(kinds[i]>2)return -1;
  }
  size_t lp=15,dp=distances,bp=bitmaps,tp=0,repeat=0;
  unsigned previous=0,have_previous=0,distance=8;
  uint32_t groups=(plain+7)/8;size_t frame_end=produced+plain;
  for(uint32_t group=0;group<groups;group++) {
   unsigned token;
   if(repeat){token=previous;repeat--;}
   else {
    if(nibble(frame+tokens,dict-tokens,&tp,&token))return -1;
    if(token==15) {
     if(!have_previous)return -1;
     repeat=3;unsigned extra;
     do {
      if(nibble(frame+tokens,dict-tokens,&tp,&extra))return -1;
      if(repeat>SIZE_MAX-extra)return -1;
      repeat+=extra;
     }while(extra==15);
     token=previous;repeat--;
    } else {previous=token;have_previous=1;}
   }
   unsigned mask=masks[token],kind=kinds[token];
   if(token<3){if(bp>=tokens)return -1;mask=frame[bp++];}
   if(kind==1){if(dp>=bitmaps)return -1;distance=frame[dp++];}
   if(kind==2){if(dp>bitmaps||bitmaps-dp<2)return -1;distance=frame[dp]|(unsigned)frame[dp+1]<<8;dp+=2;}
   for(unsigned bit=0;bit<8&&produced<frame_end;bit++,mask>>=1) {
    if(mask&1){if(lp>=distances)return -1;dst[produced++]=frame[lp++];}
    else {if(!distance||distance>produced)return -1;dst[produced]=dst[produced-distance];produced++;}
   }
  }
  /* Apple pads the final token groups, so their unused literals, distances
   * and bitmaps may remain. Each consumed byte was checked against its own
   * substream; never decode padding past the declared output size. */
  if(lp>distances||dp>bitmaps||bp>tokens)return -1;
  /* Apple frames may carry padded tokens beyond the last output group.
   * Unused tokens remain bounded by this frame; no output is decoded from them. */
  size_t total_nibbles=(dict-tokens)*2;
  if(tp>total_nibbles)return -1;
  cursor+=packed;
 }
 return -1; /* Missing valid end-of-stream. */
}
