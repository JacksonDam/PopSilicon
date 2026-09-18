
#include <OpenAL/al.h>
#include <OpenAL/alc.h>

/* OpenAL has been deprecated since macOS 10.15 in favour of AVAudioEngine, but
   it still ships and still works, and the guest calls the OpenAL C API itself,
   so there is nothing to move to.  The build is -Werror, so silence that one
   diagnostic here rather than loosen it for the whole runtime. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

enum {
    kFfSampleDesired = 8, kFfSampleActual = 16, kFfSampleBuffer = 24,
    kFfSampleBufferSize = 28, kFfSampleFlags = 32, kFfSampleSize = 36,
    /* Sound_AudioInfo: Uint16 format, Uint8 channels, Uint32 rate. */
    kFfAudioFormat = 0, kFfAudioChannels = 2, kFfAudioRate = 4,
    /* AUDIO_S16SYS, the only format this game asks for. */
    kFfAudioS16 = 0x8010,
    /* Sound_SampleFlags */
    kFfSampleEOF = 0x40000000, kFfSampleError = 0x80000000,
};

/* Host pointers the guest may hold: ALC devices and contexts. */
#define kFfHandleCapacity 32
static void *ff_handles[kFfHandleCapacity];

static uint32_t ff_handle_register(void *pointer)
{
    if (!pointer) return 0;
    for (uint32_t i = 0; i < kFfHandleCapacity; ++i) {
        if (!ff_handles[i]) { ff_handles[i] = pointer; return i + 1; }
    }
    return 0;
}

static void *ff_handle_lookup(uint32_t token)
{
    if (!token || token > kFfHandleCapacity) return NULL;
    return ff_handles[token - 1];
}

static void ff_handle_release(uint32_t token)
{
    if (token && token <= kFfHandleCapacity) ff_handles[token - 1] = NULL;
}

/* One decode stream per live Sound_Sample. */
struct ff_sample {
    uint32_t sample;
    HSTREAM stream;
};
#define kFfSampleCapacity 64
static struct ff_sample ff_samples[kFfSampleCapacity];
static bool ff_bass_started;

static HSTREAM ff_stream_for(uint32_t sample)
{
    for (uint32_t i = 0; i < kFfSampleCapacity; ++i) {
        if (ff_samples[i].sample == sample) return ff_samples[i].stream;
    }
    return 0;
}

static void ff_sample_forget(uint32_t sample)
{
    for (uint32_t i = 0; i < kFfSampleCapacity; ++i) {
        if (ff_samples[i].sample != sample) continue;
        if (ff_samples[i].stream) BASS_StreamFree(ff_samples[i].stream);
        ff_samples[i].sample = 0;
        ff_samples[i].stream = 0;
    }
}

static float ff_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Decode the whole stream; the caller owns the returned host buffer. */
static void *ff_decode_stream(HSTREAM stream, size_t *out_size)
{
    size_t total = 0, capacity = 1u << 20;
    char *host = malloc(capacity);
    *out_size = 0;
    if (!host) return NULL;
    for (;;) {
        if (total == capacity) {
            size_t grown = capacity * 2;
            char *bigger = realloc(host, grown);
            if (!bigger) break;
            host = bigger;
            capacity = grown;
        }
        DWORD got = BASS_ChannelGetData(stream, host + total,
                                        (DWORD)(capacity - total));
        if (got == (DWORD)-1 || got == 0) break;
        total += got;
    }
    *out_size = total;
    return host;
}

static int feeding_frenzy_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define OUT(i, v) do { if (a[i]) *(uint32_t *)P(i) = (uint32_t)(v); } while (0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    if (lp32_profile()->title != LP32_TITLE_FEEDING_FRENZY) return 0;

    /* --- OpenAL: devices and contexts --- */
    if (IS("_alcOpenDevice")) {
        RET(ff_handle_register(alcOpenDevice(a[0] >= 0x1000 ? P(0) : NULL)));
    }
    if (IS("_alcCloseDevice")) {
        ALCdevice *device = ff_handle_lookup(a[0]);
        ALCboolean ok = device ? alcCloseDevice(device) : ALC_FALSE;
        ff_handle_release(a[0]);
        RET(ok);
    }
    if (IS("_alcCreateContext")) {
        ALCdevice *device = ff_handle_lookup(a[0]);
        /* The attribute list is a guest array of ALCint pairs; this game
           passes none. */
        RET(device ? ff_handle_register(alcCreateContext(device, NULL)) : 0);
    }
    if (IS("_alcDestroyContext")) {
        ALCcontext *context = ff_handle_lookup(a[0]);
        if (context) alcDestroyContext(context);
        ff_handle_release(a[0]);
        RET(0);
    }
    if (IS("_alcMakeContextCurrent")) RET(alcMakeContextCurrent(ff_handle_lookup(a[0])));
    if (IS("_alcProcessContext")) {
        ALCcontext *context = ff_handle_lookup(a[0]);
        if (context) alcProcessContext(context);
        RET(0);
    }
    if (IS("_alcSuspendContext")) {
        ALCcontext *context = ff_handle_lookup(a[0]);
        if (context) alcSuspendContext(context);
        RET(0);
    }

    /* --- OpenAL: buffers and sources (32-bit names, passed through) --- */
    if (IS("_alGenBuffers") || IS("_alGenSources")) {
        ALuint names[32];
        ALsizei count = (ALsizei)a[0];
        if (count < 0 || count > (ALsizei)(sizeof(names) / sizeof(names[0]))) RET(0);
        if (IS("_alGenBuffers")) alGenBuffers(count, names);
        else alGenSources(count, names);
        uint32_t *out = P(1);
        if (out) for (ALsizei i = 0; i < count; ++i) out[i] = names[i];
        RET(0);
    }
    if (IS("_alDeleteBuffers") || IS("_alDeleteSources")) {
        ALsizei count = (ALsizei)a[0];
        const uint32_t *in = P(1);
        ALuint names[32];
        if (!in || count < 0 || count > (ALsizei)(sizeof(names) / sizeof(names[0]))) RET(0);
        for (ALsizei i = 0; i < count; ++i) names[i] = in[i];
        if (IS("_alDeleteBuffers")) alDeleteBuffers(count, names);
        else alDeleteSources(count, names);
        RET(0);
    }
    if (IS("_alIsSource")) RET(alIsSource(a[0]));
    if (IS("_alBufferData")) {
        alBufferData(a[0], (ALenum)a[1], a[2] >= 0x1000 ? P(2) : NULL,
                     (ALsizei)a[3], (ALsizei)a[4]);
        RET(0);
    }
    if (IS("_alSourceQueueBuffers") || IS("_alSourceUnqueueBuffers")) {
        ALsizei count = (ALsizei)a[1];
        uint32_t *names32 = P(2);
        ALuint names[32];
        if (!names32 || count < 0 || count > (ALsizei)(sizeof(names) / sizeof(names[0]))) RET(0);
        if (IS("_alSourceQueueBuffers")) {
            for (ALsizei i = 0; i < count; ++i) names[i] = names32[i];
            alSourceQueueBuffers(a[0], count, names);
        } else {
            alSourceUnqueueBuffers(a[0], count, names);
            for (ALsizei i = 0; i < count; ++i) names32[i] = names[i];
        }
        RET(0);
    }
    if (IS("_alSourcePlay")) { alSourcePlay(a[0]); RET(0); }
    if (IS("_alSourceStop")) { alSourceStop(a[0]); RET(0); }
    if (IS("_alSourcePause")) { alSourcePause(a[0]); RET(0); }
    if (IS("_alSourcei")) { alSourcei(a[0], (ALenum)a[1], (ALint)a[2]); RET(0); }
    if (IS("_alSourcef")) { alSourcef(a[0], (ALenum)a[1], ff_float(a[2])); RET(0); }
    if (IS("_alSource3f")) {
        alSource3f(a[0], (ALenum)a[1], ff_float(a[2]), ff_float(a[3]), ff_float(a[4]));
        RET(0);
    }
    if (IS("_alListenerf")) { alListenerf((ALenum)a[0], ff_float(a[1])); RET(0); }
    if (IS("_alGetSourcei")) {
        ALint value = 0;
        alGetSourcei(a[0], (ALenum)a[1], &value);
        OUT(2, value);
        RET(0);
    }
    if (IS("_alGetError")) RET((uint32_t)alGetError());
    if (IS("_alGetString")) {
        const ALchar *text = alGetString((ALenum)a[0]);
        RET(compat_runtime32_copy_cstring(text ? text : ""));
    }
    if (IS("_alGetEnumValue")) {
        RET(a[0] >= 0x1000 ? (uint32_t)alGetEnumValue(P(0)) : 0);
    }
    if (IS("_alIsExtensionPresent")) {
        RET(a[0] >= 0x1000 ? alIsExtensionPresent(P(0)) : AL_FALSE);
    }

    /* --- SDL_sound, decoded by BASS --- */
    if (IS("_Sound_Init")) {
        if (!ff_bass_started) {
            /* Device 0 is BASS's "no sound" device: this decodes only, and
               OpenAL does the playing. */
            ff_bass_started = BASS_Init(0, 44100, 0, NULL, NULL) != FALSE;
        }
        RET(1);
    }
    if (IS("_Sound_Quit")) {
        for (uint32_t i = 0; i < kFfSampleCapacity; ++i) {
            if (ff_samples[i].stream) BASS_StreamFree(ff_samples[i].stream);
            ff_samples[i].sample = 0;
            ff_samples[i].stream = 0;
        }
        if (ff_bass_started) { BASS_Free(); ff_bass_started = false; }
        RET(1);
    }
    if (IS("_Sound_GetLinkedVersion")) {
        /* SDL_version: three bytes, the release this game was built against. */
        uint8_t *version = P(0);
        if (version) { version[0] = 1; version[1] = 0; version[2] = 1; }
        RET(0);
    }
    if (IS("_Sound_AvailableDecoders")) RET(0); /* only logged */
    if (IS("_Sound_ClearError")) RET(0);
    if (IS("_Sound_GetError")) RET(compat_runtime32_copy_cstring(""));
    if (IS("_Sound_NewSampleFromFile")) {
        if (a[0] < 0x1000) RET(0);
        HSTREAM stream = BASS_StreamCreateFile(FALSE, P(0), 0, 0, BASS_STREAM_DECODE);
        if (!stream) RET(0);
        uint32_t sample = compat_runtime32_allocate(kFfSampleSize, 1);
        if (!sample) { BASS_StreamFree(stream); RET(0); }
        uint32_t slot = kFfSampleCapacity;
        for (uint32_t i = 0; i < kFfSampleCapacity; ++i) {
            if (!ff_samples[i].sample) { slot = i; break; }
        }
        if (slot == kFfSampleCapacity) {
            BASS_StreamFree(stream);
            compat_runtime32_deallocate(sample);
            RET(0);
        }
        ff_samples[slot].sample = sample;
        ff_samples[slot].stream = stream;
        BASS_CHANNELINFO info;
        BASS_ChannelGetInfo(stream, &info);
        uint8_t *bytes = (void *)(uintptr_t)sample;
        /* desired: what the game asked for, copied back as SDL_sound does. */
        if (a[1] >= 0x1000) {
            memcpy(bytes + kFfSampleDesired, P(1), 8);
        }
        /* actual: what BASS will hand back -- signed 16-bit, its own rate. */
        uint16_t format = kFfAudioS16;
        memcpy(bytes + kFfSampleActual + kFfAudioFormat, &format, 2);
        bytes[kFfSampleActual + kFfAudioChannels] = (uint8_t)info.chans;
        uint32_t rate = info.freq;
        memcpy(bytes + kFfSampleActual + kFfAudioRate, &rate, 4);
        /* The game passes desired.rate to alBufferData, so it has to be the
           rate the samples are actually in. */
        memcpy(bytes + kFfSampleDesired + kFfAudioRate, &rate, 4);
        bytes[kFfSampleDesired + kFfAudioChannels] = (uint8_t)info.chans;
        memcpy(bytes + kFfSampleDesired + kFfAudioFormat, &format, 2);
        uint32_t size = a[2] ? a[2] : 0x20000;
        uint32_t buffer = compat_runtime32_allocate(size, 1);
        memcpy(bytes + kFfSampleBuffer, &buffer, 4);
        memcpy(bytes + kFfSampleBufferSize, &size, 4);
        uint32_t flags = 0;
        memcpy(bytes + kFfSampleFlags, &flags, 4);
        RET(sample);
    }
    if (IS("_Sound_FreeSample")) {
        if (a[0] >= 0x1000) {
            uint32_t buffer;
            memcpy(&buffer, (const void *)(uintptr_t)(a[0] + kFfSampleBuffer), 4);
            if (buffer) compat_runtime32_deallocate(buffer);
            ff_sample_forget(a[0]);
            compat_runtime32_deallocate(a[0]);
        }
        RET(0);
    }
    if (IS("_Sound_Rewind")) {
        HSTREAM stream = a[0] >= 0x1000 ? ff_stream_for(a[0]) : 0;
        RET(stream ? BASS_ChannelSetPosition(stream, 0, BASS_POS_BYTE) : 0);
    }
    if (IS("_Sound_DecodeAll")) {
        HSTREAM stream = a[0] >= 0x1000 ? ff_stream_for(a[0]) : 0;
        if (!stream) RET(0);
        size_t total = 0;
        void *host = ff_decode_stream(stream, &total);
        uint8_t *bytes = (void *)(uintptr_t)a[0];
        uint32_t previous;
        memcpy(&previous, bytes + kFfSampleBuffer, 4);
        uint32_t buffer = compat_runtime32_allocate(total ? (uint32_t)total : 1, 1);
        if (buffer && host && total) memcpy((void *)(uintptr_t)buffer, host, total);
        free(host);
        if (previous) compat_runtime32_deallocate(previous);
        uint32_t size = (uint32_t)total;
        memcpy(bytes + kFfSampleBuffer, &buffer, 4);
        memcpy(bytes + kFfSampleBufferSize, &size, 4);
        /* The whole sound is in the buffer now.  The game skips a sample whose
           EOF flag is set, so report a completed decode as flagless rather
           than at end of stream. */
        uint32_t flags = buffer ? 0 : kFfSampleError;
        memcpy(bytes + kFfSampleFlags, &flags, 4);
        RET(size);
    }
    if (IS("_Sound_Decode")) {
        HSTREAM stream = a[0] >= 0x1000 ? ff_stream_for(a[0]) : 0;
        if (!stream) RET(0);
        uint8_t *bytes = (void *)(uintptr_t)a[0];
        uint32_t buffer, size;
        memcpy(&buffer, bytes + kFfSampleBuffer, 4);
        memcpy(&size, bytes + kFfSampleBufferSize, 4);
        if (!buffer || !size) RET(0);
        DWORD got = BASS_ChannelGetData(stream, (void *)(uintptr_t)buffer, size);
        if (got == (DWORD)-1) got = 0;
        uint32_t flags = got < size ? kFfSampleEOF : 0;
        memcpy(bytes + kFfSampleFlags, &flags, 4);
        RET(got);
    }
    return 0;
#undef RET
#undef OUT
#undef P
#undef IS
}

#pragma clang diagnostic pop
