/* Kernel-owned reachability manifest, rebuilt with context and GPU locks held.
 * All live mappings participate, including command/descriptor/indirect targets.
 * Each interval is descriptor-relative: NEVER deduplicate different resources
 * by host VA or physical page. They may be shared aliases or have different
 * cache attributes. Shared memory retains explicit maintenance each submit.
 * This is a reachability bound, not a claim of CPU write exclusion or dirtiness.
 */
using RenderCacheSpan = X9RenderCacheSpan;
struct RenderCachePlan {
 RenderCacheSpan spans[2][X9_RENDER_MAX_MAPPINGS];
 unsigned count[2],first[2][X9_RENDER_MAX_MAPPINGS+1];
 uint64_t holes[2],overlap[2];
};
static bool cache_span_less(const RenderCacheSpan &a,const RenderCacheSpan &b) {
 return a.resource<b.resource||(a.resource==b.resource&&
  (a.begin<b.begin||(a.begin==b.begin&&a.end<b.end)));
}
static void cache_span_sift(RenderCacheSpan *a,unsigned root,unsigned n) {
 while(root<n/2) {
  unsigned child=root*2+1;
  if(child+1<n&&cache_span_less(a[child],a[child+1]))child++;
  if(!cache_span_less(a[root],a[child]))return;
  auto temp=a[root];a[root]=a[child];a[child]=temp;root=child;
 }
}
static void cache_span_sort(RenderCacheSpan *a,unsigned n) {
 for(unsigned i=n/2;i;i--)cache_span_sift(a,i-1,n);
 for(unsigned i=n;i>1;i--) {
  auto temp=a[0];a[0]=a[i-1];a[i-1]=temp;cache_span_sift(a,0,i-1);
 }
}
static bool render_cache_plan_build(X9RenderContext *c,RenderCachePlan *p) {
 if(!p||c->mapping_count>X9_RENDER_MAX_MAPPINGS)return false;
 p->count[0]=p->count[1]=0;
 uintptr_t base=(uintptr_t)c->resources,limit=(uintptr_t)(c->resources+X9_RENDER_MAX_MAPPINGS);
 for(unsigned i=0;i<c->mapping_count;i++) {
  const auto &m=c->mappings[i];uintptr_t ptr=(uintptr_t)m.resource;
  if(ptr<base||ptr>=limit||(ptr-base)%sizeof(X9RenderResource))return false;
  unsigned id=(unsigned)((ptr-base)/sizeof(X9RenderResource));
  const auto &r=c->resources[id];
  if(!r.memory||!r.prepared||!m.bytes||!m.flags||(m.flags&~r.flags)||
     (m.flags&~(X9RenderGPURead|X9RenderGPUWrite|X9RenderGPUExecute))||
     ((m.offset|m.bytes)&(X9_RENDER_CACHE_ATOM-1))||m.offset>r.bytes||m.bytes>r.bytes-m.offset)return false;
  RenderCacheSpan span={m.offset,m.offset+m.bytes,id};
  p->spans[0][p->count[0]++]=span;
  if(m.flags&X9RenderGPUWrite)p->spans[1][p->count[1]++]=span;
 }
 for(unsigned phase=0;phase<2;phase++) {
  auto *a=p->spans[phase];unsigned n=p->count[phase],merged=0;
  p->holes[phase]=p->overlap[phase]=0;
  cache_span_sort(a,n);
  for(unsigned i=0;i<n;i++) {
   auto next=a[i];
   if(merged&&a[merged-1].resource==next.resource&&next.begin<=a[merged-1].end) {
    auto &last=a[merged-1];
    uint64_t shared=(next.end<last.end?next.end:last.end)-next.begin;
    p->overlap[phase]+=shared;
    if(next.end>last.end)last.end=next.end;
   }else a[merged++]=next;
  }
  p->count[phase]=merged;
  unsigned at=0;
  for(unsigned id=0;id<X9_RENDER_MAX_MAPPINGS;id++) {
   p->first[phase][id]=at;
   uint64_t bytes=0;
   while(at<merged&&a[at].resource==id){bytes+=a[at].end-a[at].begin;at++;}
   const auto &r=c->resources[id];
   uint64_t begin=phase?r.write_begin:r.cache_begin,end=phase?r.write_end:r.cache_end;
   uint64_t bounding=end>begin?end-begin:0;
   if(bytes>bounding)return false;
   /* Match executed shared-resource work; private zero-fill remains whole. */
   if(r.memory&&r.cpu_exposed)p->holes[phase]+=bounding-bytes;
  }
  p->first[phase][X9_RENDER_MAX_MAPPINGS]=at;
  if(at!=merged)return false;
 }
 return true;
}
static IOReturn render_cache_plan_resource(X9RenderContext *c,RenderCachePlan *p,
 X9RenderResource &r,unsigned phase,uint64_t *bytes) {
 unsigned id=(unsigned)(&r-c->resources);
 if(phase>1||id>=X9_RENDER_MAX_MAPPINGS)return kIOReturnBadArgument;
 unsigned first=p->first[phase][id],last=p->first[phase][id+1];
 if(first>last||last>p->count[phase])return kIOReturnBadArgument;
 return x9_render_memory_cache_spans(c,&r,p->spans[phase]+first,last-first,
  X9RenderClean|X9RenderInvalidate,bytes);
}
