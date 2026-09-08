/*
 * The classic Mac Toolbox, for the two earliest titles the runtime carries:
 * Bejeweled 2 Deluxe (2004) and Zuma Deluxe (2003).  Both are the same early
 * PopCap engine and reach for a much older Mac OS X than the later games do:
 * the Memory, Queue, Notification, Process, Multiprocessing and Sound
 * Managers, plus Open Transport.  Their Carbon window, event and AGL surface
 * is already emulated in peggle_window.inc; what follows is everything else.
 *
 * The Sound Manager is the substantial part.  Both games statically link
 * FMOD 3, whose Mac output driver mixes into a double buffer that the game's
 * own CarbonSndPlayDoubleBuffer feeds to a Sound Manager channel with
 * bufferCmd/callBackCmd pairs, refilling from the channel callback by writing
 * the next commands straight into the channel's command queue.  So the
 * emulation keeps a faithful SndChannel (queue array, qHead/qTail) in guest
 * memory and plays each queued buffer through one AudioQueue per channel;
 * a buffer finishing is what advances the queue to the callback that refills
 * it, exactly as the real serial channel behaved.
 */

/* --- classic Memory Manager ------------------------------------------- */

/* NewPtr blocks are ordinary guest allocations.  Only MPGetAllocatedBlockSize
   needs the size back, so the size lives in a header word ahead of the block
   (16 bytes keeps the returned pointer 16-byte aligned, as the Memory Manager
   guaranteed). */
enum { kCtPtrHeader = 16 };

static uint32_t ct_new_ptr(uint32_t size, bool clear)
{
    if (size > UINT32_MAX - kCtPtrHeader) return 0;
    uint32_t block = compat_runtime32_allocate(size + kCtPtrHeader, clear);
    if (!block) return 0;
    *(uint32_t *)(uintptr_t)block = size;
    return block + kCtPtrHeader;
}

static uint32_t ct_ptr_size(uint32_t pointer)
{
    return pointer >= kCtPtrHeader ? *(uint32_t *)(uintptr_t)(pointer - kCtPtrHeader) : 0;
}

static void ct_dispose_ptr(uint32_t pointer)
{
    if (pointer >= kCtPtrHeader) compat_runtime32_deallocate(pointer - kCtPtrHeader);
}

/* --- Multiprocessing Services ----------------------------------------- */

/* MP tasks are pthreads running a guest entry point, MP critical regions are
   recursive mutexes (an MP task may re-enter one it already holds).  Both are
   addressed by small integer ids, which is all the guest ever stores. */
enum { kCtMPCapacity = 64 };
static struct { pthread_t thread; bool live; } ct_mp_tasks[kCtMPCapacity];
static pthread_mutex_t ct_mp_regions[kCtMPCapacity];
static bool ct_mp_region_live[kCtMPCapacity];
static pthread_mutex_t ct_mp_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t ct_mp_current_task;

struct ct_mp_start { uint32_t entry, parameter, identifier; };

static void *ct_mp_run(void *opaque)
{
    struct ct_mp_start start = *(struct ct_mp_start *)opaque;
    free(opaque);
    ct_mp_current_task = start.identifier;
    uint32_t argument = start.parameter;
    return (void *)(uintptr_t)compat_runtime32_call(start.entry, &argument, 1);
}

/* --- FSIterator handles ------------------------------------------------ */

/* FSIterator is an opaque host pointer; the guest keeps a 32-bit token. */
enum { kCtIteratorBase = 0x7f0c0000, kCtIteratorCapacity = 16 };
static FSIterator ct_iterators[kCtIteratorCapacity];

static uint32_t ct_iterator_register(FSIterator iterator)
{
    for (uint32_t i = 0; i < kCtIteratorCapacity; ++i) {
        if (!ct_iterators[i]) { ct_iterators[i] = iterator; return kCtIteratorBase + i; }
    }
    return 0;
}

static FSIterator ct_iterator_for(uint32_t handle)
{
    uint32_t index = handle - kCtIteratorBase;
    return index < kCtIteratorCapacity ? ct_iterators[index] : NULL;
}

/* --- Sound Manager ----------------------------------------------------- */

enum {
    kCtSndNullCmd = 0, kCtSndQuietCmd = 3, kCtSndFlushCmd = 4,
    kCtSndWaitCmd = 10, kCtSndPauseCmd = 11, kCtSndResumeCmd = 12,
    kCtSndCallBackCmd = 13, kCtSndAmpCmd = 43, kCtSndVolumeCmd = 46,
    kCtSndBufferCmd = 81, kCtSndRateMultiplierCmd = 86,
    /* SndChannel field offsets (68k-aligned, as the guest sees them). */
    kCtChanCallBack = 8, kCtChanUserInfo = 12, kCtChanFlags = 28,
    kCtChanQLength = 30, kCtChanQHead = 32, kCtChanQTail = 34,
    kCtChanQueue = 36, kCtChanQSlots = 128,
    kCtChannelSize = kCtChanQueue + kCtChanQSlots * 8,
    kCtSndChannelCapacity = 8,
    kCtSndQueueBuffers = 3,
};

struct ct_snd_channel {
    uint32_t handle;            /* guest SndChannelPtr */
    AudioQueueRef queue;
    AudioStreamBasicDescription format;
    pthread_mutex_t lock;
    /* Buffers handed to the queue that have not played out yet: the command
       queue may not run further ahead than kCtSndQueueBuffers of them. */
    unsigned buffers_in_flight;
    bool started;
    bool disposing;
    float amplitude;
    uint32_t command;           /* guest SndCommand handed to the callback */
};

static struct ct_snd_channel ct_snd_channels[kCtSndChannelCapacity];
static int ct_snd_trace = -1;

static bool ct_snd_tracing(void)
{
    if (ct_snd_trace < 0) ct_snd_trace = getenv("LP32_TRACE_SND") != NULL;
    return ct_snd_trace != 0;
}

static struct ct_snd_channel *ct_channel_for(uint32_t handle)
{
    for (unsigned i = 0; i < kCtSndChannelCapacity; ++i) {
        if (ct_snd_channels[i].handle && ct_snd_channels[i].handle == handle) {
            return &ct_snd_channels[i];
        }
    }
    return NULL;
}

static int16_t ct_chan_read16(uint32_t channel, uint32_t offset)
{
    return *(int16_t *)(uintptr_t)(channel + offset);
}

static void ct_chan_write16(uint32_t channel, uint32_t offset, int16_t value)
{
    *(int16_t *)(uintptr_t)(channel + offset) = value;
}

/* One sound header as the guest laid it out (stdSH, extSH or cmpSH). */
struct ct_sound_header {
    uint32_t samples;
    uint32_t channels;
    double rate;
    uint32_t frames;
    uint32_t sample_bits;
    uint32_t format;
    bool big_endian;
};

static bool ct_parse_sound_header(uint32_t header, struct ct_sound_header *out)
{
    if (header < 0x1000) return false;
    const uint8_t *bytes = (const void *)(uintptr_t)header;
    uint32_t sample_rate_fixed;
    memcpy(&out->samples, bytes + 0, 4);
    memcpy(&sample_rate_fixed, bytes + 8, 4);
    out->rate = (double)sample_rate_fixed / 65536.0;
    uint8_t encode = bytes[20];
    out->format = 0;
    if (encode == 0xff || encode == 0xfe) { /* extSH / cmpSH */
        memcpy(&out->channels, bytes + 4, 4);
        memcpy(&out->frames, bytes + 22, 4);
        uint16_t sample_size;
        memcpy(&sample_size, bytes + 62, 2);
        out->sample_bits = sample_size ? sample_size : 16;
        if (encode == 0xfe) memcpy(&out->format, bytes + 40, 4);
    } else { /* stdSH: 8-bit unsigned mono, length in bytes */
        uint32_t length;
        memcpy(&length, bytes + 4, 4);
        out->channels = 1;
        out->frames = length;
        out->sample_bits = 8;
    }
    if (!out->channels || out->channels > 2) out->channels = 1;
    if (out->rate < 1000.0 || out->rate > 192000.0) out->rate = 44100.0;
    /* 'twos' is the big-endian flavour of uncompressed 16-bit; 'NONE' and
       'sowt' are the host's own order (FMOD mixes natively). */
    out->big_endian = out->format == 0x74776f73;
    return out->samples >= 0x1000 && out->frames != 0;
}

static void ct_snd_pump(struct ct_snd_channel *channel);

static void ct_snd_output_callback(void *opaque, AudioQueueRef queue,
                                   AudioQueueBufferRef buffer)
{
    struct ct_snd_channel *channel = opaque;
    AudioQueueFreeBuffer(queue, buffer);
    pthread_mutex_lock(&channel->lock);
    if (channel->buffers_in_flight) --channel->buffers_in_flight;
    pthread_mutex_unlock(&channel->lock);
    /* The finished buffer is what lets the channel reach the callBackCmd that
       follows it, which is where the game refills and re-queues. */
    if (!channel->disposing) ct_snd_pump(channel);
}

static bool ct_snd_ensure_queue(struct ct_snd_channel *channel,
                                const struct ct_sound_header *header)
{
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = header->rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsPacked |
        (header->sample_bits == 8 ? 0 : kAudioFormatFlagIsSignedInteger) |
        (header->big_endian ? kAudioFormatFlagIsBigEndian : 0);
    format.mChannelsPerFrame = header->channels;
    format.mBitsPerChannel = header->sample_bits;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = header->channels * header->sample_bits / 8;
    format.mBytesPerPacket = format.mBytesPerFrame;
    if (channel->queue) {
        if (memcmp(&channel->format, &format, sizeof(format)) == 0) return true;
        /* A format change replaces the queue.  Disposing it here would block
           on the callback thread while this one holds the channel lock, so
           hand the old queue over asynchronously. */
        AudioQueueRef stale = channel->queue;
        channel->queue = NULL;
        channel->started = false;
        channel->buffers_in_flight = 0;
        AudioQueueDispose(stale, false);
    }
    OSStatus status = AudioQueueNewOutput(&format, ct_snd_output_callback,
                                          channel, NULL, NULL, 0, &channel->queue);
    if (status != noErr) {
        fprintf(stderr, "compat32: AudioQueueNewOutput failed (%d)\n", (int)status);
        channel->queue = NULL;
        return false;
    }
    channel->format = format;
    AudioQueueSetParameter(channel->queue, kAudioQueueParam_Volume,
                           channel->amplitude);
    return true;
}

/* Plays one bufferCmd; returns true when the channel must now wait for it. */
static bool ct_snd_play_buffer(struct ct_snd_channel *channel, uint32_t header_address)
{
    struct ct_sound_header header;
    if (!ct_parse_sound_header(header_address, &header)) return false;
    if (!ct_snd_ensure_queue(channel, &header)) return false;
    uint32_t bytes = header.frames * channel->format.mBytesPerFrame;
    AudioQueueBufferRef buffer = NULL;
    if (AudioQueueAllocateBuffer(channel->queue, bytes, &buffer) != noErr) return false;
    memcpy(buffer->mAudioData, (const void *)(uintptr_t)header.samples, bytes);
    buffer->mAudioDataByteSize = bytes;
    if (AudioQueueEnqueueBuffer(channel->queue, buffer, 0, NULL) != noErr) {
        AudioQueueFreeBuffer(channel->queue, buffer);
        return false;
    }
    ++channel->buffers_in_flight;
    if (!channel->started) {
        channel->started = AudioQueueStart(channel->queue, NULL) == noErr;
    }
    return true;
}

/*
 * Runs the channel's command queue until it must wait.  The Sound Manager
 * played a channel serially: a bufferCmd holds the queue until its buffer has
 * been consumed, and only then does the callBackCmd behind it fire.  Keeping
 * kCtSndQueueBuffers buffers in flight bounds how far ahead the game mixes
 * while keeping the output fed.
 */
static void ct_snd_pump(struct ct_snd_channel *channel)
{
    for (;;) {
        pthread_mutex_lock(&channel->lock);
        uint32_t handle = channel->handle;
        if (!handle || channel->disposing ||
            channel->buffers_in_flight >= kCtSndQueueBuffers) {
            pthread_mutex_unlock(&channel->lock);
            return;
        }
        int16_t head = ct_chan_read16(handle, kCtChanQHead);
        int16_t tail = ct_chan_read16(handle, kCtChanQTail);
        if (head < 0 || head >= kCtChanQSlots) {
            pthread_mutex_unlock(&channel->lock);
            return;
        }
        const uint8_t *slot = (const void *)(uintptr_t)(handle + kCtChanQueue + head * 8);
        uint16_t command;
        uint32_t parameter2;
        int16_t parameter1;
        memcpy(&command, slot + 0, 2);
        memcpy(&parameter1, slot + 2, 2);
        memcpy(&parameter2, slot + 4, 4);
        int16_t next = (int16_t)((head + 1) % kCtChanQSlots);
        ct_chan_write16(handle, kCtChanQHead, next == tail ? -1 : next);
        command &= 0x7fff; /* dataOffsetFlag: never set by this game */
        if (ct_snd_tracing()) {
            fprintf(stderr, "compat32: snd channel 0x%08x cmd=%u param1=%d param2=0x%08x\n",
                    handle, command, parameter1, parameter2);
        }
        if (command == kCtSndBufferCmd) {
            bool waiting = ct_snd_play_buffer(channel, parameter2);
            pthread_mutex_unlock(&channel->lock);
            if (waiting) return;
            continue;
        }
        if (command == kCtSndAmpCmd || command == kCtSndVolumeCmd) {
            float level = command == kCtSndAmpCmd ? (float)parameter1 / 255.0f :
                (float)(parameter2 & 0xffff) / 256.0f;
            channel->amplitude = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
            if (channel->queue) {
                AudioQueueSetParameter(channel->queue, kAudioQueueParam_Volume,
                                       channel->amplitude);
            }
            pthread_mutex_unlock(&channel->lock);
            continue;
        }
        if (command == kCtSndCallBackCmd) {
            uint32_t callback = *(uint32_t *)(uintptr_t)(handle + kCtChanCallBack);
            uint32_t command_block = channel->command;
            pthread_mutex_unlock(&channel->lock);
            if (callback && command_block) {
                uint16_t stored = (uint16_t)command;
                memcpy((void *)(uintptr_t)(command_block + 0), &stored, 2);
                memcpy((void *)(uintptr_t)(command_block + 2), &parameter1, 2);
                memcpy((void *)(uintptr_t)(command_block + 4), &parameter2, 4);
                uint32_t arguments[2] = {handle, command_block};
                /* The game's callback prepends the next bufferCmd/callBackCmd
                   straight into the queue, so the loop picks them up. */
                compat_runtime32_call(callback, arguments, 2);
            }
            continue;
        }
        /* quietCmd/flushCmd reached in order simply drain what is queued;
           the immediate forms below are the ones that stop playback. */
        pthread_mutex_unlock(&channel->lock);
    }
}

/*
 * AudioQueueStop/Dispose block until the queue's own callback thread has
 * finished, and that thread runs ct_snd_pump, so neither may be called with
 * the channel lock held or the two would wait on each other.  Every stop
 * therefore reads the queue out under the lock and acts on it outside.
 */
static void ct_snd_stop(struct ct_snd_channel *channel, bool flush)
{
    pthread_mutex_lock(&channel->lock);
    AudioQueueRef queue = channel->queue;
    channel->started = false;
    channel->buffers_in_flight = 0;
    if (flush && channel->handle) {
        ct_chan_write16(channel->handle, kCtChanQHead, -1);
        ct_chan_write16(channel->handle, kCtChanQTail, 0);
    }
    pthread_mutex_unlock(&channel->lock);
    if (queue) {
        AudioQueueStop(queue, true);
        if (flush) AudioQueueReset(queue);
    }
}

static uint32_t ct_snd_new_channel(uint32_t channel_out, uint32_t callback)
{
    struct ct_snd_channel *channel = NULL;
    for (unsigned i = 0; i < kCtSndChannelCapacity; ++i) {
        if (!ct_snd_channels[i].handle) { channel = &ct_snd_channels[i]; break; }
    }
    if (!channel) return (uint32_t)(int32_t)-203; /* notEnoughHardwareErr */
    uint32_t handle = channel_out && *(uint32_t *)(uintptr_t)channel_out ?
        *(uint32_t *)(uintptr_t)channel_out :
        compat_runtime32_allocate(kCtChannelSize, 1);
    if (!handle) return (uint32_t)(int32_t)-108; /* memFullErr */
    memset((void *)(uintptr_t)handle, 0, kCtChannelSize);
    *(uint32_t *)(uintptr_t)(handle + kCtChanCallBack) = callback;
    ct_chan_write16(handle, kCtChanQLength, kCtChanQSlots);
    ct_chan_write16(handle, kCtChanQHead, -1);
    ct_chan_write16(handle, kCtChanQTail, 0);
    memset(channel, 0, sizeof(*channel));
    pthread_mutex_init(&channel->lock, NULL);
    channel->handle = handle;
    channel->amplitude = 1.0f;
    channel->command = compat_runtime32_allocate(8, 1);
    if (channel_out) *(uint32_t *)(uintptr_t)channel_out = handle;
    return 0;
}

static uint32_t ct_snd_do_command(uint32_t handle, uint32_t command_address)
{
    struct ct_snd_channel *channel = ct_channel_for(handle);
    if (!channel || command_address < 0x1000) return (uint32_t)(int32_t)-205; /* badChannel */
    pthread_mutex_lock(&channel->lock);
    int16_t head = ct_chan_read16(handle, kCtChanQHead);
    int16_t tail = ct_chan_read16(handle, kCtChanQTail);
    if (tail < 0 || tail >= kCtChanQSlots) tail = 0;
    memcpy((void *)(uintptr_t)(handle + kCtChanQueue + tail * 8),
           (const void *)(uintptr_t)command_address, 8);
    if (head < 0) ct_chan_write16(handle, kCtChanQHead, tail);
    ct_chan_write16(handle, kCtChanQTail, (int16_t)((tail + 1) % kCtChanQSlots));
    pthread_mutex_unlock(&channel->lock);
    ct_snd_pump(channel);
    return 0;
}

static uint32_t ct_snd_do_immediate(uint32_t handle, uint32_t command_address)
{
    struct ct_snd_channel *channel = ct_channel_for(handle);
    if (!channel || command_address < 0x1000) return (uint32_t)(int32_t)-205;
    uint16_t command;
    memcpy(&command, (const void *)(uintptr_t)command_address, 2);
    command &= 0x7fff;
    switch (command) {
    case kCtSndQuietCmd:
    case kCtSndFlushCmd:
        ct_snd_stop(channel, true);
        return 0;
    case kCtSndPauseCmd:
        if (channel->queue) AudioQueuePause(channel->queue);
        return 0;
    case kCtSndResumeCmd:
        if (channel->queue) AudioQueueStart(channel->queue, NULL);
        return 0;
    default:
        break;
    }
    return ct_snd_do_command(handle, command_address);
}

static uint32_t ct_snd_dispose_channel(uint32_t handle)
{
    struct ct_snd_channel *channel = ct_channel_for(handle);
    if (!channel) return 0;
    channel->disposing = true; /* stops ct_snd_pump before it takes the lock */
    ct_snd_stop(channel, true);
    pthread_mutex_lock(&channel->lock);
    AudioQueueRef queue = channel->queue;
    channel->queue = NULL;
    channel->handle = 0;
    pthread_mutex_unlock(&channel->lock);
    if (queue) AudioQueueDispose(queue, true);
    return 0;
}

/* --- dispatch ---------------------------------------------------------- */

static int classic_toolbox_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define OUT(i, v) do { if (a[i]) *(uint32_t *)P(i) = (uint32_t)(v); } while (0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    enum lp32_title title = lp32_profile()->title;
    if (title != LP32_TITLE_BEJEWELED2 && title != LP32_TITLE_ZUMA) return 0;

    /* --- Memory Manager --- */
    if (IS("_NewPtr")) RET(ct_new_ptr(a[0], false));
    if (IS("_NewPtrClear")) RET(ct_new_ptr(a[0], true));
    if (IS("_DisposePtr")) { ct_dispose_ptr(a[0]); RET(0); }
    if (IS("_MemError")) RET(0);
    if (IS("_GetPtrSize")) RET(ct_ptr_size(a[0]));

    /* --- classic OS queues (QHdr is 68k-aligned: flags@0, head@2, tail@6) --- */
    if (IS("_Enqueue")) {
        uint32_t element = a[0], header = a[1];
        if (!element || !header) RET(0);
        *(uint32_t *)(uintptr_t)element = 0;
        uint32_t *head = (void *)(uintptr_t)(header + 2);
        uint32_t *tail = (void *)(uintptr_t)(header + 6);
        if (*head) *(uint32_t *)(uintptr_t)*tail = element; else *head = element;
        *tail = element;
        RET(0);
    }
    if (IS("_Dequeue")) {
        uint32_t element = a[0], header = a[1];
        if (!element || !header) RET((uint32_t)(int32_t)-1); /* qErr */
        uint32_t *head = (void *)(uintptr_t)(header + 2);
        uint32_t *tail = (void *)(uintptr_t)(header + 6);
        uint32_t previous = 0, current = *head;
        while (current && current != element) {
            previous = current;
            current = *(uint32_t *)(uintptr_t)current;
        }
        if (!current) RET((uint32_t)(int32_t)-1);
        uint32_t next = *(uint32_t *)(uintptr_t)current;
        if (previous) *(uint32_t *)(uintptr_t)previous = next; else *head = next;
        if (*tail == element) *tail = previous;
        RET(0);
    }
    if (IS("_BitTst")) {
        const uint8_t *base = P(0);
        int32_t bit = (int32_t)a[1];
        RET(base ? (base[bit >> 3] >> (7 - (bit & 7))) & 1 : 0);
    }
    if (IS("_BitClr")) {
        uint8_t *base = P(0);
        int32_t bit = (int32_t)a[1];
        if (base) base[bit >> 3] &= (uint8_t)~(1u << (7 - (bit & 7)));
        RET(0);
    }

    /* --- Notification Manager: the response runs on the caller's thread,
           which is where the deferred work would have ended up anyway. --- */
    if (IS("_NewNMUPP")) RET(a[0]);
    if (IS("_NMInstall")) {
        uint32_t record = a[0];
        uint32_t response = record ? *(uint32_t *)(uintptr_t)(record + 28) : 0;
        if (response) compat_runtime32_call(response, &record, 1);
        RET(0);
    }
    if (IS("_NMRemove")) RET(0);

    /* --- Multiprocessing Services --- */
    if (IS("__MPIsFullyInitialized")) RET(1);
    if (IS("_MPCreateTask")) {
        pthread_mutex_lock(&ct_mp_lock);
        uint32_t identifier = 0;
        for (uint32_t i = 1; i < kCtMPCapacity; ++i) {
            if (!ct_mp_tasks[i].live) { identifier = i; break; }
        }
        if (identifier) ct_mp_tasks[identifier].live = true;
        pthread_mutex_unlock(&ct_mp_lock);
        if (!identifier) RET((uint32_t)(int32_t)-1); /* out of task slots */
        struct ct_mp_start *start = malloc(sizeof(*start));
        *start = (struct ct_mp_start){a[0], a[1], identifier};
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        if (a[2] >= 65536) pthread_attr_setstacksize(&attributes, a[2]);
        int error = pthread_create(&ct_mp_tasks[identifier].thread, &attributes,
                                   ct_mp_run, start);
        pthread_attr_destroy(&attributes);
        if (error) {
            free(start);
            ct_mp_tasks[identifier].live = false;
            RET((uint32_t)(int32_t)-1);
        }
        OUT(7, identifier);
        RET(0);
    }
    if (IS("_MPTerminateTask")) {
        uint32_t identifier = a[0];
        if (identifier && identifier < kCtMPCapacity && ct_mp_tasks[identifier].live) {
            pthread_cancel(ct_mp_tasks[identifier].thread);
            ct_mp_tasks[identifier].live = false;
        }
        RET(0);
    }
    if (IS("_MPCurrentTaskID")) RET(ct_mp_current_task);
    if (IS("_MPCreateCriticalRegion")) {
        pthread_mutex_lock(&ct_mp_lock);
        uint32_t identifier = 0;
        for (uint32_t i = 1; i < kCtMPCapacity; ++i) {
            if (!ct_mp_region_live[i]) { identifier = i; break; }
        }
        if (identifier) {
            pthread_mutexattr_t attributes;
            pthread_mutexattr_init(&attributes);
            pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&ct_mp_regions[identifier], &attributes);
            pthread_mutexattr_destroy(&attributes);
            ct_mp_region_live[identifier] = true;
        }
        pthread_mutex_unlock(&ct_mp_lock);
        if (!identifier) RET((uint32_t)(int32_t)-1);
        OUT(0, identifier);
        RET(0);
    }
    if (IS("_MPDeleteCriticalRegion")) {
        if (a[0] && a[0] < kCtMPCapacity && ct_mp_region_live[a[0]]) {
            ct_mp_region_live[a[0]] = false;
            pthread_mutex_destroy(&ct_mp_regions[a[0]]);
        }
        RET(0);
    }
    if (IS("_MPEnterCriticalRegion")) {
        if (!a[0] || a[0] >= kCtMPCapacity || !ct_mp_region_live[a[0]]) RET((uint32_t)(int32_t)-1);
        if (a[1] == 0) RET(pthread_mutex_trylock(&ct_mp_regions[a[0]]) == 0 ? 0 : (uint32_t)(int32_t)-3);
        pthread_mutex_lock(&ct_mp_regions[a[0]]);
        RET(0);
    }
    if (IS("_MPExitCriticalRegion")) {
        if (a[0] && a[0] < kCtMPCapacity && ct_mp_region_live[a[0]]) {
            pthread_mutex_unlock(&ct_mp_regions[a[0]]);
        }
        RET(0);
    }
    if (IS("_MPAllocateAligned")) RET(ct_new_ptr(a[0], false));
    if (IS("_MPFree")) { ct_dispose_ptr(a[0]); RET(0); }
    if (IS("_MPGetAllocatedBlockSize")) RET(ct_ptr_size(a[0]));
    if (IS("_MPBlockCopy")) { if (a[0] && a[1]) memcpy(P(1), P(0), a[2]); RET(0); }

    /* --- Sound Manager --- */
    if (IS("_SndSoundManagerVersion")) RET(0x03000000); /* NumVersion 3.0 */
    if (IS("_NewSndCallBackUPP") || IS("_NewSICompletionUPP")) RET(a[0]);
    if (IS("_SndNewChannel")) RET(ct_snd_new_channel(a[0], a[3]));
    if (IS("_SndDoCommand")) RET(ct_snd_do_command(a[0], a[1]));
    if (IS("_SndDoImmediate")) RET(ct_snd_do_immediate(a[0], a[1]));
    if (IS("_SndDisposeChannel")) RET(ct_snd_dispose_channel(a[0]));
    if (IS("_GetCompressionInfo")) {
        /* Only the uncompressed formats matter here: FMOD hands the Sound
           Manager 8- or 16-bit PCM. */
        uint8_t *info = P(4);
        if (!info) RET((uint32_t)(int32_t)-231); /* badFormat */
        uint32_t record_size = 20, format = a[1] ? a[1] : 0x4e4f4e45 /* 'NONE' */;
        uint16_t channels = (uint16_t)(a[2] ? a[2] : 1);
        uint16_t sample_bits = (uint16_t)(a[3] ? a[3] : 16);
        uint16_t bytes_per_sample = (uint16_t)(sample_bits / 8);
        uint16_t values[] = {(uint16_t)a[0], 1, bytes_per_sample,
                             (uint16_t)(bytes_per_sample * channels), bytes_per_sample, 0};
        memcpy(info + 0, &record_size, 4);
        memcpy(info + 4, &format, 4);
        memcpy(info + 8, values, sizeof(values));
        RET(0);
    }
    /* Sound input (FMOD's recording support) has no device here. */
    if (strncmp(n, "_SPB", 4) == 0) RET((uint32_t)(int32_t)-228); /* siNoSoundInHardware */

    /* --- Open Transport: this build has no working network back end, so
           report "not available" and the game's online paths stay quiet. --- */
    if (IS("_InitOpenTransportInContext")) { OUT(1, 0); RET((uint32_t)(int32_t)-3201); }
    if (IS("_NewOTNotifyUPP")) RET(a[0]);
    if (strncmp(n, "_OT", 3) == 0 || IS("_CloseOpenTransportInContext")) {
        RET((uint32_t)(int32_t)-3201); /* kOTNotFoundErr */
    }

    /* --- Process Manager: report an empty process list (the game only looks
           for a second copy of itself) and honour "come to the front". --- */
    if (IS("_GetNextProcess") || IS("_GetProcessInformation")) {
        RET((uint32_t)(int32_t)-600); /* procNotFound */
    }
    if (IS("_SameProcess")) {
        const uint32_t *first = P(0), *second = P(1);
        OUT(2, first && second && first[0] == second[0] && first[1] == second[1]);
        RET(0);
    }
    if (IS("_SetFrontProcess")) {
        [NSApp activateIgnoringOtherApps:YES];
        RET(0);
    }

    /* --- File Manager pieces beyond the ones Peggle needs --- */
    if (IS("_FSGetCatalogInfo")) RET((uint32_t)FSGetCatalogInfo(P(0), a[1], P(2), P(3), P(4), P(5)));
    if (IS("_FSSetCatalogInfo")) RET((uint32_t)FSSetCatalogInfo(P(0), a[1], P(2)));
    if (IS("_FSDeleteObject")) RET((uint32_t)FSDeleteObject(P(0)));
    if (IS("_FSRenameUnicode")) RET((uint32_t)FSRenameUnicode(P(0), a[1], P(2), a[3], P(4)));
    if (IS("_FSOpenIterator")) {
        FSIterator iterator = NULL;
        OSErr error = FSOpenIterator(P(0), a[1], &iterator);
        uint32_t handle = error == noErr ? ct_iterator_register(iterator) : 0;
        if (error == noErr && !handle) { FSCloseIterator(iterator); error = memFullErr; }
        OUT(2, handle);
        RET((uint32_t)error);
    }
    if (IS("_FSCloseIterator")) {
        FSIterator iterator = ct_iterator_for(a[0]);
        if (iterator) {
            FSCloseIterator(iterator);
            ct_iterators[a[0] - kCtIteratorBase] = NULL;
        }
        RET(0);
    }
    if (IS("_FSGetCatalogInfoBulk")) {
        FSIterator iterator = ct_iterator_for(a[0]);
        if (!iterator) RET((uint32_t)(int32_t)-1417); /* errFSBadIteratorFlags */
        RET((uint32_t)FSGetCatalogInfoBulk(iterator, a[1], P(2), P(3), a[4],
                                           P(5), P(6), P(7), P(8)));
    }

    /* --- time --- */
    if (IS("_TickCount")) {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        uint64_t nanoseconds = mach_absolute_time() * timebase.numer / timebase.denom;
        RET((uint32_t)(nanoseconds * 60 / 1000000000ull));
    }
    if (IS("_AbsoluteToDuration")) {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        uint64_t ticks = (uint64_t)a[0] | ((uint64_t)a[1] << 32);
        uint64_t nanoseconds = ticks * timebase.numer / timebase.denom;
        /* Durations are milliseconds, or negative microseconds below 2 ms. */
        if (nanoseconds < 2000000ull) RET((uint32_t)-(int32_t)(nanoseconds / 1000));
        RET((uint32_t)(int32_t)(nanoseconds / 1000000));
    }
    if (IS("_AddDurationToAbsolute")) {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        int32_t duration = (int32_t)a[0];
        uint64_t time = (uint64_t)a[1] | ((uint64_t)a[2] << 32);
        int64_t nanoseconds = duration < 0 ? -(int64_t)duration * 1000ll :
                                              (int64_t)duration * 1000000ll;
        RET(time + (uint64_t)(nanoseconds * timebase.denom / timebase.numer));
    }
    if (IS("_YieldToAnyThread")) { sched_yield(); RET(0); }

    /* --- odds and ends --- */
    if (IS("_SetA5") || IS("_SetCurrentA5")) RET(0); /* 68k world, long gone */
    if (IS("_Debugger")) RET(0);
    if (IS("_Fix2Long")) RET((uint32_t)(((int32_t)a[0] + 0x8000) >> 16));
    if (IS("_LocalToGlobal")) {
        struct pg_win *window = pg_main_win();
        int16_t *point = P(0);
        if (point && window) {
            int16_t bounds[4];
            pg_qd_from_rect(pg_content_rect(window), bounds);
            point[0] += bounds[0];
            point[1] += bounds[1];
        }
        RET(0);
    }
    if (IS("_CollapseWindow") || IS("_SetWindowGroupLevel") ||
        IS("_ActivateWindow") || IS("_BringToFront")) RET(0);
    if (IS("_GetWindowGroup")) RET(2);
    if (IS("_GetCursor")) {
        /* A zeroed Cursor behind a real handle: the game hands the result
           straight to SetCursor, which this bridge ignores anyway. */
        static uint32_t cursor_handle;
        if (!cursor_handle) {
            uint32_t cursor = compat_runtime32_allocate(68, 1);
            cursor_handle = compat_runtime32_allocate(4, 1);
            if (cursor_handle) *(uint32_t *)(uintptr_t)cursor_handle = cursor;
        }
        RET(cursor_handle);
    }
    if (IS("_SetCursor")) RET(0);
    if (IS("_GetMainDevice")) {
        /* A minimal GDevice/PixMap pair describing the game's display, for
           the depth and bounds queries the engine makes of it. */
        static uint32_t device_handle;
        if (!device_handle) {
            NSSize size = guest_display_mode_active() ? guest_display_mode :
                preferred_display_native_size();
            uint32_t pixmap = compat_runtime32_allocate(50, 1);
            uint32_t pixmap_handle = compat_runtime32_allocate(4, 1);
            uint32_t device = compat_runtime32_allocate(62, 1);
            device_handle = compat_runtime32_allocate(4, 1);
            if (!pixmap || !pixmap_handle || !device || !device_handle) {
                device_handle = 0;
                RET(0);
            }
            int16_t bounds[4] = {0, 0, (int16_t)size.height, (int16_t)size.width};
            int16_t row_bytes = (int16_t)(0x8000 | (uint16_t)((int)size.width * 4));
            memcpy((uint8_t *)(uintptr_t)pixmap + 4, &row_bytes, 2);
            memcpy((uint8_t *)(uintptr_t)pixmap + 6, bounds, sizeof(bounds));
            int16_t pixel_size = 32, component_count = 3, component_size = 8;
            memcpy((uint8_t *)(uintptr_t)pixmap + 32, &pixel_size, 2);
            memcpy((uint8_t *)(uintptr_t)pixmap + 34, &component_count, 2);
            memcpy((uint8_t *)(uintptr_t)pixmap + 36, &component_size, 2);
            *(uint32_t *)(uintptr_t)pixmap_handle = pixmap;
            *(uint32_t *)(uintptr_t)(device + 22) = pixmap_handle;
            memcpy((uint8_t *)(uintptr_t)device + 34, bounds, sizeof(bounds));
            *(uint32_t *)(uintptr_t)device_handle = device;
        }
        RET(device_handle);
    }
    /* Same "best mode" logic as the older call, with the refresh rate (a
       double, so two words) sitting between the size and the match flag. */
    if (IS("_CGDisplayBestModeForParametersAndRefreshRate")) {
        uint32_t forwarded[5] = {a[0], a[1], a[2], a[3], a[6]};
        return objc_bridge32_dispatch("_CGDisplayBestModeForParameters", forwarded, r);
    }
    return 0;
#undef RET
#undef OUT
#undef P
#undef IS
}
