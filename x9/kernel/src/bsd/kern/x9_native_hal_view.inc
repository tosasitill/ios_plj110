/* Snapshot the actual native HAL view, not an assumed vendor bundle name.
 * Keep every visible vnode across publication, including earlier union layers.
 * Only PID1's sequential MIC/haptic/speaker installers use these bounded helpers. */
#ifndef X9_NATIVE_HAL_VIEW_INC
#define X9_NATIVE_HAL_VIEW_INC
#include <sys/dirent.h>
#define X9_NATIVE_HAL_ENTRIES 32
struct x9_native_hal_view {
    unsigned count;
    struct { char name[128]; vnode_t vnode; } entry[X9_NATIVE_HAL_ENTRIES];
};
static void x9_native_hal_release(struct x9_native_hal_view *view)
{
    if(!view)return;
    for(unsigned i=0;i<view->count;i++)vnode_put(view->entry[i].vnode);
    kfree_data(view,sizeof(*view));
}
static int x9_native_hal_capture(const char *point,struct x9_native_hal_view **out)
{
    *out=NULL;
    struct x9_native_hal_view *view=kalloc_data(sizeof(*view),Z_WAITOK|Z_ZERO);
    if(!view)return ENOMEM;
    vnode_t vp=NULLVP;vfs_context_t ctx=vfs_context_kernel();
    int e=vnode_lookup(point,0,&vp,ctx);if(e)goto done;
    if(!vnode_isdir(vp)){e=ENOTDIR;goto done;}
    unsigned layers=0;
    while(!e && vp) {
        if(++layers>8){e=E2BIG;break;}
        off_t offset=0;int eof=0;
        for(unsigned pass=0;!e && !eof && pass<16;pass++) {
            uint64_t aligned[256];char *data=(char *)aligned;
            uio_t u=uio_create(1,offset,UIO_SYSSPACE,UIO_READ);
            if(!u){e=ENOMEM;break;}
            e=uio_addiov(u,CAST_USER_ADDR_T(data),sizeof(aligned));int entries=0;
            if(!e)e=vnode_readdir64(vp,u,VNODE_READDIR_EXTENDED,&eof,&entries,ctx);
            user_ssize_t residual=uio_resid(u);off_t next=uio_offset(u);
            if(!e && (residual<0 || (uint64_t)residual>sizeof(aligned)))e=EIO;
            size_t bytes=e?0:sizeof(aligned)-(size_t)residual;uio_free(u);
            if(!e && !bytes && !eof)e=EIO;
            for(size_t off=0;!e && off<bytes;) {
                struct direntry *d=(struct direntry *)(void *)(data+off);
                size_t head=offsetof(struct direntry,d_name);
                if(bytes-off<head || (off&7) || d->d_reclen<head+1 ||
                   (d->d_reclen&7) || d->d_reclen>bytes-off ||
                   d->d_namlen>=d->d_reclen-head || d->d_name[d->d_namlen] ||
                   strnlen(d->d_name,d->d_namlen)!=d->d_namlen){e=EIO;break;}
                if(d->d_ino && strcmp(d->d_name,".") && strcmp(d->d_name,"..")) {
                    if(!d->d_namlen || strchr(d->d_name,'/')){e=EINVAL;break;}
                    unsigned i=0;for(;i<view->count;i++)
                        if(!strcmp(view->entry[i].name,d->d_name))break;
                    /* Upper entries shadow equal lower names. Lookup through
                     * the real mountpoint, not a fabricated lower alias. */
                    if(i==view->count) {
                        if(i==X9_NATIVE_HAL_ENTRIES){e=E2BIG;break;}
                        if(d->d_namlen>=sizeof(view->entry[i].name)){e=ENAMETOOLONG;break;}
                        char path[256];int n=snprintf(path,sizeof(path),"%s/%s",point,d->d_name);
                        if(n<0 || (size_t)n>=sizeof(path)){e=E2BIG;break;}
                        vnode_t visible=NULLVP;e=vnode_lookup(path,0,&visible,ctx);if(e)break;
                        strlcpy(view->entry[i].name,d->d_name,sizeof(view->entry[i].name));
                        view->entry[i].vnode=visible;view->count++;
                    }
                }
                off+=d->d_reclen;
            }
            if(!e && !eof && next==offset)e=EIO;
            offset=next;
        }
        if(!e && !eof)e=E2BIG;
        vnode_t lower=NULLVP;
        if(!e && (vp->v_mount->mnt_flag&MNT_UNION))e=lookup_traverse_union(vp,&lower,ctx);
        vnode_put(vp);vp=lower;
    }
    if(!e && !view->count)e=ENOENT;
done:
    if(vp)vnode_put(vp);
    if(e)x9_native_hal_release(view);else *out=view;
    return e;
}
static int x9_native_hal_verify(const char *point,const struct x9_native_hal_view *view)
{
    if(!view || !view->count)return EINVAL;
    for(unsigned i=0;i<view->count;i++) {
        char path[256];int n=snprintf(path,sizeof(path),"%s/%s",point,view->entry[i].name);
        if(n<0 || (size_t)n>=sizeof(path))return E2BIG;
        vnode_t visible=NULLVP;int e=vnode_lookup(path,0,&visible,vfs_context_kernel());
        if(e)return e;
        if(visible!=view->entry[i].vnode)e=EIO;
        vnode_put(visible);if(e)return e;
    }
    return 0;
}
#endif
