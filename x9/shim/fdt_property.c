/* Complete-tree lookup. An invalid tree or missing node is not absence. */
#include "fdt.h"

static uint32_t be(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int eq(const char *a,const char *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static int overlap(size_t a,size_t n,size_t b,size_t m)
{return n&&m&&a<b+m&&b<a+n;}

static int lookup(uintptr_t address,const char *wanted_path,const char *wanted_key,
                  const uint8_t **value,size_t *length,const char keys[][32],unsigned count,
                  struct fdt_node_property *props,uint32_t *mask,unsigned *children,uint32_t handle,
                  const char capture_paths[][FDT_CAPTURE_PATH_BYTES],unsigned capture_count,
                  struct fdt_captured_property *captured,unsigned capacity,unsigned *captured_count)
{
 if(value)*value=0;
 if(length)*length=0;
 if(mask)*mask=0;
 if(children)*children=0;
 if(captured_count)*captured_count=0;
 if(props)for(unsigned i=0;i<count;i++)props[i]=(struct fdt_node_property){0};
 if(!address||(address&7)||!wanted_path||!value||!length||
    (!keys&&!handle&&(!wanted_key||!*wanted_key)))
  return FDT_PROPERTY_INVALID;
 const uint8_t *b=(const uint8_t *)address;
 if(be(b)!=0xd00dfeed)return FDT_PROPERTY_INVALID;
 uint32_t total=be(b+4),start=be(b+8),strings=be(b+12),reserve=be(b+16);
 uint32_t version=be(b+20),last=be(b+24),ss=be(b+32),sz=be(b+36);
 if(total<40||total>0x400000||version!=17||last>17||last>version||
    start<40||(start&3)||start>total||sz>total-start||(sz&3)||
    strings<40||strings>total||ss>total-strings||
    reserve<40||(reserve&7)||reserve>total||overlap(start,sz,strings,ss))
  return FDT_PROPERTY_INVALID;
 /* The reservation block must terminate within the declared blob without
  * reading structure/string bytes as a fake zero terminator. */
 size_t r=reserve;
 for(;;) {
  if(total-r<16||overlap(r,16,start,sz)||overlap(r,16,strings,ss))return FDT_PROPERTY_INVALID;
  unsigned nonzero=0;
  for(unsigned i=0;i<16;i++)nonzero|=b[r+i];
  r+=16;
  if(!nonzero)break;
 }
 size_t at=start,end=(size_t)start+sz,used=0,parents[32];
 unsigned depth=0,nodes=0,targets=0,matches=0,roots=0;
 unsigned child[32]={0};
 unsigned target_depth=0,direct_children=0;uint32_t seen=0;
 unsigned handle_matches=0;
 struct fdt_node_property schema[32]={0};
 struct fdt_captured_property capture[FDT_CAPTURE_MAX_PROPERTIES];
 unsigned capture_node[32],recorded=0;uint32_t capture_seen=0;
 char path[256]={0};const uint8_t *found=0;size_t found_size=0;
 while(end-at>=4) {
  unsigned t=be(b+at);at+=4;
  if(t==1) {
   if(depth==32||++nodes>8192)return FDT_PROPERTY_INVALID;
   if(target_depth&&depth==target_depth)direct_children++;
   size_t first=at;
   while(at<end&&b[at]) {if(b[at]=='/')return FDT_PROPERTY_INVALID;at++;}
   if(at==end)return FDT_PROPERTY_INVALID;
   parents[depth]=used;child[depth]=0;
   if(depth) {
    child[depth-1]=1;
    size_t n=at-first;
    if(!n||used+1+n>=sizeof(path))return FDT_PROPERTY_INVALID;
    path[used++]='/';
    for(size_t i=0;i<n;i++)path[used++]=(char)b[first+i];
    path[used]=0;
   } else if(at!=first||++roots!=1)return FDT_PROPERTY_INVALID;
   if(!capture_paths&&eq(path,wanted_path)) {
    if(++targets>1)return FDT_PROPERTY_INVALID;
    target_depth=depth+1;
   }
   if(capture_paths) {
    capture_node[depth]=UINT32_MAX;
    for(unsigned i=0;i<capture_count;i++)if(eq(path,capture_paths[i])) {
     if(capture_seen&(1u<<i))return FDT_PROPERTY_INVALID;
     capture_seen|=1u<<i;capture_node[depth]=i;
    }
   }
   depth++;at=(at+4)&~(size_t)3;
  } else if(t==2) {
   if(!depth)return FDT_PROPERTY_INVALID;
   if(depth==target_depth)target_depth=0;
   used=parents[--depth];path[used]=0;
  } else if(t==3) {
   if(!depth||child[depth-1]||end-at<8)return FDT_PROPERTY_INVALID;
   uint32_t n=be(b+at),offset=be(b+at+4);at+=8;
   if(n>end-at||offset>=ss)return FDT_PROPERTY_INVALID;
   const char *key=(const char *)b+strings+offset;
   size_t limit=ss-offset,i=0;
   while(i<limit&&key[i]) {
    /* Capture is a boot-critical bounded operation. Do not scan the same
     * hostile multi-megabyte name for every property, or truncate it into
     * a known key. Legacy lookup/schema contracts stay unchanged. */
    if(capture_paths&&i>=FDT_CAPTURE_KEY_BYTES-1)return FDT_PROPERTY_INVALID;
    i++;
   }
   if(!i||i==limit)return FDT_PROPERTY_INVALID;
   if(handle&&(eq(key,"phandle")||eq(key,"linux,phandle"))) {
    if(n!=4)return FDT_PROPERTY_INVALID;
    if(be(b+at)==handle && (++handle_matches!=1||!eq(path,wanted_path)))
     return FDT_PROPERTY_INVALID;
   }
   if(!capture_paths&&eq(path,wanted_path)) {
    if(keys) {
     unsigned k=0;while(k<count&&!eq(key,keys[k]))k++;
     if(k==count||(seen&(1u<<k)))return FDT_PROPERTY_INVALID;
     seen|=1u<<k;schema[k]=(struct fdt_node_property){b+at,n};
    } else if(!handle&&eq(key,wanted_key)) {
     if(++matches!=1)return FDT_PROPERTY_INVALID;
     found=b+at;found_size=n;
    }
   }
   if(capture_paths&&capture_node[depth-1]!=UINT32_MAX) {
    if(recorded==capacity)return FDT_PROPERTY_INVALID;
    unsigned node=capture_node[depth-1];
    for(unsigned i=0;i<recorded;i++)if(capture[i].node==node&&eq(capture[i].key,key))
     return FDT_PROPERTY_INVALID;
    capture[recorded++]=(struct fdt_captured_property){node,0,key,b+at,n};
   }
   at=(at+n+3)&~(size_t)3;
  } else if(t==4)continue;
  else if(t==9&&!depth&&roots==1) {
   /* No hidden second tree/property after END. Only zero padding or NOP. */
   while(end-at>=4) {
    unsigned tail=be(b+at);at+=4;
    if(tail&&tail!=4)return FDT_PROPERTY_INVALID;
   }
   if(at!=end)return FDT_PROPERTY_INVALID;
   if(capture_paths) {
    if(capture_seen!=((1u<<capture_count)-1))return FDT_PROPERTY_NODE_MISSING;
    for(unsigned i=0;i<recorded;i++)captured[i]=capture[i];
    *captured_count=recorded;return FDT_PROPERTY_PRESENT;
   }
   if(!targets)return FDT_PROPERTY_NODE_MISSING;
   if(handle)return handle_matches==1?FDT_PROPERTY_PRESENT:FDT_PROPERTY_ABSENT;
   if(keys) {
    for(unsigned i=0;i<count;i++)props[i]=schema[i];
    *mask=seen;*children=direct_children;return FDT_PROPERTY_PRESENT;
   }
   if(!matches)return FDT_PROPERTY_ABSENT;
   *value=found;*length=found_size;return FDT_PROPERTY_PRESENT;
  } else return FDT_PROPERTY_INVALID;
  if(at>end)return FDT_PROPERTY_INVALID;
 }
 return FDT_PROPERTY_INVALID;
}
int fdt_property_lookup(uintptr_t address,const char *path,const char *key,
                       const uint8_t **value,size_t *length)
{return lookup(address,path,key,value,length,0,0,0,0,0,0,0,0,0,0,0);}
int fdt_node_schema(uintptr_t address,const char *path,const char keys[][32],unsigned count,
                    struct fdt_node_property *props,uint32_t *mask,unsigned *children)
{
 if(!keys||!count||count>32||!props||!mask||!children)return FDT_PROPERTY_INVALID;
 for(unsigned i=0;i<count;i++) {
  unsigned n=0;while(n<32&&keys[i][n])n++;
  if(!n||n==32)return FDT_PROPERTY_INVALID;
  for(unsigned j=0;j<i;j++)if(eq(keys[i],keys[j]))return FDT_PROPERTY_INVALID;
 }
 const uint8_t *p;size_t n;
 return lookup(address,path,0,&p,&n,keys,count,props,mask,children,0,0,0,0,0,0);
}
int fdt_phandle_unique(uintptr_t address,const char *path,uint32_t handle)
{
 if(!handle||handle==UINT32_MAX)return FDT_PROPERTY_INVALID;
 const uint8_t *p;size_t n;
 return lookup(address,path,0,&p,&n,0,0,0,0,0,handle,0,0,0,0,0);
}
int fdt_capture_nodes(uintptr_t address,const char paths[][FDT_CAPTURE_PATH_BYTES],unsigned count,
                     struct fdt_captured_property *props,unsigned capacity,unsigned *used)
{
 if(used)*used=0;
 if(!paths||!count||count>FDT_CAPTURE_MAX_NODES||!props||!used||
    !capacity||capacity>FDT_CAPTURE_MAX_PROPERTIES)return FDT_PROPERTY_INVALID;
 for(unsigned i=0;i<count;i++) {
  unsigned n=0;while(n<FDT_CAPTURE_PATH_BYTES&&paths[i][n])n++;
  if(n==FDT_CAPTURE_PATH_BYTES||(n&&paths[i][0]!='/'))return FDT_PROPERTY_INVALID;
  for(unsigned j=0;j<i;j++)if(eq(paths[i],paths[j]))return FDT_PROPERTY_INVALID;
 }
 const uint8_t *p;size_t n;
 /* Scalar outputs are inert for capture mode; legacy matching is disabled. */
 return lookup(address,"","capture",&p,&n,0,0,0,0,0,0,paths,count,props,capacity,used);
}

/* Preserve the concurrent ADSP consumer's checked API using the same strict
 * parser. No second parser may accidentally treat a missing node as absence. */
int fdt_property_checked(uintptr_t address,const char *path,const char *key,const uint8_t **value,size_t *length)
{
 int r=fdt_property_lookup(address,path,key,value,length);
 return r<0?-1:r;
}
/* Existing positive-only callers retain their boolean contract. */
int fdt_property(uintptr_t address,const char *path,const char *key,const uint8_t **value,size_t *length)
{return fdt_property_lookup(address,path,key,value,length)==FDT_PROPERTY_PRESENT;}
