/* Read-only SHA256 comparison of pinned raw extents, 256 KiB groups. */
#define main pinfile_original_main
#include "../x9-ufs/plaintext-root-20261026-202513/pinfile.c"
#undef main
int main(int argc,char **argv)
{
 if(argc!=4){fprintf(stderr,"usage: rawdiff FILE DEV DIGESTS\n");return 2;}
 int ref=open(argv[3],O_RDONLY|O_CLOEXEC),dev=open(argv[2],O_RDONLY|O_DIRECT|O_CLOEXEC);
 if(ref<0||dev<0)return die("open read-only inputs");
 int save=dup(1);if(save<0||dup2(2,1)<0)return die("dup");
 int bad=checked_layout(argv[1],argv[2],dev);fflush(stdout);
 if(dup2(save,1)<0)return die("restore stdout");close(save);if(bad)return 1;
 struct stat st;if(stat(argv[1],&st)||st.st_size<=0||(uint64_t)st.st_size%(64u*BLK))return fail("file size not 256K aligned");
 uint8_t *buf;if(posix_memalign((void**)&buf,BLK,CHUNK))return die("alloc");
 struct sha256 hash;sha256_init(&hash);uint64_t done=0,groups=0,changed=0,within=0;
 for(int i=0;i<nL;i++)for(uint64_t off=0;off<L[i].length;){
  uint64_t n=L[i].length-off;if(n>64u*BLK-within)n=64u*BLK-within;
  if(pread(dev,buf,n,L[i].physical+off)!=(ssize_t)n)return die("pread raw");
  sha256_update(&hash,buf,n);off+=n;done+=n;within+=n;
  if(within==64u*BLK){uint8_t want[32],got[32];if(read_full(ref,want,32)!=32)return fail("short digest list");sha256_final(&hash,got);
   if(memcmp(want,got,32)){printf("%llu ",(unsigned long long)(done-within));for(int j=0;j<32;j++)printf("%02x",got[j]);putchar('\n');changed++;}
   groups++;within=0;sha256_init(&hash);
  }
 }
 uint8_t extra;if(within||read_full(ref,&extra,1)!=0||(uint64_t)st.st_size!=done)return fail("bad size or digest list");
 fprintf(stderr,"rawdiff complete: %llu bytes, %llu groups, %llu different\n",(unsigned long long)done,(unsigned long long)groups,(unsigned long long)changed);free(buf);close(ref);close(dev);return ferror(stdout)?1:0;
}
