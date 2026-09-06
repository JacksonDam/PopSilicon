#include "../vendor/bass/bass.h"

/* BASS sync callbacks.  BASS invokes a SYNCPROC from its own update/mixing
   thread; the guest's procedure runs there through compat_runtime32_call (an
   ordinary pthread, so the i386 mode switch is fine, as it is for the game's
   own worker threads).  Deferring would not do: Bejeweled 3 keeps all of its
   songs in one tracker module and switches song with a mix-time MUSICPOS sync
   that repositions the module before the next mix; BASS mixes up to two
   seconds ahead, so a late callback would let the old song leak through. */
struct pg_bass_sync { HSYNC sync; DWORD channel; uint32_t proc; uint32_t user; };
static struct pg_bass_sync pg_bass_syncs[64];
static pthread_mutex_t pg_bass_sync_lock = PTHREAD_MUTEX_INITIALIZER;

static void CALLBACK pg_bass_sync_proc(HSYNC handle, DWORD channel, DWORD data, void *user)
{
    struct pg_bass_sync *entry = user;
    uint32_t proc = entry->proc;
    if (!proc) return;
    uint32_t arguments[4] = {(uint32_t)handle, channel, data, entry->user};
    compat_runtime32_call(proc, arguments, 4);
}

static HSYNC pg_bass_set_sync(DWORD channel, DWORD type, QWORD param, uint32_t proc, uint32_t user)
{
    if (!proc) return 0;
    pthread_mutex_lock(&pg_bass_sync_lock);
    struct pg_bass_sync *entry = NULL;
    for (size_t i = 0; i < sizeof(pg_bass_syncs) / sizeof(pg_bass_syncs[0]); ++i) {
        if (!pg_bass_syncs[i].proc) { entry = &pg_bass_syncs[i]; break; }
    }
    if (entry) {
        entry->channel = channel; entry->proc = proc; entry->user = user;
        entry->sync = BASS_ChannelSetSync(channel, type, param, pg_bass_sync_proc, entry);
        if (!entry->sync) entry->proc = 0;
    }
    pthread_mutex_unlock(&pg_bass_sync_lock);
    return entry ? entry->sync : 0;
}

/* The channel's syncs die with it; release their table entries. */
static void pg_bass_forget_channel(DWORD channel)
{
    pthread_mutex_lock(&pg_bass_sync_lock);
    for (size_t i = 0; i < sizeof(pg_bass_syncs) / sizeof(pg_bass_syncs[0]); ++i) {
        if (pg_bass_syncs[i].proc && pg_bass_syncs[i].channel == channel) pg_bass_syncs[i].proc = 0;
    }
    pthread_mutex_unlock(&pg_bass_sync_lock);
}

static void pg_bass_remove_sync(DWORD channel, HSYNC sync)
{
    pthread_mutex_lock(&pg_bass_sync_lock);
    for (size_t i = 0; i < sizeof(pg_bass_syncs) / sizeof(pg_bass_syncs[0]); ++i) {
        if (pg_bass_syncs[i].proc && pg_bass_syncs[i].channel == channel && pg_bass_syncs[i].sync == sync) pg_bass_syncs[i].proc = 0;
    }
    pthread_mutex_unlock(&pg_bass_sync_lock);
}

static int peggle_bass_dispatch(const char *n,const uint32_t *a,uint64_t *r)
{
#define IS(s) (strcmp(n,"_" #s)==0)
    static int bass_trace=-1;
    if(bass_trace<0)bass_trace=getenv("LP32_TRACE_BASS")!=NULL;
#define RET(x) do{*r=(uint64_t)(x);if(bass_trace)fprintf(stderr,"compat32: bass %s(%08x,%08x,%08x,%08x,%08x)=%llx err=%d\n",n,a[0],a[1],a[2],a[3],a[4],(unsigned long long)*r,BASS_ErrorGetCode());return 1;}while(0)
    if(strncmp(n,"_BASS_",6))return 0;
    if(IS(BASS_Init))RET(BASS_Init((int)a[0],a[1],a[2],NULL,NULL));
    if(IS(BASS_SetConfig))RET(BASS_SetConfig(a[0],a[1]));
    if(IS(BASS_GetConfig))RET(BASS_GetConfig(a[0]));
    if(IS(BASS_GetInfo))RET(BASS_GetInfo((void *)(uintptr_t)a[0]));
    if(IS(BASS_Free))RET(BASS_Free());
    if(IS(BASS_Stop))RET(BASS_Stop());
    if(IS(BASS_Start))RET(BASS_Start());
    if(IS(BASS_ChannelGetLength))RET(BASS_ChannelGetLength(a[0],a[1]));
    /* HSYNC BASS_ChannelSetSync(handle, type, QWORD param, SYNCPROC *proc, void *user) */
    if(IS(BASS_ChannelSetSync))RET(pg_bass_set_sync(a[0],a[1],(uint64_t)a[2]|((uint64_t)a[3]<<32),a[4],a[5]));
    if(IS(BASS_ChannelRemoveSync)){BOOL ok=BASS_ChannelRemoveSync(a[0],a[1]);pg_bass_remove_sync(a[0],a[1]);RET(ok);}
    if(IS(BASS_StreamCreateFile))RET(BASS_StreamCreateFile(a[0],(void *)(uintptr_t)a[1],(uint64_t)a[2]|((uint64_t)a[3]<<32),(uint64_t)a[4]|((uint64_t)a[5]<<32),a[6]));
    if(IS(BASS_MusicLoad))RET(BASS_MusicLoad(a[0],(void *)(uintptr_t)a[1],(uint64_t)a[2]|((uint64_t)a[3]<<32),a[4],a[5],a[6]));
    if(IS(BASS_StreamFree)){BOOL ok=BASS_StreamFree(a[0]);pg_bass_forget_channel(a[0]);RET(ok);}
    if(IS(BASS_MusicFree)){BOOL ok=BASS_MusicFree(a[0]);pg_bass_forget_channel(a[0]);RET(ok);}
    if(IS(BASS_ChannelFlags))RET(BASS_ChannelFlags(a[0],a[1],a[2]));
    if(IS(BASS_ChannelGetAttribute))RET(BASS_ChannelGetAttribute(a[0],a[1],(void *)(uintptr_t)a[2]));
    if(IS(BASS_ChannelSetAttribute))RET(BASS_ChannelSetAttribute(a[0],a[1],guest_float(a[2])));
    if(IS(BASS_ChannelGetPosition))RET(BASS_ChannelGetPosition(a[0],a[1]));
    if(IS(BASS_ChannelSetPosition))RET(BASS_ChannelSetPosition(a[0],(uint64_t)a[1]|((uint64_t)a[2]<<32),a[3]));
    if(IS(BASS_ChannelIsActive))RET(BASS_ChannelIsActive(a[0]));
    if(IS(BASS_ChannelPause))RET(BASS_ChannelPause(a[0]));
    if(IS(BASS_ChannelPlay))RET(BASS_ChannelPlay(a[0],a[1]));
    if(IS(BASS_ChannelStop))RET(BASS_ChannelStop(a[0]));
    if(IS(BASS_ChannelUpdate))RET(BASS_ChannelUpdate(a[0],a[1]));
    return 0;
#undef RET
#undef IS
}
