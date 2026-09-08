#include <wctype.h>
#include <fnmatch.h>
#include <libkern/OSAtomic.h>
static pthread_t pg_threads[256];
/* Synchronous emulation of POSIX AIO (Bejeweled 3 streams its pak with
   aio_read): the read is performed immediately and its result remembered per
   aiocb for aio_error/aio_return.  i386 struct aiocb (off_t is only 4-byte
   aligned on i386, so it packs tightly after the fd): aio_fildes@0,
   aio_offset (8 bytes)@4, aio_buf@12, aio_nbytes@16. */
static struct { uint32_t cb; long res; } pg_aio[64];
static uint32_t pg_thread_count=2;
static _Thread_local uint32_t pg_thread_id;
struct pg_thread_start {uint32_t function,argument,identifier;};
static void *pg_run_thread(void *opaque){struct pg_thread_start t=*(struct pg_thread_start *)opaque;free(opaque);pg_thread_id=t.identifier;return (void *)(uintptr_t)compat_runtime32_call(t.function,&t.argument,1);}
static int peggle_libc_dispatch(const char *n,const uint32_t *a,uint64_t *r)
{
#define IS(s) (strcmp(n,s)==0)
#define RET(x) do{*r=(uint64_t)(x);return 1;}while(0)
    if(!lp32_title_is_peggle_engine(lp32_profile()->title))return 0;
    if(IS("_shm_open"))RET((uint32_t)shm_open((void *)(uintptr_t)a[0],a[1],0600));
    if(IS("_shm_unlink"))RET((uint32_t)shm_unlink((void *)(uintptr_t)a[0]));
    if(IS("_ftruncate"))RET((uint32_t)ftruncate(a[0],(uint64_t)a[1]|((uint64_t)a[2]<<32)));
    if(IS("_fcntl"))RET((uint32_t)fcntl(a[0],a[1],a[2]));
    if(IS("_close"))RET((uint32_t)close(a[0]));
    if(IS("_open")||IS("_open$UNIX2003"))RET((uint32_t)open((const char *)(uintptr_t)a[0],(int)a[1],(mode_t)a[2]));
    if(IS("_read")||IS("_read$UNIX2003"))RET((uint32_t)read((int)a[0],(void *)(uintptr_t)a[1],a[2]));
    if(IS("_write")||IS("_write$UNIX2003"))RET((uint32_t)write((int)a[0],(const void *)(uintptr_t)a[1],a[2]));
    if(IS("_lseek")){off_t off=(off_t)((uint64_t)a[1]|((uint64_t)a[2]<<32));RET((uint64_t)lseek((int)a[0],off,(int)a[3]));}
    if(IS("_pread")){off_t off=(off_t)((uint64_t)a[3]|((uint64_t)a[4]<<32));RET((uint32_t)pread((int)a[0],(void *)(uintptr_t)a[1],a[2],off));}
    if(IS("_aio_read")||IS("_aio_write")){uint32_t cb=a[0];const uint8_t *p=(const void *)(uintptr_t)cb;int fd=*(const int *)(p+0);off_t off=(off_t)(*(const uint32_t *)(p+4)|((uint64_t)*(const uint32_t *)(p+8)<<32));uint32_t buf=*(const uint32_t *)(p+12);uint32_t nb=*(const uint32_t *)(p+16);long got=IS("_aio_read")?(long)pread(fd,(void *)(uintptr_t)buf,nb,off):(long)pwrite(fd,(const void *)(uintptr_t)buf,nb,off);int slot=-1;for(int i=0;i<64;i++)if(pg_aio[i].cb==cb){slot=i;break;}if(slot<0)for(int i=0;i<64;i++)if(!pg_aio[i].cb){slot=i;break;}if(slot>=0){pg_aio[slot].cb=cb;pg_aio[slot].res=got;}RET(0);}
    if(IS("_aio_error"))RET(0); /* all reads complete synchronously */
    if(IS("_aio_return")){uint32_t cb=a[0];long got=-1;for(int i=0;i<64;i++)if(pg_aio[i].cb==cb){got=pg_aio[i].res;pg_aio[i].cb=0;break;}RET((uint32_t)got);}
    if(IS("_aio_suspend"))RET(0);
    if(IS("_aio_cancel"))RET(2); /* AIO_ALLDONE */
    if(IS("_mmap")) {
        static uint32_t cursor=0x71000000;size_t len=(a[1]+4095)&~4095u;uint32_t addr=__atomic_fetch_add(&cursor,(uint32_t)len,__ATOMIC_RELAXED);
        if(addr+len>0x7b000000)RET(UINT32_MAX);
        void *p=mmap((void *)(uintptr_t)addr,len,a[2],a[3]|MAP_FIXED,a[4],(uint64_t)a[5]|((uint64_t)a[6]<<32));
        if(getenv("LP32_TRACE_MMAP"))fprintf(stderr,"compat32: mmap len=%zu prot=%x flags=%x fd=%d off=%u -> 0x%08x\n",len,a[2],a[3],(int)a[4],a[5],p==MAP_FAILED?0:(uint32_t)(uintptr_t)p);
        RET(p==MAP_FAILED?UINT32_MAX:(uint32_t)(uintptr_t)p);
    }
    if(IS("_munmap"))RET((uint32_t)munmap((void *)(uintptr_t)a[0],a[1]));
    if(IS("_time")){int32_t t=(int32_t)time(NULL);if(a[0])*(int32_t *)(uintptr_t)a[0]=t;RET((uint32_t)t);}
    if(IS("_localtime")){time_t t=*(int32_t *)(uintptr_t)a[0];struct tm tm;localtime_r(&t,&tm);uint32_t result=compat_runtime32_allocate(44,1);memcpy((void *)(uintptr_t)result,&tm,36);((int32_t *)(uintptr_t)result)[9]=(int32_t)tm.tm_gmtoff;((uint32_t *)(uintptr_t)result)[10]=compat_runtime32_copy_cstring(tm.tm_zone);RET(result);}
    if(IS("_gmtime")){time_t t=*(int32_t *)(uintptr_t)a[0];struct tm tm;gmtime_r(&t,&tm);uint32_t result=compat_runtime32_allocate(44,1);memcpy((void *)(uintptr_t)result,&tm,36);((int32_t *)(uintptr_t)result)[9]=(int32_t)tm.tm_gmtoff;((uint32_t *)(uintptr_t)result)[10]=compat_runtime32_copy_cstring(tm.tm_zone);RET(result);}
    if(IS("_gmtime_r")||IS("_localtime_r")){if(!a[1])RET(0);time_t t=*(int32_t *)(uintptr_t)a[0];struct tm tm;if(IS("_gmtime_r"))gmtime_r(&t,&tm);else localtime_r(&t,&tm);memcpy((void *)(uintptr_t)a[1],&tm,36);((int32_t *)(uintptr_t)a[1])[9]=(int32_t)tm.tm_gmtoff;((uint32_t *)(uintptr_t)a[1])[10]=compat_runtime32_copy_cstring(tm.tm_zone);RET(a[1]);}
    if(IS("_asctime")){struct tm tm={0};memcpy(&tm,(void *)(uintptr_t)a[0],36);RET(compat_runtime32_copy_cstring(asctime(&tm)));}
    if(IS("_strftime")){struct tm tm={0};if(a[3]){memcpy(&tm,(void *)(uintptr_t)a[3],36);tm.tm_gmtoff=((int32_t *)(uintptr_t)a[3])[9];uint32_t z=((uint32_t *)(uintptr_t)a[3])[10];tm.tm_zone=z?(char *)(uintptr_t)z:(char *)"";}size_t n=strftime((char *)(uintptr_t)a[0],a[1],(const char *)(uintptr_t)a[2],&tm);RET((uint32_t)n);}
    if(IS("_mktime")){struct tm tm={0};if(a[0]){memcpy(&tm,(void *)(uintptr_t)a[0],36);tm.tm_gmtoff=((int32_t *)(uintptr_t)a[0])[9];}RET((uint32_t)(int32_t)mktime(&tm));}
    if(IS("_strtod")){char *end;double v=strtod((void *)(uintptr_t)a[0],&end);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET(return_guest_double(v));}
    if(IS("_strtol")){char *end;long v=strtol((const char *)(uintptr_t)a[0],&end,(int)a[2]);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET((uint32_t)v);}
    if(IS("_strtoul")){char *end;unsigned long v=strtoul((const char *)(uintptr_t)a[0],&end,(int)a[2]);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET((uint32_t)v);}
    if(IS("_strtoll")||IS("_strtoq")){char *end;long long v=strtoll((const char *)(uintptr_t)a[0],&end,(int)a[2]);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET((uint64_t)v);}
    if(IS("_strtoull")||IS("_strtouq")){char *end;unsigned long long v=strtoull((const char *)(uintptr_t)a[0],&end,(int)a[2]);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET((uint64_t)v);}
    if(IS("_atoll")||IS("_atoq"))RET((uint64_t)atoll((const char *)(uintptr_t)a[0]));
    if(IS("_strcasecmp"))RET((uint32_t)strcasecmp((const char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1]));
    if(IS("_strspn"))RET((uint32_t)strspn((const char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1]));
    if(IS("_strcspn"))RET((uint32_t)strcspn((const char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1]));
    if(IS("_strcasestr"))RET((uint32_t)(uintptr_t)strcasestr((const char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1]));
    if(IS("_strlcpy"))RET((uint32_t)strlcpy((char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1],a[2]));
    if(IS("_strlcat"))RET((uint32_t)strlcat((char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1],a[2]));
    if(IS("_strncat")){strncat((char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1],a[2]);RET(a[0]);}
    if(IS("_access"))RET((uint32_t)access((const char *)(uintptr_t)a[0],(int)a[1]));
    if(IS("_creat"))RET((uint32_t)open((const char *)(uintptr_t)a[0],O_CREAT|O_TRUNC|O_WRONLY,(mode_t)a[1]));
    if(IS("_rmdir"))RET((uint32_t)rmdir((const char *)(uintptr_t)a[0]));
    if(IS("_perror")){fprintf(stderr,"compat32: %s: %s\n",a[0]?(const char *)(uintptr_t)a[0]:"",strerror(errno));RET(0);}
    if(IS("_putenv"))RET((uint32_t)putenv(strdup((const char *)(uintptr_t)a[0])));
    if(IS("_log10"))RET(return_guest_double(log10(guest_double(a))));
    /* div/ldiv return an 8-byte struct, which i386 hands back in edx:eax. */
    if(IS("_div")||IS("_ldiv")){int32_t n=(int32_t)a[0],d=(int32_t)a[1];if(!d)RET(0);RET(((uint64_t)(uint32_t)(n%d)<<32)|(uint32_t)(n/d));}
    /* localeconv: a C locale lconv the guest can read decimal_point out of. */
    if(IS("_localeconv")){
        static uint32_t conv;
        if(!conv){
            conv=compat_runtime32_allocate(64,1);
            uint32_t point=compat_runtime32_copy_cstring("."),comma=compat_runtime32_copy_cstring(","),empty=compat_runtime32_copy_cstring("");
            uint32_t *fields=(void *)(uintptr_t)conv;
            for(int i=0;i<10;i++)fields[i]=empty;
            fields[0]=point;fields[1]=comma;
            memset((uint8_t *)(uintptr_t)conv+40,CHAR_MAX,24);
        }
        RET(conv);
    }
    /* The game shells out only to hand a URL or a file to the default
       handler; do that directly instead of running a shell. */
    if(IS("_system")){
        const char *command=a[0]?(const char *)(uintptr_t)a[0]:"";
        if(strncmp(command,"open ",5)==0){objc_bridge32_open_url(command+5);RET(0);}
        fprintf(stderr,"compat32: refused system(\"%s\")\n",command);
        RET(-1);
    }
    if(IS("_strncasecmp"))RET((uint32_t)strncasecmp((const char *)(uintptr_t)a[0],(const char *)(uintptr_t)a[1],a[2]));
    /* Wide-char string helpers: i386 and host wchar_t are both 4 bytes and
       guest memory maps 1:1 in low address space, so host wcs* operate on the
       guest pointers/values directly. */
    if(IS("_wcslen"))RET((uint32_t)wcslen((const wchar_t *)(uintptr_t)a[0]));
    if(IS("_wcschr"))RET((uint32_t)(uintptr_t)wcschr((const wchar_t *)(uintptr_t)a[0],(wchar_t)a[1]));
    if(IS("_wcsrchr"))RET((uint32_t)(uintptr_t)wcsrchr((const wchar_t *)(uintptr_t)a[0],(wchar_t)a[1]));
    if(IS("_wcscmp"))RET((uint32_t)wcscmp((const wchar_t *)(uintptr_t)a[0],(const wchar_t *)(uintptr_t)a[1]));
    if(IS("_wcsncmp"))RET((uint32_t)wcsncmp((const wchar_t *)(uintptr_t)a[0],(const wchar_t *)(uintptr_t)a[1],a[2]));
    if(IS("_wcscpy")){wcscpy((wchar_t *)(uintptr_t)a[0],(const wchar_t *)(uintptr_t)a[1]);RET(a[0]);}
    if(IS("_wcsncpy")){wcsncpy((wchar_t *)(uintptr_t)a[0],(const wchar_t *)(uintptr_t)a[1],a[2]);RET(a[0]);}
    if(IS("_wcscat")){wcscat((wchar_t *)(uintptr_t)a[0],(const wchar_t *)(uintptr_t)a[1]);RET(a[0]);}
    if(IS("_wmemcpy")){wmemcpy((wchar_t *)(uintptr_t)a[0],(const wchar_t *)(uintptr_t)a[1],a[2]);RET(a[0]);}
    if(IS("_wmemmove")){wmemmove((wchar_t *)(uintptr_t)a[0],(const wchar_t *)(uintptr_t)a[1],a[2]);RET(a[0]);}
    if(IS("_wmemset")){wmemset((wchar_t *)(uintptr_t)a[0],(wchar_t)a[1],a[2]);RET(a[0]);}
    if(IS("_wcstol")){wchar_t *end;long v=wcstol((const wchar_t *)(uintptr_t)a[0],&end,(int)a[2]);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET((uint32_t)v);}
    if(IS("_wcstoul")){wchar_t *end;unsigned long v=wcstoul((const wchar_t *)(uintptr_t)a[0],&end,(int)a[2]);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET((uint32_t)v);}
    if(IS("_wcstod")){wchar_t *end;double v=wcstod((const wchar_t *)(uintptr_t)a[0],&end);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET(return_guest_double(v));}
    if(IS("_pthread_self")){if(!pg_thread_id){pg_thread_id=1;pg_threads[1]=pthread_self();}RET(pg_thread_id);}
    if(IS("_pthread_create")){uint32_t i=__atomic_fetch_add(&pg_thread_count,1,__ATOMIC_RELAXED);if(i>=256)RET(EAGAIN);struct pg_thread_start *t=malloc(sizeof(*t));*t=(struct pg_thread_start){a[2],a[3],i};int e=pthread_create(&pg_threads[i],NULL,pg_run_thread,t);if(e)free(t);else if(a[0])*(uint32_t *)(uintptr_t)a[0]=i;RET(e);}
    if(IS("_pthread_join")){if(a[0]>=256||!pg_threads[a[0]])RET(ESRCH);void *v;int e=pthread_join(pg_threads[a[0]],&v);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)v;RET(e);}
    if(IS("_pthread_cancel"))RET(a[0]<256&&pg_threads[a[0]]?pthread_cancel(pg_threads[a[0]]):ESRCH);
    if(IS("_pthread_getschedparam")){if(a[0]>=256||!pg_threads[a[0]])RET(ESRCH);RET(pthread_getschedparam(pg_threads[a[0]],(void *)(uintptr_t)a[1],(void *)(uintptr_t)a[2]));}
    if(IS("_pthread_attr_setdetachstate"))RET(0);
    if(IS("_sched_get_priority_max"))RET(sched_get_priority_max(a[0]));
    if(IS("_sched_get_priority_min"))RET(sched_get_priority_min(a[0]));
    if(IS("_floorf"))RET(return_guest_float(floorf(guest_float(a[0]))));
    if(IS("_fmodf"))RET(return_guest_float(fmodf(guest_float(a[0]),guest_float(a[1]))));
    if(IS("_strncasecmp"))RET((uint32_t)strncasecmp((void *)(uintptr_t)a[0],(void *)(uintptr_t)a[1],a[2]));
    if(IS("_fnmatch"))RET(fnmatch((void *)(uintptr_t)a[0],(void *)(uintptr_t)a[1],a[2]));
    if(IS("_getcwd"))RET(getcwd((void *)(uintptr_t)a[0],a[1])?a[0]:0);
    if(IS("_getenv")){const char *v=getenv((void *)(uintptr_t)a[0]);RET(v?compat_runtime32_copy_cstring(v):0);}
    if(IS("_fflush")){FILE *f=a[0]?lock_host_file_for_guest(a[0]):NULL;int e=fflush(f);if(a[0])unlock_host_file();RET((uint32_t)e);}
    if(IS("_feof")||IS("_fgetc")||IS("_fgets")){uint32_t h=IS("_fgets")?a[2]:a[0];FILE *f=lock_host_file_for_guest(h);if(!f)RET(IS("_fgetc")?UINT32_MAX:0);uint32_t v=IS("_feof")?feof(f):IS("_fgetc")?(uint32_t)fgetc(f):(fgets((void *)(uintptr_t)a[0],a[1],f)?a[0]:0);unlock_host_file();RET(v);}
    if(IS("_fputc")||IS("_ungetc")){FILE *f=lock_host_file_for_guest(a[1]);if(!f)RET(UINT32_MAX);int v=IS("_fputc")?fputc(a[0],f):ungetc(a[0],f);unlock_host_file();RET((uint32_t)v);}
    if(IS("_setjmp")||IS("__setjmp")||IS("_sigsetjmp"))RET(0); /* longjmp escapes the guest instead */
    if(IS("_getpid"))RET(getpid());
    if(IS("_wcslen"))RET(wcslen((void *)(uintptr_t)a[0]));
    if(IS("_wcstombs"))RET(wcstombs((void *)(uintptr_t)a[0],(void *)(uintptr_t)a[1],a[2]));
    /* char *fcvt(double value, int ndigit, int *decpt, int *sign) and ecvt:
       Bejeweled 3's Poker mode formats its payouts with them.  The digit
       string lives in a static buffer; ours is a guest allocation so the
       returned pointer is guest-addressable. */
    if(IS("_fcvt")||IS("_ecvt")){
        static uint32_t cvt_buffer;
        if(!cvt_buffer)cvt_buffer=compat_runtime32_allocate(512,1);
        int decpt=0,sign=0;
        const char *digits=IS("_fcvt")?fcvt(guest_double(a),(int)a[2],&decpt,&sign):ecvt(guest_double(a),(int)a[2],&decpt,&sign);
        if(a[3])*(int32_t *)(uintptr_t)a[3]=decpt;
        if(a[4])*(int32_t *)(uintptr_t)a[4]=sign;
        strlcpy((char *)(uintptr_t)cvt_buffer,digits?digits:"",512);
        RET(cvt_buffer);
    }
    /* unsigned long __maskrune(int c, unsigned long mask): the inline ctype
       macros call it for bytes outside ASCII (Bejeweled 3 hit it classifying
       0xC4 while rebuilding the main menu after Zen mode).  The host's C
       locale gives the same answer the game's own C locale would. */
    if(IS("___maskrune"))RET((uint32_t)__maskrune((int32_t)a[0],a[1]));
    /* Bejeweled 3's Lightning mode parses its timer text with wcstof. */
    if(IS("_wcstof")){wchar_t *end;float v=wcstof((const wchar_t *)(uintptr_t)a[0],&end);if(a[1])*(uint32_t *)(uintptr_t)a[1]=(uint32_t)(uintptr_t)end;RET(return_guest_float(v));}
    if(IS("_mbstowcs")){
        wchar_t *dst=(wchar_t *)(uintptr_t)a[0];
        const unsigned char *src=(const void *)(uintptr_t)a[1];
        size_t n=a[2];
        if(!src)RET((uint32_t)-1);
        size_t i=0;
        /* Byte->wchar 1:1 (Latin-1), matching how the guest itself widens its
           strings; avoids C-locale mbstowcs failing on extended bytes.  A NULL
           destination requests only the length. */
        if(dst){ for(;i<n && src[i];++i) dst[i]=(wchar_t)src[i]; if(i<n) dst[i]=0; }
        else { while(src[i]) ++i; }
        RET((uint32_t)i);
    }
    if(IS("_iswspace"))RET(iswspace(a[0]));
    if(IS("_toupper"))RET(toupper(a[0]));
    if(IS("_tolower"))RET(tolower(a[0]));
    if(IS("_isspace"))RET(isspace(a[0]));
    if(IS("_isalnum"))RET(isalnum(a[0]));
    if(IS("_isxdigit"))RET(isxdigit(a[0]));
    if(IS("_isprint"))RET(isprint(a[0]));
    if(IS("_ispunct"))RET(ispunct(a[0]));
    if(IS("_puts")){puts((const char *)(uintptr_t)a[0]);RET(0);}
    /* syslog is only ever used for diagnostics here; mirror it to stderr so a
       message the game logs still shows up in the session log. */
    if(IS("_openlog")||IS("_closelog"))RET(0);
    if(IS("_syslog")){fprintf(stderr,"compat32: syslog(%d) %s\n",(int)a[0],(const char *)(uintptr_t)a[1]);RET(0);}
    if(IS("_atoi"))RET(atoi((void *)(uintptr_t)a[0]));
    if(IS("_atof"))RET(return_guest_double(atof((void *)(uintptr_t)a[0])));
    if(IS("_atol"))RET((uint32_t)atol((void *)(uintptr_t)a[0]));
    if(IS("_srandom")){srandom(a[0]);RET(0);}
    if(IS("_random"))RET(random());
    if(IS("_rand"))RET(rand());
    if(IS("_atan"))RET(return_guest_double(atan(guest_double(a))));
    if(IS("_atan2"))RET(return_guest_double(atan2(guest_double(a),guest_double(a+2))));
    if(IS("_cbrt"))RET(return_guest_double(cbrt(guest_double(a))));
    if(IS("_rint"))RET(return_guest_double(rint(guest_double(a))));
    if(IS("_exp"))RET(return_guest_double(exp(guest_double(a))));
    if(IS("_lrint"))RET((uint32_t)lrint(guest_double(a)));
    if(IS("_ldexp"))RET(return_guest_double(ldexp(guest_double(a),(int)a[2])));
    if(IS("_frexp"))RET(return_guest_double(frexp(guest_double(a),(void *)(uintptr_t)a[2])));
    if(IS("_frexpf"))RET(return_guest_float(frexpf(guest_float(a[0]),(void *)(uintptr_t)a[1])));
    if(IS("_acosf"))RET(return_guest_float(acosf(guest_float(a[0]))));
    if(IS("_asinf"))RET(return_guest_float(asinf(guest_float(a[0]))));
    if(IS("_atanf"))RET(return_guest_float(atanf(guest_float(a[0]))));
    if(IS("_tanf"))RET(return_guest_float(tanf(guest_float(a[0]))));
    if(IS("_cbrtf"))RET(return_guest_float(cbrtf(guest_float(a[0]))));
    if(IS("_powf"))RET(return_guest_float(powf(guest_float(a[0]),guest_float(a[1]))));
    if(IS("_sqrtf"))RET(return_guest_float(sqrtf(guest_float(a[0]))));
    if(IS("_atan2f"))RET(return_guest_float(atan2f(guest_float(a[0]),guest_float(a[1]))));
    if(IS("_rintf"))RET(return_guest_float(rintf(guest_float(a[0]))));
    if(IS("_roundf"))RET(return_guest_float(roundf(guest_float(a[0]))));
    if(IS("_lroundf"))RET((uint32_t)lroundf(guest_float(a[0])));
    if(IS("_truncf"))RET(return_guest_float(truncf(guest_float(a[0]))));
    if(IS("_modff"))RET(return_guest_float(modff(guest_float(a[0]),(void *)(uintptr_t)a[1])));
    if(IS("___udivdi3")){uint64_t x=(uint64_t)a[0]|((uint64_t)a[1]<<32),y=(uint64_t)a[2]|((uint64_t)a[3]<<32);RET(x/y);}
    if(IS("___divdi3")){int64_t x=(uint64_t)a[0]|((uint64_t)a[1]<<32),y=(uint64_t)a[2]|((uint64_t)a[3]<<32);RET(x/y);}
    if(IS("_IncrementAtomic"))RET(__atomic_fetch_add((int32_t *)(uintptr_t)a[0],1,__ATOMIC_SEQ_CST));
    if(IS("_DecrementAtomic"))RET(__atomic_fetch_sub((int32_t *)(uintptr_t)a[0],1,__ATOMIC_SEQ_CST));
    if(IS("_OSAtomicCompareAndSwap32Barrier")){int32_t expected=a[0];RET(__atomic_compare_exchange_n((int32_t *)(uintptr_t)a[2],&expected,a[1],false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST));}
    if(IS("_OSAtomicOr32Barrier"))RET(__atomic_or_fetch((uint32_t *)(uintptr_t)a[1],a[0],__ATOMIC_SEQ_CST));
    if(IS("_OSAtomicTestAndClearBarrier")){uint8_t *p=(void *)(uintptr_t)(a[1]+a[0]/8);uint8_t mask=0x80>>(a[0]%8);RET((__atomic_fetch_and(p,(uint8_t)~mask,__ATOMIC_SEQ_CST)&mask)!=0);}
    if(IS("_OSSpinLockTry")){int32_t expected=0;RET(__atomic_compare_exchange_n((int32_t *)(uintptr_t)a[0],&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED));}
    if(IS("_OSSpinLockUnlock")){__atomic_store_n((int32_t *)(uintptr_t)a[0],0,__ATOMIC_RELEASE);RET(0);}
    if(IS("_OSSpinLockLock")){int32_t expected;do{expected=0;}while(!__atomic_compare_exchange_n((int32_t *)(uintptr_t)a[0],&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED));RET(0);}
    if(IS("_OSAtomicOr32Barrier"))RET(__atomic_or_fetch((uint32_t *)(uintptr_t)a[1],a[0],__ATOMIC_SEQ_CST));
    if(IS("_OSAtomicAnd32Barrier"))RET(__atomic_and_fetch((uint32_t *)(uintptr_t)a[1],a[0],__ATOMIC_SEQ_CST));
    if(IS("_OSAtomicTestAndClearBarrier")){uint32_t n=a[0];uint8_t *p=(uint8_t *)(uintptr_t)a[1];uint8_t mask=(uint8_t)(0x80>>(n&7));uint8_t old=__atomic_fetch_and(&p[n>>3],(uint8_t)~mask,__ATOMIC_SEQ_CST);RET((old&mask)!=0);}
    if(IS("_OSAtomicTestAndSetBarrier")){uint32_t n=a[0];uint8_t *p=(uint8_t *)(uintptr_t)a[1];uint8_t mask=(uint8_t)(0x80>>(n&7));uint8_t old=__atomic_fetch_or(&p[n>>3],mask,__ATOMIC_SEQ_CST);RET((old&mask)!=0);}
    if(IS("_pthread_mutexattr_destroy")||IS("_pthread_setcancelstate")||IS("_pthread_setcanceltype"))RET(0);
    return 0;
#undef RET
#undef IS
}
