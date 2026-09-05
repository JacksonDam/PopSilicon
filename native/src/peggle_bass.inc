#include "../vendor/bass/bass.h"
static int peggle_bass_dispatch(const char *n,const uint32_t *a,uint64_t *r)
{
#define IS(s) (strcmp(n,"_" #s)==0)
#define RET(x) do{*r=(uint64_t)(x);return 1;}while(0)
    if(strncmp(n,"_BASS_",6))return 0;
    if(IS(BASS_Init))RET(BASS_Init((int)a[0],a[1],a[2],NULL,NULL));
    if(IS(BASS_SetConfig))RET(BASS_SetConfig(a[0],a[1]));
    if(IS(BASS_GetConfig))RET(BASS_GetConfig(a[0]));
    if(IS(BASS_GetInfo))RET(BASS_GetInfo((void *)(uintptr_t)a[0]));
    if(IS(BASS_Free))RET(BASS_Free());
    if(IS(BASS_Stop))RET(BASS_Stop());
    if(IS(BASS_StreamCreateFile))RET(BASS_StreamCreateFile(a[0],(void *)(uintptr_t)a[1],(uint64_t)a[2]|((uint64_t)a[3]<<32),(uint64_t)a[4]|((uint64_t)a[5]<<32),a[6]));
    if(IS(BASS_MusicLoad))RET(BASS_MusicLoad(a[0],(void *)(uintptr_t)a[1],(uint64_t)a[2]|((uint64_t)a[3]<<32),a[4],a[5],a[6]));
    if(IS(BASS_StreamFree))RET(BASS_StreamFree(a[0]));
    if(IS(BASS_MusicFree))RET(BASS_MusicFree(a[0]));
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
