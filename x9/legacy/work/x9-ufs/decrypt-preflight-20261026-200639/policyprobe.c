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

static int fail(const char *stage,long error)
{print("{\"error_stage\":\"");print(stage);print("\",\"errno\":");number(error<0?(u64)-error:(u64)error);print("}\n");return 1;}
struct extent {u64 logical,physical,length,reserved64[2];u32 flags,reserved[3];};
struct map {u64 start,length;u32 flags,mapped,count,reserved;struct extent extents[4096];};
_Static_assert(sizeof(struct extent)==56,"FIEMAP extent ABI");
_Static_assert(__builtin_offsetof(struct map,extents)==32,"FIEMAP header ABI");



int program(long argc,char **argv)
{
 if(argc!=2)return fail("usage: FILE",22);
 long fd=call(56,-100,(long)argv[1],0x20000|0x80000,0,0,0);
 if(fd<0)return fail("open readonly",fd);
 u8 policy[32]={0};*(u64 *)policy=24;
 long e=call(29,fd,0xc0096616UL,(long)policy,0,0,0);
 if(e<0){call(57,fd,0,0,0,0,0);return fail("GET_ENCRYPTION_POLICY_EX",e);}
 print("{\"version\":");number(policy[8]);print(",\"content_mode\":");number(policy[9]);
 print(",\"filename_mode\":");number(policy[10]);print(",\"flags\":");number(policy[11]);
 print(",\"log2_data_unit_size\":");number(policy[8]==2?policy[12]:0);
 u8 nonce[16]={0};e=call(29,fd,0x8010661bUL,(long)nonce,0,0,0);call(57,fd,0,0,0,0,0);
 print(",\"nonce_available\":");print(e<0?"false":"true");print("}\n");return 0;
}
