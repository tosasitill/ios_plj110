/* Default-off BTAudio XPC recorder; exact embedded bytes and existing injection chains are preserved. */
#define X9_BT_AUDIO_DIAG_PATH "/usr/lib/X9BTAudioDiag.dylib"

static int x9_bt_audio_diag_installed(void)
{
#if X9_BT_ROOT_PRESENT
    for(size_t i=0;i<sizeof(x9_btr_files)/sizeof(x9_btr_files[0]);i++)
        if(!strcmp(x9_btr_files[i].path,X9_BT_AUDIO_DIAG_PATH))
            return x9_btr_check(X9_BT_AUDIO_DIAG_PATH,x9_btr_files[i].bytes,
                                x9_btr_files[i].sha,x9_btr_files[i].mode);
    return ENOENT;
#else
    return ENOENT;
#endif
}

static int x9_bt_audio_exec_env(struct image_params *imgp,int ptr_size)
{
    if(!sweet_early_is_x9() || sweet_x9_arg("x9_bt_audio_diag",0)!=1)return 0;
    if(strcmp(imgp->ip_strings+strlen(EXECUTABLE_KEY),
              "/usr/sbin/coreaudiod"))return 0;
    const char *s=imgp->ip_endargv;
    for(int i=0;i<imgp->ip_envc;i++,s+=strlen(s)+1) {
        if(!strncmp(s,"DYLD_INSERT_LIBRARIES=",22) ||
           !strncmp(s,"X9_BT_AUDIO_DIAG=",17)) {
            printf("X9 BT AUDIO: existing environment kept; recorder skipped\n");
            return 0;
        }
    }
    int error=x9_bt_audio_diag_installed();
    if(error) {
        printf("X9 BT AUDIO: installed recorder rejected errno=%d; coreaudiod left unmodified\n",error);
        return 0;
    }
    static const char enable[]="X9_BT_AUDIO_DIAG=1";
    static const char inject[]="DYLD_INSERT_LIBRARIES=" X9_BT_AUDIO_DIAG_PATH;
    size_t required=sizeof(enable)+sizeof(inject)+2*(size_t)ptr_size;
    if(imgp->ip_argspace<required)return E2BIG;
    error=exec_add_user_string(imgp,CAST_USER_ADDR_T(enable),UIO_SYSSPACE,TRUE);
    if(error)return error;
    imgp->ip_argspace-=ptr_size;imgp->ip_envc++;
    error=exec_add_user_string(imgp,CAST_USER_ADDR_T(inject),UIO_SYSSPACE,TRUE);
    if(error)return error;
    imgp->ip_argspace-=ptr_size;imgp->ip_envc++;
    printf("X9 BT AUDIO: hash-verified coreaudiod recorder appended\n");
    return 0;
}
