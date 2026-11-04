/* Read-only copy of the live LK FDT facts the Wi-Fi bring-up needs, handed to
 * XNU as text in the ADT placeholder /chosen/x9-lk-wifi-dt (mkadt.py). Shim
 * output past the first lines does not reach the device logs, and LK's FDT is
 * not protected once XNU runs, so the kernel prints this copy instead.
 *
 * Lines (each '\n' terminated):
 *   R <reserved-memory child> reg=<hex>              every /reserved-memory node
 *   N <path> <key>=<string|hex>                       every property of the nodes
 *                                                     named pcie@169*, phy@169*,
 *                                                     connv3*, wifi*, connsys-emi*
 * Values longer than 48 bytes are cut and marked "+". Any malformed FDT or a
 * missing placeholder just leaves the placeholder untouched: never a boot
 * failure. No device register is read or written. */
#include "fdt.h"
#include <stddef.h>
#include <stdint.h>

#define DUMP_SIZE 16384u
#define VALUE_MAX 48u

static uint32_t be(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}

struct out { uint8_t *p; size_t used, cap; int full; };
static void put(struct out *o, char c) {
 if (o->used + 2 >= o->cap) { o->full = 1; return; }   /* keep room for the final NUL */
 o->p[o->used++] = (uint8_t)c;
}
static void puts_(struct out *o, const char *s, size_t n) { for (size_t i = 0; i < n && s[i]; i++) put(o, s[i]); }
static void putz(struct out *o, const char *s) { while (*s) put(o, *s++); }
static void puthex(struct out *o, const uint8_t *v, size_t n) {
 static const char h[] = "0123456789abcdef";
 size_t m = n > VALUE_MAX ? VALUE_MAX : n;
 for (size_t i = 0; i < m; i++) { if (i && !(i & 3)) put(o, '.'); put(o, h[v[i] >> 4]); put(o, h[v[i] & 15]); }
 if (m < n) put(o, '+');
}
static int printable(const uint8_t *v, size_t n) {
 if (!n || v[n-1] || !v[0]) return 0;   /* zero words are values, not empty strings */
 for (size_t i = 0; i + 1 < n; i++) if (v[i] && (v[i] < 32 || v[i] > 126)) return 0;
 return 1;
}
static void putval(struct out *o, const uint8_t *v, size_t n) {
 if (printable(v, n)) {
  put(o, '"');
  size_t m = n > VALUE_MAX ? VALUE_MAX : n;
  for (size_t i = 0; i + 1 < m; i++) put(o, v[i] ? (char)v[i] : ',');
  put(o, '"'); if (m < n) put(o, '+');
 } else puthex(o, v, n);
}
static int prefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int wanted(const char *name) {
 return prefix(name, "pcie@169") || prefix(name, "phy@169") || prefix(name, "connv3") ||
        prefix(name, "wifi") || prefix(name, "connsys-emi");
}

/* Bounded walk; returns 0 and leaves o untouched on any inconsistency. */
static int walk(uintptr_t address, struct out *o) {
 if (!address || (address & 3)) return 0;
 const uint8_t *b = (const uint8_t *)address; if (be(b) != 0xd00dfeed) return 0;
 uint32_t total=be(b+4),start=be(b+8),strings=be(b+12),ss=be(b+32),sz=be(b+36);
 if (total<40||total>0x400000||start>total||sz>total-start||strings>total||ss>total-strings) return 0;
 size_t at=start,end=(size_t)start+sz,used=0,parents[32];unsigned depth=0;
 char path[256]={0}; const char *name[32]; int dump[32], reserved[32];
 while (end-at>=4) {
  unsigned t=be(b+at);at+=4;
  if (t==1) {
   if (depth==32) return 0;
   size_t first=at;while(at<end&&b[at])at++;if(at==end)return 0;
   parents[depth]=used; name[depth]=(const char *)b+first;
   if (depth) {
    size_t n=at-first;if(used+1+n>=sizeof(path))return 0;
    path[used++]='/';for(size_t i=0;i<n;i++)path[used++]=(char)b[first+i];path[used]=0;
   }
   reserved[depth] = depth==2 && prefix(path, "/reserved-memory/");
   dump[depth] = depth && !reserved[depth] && wanted(name[depth]);
   depth++;at=(at+4)&~(size_t)3;
  } else if (t==2) {
   if (!depth) return 0;
   used = parents[--depth]; path[used] = 0;
  } else if (t==3) {
   if(!depth||end-at<8)return 0;
   uint32_t n=be(b+at),offset=be(b+at+4);at+=8;
   if (n>end-at||offset>=ss) return 0;
   const char *key=(const char *)b+strings+offset;
   size_t limit=ss-offset,i=0;while(i<limit&&key[i])i++;if(i==limit)return 0;
   unsigned d=depth-1;
   if (reserved[d] && key[0]=='r' && key[1]=='e' && key[2]=='g' && !key[3]) {
    putz(o,"R "); putz(o,name[d]); putz(o," reg="); puthex(o,b+at,n); put(o,'\n');
   } else if (reserved[d] && prefix(key,"size") && !key[4]) {
    putz(o,"R "); putz(o,name[d]); putz(o," size="); puthex(o,b+at,n); put(o,'\n');
   } else if (dump[d]) {
    putz(o,"N "); puts_(o,path,sizeof(path)); put(o,' '); putz(o,key); put(o,'=');
    putval(o,b+at,n); put(o,'\n');
   }
   at=(at+n+3)&~(size_t)3;
  } else if (t==4) continue;
  else if (t==9&&!depth) return 1;
  else return 0;
  if (at>end) return 0;
 }
 return 0;
}

/* Find /chosen/x9-lk-wifi-dt in the copied Apple DT (little endian). */
struct adt_walk { uint8_t *p; size_t size, at, found; unsigned nodes, matches; uint32_t len; };
static int eqk(const uint8_t *k, const char *s) { for (unsigned i=0;i<32;i++){ if(k[i]!=(uint8_t)s[i])return 0; if(!s[i])return 1;} return 0; }
static int adt_node(struct adt_walk *w, unsigned depth) {
 if (depth>32 || ++w->nodes>1024 || w->size-w->at<8) return -1;
 uint32_t props=le32(w->p+w->at),children=le32(w->p+w->at+4);w->at+=8;
 if (props>1024||children>1024) return -1;
 unsigned chosen=0;
 for (unsigned i=0;i<props;i++) {
  if (w->size-w->at<36) return -1;
  uint8_t *key=w->p+w->at;uint32_t n=le32(key+32)&0x7fffffff;w->at+=36;
  size_t padded=((size_t)n+3)&~(size_t)3;
  if (padded>w->size-w->at) return -1;
  if (eqk(key,"name")) chosen = depth==1 && n>=7 && eqk(w->p+w->at,"chosen");
  if (chosen && eqk(key,"x9-lk-wifi-dt")) { w->found=w->at; w->len=n; w->matches++; }
  w->at+=padded;
 }
 for (unsigned i=0;i<children;i++) if (adt_node(w,depth+1)) return -1;
 return 0;
}

/* 0: written, 1: no placeholder (older ADT), -1: FDT unusable (placeholder untouched). */
int fdt_wifi_dump_adt(uintptr_t dtb, void *adt, size_t size) {
 struct adt_walk w = {(uint8_t *)adt, size, 0, 0, 0, 0, 0};
 if (adt_node(&w,0) || w.at!=size || w.matches!=1 || w.len!=DUMP_SIZE) return 1;
 uint8_t *p = w.p + w.found;
 /* header: "X9DT", u32 used; text follows at +8 */
 struct out o = { p + 8, 0, DUMP_SIZE - 8, 0 };
 if (!walk(dtb, &o)) return -1;
 if (o.full) { o.used = o.used > 2 ? o.used - 2 : 0; p[8 + o.used++] = '+'; p[8 + o.used++] = '\n'; }
 p[8 + o.used] = 0;
 p[0]='X';p[1]='9';p[2]='D';p[3]='T';
 p[4]=(uint8_t)o.used;p[5]=(uint8_t)(o.used>>8);p[6]=(uint8_t)(o.used>>16);p[7]=0;
 return 0;
}
