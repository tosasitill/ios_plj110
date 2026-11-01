/* Apple BSD-licensed LZVN/LZFSE decoders, explicit bounded caller scratch. */
#include "all_resource.h"
#include "vendor/lzfse_internal.h"
#include "vendor/lzvn_decode_base.h"
int x9d_lzbitmap(const uint8_t *,size_t,uint8_t *,size_t);
_Static_assert(sizeof(lzfse_decoder_state)<=X9D_CODEC_SCRATCH,"LZFSE scratch bound");
int x9d_codec_decode(unsigned type,const uint8_t *src,size_t n,uint8_t *dst,size_t expected,void *scratch)
{
 if(!src||!dst||!n||n>X9DC_INPUT||expected>X9DC_BLOCK)return X9DC_FORMAT;
 if(type==7||type==8) {
  lzvn_decoder_state state={0};state.src=src;state.src_end=src+n;
  state.dst=dst;state.dst_begin=dst;state.dst_current=dst;state.dst_end=dst+expected;
  lzvn_decode(&state);
  return state.end_of_stream&&(size_t)(state.dst-dst)==expected?0:X9DC_INFLATE;
 }
 if(type==11||type==12) {
  if(!scratch)return X9DC_FORMAT;
  lzfse_decoder_state *state=scratch;memset(state,0,sizeof(*state));
  state->src=src;state->src_begin=src;state->src_end=src+n;
  state->dst=dst;state->dst_begin=dst;state->dst_end=dst+expected;
  int result=lzfse_decode(state);
  return result==LZFSE_STATUS_OK&&state->end_of_stream&&(size_t)(state->dst-dst)==expected?0:X9DC_INFLATE;
 }
 if(type==13||type==14)return x9d_lzbitmap(src,n,dst,expected)?X9DC_INFLATE:0;
 return X9DC_FORMAT;
}
