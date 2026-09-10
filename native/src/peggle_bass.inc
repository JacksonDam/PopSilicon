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

/* BASSMOD (un4seen's cut-down MOD-only library) over BASS.  Bejeweled 2 ships
   an i386-only libbassmod.dylib and plays its single Impulse Tracker module
   through it; the two libraries share module flags, position encoding and
   activity codes, so each BASSMOD entry point is BASS on the one module the
   API allows.  BASSMOD has no handles: the module is implicit global state. */
static HMUSIC pg_bassmod_music;
static DWORD pg_bassmod_volume = 100;

static void pg_bassmod_apply_volume(void)
{
    if (pg_bassmod_music) {
        BASS_ChannelSetAttribute(pg_bassmod_music, BASS_ATTRIB_VOL,
                                 (float)pg_bassmod_volume / 100.0f);
    }
}

static int peggle_bassmod_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, "_" #s) == 0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    if (IS(BASSMOD_Init)) RET(BASS_Init((int)a[0], a[1], 0, NULL, NULL));
    if (IS(BASSMOD_Free)) { pg_bassmod_music = 0; RET(BASS_Free()); }
    if (IS(BASSMOD_ErrorGetCode)) RET((uint32_t)BASS_ErrorGetCode());
    if (IS(BASSMOD_SetVolume)) {
        pg_bassmod_volume = a[0] > 100 ? 100 : a[0];
        pg_bassmod_apply_volume();
        RET(1);
    }
    if (IS(BASSMOD_MusicLoad)) {
        if (pg_bassmod_music) BASS_MusicFree(pg_bassmod_music);
        /* BASSMOD's music flags have the same values as BASS's, so the game's
           request passes through unchanged. */
        pg_bassmod_music = BASS_MusicLoad(a[0], (void *)(uintptr_t)a[1],
                                          a[2], a[3], a[4], 0);
        pg_bassmod_apply_volume();
        RET(pg_bassmod_music != 0);
    }
    if (IS(BASSMOD_MusicFree)) {
        if (pg_bassmod_music) BASS_MusicFree(pg_bassmod_music);
        pg_bassmod_music = 0;
        RET(0);
    }
    /* MusicPlay resumes at the current position: the game sets the song's
       order with MusicSetPosition first and expects playback to start there. */
    if (IS(BASSMOD_MusicPlay)) RET(BASS_ChannelPlay(pg_bassmod_music, FALSE));
    if (IS(BASSMOD_MusicPause)) RET(BASS_ChannelPause(pg_bassmod_music));
    if (IS(BASSMOD_MusicStop)) RET(BASS_ChannelStop(pg_bassmod_music));
    if (IS(BASSMOD_MusicSetPosition))
        RET(BASS_ChannelSetPosition(pg_bassmod_music, a[0], BASS_POS_MUSIC_ORDER));
    if (IS(BASSMOD_MusicIsActive)) RET(BASS_ChannelIsActive(pg_bassmod_music));
    return 0;
#undef RET
#undef IS
}

/* --- BASS 2.3 over the vendored BASS 2.4 -------------------------------- */

/*
 * Chuzzle links the 2.3-era BASS API.  Most calls are unchanged in 2.4, but
 * four are not: the channel attributes were split into one call per
 * attribute, flags gained a mask, the position gained a mode, and
 * BASS_CHANNELINFO grew three fields past the five the game's struct has room
 * for -- so that one is filled through a host struct and truncated.
 */
static int chuzzle_bass_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, "_" #s) == 0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    if (IS(BASS_GetVersion)) RET(0x02030000); /* the version the game checks for */
    if (IS(BASS_SetConfig) || IS(BASS_GetConfig)) {
        /* 2.3 took the global volumes as a percentage; 2.4 takes them in
           hundredths of a percent.  Chuzzle asks for sample 100 and music 75,
           which 2.4 read as 1% and 0.75% -- the game was mixing and playing
           everything correctly, just far below hearing. */
        uint32_t option = a[0];
        bool global_volume = option == BASS_CONFIG_GVOL_SAMPLE ||
                             option == BASS_CONFIG_GVOL_STREAM ||
                             option == BASS_CONFIG_GVOL_MUSIC;
        if (IS(BASS_GetConfig)) {
            DWORD value = BASS_GetConfig(option);
            RET(global_volume && value != (DWORD)-1 ? value / 100 : value);
        }
        uint32_t value = a[1];
        if (global_volume) value = (value > 100 ? 100 : value) * 100;
        RET(BASS_SetConfig(option, value));
    }
    if (IS(BASS_MusicLoad)) {
        RET(BASS_MusicLoad(a[0], (void *)(uintptr_t)a[1], a[2], a[3], a[4], 0));
    }
    if (IS(BASS_SampleLoad)) {
        RET(BASS_SampleLoad(a[0], (void *)(uintptr_t)a[1], a[2], a[3], a[4], a[5]));
    }
    if (IS(BASS_ChannelSetAttributes)) {
        /* (handle, freq, volume 0-100, pan -100..100); -1 and -101 mean
           "leave alone". */
        BOOL ok = TRUE;
        if ((int32_t)a[1] != -1) {
            ok &= BASS_ChannelSetAttribute(a[0], BASS_ATTRIB_FREQ, (float)(int32_t)a[1]);
        }
        if ((int32_t)a[2] != -1) {
            ok &= BASS_ChannelSetAttribute(a[0], BASS_ATTRIB_VOL, (float)(int32_t)a[2] / 100.0f);
        }
        if ((int32_t)a[3] != -101) {
            ok &= BASS_ChannelSetAttribute(a[0], BASS_ATTRIB_PAN, (float)(int32_t)a[3] / 100.0f);
        }
        RET(ok);
    }
    if (IS(BASS_ChannelSetFlags)) {
        /* The game reads the current flags back and toggles only the loop bit,
           so that is the only bit worth handing to the masked form. */
        RET(BASS_ChannelFlags(a[0], a[1], BASS_SAMPLE_LOOP));
    }
    if (IS(BASS_ChannelSetPosition)) {
        /* 2.3 packed "this is an order, not a byte offset" into the position's
           top bit; 2.4 takes it as a separate mode. */
        uint64_t position = (uint64_t)a[1] | ((uint64_t)a[2] << 32);
        DWORD mode = (a[1] & 0x80000000u) ? BASS_POS_MUSIC_ORDER : BASS_POS_BYTE;
        if (mode == BASS_POS_MUSIC_ORDER) position = a[1] & 0x7fffffffu;
        RET(BASS_ChannelSetPosition(a[0], position, mode));
    }
    if (IS(BASS_ChannelGetInfo)) {
        BASS_CHANNELINFO info;
        memset(&info, 0, sizeof(info));
        BOOL ok = BASS_ChannelGetInfo(a[0], &info);
        if (ok && a[1] >= 0x1000) {
            /* Only freq, chans, flags, ctype and origres exist in the guest's
               20-byte struct; 2.4's extra fields must not be written past it. */
            uint32_t fields[5] = {info.freq, info.chans, info.flags,
                                  info.ctype, info.origres};
            memcpy((void *)(uintptr_t)a[1], fields, sizeof(fields));
        }
        RET(ok);
    }
    return 0;
#undef RET
#undef IS
}

static int peggle_bass_dispatch(const char *n,const uint32_t *a,uint64_t *r)
{
#define IS(s) (strcmp(n,"_" #s)==0)
    static int bass_trace=-1;
    if(bass_trace<0)bass_trace=getenv("LP32_TRACE_BASS")!=NULL;
#define RET(x) do{*r=(uint64_t)(x);if(bass_trace)fprintf(stderr,"compat32: bass %s(%08x,%08x,%08x,%08x,%08x)=%llx err=%d\n",n,a[0],a[1],a[2],a[3],a[4],(unsigned long long)*r,BASS_ErrorGetCode());return 1;}while(0)
    if(strncmp(n,"_BASSMOD_",9)==0){int handled=peggle_bassmod_dispatch(n,a,r);if(handled&&bass_trace)fprintf(stderr,"compat32: bassmod %s(%08x,%08x,%08x,%08x,%08x)=%llx err=%d\n",n,a[0],a[1],a[2],a[3],a[4],(unsigned long long)*r,BASS_ErrorGetCode());return handled;}
    if(strncmp(n,"_BASS_",6))return 0;
    /* Chuzzle links the older API; its shims come first and fall through for
       the calls the two versions share. */
    if(lp32_profile()->title==LP32_TITLE_CHUZZLE&&chuzzle_bass_dispatch(n,a,r)){
        if(bass_trace)fprintf(stderr,"compat32: bass(2.3) %s(%08x,%08x,%08x,%08x,%08x)=%llx err=%d\n",n,a[0],a[1],a[2],a[3],a[4],(unsigned long long)*r,BASS_ErrorGetCode());
        return 1;
    }
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
    if(IS(BASS_ErrorGetCode))RET((uint32_t)BASS_ErrorGetCode());
    if(IS(BASS_Pause))RET(BASS_Pause());
    if(IS(BASS_SampleLoad))RET(BASS_SampleLoad(a[0],(void *)(uintptr_t)a[1],(uint64_t)a[2]|((uint64_t)a[3]<<32),a[4],a[5],a[6]));
    if(IS(BASS_SampleFree))RET(BASS_SampleFree(a[0]));
    if(IS(BASS_SampleGetChannel))RET(BASS_SampleGetChannel(a[0],a[1]));
    if(IS(BASS_ChannelGetInfo))RET(BASS_ChannelGetInfo(a[0],(void *)(uintptr_t)a[1]));
    if(IS(BASS_ChannelPause))RET(BASS_ChannelPause(a[0]));
    if(IS(BASS_ChannelPlay))RET(BASS_ChannelPlay(a[0],a[1]));
    if(IS(BASS_ChannelStop))RET(BASS_ChannelStop(a[0]));
    if(IS(BASS_ChannelUpdate))RET(BASS_ChannelUpdate(a[0],a[1]));
    return 0;
#undef RET
#undef IS
}
