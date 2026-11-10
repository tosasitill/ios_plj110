/* Native AudioServerPlugIn publication, following UniXNU's sheng audio union
 * view and HakiOS's System HAL discovery path. The full signed own-code bundle
 * is embedded in boot; no mailbox, Apple bundle replacement or DYLD injection.
 * Content-addressed storage lives outside HAL discovery. A fresh boot mounts
 * only this image's version, so old versions do not add duplicate Actuator UIDs.
 */
#include <sys/dirent.h>
#include "x9_native_hal_view.inc"
#include "../../../generated/x9_haptic_hal.inc"
extern int sweet_x9_haptic_mode(void);
static thread_t x9_haptic_mount_owner;
static bool x9_haptic_hal_mounted;
int sweet_x9_haptic_boot_mount_allowed(const char *source);
int sweet_x9_haptic_boot_mount_allowed(const char *source)
{
    return __atomic_load_n(&x9_haptic_mount_owner,__ATOMIC_ACQUIRE)==current_thread() &&
        proc_getpid(current_proc())==1 && sweet_early_is_x9() &&
        sweet_x9_haptic_mode()==2 && sweet_x9_ufs_root_active() &&
        sweet_x9_ufs_root_writable() && !strcmp(source,X9_HAPTIC_HAL_SOURCE);
}

/* Refuse extra entries in the mounted source (including stale temp files).
 * Otherwise a checked four-file manifest could accidentally publish a fifth
 * unrelated HAL driver. Root ownership is required for every private directory.
 * Keep the scan bounded and use the native 64-bit directory record layout.
 */
static int x9_haptic_directory(const char *path,const char *const *names,unsigned count)
{
    if(!names || !count || count>16)return EINVAL;
    vnode_t vp=NULLVP;vfs_context_t ctx=vfs_context_kernel();
    int e=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&vp,ctx);if(e)return e;
    if(!vnode_isdir(vp)){vnode_put(vp);return ENOTDIR;}
    struct vnode_attr a;VATTR_INIT(&a);VATTR_WANTED(&a,va_mode);
    VATTR_WANTED(&a,va_uid);VATTR_WANTED(&a,va_gid);
    e=vnode_getattr(vp,&a,ctx);
    if(!e && ((a.va_mode&07777)!=0755 || a.va_uid || a.va_gid))e=EPERM;
    unsigned seen=0;off_t offset=0;int eof=0;
    for(unsigned pass=0;!e && !eof && pass<16;pass++) {
        char data[2048];uio_t u=uio_create(1,offset,UIO_SYSSPACE,UIO_READ);
        if(!u){e=ENOMEM;break;}
        e=uio_addiov(u,CAST_USER_ADDR_T(data),sizeof(data));int entries=0;
        if(!e)e=vnode_readdir64(vp,u,VNODE_READDIR_EXTENDED,&eof,&entries,ctx);
        user_ssize_t residual=uio_resid(u);off_t next=uio_offset(u);
        if(!e && (residual<0 || (uint64_t)residual>sizeof(data)))e=EIO;
        size_t bytes=e?0:sizeof(data)-(size_t)residual;
        uio_free(u);
        if(e)break;
        if(!bytes && !eof){e=EIO;break;}
        for(size_t off=0;!e && off<bytes;) {
            struct direntry *d=(struct direntry *)(void *)(data+off);
            size_t head=offsetof(struct direntry,d_name);
            if(bytes-off<head || (off&7) || d->d_reclen<head+1 || (d->d_reclen&7) || d->d_reclen>bytes-off ||
               d->d_namlen>=d->d_reclen-head || d->d_name[d->d_namlen] ||
               strnlen(d->d_name,d->d_namlen)!=d->d_namlen){e=EIO;break;}
            if(d->d_ino && strcmp(d->d_name,".") && strcmp(d->d_name,"..")) {
                unsigned i=0;for(;i<count;i++)if(!strcmp(d->d_name,names[i]))break;
                if(i==count || (seen&(1u<<i))){e=EEXIST;break;}
                seen|=1u<<i;
            }
            off+=d->d_reclen;
        }
        if(!e && !eof && next==offset)e=EIO;
        offset=next;
    }
    if(!e && (!eof || seen!=((1u<<count)-1)))e=EIO;
    vnode_put(vp);return e;
}
static int x9_haptic_bundle_equal(const char *base)
{
    for(unsigned i=0;i<sizeof(x9_haptic_hal_files)/sizeof(x9_haptic_hal_files[0]);i++) {
        const struct x9_haptic_hal_file *f=&x9_haptic_hal_files[i];char path[256];
        int n=snprintf(path,sizeof(path),"%s/X9Haptic.driver/%s",base,f->name);
        if(n<0||(size_t)n>=sizeof(path))return E2BIG;
        int e=x9_bbfw_equal(path,f->sha,f->bytes);if(e)return e;
    }
    return 0;
}
static int x9_haptic_install(proc_t p,user_addr_t scratch,user_addr_t end)
{
    if(!sweet_early_is_x9() || proc_getpid(p)!=1 || sweet_x9_haptic_mode()!=2)return 0;
    if(x9_haptic_hal_mounted)return 0;
    if(!proc_is64bit(p) || !sweet_x9_ufs_root_active() || !sweet_x9_ufs_root_writable())return EROFS;
    struct x9_native_hal_view *lower=NULL;vfs_context_t ctx=vfs_context_kernel();
    int e=x9_native_hal_capture(X9_HAPTIC_HAL_POINT,&lower);
    if(e){printf("X9 HAPTIC HAL: native view capture errno=%d\n",e);return e;}
    char path[256];
    int n=snprintf(path,sizeof(path),"%s/X9Haptic.driver",X9_HAPTIC_HAL_POINT);
    vnode_t conflict=NULLVP;
    if(n<0||(size_t)n>=sizeof(path)){e=E2BIG;goto out;}
    e=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&conflict,ctx);
    if(!e){vnode_put(conflict);e=EEXIST;goto out;}
    if(e!=ENOENT)goto out;
    unsigned created=0;
    /* Validate every embedded input before the first root write. */
    for(unsigned i=0;i<sizeof(x9_haptic_hal_files)/sizeof(x9_haptic_hal_files[0]);i++) {
        const struct x9_haptic_hal_file *f=&x9_haptic_hal_files[i];uint8_t sum[32];SHA256_CTX h;
        SHA256_Init(&h);SHA256_Update(&h,f->data,f->bytes);SHA256_Final(sum,&h);
        if(memcmp(sum,f->sha,32)){e=EIO;goto out;}
    }
    for(unsigned i=0;i<sizeof(x9_haptic_hal_files)/sizeof(x9_haptic_hal_files[0]);i++) {
        const struct x9_haptic_hal_file *f=&x9_haptic_hal_files[i];
        n=snprintf(path,sizeof(path),"%s/X9Haptic.driver/%s",X9_HAPTIC_HAL_SOURCE,f->name);
        if(n<0||(size_t)n>=sizeof(path)){e=E2BIG;goto out;}
        char parent[256];strlcpy(parent,path,sizeof(parent));*strrchr(parent,'/')=0;
        e=x9_bbfw_directory(p,scratch,end,parent);if(e)goto out;
        int made=0;e=x9_bbfw_write(p,scratch,end,path,f->data,f->bytes,f->sha,&made);
        if(e)goto out;created+=(unsigned)made;
    }
    static const char *const root_names[]={"X9Haptic.driver"};
    static const char *const bundle_names[]={"Info.plist","LICENSE.hakios","X9Haptic","_CodeSignature"};
    static const char *const signature_names[]={"CodeResources"};
    e=x9_haptic_directory(X9_HAPTIC_HAL_SOURCE,root_names,1);if(e)goto out;
    n=snprintf(path,sizeof(path),"%s/X9Haptic.driver",X9_HAPTIC_HAL_SOURCE);
    if(n<0||(size_t)n>=sizeof(path)){e=E2BIG;goto out;}
    e=x9_haptic_directory(path,bundle_names,4);if(e)goto out;
    strlcat(path,"/_CodeSignature",sizeof(path));
    e=x9_haptic_directory(path,signature_names,1);if(e)goto out;
    e=x9_haptic_bundle_equal(X9_HAPTIC_HAL_SOURCE);if(e)goto out;
    {
        size_t pn=sizeof(X9_HAPTIC_HAL_POINT),sn=sizeof(X9_HAPTIC_HAL_SOURCE);
        if(scratch>end || 7+pn+sn>end-scratch){e=E2BIG;goto out;}
        struct mount_args m={.type=scratch,.path=scratch+7,
            .flags=MNT_RDONLY|MNT_UNION,.data=scratch+7+pn};int32_t rv=0;
        if((e=copyout("bindfs",m.type,7)) || (e=copyout(X9_HAPTIC_HAL_POINT,m.path,pn)) ||
           (e=copyout(X9_HAPTIC_HAL_SOURCE,m.data,sn)))goto out;
        __atomic_store_n(&x9_haptic_mount_owner,current_thread(),__ATOMIC_RELEASE);
        e=mount(p,&m,&rv);
        __atomic_store_n(&x9_haptic_mount_owner,NULL,__ATOMIC_RELEASE);
        if(e)goto out;
    }
    /* Authoritative native discovery view, not a source-only check. */
    e=x9_haptic_bundle_equal(X9_HAPTIC_HAL_POINT);
    if(!e)e=x9_native_hal_verify(X9_HAPTIC_HAL_POINT,lower);
    if(e) {
        /* Do not leave a failed/partly verified view covering the native HAL. */
        int ce=copyout(X9_HAPTIC_HAL_POINT,scratch,sizeof(X9_HAPTIC_HAL_POINT));
        if(!ce){struct unmount_args u={.path=scratch,.flags=0};int32_t rv=0;ce=unmount(p,&u,&rv);}
        printf("X9 HAPTIC HAL: union verification errno=%d rollback=%d\n",e,ce);
    } else {
        x9_haptic_hal_mounted=true;
        printf("X9 HAPTIC HAL: signed bundle native union visible created=%u lower-preserved=1 native-entries=%u; physical playback verification separate\n",created,lower->count);
    }
out:
    x9_native_hal_release(lower);return e;
}
