/* Own-code signed output HAL publication. spk28: default ON with x9_audio=1/2
 * (user 2026-11-14: "好的功能直接默认开启"); x9_speaker_hal=0 disables it.
 * No Apple replacement, DYLD injection, mailbox or hardware activation.
 * Included after MIC/haptic: x9_microphone_directory is the existing generic
 * bounded exact-directory checker, with no microphone state or operations. */
#include "../dev/x9_speaker_hal.h"
#include "x9_native_hal_view.inc"
static thread_t x9_speaker_mount_owner;
static bool x9_speaker_hal_attempted;
static int x9_speaker_hal_error;
static int x9_speaker_hal_mode(void)
{
    int mode=sweet_x9_arg("x9_audio",0);
    return sweet_early_is_x9() && (mode==1 || mode==2) &&
        sweet_x9_arg("x9_speaker_hal",1)==1;
}
int sweet_x9_speaker_boot_mount_allowed(const char *source);
int sweet_x9_speaker_boot_mount_allowed(const char *source)
{
    return source && __atomic_load_n(&x9_speaker_mount_owner,__ATOMIC_ACQUIRE)==current_thread() &&
        proc_getpid(current_proc())==1 && x9_speaker_hal_mode() &&
        sweet_x9_ufs_root_active() && sweet_x9_ufs_root_writable() &&
        !strcmp(source,x9_speaker_hal_source);
}
static int x9_speaker_manifest(void)
{
    static const char *const names[X9_SPEAKER_HAL_FILES]={
        "Info.plist","LICENSE.UniXNU","X9Audio","_CodeSignature/CodeResources"};
    SHA256_CTX identity;SHA256_Init(&identity);uint32_t total=0;
    for(unsigned i=0;i<X9_SPEAKER_HAL_FILES;i++) {
        const struct x9_speaker_hal_file *f=&x9_speaker_hal_files[i];
        if(!f->name || strcmp(f->name,names[i]) || !f->data || !f->bytes ||
           f->bytes>X9_SPEAKER_HAL_MAX_BYTES-total)return EINVAL;
        total+=f->bytes;SHA256_CTX hash;uint8_t digest[32],length[8];
        SHA256_Init(&hash);SHA256_Update(&hash,f->data,f->bytes);SHA256_Final(digest,&hash);
        if(memcmp(digest,f->sha,sizeof(digest)))return EIO;
        for(unsigned j=0;j<8;j++)length[j]=(uint8_t)((uint64_t)f->bytes>>(j*8));
        SHA256_Update(&identity,f->name,strlen(f->name)+1);
        SHA256_Update(&identity,length,sizeof(length));
        SHA256_Update(&identity,digest,sizeof(digest));
    }
    uint8_t digest[32];SHA256_Final(digest,&identity);
    char hex[65],expected[X9_SPEAKER_HAL_SOURCE_CAP];
    static const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){hex[i*2]=digits[digest[i]>>4];hex[i*2+1]=digits[digest[i]&15];}
    hex[64]=0;
    int n=snprintf(expected,sizeof(expected),"%s%s/hal",X9_SPEAKER_HAL_PREFIX,hex);
    if(n<0 || (size_t)n>=sizeof(expected) ||
       strnlen(x9_speaker_hal_source,sizeof(x9_speaker_hal_source))!=strlen(expected) ||
       strcmp(x9_speaker_hal_source,expected))return EINVAL;
    for(unsigned i=(unsigned)n+1;i<sizeof(x9_speaker_hal_source);i++)
        if(x9_speaker_hal_source[i])return EINVAL;
    return 0;
}
static int x9_speaker_bundle_equal(const char *base)
{
    for(unsigned i=0;i<X9_SPEAKER_HAL_FILES;i++) {
        const struct x9_speaker_hal_file *f=&x9_speaker_hal_files[i];char path[256];
        int n=snprintf(path,sizeof(path),"%s/X9Audio.driver/%s",base,f->name);
        if(n<0 || (size_t)n>=sizeof(path))return E2BIG;
        int e=x9_bbfw_equal(path,f->sha,f->bytes);if(e)return e;
    }
    return 0;
}
/* Check each ancestor before creating anything below it. The shared /private
 * and /private/var directories are not chmod'ed; every speaker-private level
 * must already have, or be created with, root:wheel/0755 permissions. */
static int x9_speaker_directory(proc_t p,user_addr_t scratch,user_addr_t end,const char *path)
{
    char parent[256];size_t n=strlen(path);
    if(n>=sizeof(parent) || strncmp(path,X9_SPEAKER_HAL_PREFIX,
       sizeof(X9_SPEAKER_HAL_PREFIX)-1))return EINVAL;
    memcpy(parent,path,n+1);
    for(size_t i=1;i<=n;i++)if(parent[i]=='/' || !parent[i]) {
        char saved=parent[i];parent[i]=0;
        int e=x9_update_directory(p,scratch,end,parent);
        if(!e) {
            vnode_t vp=NULLVP;vfs_context_t ctx=vfs_context_kernel();
            e=vnode_lookup(parent,VNODE_LOOKUP_NOFOLLOW,&vp,ctx);
            if(!e) {
                struct vnode_attr a;VATTR_INIT(&a);
                VATTR_WANTED(&a,va_mode);VATTR_WANTED(&a,va_uid);VATTR_WANTED(&a,va_gid);
                e=vnode_getattr(vp,&a,ctx);
                if(!e && (!vnode_isdir(vp) || !VATTR_IS_SUPPORTED(&a,va_mode) ||
                   !VATTR_IS_SUPPORTED(&a,va_uid) || !VATTR_IS_SUPPORTED(&a,va_gid) ||
                   a.va_uid || a.va_gid || (a.va_mode&0022) ||
                   (i>=sizeof("/private/var/x9")-1 && (a.va_mode&07777)!=0755)))e=EPERM;
                vnode_put(vp);
            }
        }
        parent[i]=saved;if(e)return e;
    }
    return 0;
}
static int x9_speaker_install(proc_t p,user_addr_t scratch,user_addr_t end)
{
    if(!x9_speaker_hal_mode() || proc_getpid(p)!=1)return 0;
    if(x9_speaker_hal_attempted)return x9_speaker_hal_error;
    x9_speaker_hal_attempted=true;
    int e=0;struct x9_native_hal_view *lower=NULL;vfs_context_t ctx=vfs_context_kernel();
    if(!proc_is64bit(p) || !sweet_x9_ufs_root_active() || !sweet_x9_ufs_root_writable()){e=EROFS;goto out;}
    /* Check names, every resource and the content-addressed source identity
     * before any root write. This is not DSP/profile/PA readiness. */
    e=x9_speaker_manifest();if(e)goto out;
    e=x9_native_hal_capture(X9_SPEAKER_HAL_POINT,&lower);if(e)goto out;
    char path[256];int n=snprintf(path,sizeof(path),"%s/X9Audio.driver",X9_SPEAKER_HAL_POINT);
    if(n<0 || (size_t)n>=sizeof(path)){e=E2BIG;goto out;}
    vnode_t conflict=NULLVP;e=vnode_lookup(path,VNODE_LOOKUP_NOFOLLOW,&conflict,ctx);
    if(!e){vnode_put(conflict);e=EEXIST;goto out;}
    if(e!=ENOENT)goto out;
    unsigned created=0;
    for(unsigned i=0;i<X9_SPEAKER_HAL_FILES;i++) {
        const struct x9_speaker_hal_file *f=&x9_speaker_hal_files[i];
        n=snprintf(path,sizeof(path),"%s/X9Audio.driver/%s",x9_speaker_hal_source,f->name);
        if(n<0 || (size_t)n>=sizeof(path)){e=E2BIG;goto out;}
        char parent[256];strlcpy(parent,path,sizeof(parent));*strrchr(parent,'/')=0;
        e=x9_speaker_directory(p,scratch,end,parent);if(e)goto out;
        int made=0;e=x9_bbfw_write(p,scratch,end,path,f->data,f->bytes,f->sha,&made);
        if(e)goto out;created+=(unsigned)made;
    }
    static const char *const root_names[]={"X9Audio.driver"};
    static const char *const bundle_names[]={"Info.plist","LICENSE.UniXNU","X9Audio","_CodeSignature"};
    static const char *const signature_names[]={"CodeResources"};
    e=x9_microphone_directory(x9_speaker_hal_source,root_names,1);if(e)goto out;
    n=snprintf(path,sizeof(path),"%s/X9Audio.driver",x9_speaker_hal_source);
    if(n<0 || (size_t)n>=sizeof(path)){e=E2BIG;goto out;}
    e=x9_microphone_directory(path,bundle_names,4);if(e)goto out;
    if(strlcat(path,"/_CodeSignature",sizeof(path))>=sizeof(path)){e=E2BIG;goto out;}
    e=x9_microphone_directory(path,signature_names,1);if(e)goto out;
    e=x9_speaker_bundle_equal(x9_speaker_hal_source);if(e)goto out;
    {
        size_t pn=sizeof(X9_SPEAKER_HAL_POINT),sn=strlen(x9_speaker_hal_source)+1;
        if(scratch>end || 7+pn+sn>end-scratch){e=E2BIG;goto out;}
        struct mount_args m={.type=scratch,.path=scratch+7,
            .flags=MNT_RDONLY|MNT_UNION,.data=scratch+7+pn};int32_t rv=0;
        if((e=copyout("bindfs",m.type,7)) || (e=copyout(X9_SPEAKER_HAL_POINT,m.path,pn)) ||
           (e=copyout(x9_speaker_hal_source,m.data,sn)))goto out;
        __atomic_store_n(&x9_speaker_mount_owner,current_thread(),__ATOMIC_RELEASE);
        e=mount(p,&m,&rv);
        __atomic_store_n(&x9_speaker_mount_owner,NULL,__ATOMIC_RELEASE);
        if(e)goto out;
    }
    /* Actual native discovery surface must contain the full current bundle
     * and preserve all visible native/MIC/haptic vnodes in earlier layers. */
    e=x9_speaker_bundle_equal(X9_SPEAKER_HAL_POINT);
    if(!e)e=x9_native_hal_verify(X9_SPEAKER_HAL_POINT,lower);
    if(e) {
        int ce=copyout(X9_SPEAKER_HAL_POINT,scratch,sizeof(X9_SPEAKER_HAL_POINT));
        if(!ce){struct unmount_args u={.path=scratch,.flags=0};int32_t rv=0;ce=unmount(p,&u,&rv);}
        if(!ce)ce=x9_native_hal_verify(X9_SPEAKER_HAL_POINT,lower);
        printf("X9 SPEAKER HAL: union verification errno=%d rollback=%d; no publication retry\n",e,ce);
    } else printf("X9 SPEAKER HAL: signed bundle native union visible created=%u lower-preserved=1 native-entries=%u; physical Ready separate\n",created,lower->count);
out:
    x9_native_hal_release(lower);x9_speaker_hal_error=e;return e;
}
