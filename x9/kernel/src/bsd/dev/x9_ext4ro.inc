/* Minimal read-only ext4 reader (4 KiB blocks, extents, 32/64-byte group
 * descriptors, linear directory scan; no inline data, no journal replay).
 * Used by the modem FSD to serve the Android NV partitions (nvdata, protect1,
 * protect2, nvcfg, md_sec) without a RAM copy. Portable: the caller supplies
 * the byte-granular read callback, so this file is also built by a host test.
 *
 * Not reentrant: uses static scratch buffers (the modem thread is the only user).
 */

struct x9e4 {
 int (*rd)(void *ctx,uint64_t off,void *buf,size_t n);   /* 0 on success */
 void *ctx;
 int ok;
 uint32_t bs,ipg,isz,dsz,ngroups;
 uint64_t blocks;
 uint32_t gdt_block;
};

#define X9E4_CACHE 32
struct x9e4_ce{const struct x9e4 *m;uint64_t blk;int valid;};
static struct x9e4_ce x9e4_ce[X9E4_CACHE];
static uint8_t x9e4_cd[X9E4_CACHE][4096];
static unsigned x9e4_rr;

static uint32_t x9e4_u16(const uint8_t *p){return p[0]|(p[1]<<8);}
static uint32_t x9e4_u32(const uint8_t *p){return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24);}

/* returns pointer to a cached 4 KiB block, or NULL */
static const uint8_t *x9e4_blk(const struct x9e4 *m,uint64_t blk){
 for(unsigned i=0;i<X9E4_CACHE;i++)if(x9e4_ce[i].valid&&x9e4_ce[i].m==m&&x9e4_ce[i].blk==blk)return x9e4_cd[i];
 if(blk>=m->blocks)return NULL;
 unsigned s=x9e4_rr++%X9E4_CACHE;
 if(m->rd(m->ctx,blk*4096ULL,x9e4_cd[s],4096)){x9e4_ce[s].valid=0;return NULL;}
 x9e4_ce[s].m=m;x9e4_ce[s].blk=blk;x9e4_ce[s].valid=1;
 return x9e4_cd[s];
}
static void x9e4_flush(void){for(unsigned i=0;i<X9E4_CACHE;i++)x9e4_ce[i].valid=0;}

static int x9e4_mount(struct x9e4 *m){
 uint8_t sb[1024];
 m->ok=0;
 if(m->rd(m->ctx,1024,sb,1024))return -1;
 if(x9e4_u16(sb+0x38)!=0xef53)return -2;
 if((1024u<<x9e4_u32(sb+0x18))!=4096)return -3;
 uint32_t incompat=x9e4_u32(sb+0x60);
 if(incompat&0x8000)return -4;                       /* inline data */
 m->bs=4096;
 m->blocks=x9e4_u32(sb+4);
 if(incompat&0x80)m->blocks|=(uint64_t)x9e4_u32(sb+0x150)<<32;
 m->ipg=x9e4_u32(sb+0x28);
 m->isz=x9e4_u16(sb+0x58);
 m->dsz=(incompat&0x80)?x9e4_u16(sb+0xfe):32;
 if(m->dsz<32)m->dsz=32;
 uint32_t inodes=x9e4_u32(sb);
 if(!m->ipg||m->isz<128||m->isz>4096)return -5;
 m->ngroups=(inodes+m->ipg-1)/m->ipg;
 m->gdt_block=x9e4_u32(sb+0x14)+1;                   /* first_data_block + 1 */
 m->ok=1;
 return 0;
}

struct x9e4_inode{uint16_t mode;uint64_t size;uint32_t flags;uint8_t iblock[60];};

static int x9e4_inode(const struct x9e4 *m,uint32_t ino,struct x9e4_inode *out){
 if(!ino)return -1;
 uint32_t g=(ino-1)/m->ipg,idx=(ino-1)%m->ipg;
 if(g>=m->ngroups)return -1;
 uint64_t go=(uint64_t)g*m->dsz;
 const uint8_t *gb=x9e4_blk(m,m->gdt_block+go/4096);
 if(!gb)return -1;
 const uint8_t *gd=gb+go%4096;
 uint64_t itab=x9e4_u32(gd+8);
 if(m->dsz>=64)itab|=(uint64_t)x9e4_u32(gd+0x28)<<32;
 uint64_t io=itab*4096ULL+(uint64_t)idx*m->isz;
 const uint8_t *ib=x9e4_blk(m,io/4096);
 if(!ib)return -1;
 const uint8_t *in=ib+io%4096;
 out->mode=x9e4_u16(in);
 out->size=x9e4_u32(in+4)|((uint64_t)x9e4_u32(in+0x6c)<<32);
 out->flags=x9e4_u32(in+0x20);
 memcpy(out->iblock,in+0x28,60);
 return 0;
}

static uint8_t x9e4_node[4096];
/* logical -> physical block; 0 = hole / unmapped, -1 error */
static int64_t x9e4_map(const struct x9e4 *m,const struct x9e4_inode *ino,uint64_t lblk){
 if(!(ino->flags&0x80000))return -1;                 /* not extents */
 uint8_t local[60];
 memcpy(local,ino->iblock,60);
 const uint8_t *node=local;
 for(int level=0;level<6;level++){
  if(x9e4_u16(node)!=0xf30a)return -1;
  unsigned entries=x9e4_u16(node+2),depth=x9e4_u16(node+6);
  if(depth==0){
   for(unsigned i=0;i<entries;i++){
    const uint8_t *e=node+12+i*12;
    uint64_t first=x9e4_u32(e);unsigned len=x9e4_u16(e+4);
    if(len>32768)len-=32768;                         /* uninitialised extent: still maps blocks */
    if(lblk>=first&&lblk<first+len)
     return (int64_t)((((uint64_t)x9e4_u16(e+6))<<32|x9e4_u32(e+8))+(lblk-first));
   }
   return 0;
  }
  uint64_t next=0;int found=0;
  for(unsigned i=0;i<entries;i++){
   const uint8_t *e=node+12+i*12;
   if(x9e4_u32(e)<=lblk){next=x9e4_u32(e+4)|((uint64_t)x9e4_u16(e+8)<<32);found=1;}
   else break;
  }
  if(!found)return 0;
  const uint8_t *b=x9e4_blk(m,next);
  if(!b)return -1;
  memcpy(x9e4_node,b,4096);
  node=x9e4_node;
 }
 return -1;
}

/* read file data; returns bytes read or <0 */
static int64_t x9e4_read(const struct x9e4 *m,const struct x9e4_inode *ino,uint64_t off,void *buf,size_t n){
 if(off>=ino->size)return 0;
 if(n>ino->size-off)n=(size_t)(ino->size-off);
 uint8_t *o=buf;size_t done=0;
 while(done<n){
  uint64_t lb=(off+done)/4096;uint32_t bo=(uint32_t)((off+done)%4096);
  size_t c=4096-bo;if(c>n-done)c=n-done;
  int64_t pb=x9e4_map(m,ino,lb);
  if(pb<0)return -1;
  if(pb==0)memset(o+done,0,c);
  else{const uint8_t *b=x9e4_blk(m,(uint64_t)pb);if(!b)return -1;memcpy(o+done,b+bo,c);}
  done+=c;
 }
 return (int64_t)done;
}

/* find name in directory; returns inode or 0 */
static uint32_t x9e4_dir_find(const struct x9e4 *m,const struct x9e4_inode *dir,const char *name,size_t nl){
 uint64_t nb=(dir->size+4095)/4096;
 for(uint64_t lb=0;lb<nb;lb++){
  int64_t pb=x9e4_map(m,dir,lb);
  if(pb<=0)continue;
  const uint8_t *b=x9e4_blk(m,(uint64_t)pb);
  if(!b)return 0;
  for(uint32_t o=0;o+8<=4096;){
   uint32_t ino=x9e4_u32(b+o),rl=x9e4_u16(b+o+4),nlen=b[o+6];
   if(rl<8||o+rl>4096)break;
   if(ino&&nlen==nl&&!memcmp(b+o+8,name,nl))return ino;
   o+=rl;
  }
 }
 return 0;
}

/* walk "/a/b/c" (leading '/' optional); fills inode */
static int x9e4_lookup(const struct x9e4 *m,const char *path,struct x9e4_inode *out,uint32_t *ino_out){
 if(!m->ok)return -1;
 uint32_t ino=2;
 struct x9e4_inode cur;
 if(x9e4_inode(m,ino,&cur))return -1;
 const char *p=path;
 while(*p){
  while(*p=='/')p++;
  if(!*p)break;
  const char *e=p;while(*e&&*e!='/')e++;
  if((cur.mode&0xf000)!=0x4000)return -2;
  uint32_t n=x9e4_dir_find(m,&cur,p,(size_t)(e-p));
  if(!n)return -2;
  ino=n;
  if(x9e4_inode(m,ino,&cur))return -1;
  p=e;
 }
 *out=cur;if(ino_out)*ino_out=ino;
 return 0;
}
