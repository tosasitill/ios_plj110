/* Minimal Android arm64 pin/FIEMAP qualification helper. No raw devices,
 * deletion, reboot, unpin, or writes to existing files. Linux syscalls only.
 * create: O_EXCL new 8MiB file -> SET_PIN -> fallocate -> write zeros -> fsync.
 * inspect: O_RDONLY, GETFLAGS/GET_PIN/FIEMAP; no content write.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef long s64;
_Static_assert(sizeof(u64)==8,"arm64 ABI");
static long call(long nr,long a,long b,long c,long d,long e,long f)
{
 register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
 register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x5 __asm__("x5")=f;
 register long x8 __asm__("x8")=nr;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory");
 return x0;
}
static void print(const char *s)
{u64 n=0;while(s[n])n++;while(n){long r=call(64,1,(long)s,n,0,0,0);if(r<=0)break;s+=r;n-=r;}}
static void number(u64 n)
{char b[32];unsigned i=sizeof(b);b[--i]=0;do{b[--i]='0'+n%10;n/=10;}while(n);print(b+i);}
static int eq(const char *a,const char *b)
{while(*a&&*a==*b){a++;b++;}return *a==*b;}
static int fail(const char *stage,long error)
{print("{\"error_stage\":\"");print(stage);print("\",\"errno\":");number(error<0?(u64)-error:(u64)error);print("}\n");return 1;}
struct extent {u64 logical,physical,length,reserved64[2];u32 flags,reserved[3];};
struct map {u64 start,length;u32 flags,mapped,count,reserved;struct extent extents[4096];};
_Static_assert(sizeof(struct extent)==56,"FIEMAP extent ABI");
_Static_assert(__builtin_offsetof(struct map,extents)==32,"FIEMAP header ABI");
static struct map mapping;
static u8 zeros[65536];

int program(long argc,char **argv)
{
 if(argc!=3||(!eq(argv[1],"create")&&!eq(argv[1],"inspect")))return fail("usage: create|inspect FILE",22);
 const char *path=argv[2];
 /* Require an absolute /data/local/tmp/... path; never accept a device node. */
 const char prefix[]="/data/local/tmp/";
 for(unsigned i=0;i<sizeof(prefix)-1;i++)if(path[i]!=prefix[i])return fail("path outside test directory",22);
 int created=eq(argv[1],"create");
 long fd=call(56,-100,(long)path,created?(2|64|128|0x20000|0x80000):(0|0x20000|0x80000),0600,0,0);
 if(fd<0)return fail("open (create is exclusive)",fd);
 long error=0;
 if(created) {
  u32 pin=1;error=call(29,fd,0x4004f50dUL,(long)&pin,0,0,0);
  if(error<0){call(57,fd,0,0,0,0,0);return fail("SET_PIN_FILE",error);}
  error=call(47,fd,0,0,8UL<<20,0,0);
  if(error<0){call(57,fd,0,0,0,0,0);return fail("fallocate 8MiB",error);}
  for(u64 offset=0;offset<(8UL<<20);offset+=sizeof(zeros)) {
   u64 done=0;
   while(done<sizeof(zeros)) {
    error=call(68,fd,(long)(zeros+done),sizeof(zeros)-done,offset+done,0,0);
    if(error<=0){call(57,fd,0,0,0,0,0);return fail("pwrite new-file zeros",error?error:-5);}
    done+=(u64)error;
   }
  }
  error=call(82,fd,0,0,0,0,0);
  if(error<0){call(57,fd,0,0,0,0,0);return fail("fsync",error);}
 }
 u64 flags=0;u32 gc=0;u8 statbuf[128]={0};
 error=call(29,fd,0x80086601UL,(long)&flags,0,0,0);
 if(error<0){call(57,fd,0,0,0,0,0);return fail("GETFLAGS",error);}
 error=call(29,fd,0x8004f50eUL,(long)&gc,0,0,0);
 if(error<0){call(57,fd,0,0,0,0,0);return fail("GET_PIN_FILE",error);}
 error=call(80,fd,(long)statbuf,0,0,0,0);
 if(error<0){call(57,fd,0,0,0,0,0);return fail("fstat",error);}
 mapping.start=0;mapping.length=~0UL;mapping.flags=1;mapping.count=4096;
 error=call(29,fd,0xc020660bUL,(long)&mapping,0,0,0);
 call(57,fd,0,0,0,0,0);
 if(error<0)return fail("FIEMAP",error);
 if(!mapping.mapped||mapping.mapped>=4096)return fail("incomplete FIEMAP",75);
 /* Kernel-produced structure; print no filenames, file data or crypto fields. */
 print("{\"created\":");print(created?"true":"false");
 print(",\"size\":");number(*(u64 *)(statbuf+48));
 print(",\"allocated_512_blocks\":");number(*(u64 *)(statbuf+64));
 print(",\"inode\":");number(*(u64 *)(statbuf+8));
 print(",\"flags\":");number(flags);
 print(",\"pinned\":");print((flags&0x00800000UL)?"true":"false");
 print(",\"gc_move_failures\":");number(gc);
 print(",\"extents\":[");
 for(unsigned i=0;i<mapping.mapped;i++) {
  struct extent *e=&mapping.extents[i];if(i)print(",");
  print("[");number(e->logical);print(",");number(e->physical);print(",");number(e->length);print(",");number(e->flags);print("]");
 }
 print("]}\n");return 0;
}
