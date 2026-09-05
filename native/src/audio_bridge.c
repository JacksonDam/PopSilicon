#include "audio_bridge.h"
#include "compat_runtime.h"
#include "game_profile.h"
#include "name_match.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * AUGraph and AudioUnit are pointers in the 64-bit host ABI.  The title only
 * has room for four-byte values, so it sees typed handles in otherwise unused
 * low address ranges.  AUNode and AudioDeviceID are already 32-bit values and
 * pass through unchanged.
 */
enum {
    kGuestGraphHandleBase = 0x7f050000,
    kGuestUnitHandleBase = 0x7f060000,
    kGuestAudioHandleStride = 16,
    kGuestAudioHandleCapacity = 4096,
    kAudioCallbackCapacity = 256,
    kAudioCallbackBufferCapacity = 32,
    kAudioWorkerCount = 8,
};

static AUGraph graph_objects[kGuestAudioHandleCapacity];
static AudioUnit unit_objects[kGuestAudioHandleCapacity];
static AUGraph unit_owner_graphs[kGuestAudioHandleCapacity];
static uint32_t graph_object_count;
static uint32_t unit_object_count;
static pthread_mutex_t audio_object_lock = PTHREAD_MUTEX_INITIALIZER;

struct guest_audio_buffer {
    uint32_t channels;
    uint32_t byte_size;
    uint32_t data;
};

struct audio_callback_context {
    uint32_t callback_index;
    const char *callback_kind;
    AUGraph owner_graph;
    AUNode callback_node;
    UInt32 callback_bus;
    bool in_use;
    bool callback_attached;
    /* Set when the guest has stopped or disposed the owning graph but the
       host teardown is still pending; the callback then renders silence
       without entering guest code. */
    bool muted;
    uint32_t guest_function;
    uint32_t guest_refcon;
    uint32_t guest_flags;
    uint32_t guest_timestamp;
    uint32_t guest_buffer_list;
    uint32_t guest_data[kAudioCallbackBufferCapacity];
    size_t guest_data_capacity[kAudioCallbackBufferCapacity];
    pthread_mutex_t lock;
    pthread_cond_t request_condition;
    pthread_cond_t response_condition;
    bool request_pending;
    bool response_ready;
    bool host_has_buffer_list;
    uint32_t bus;
    uint32_t frame_count;
    int32_t guest_status;
    uint64_t host_call_count;
    /* Expected mSampleTime of the next render on this callback; a larger
       value means the HAL skipped IO cycles between two renders. */
    double next_sample_time;
    bool next_sample_time_valid;
    /* Set when the guest asks for AUGraphStart on the owner graph; cleared
       (and reported) by the first render that follows. */
    uint64_t start_requested_ns;
};

static uint64_t render_silent_count;
static uint64_t render_gap_count;
static uint64_t render_gap_frames;
static uint64_t render_callback_max_ns;

/*
 * Audio hold.  The engine's tick clamps each frame's delta to 100 ms, so a
 * frame that takes longer (first-use shader compilation inside the GL driver
 * is the usual cause, several hundred ms under Rosetta) freezes the picture
 * while real-time audio keeps playing; everything after the stall then sounds
 * early against the animation, and cutscenes never recover.  The render
 * callbacks watch the presentation clock: once a frame has been outstanding
 * for longer than the clamp they output silence without advancing the guest's
 * streams, for exactly the part of the frame the engine is about to discard.
 */
static uint64_t hold_last_frame_ns;
static uint64_t hold_renders;
static uint64_t hold_episodes;
static uint64_t hold_total_ns;

static struct audio_callback_context *audio_callbacks[kAudioCallbackCapacity];
static uint32_t audio_callback_count;
static pthread_mutex_t audio_callback_registry_lock = PTHREAD_MUTEX_INITIALIZER;

struct audio_callback_job {
    struct audio_callback_context *context;
    struct audio_callback_job *next;
};

static pthread_mutex_t audio_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audio_job_condition = PTHREAD_COND_INITIALIZER;
static struct audio_callback_job *audio_job_head;
static struct audio_callback_job *audio_job_tail;
static pthread_once_t audio_worker_once = PTHREAD_ONCE_INIT;
static bool audio_worker_available;

static uint64_t monotonic_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static bool trace_audio(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_TRACE_AUDIO") != NULL;
    return enabled != 0;
}

static bool trace_audio_latency(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_TRACE_AUDIO_LATENCY") != NULL;
    return enabled != 0;
}

static bool mute_audio_output(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_MUTE_AUDIO") != NULL;
    return enabled != 0;
}

/* Frame delta clamp shared by both titles (0.1f in the tick).  LP32_AUDIO_HOLD_MS
   overrides it; 0 disables the hold. */
static uint64_t hold_threshold_ns(void)
{
    static uint64_t threshold = UINT64_MAX;
    if (threshold == UINT64_MAX) {
        const char *value = getenv("LP32_AUDIO_HOLD_MS");
        threshold = (value ? strtoull(value, NULL, 0) : 100) * 1000000ull;
    }
    return threshold;
}

static bool audio_hold_active(void)
{
    uint64_t threshold = hold_threshold_ns();
    uint64_t last = __atomic_load_n(&hold_last_frame_ns, __ATOMIC_ACQUIRE);
    return threshold && last && monotonic_ns() - last > threshold;
}

void audio_bridge32_note_frame_presented(void)
{
    uint64_t now = monotonic_ns();
    uint64_t last = __atomic_exchange_n(&hold_last_frame_ns, now, __ATOMIC_ACQ_REL);
    uint64_t threshold = hold_threshold_ns();
    if (last && threshold && now - last > threshold) {
        __atomic_fetch_add(&hold_episodes, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&hold_total_ns, now - last - threshold, __ATOMIC_RELAXED);
        if (trace_audio_latency()) {
            fprintf(stderr,
                    "compat32: audio hold t=%.3f frame took %.0f ms, held %.0f ms\n",
                    now / 1e9, (now - last) / 1e6, (now - last - threshold) / 1e6);
        }
    }
}

static const char *fourcc_text(uint32_t value, char text[5])
{
    for (unsigned index = 0; index < 4; ++index) {
        unsigned char character = (unsigned char)(value >> (24 - index * 8));
        text[index] = character >= 0x20 && character <= 0x7e ?
                      (char)character : '.';
    }
    text[4] = '\0';
    return text;
}

static void trace_audio_status(const char *operation, OSStatus status)
{
    if (!trace_audio()) return;
    char text[5];
    fprintf(stderr, "compat32: audio %s -> %d (0x%08x '%s')\n",
            operation, (int)status, (uint32_t)status,
            fourcc_text((uint32_t)status, text));
}

/*
 * AudioBufferList has a four-byte-aligned, 12-byte AudioBuffer in the i386
 * ABI and an eight-byte-aligned, 16-byte AudioBuffer in the x86_64 ABI.  The
 * stream-configuration property is the one legacy CoreAudio query in this
 * game that exposes the structure directly.  Convert it instead of letting
 * the guest interpret host alignment padding as a channel count.
 */
static OSStatus get_host_stream_configuration(AudioDeviceID device,
                                               UInt32 channel,
                                               Boolean is_input,
                                               AudioBufferList **output,
                                               UInt32 *output_size)
{
    UInt32 size = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    OSStatus status = AudioDeviceGetPropertyInfo(
        device, channel, is_input, kAudioDevicePropertyStreamConfiguration,
        &size, NULL);
    if (status != noErr) return status;
    AudioBufferList *list = malloc(size ? size : sizeof(AudioBufferList));
    if (!list) return kAudio_MemFullError;
    UInt32 actual_size = size;
    status = AudioDeviceGetProperty(
        device, channel, is_input, kAudioDevicePropertyStreamConfiguration,
        &actual_size, list);
#pragma clang diagnostic pop
    if (status != noErr) {
        free(list);
        return status;
    }
    *output = list;
    *output_size = actual_size;
    return noErr;
}

static uint32_t bounded_host_audio_buffer_count(const AudioBufferList *list,
                                                UInt32 byte_size)
{
    size_t buffer_offset = offsetof(AudioBufferList, mBuffers);
    if (!list || byte_size < buffer_offset) return 0;
    size_t capacity = (byte_size - buffer_offset) / sizeof(AudioBuffer);
    return list->mNumberBuffers < capacity ? list->mNumberBuffers :
                                             (uint32_t)capacity;
}

static UInt32 guest_audio_buffer_list_size(uint32_t buffer_count)
{
    return (UInt32)(sizeof(uint32_t) +
                    buffer_count * sizeof(struct guest_audio_buffer));
}

static void *audio_callback_worker(void *opaque)
{
    (void)opaque;
    for (;;) {
        pthread_mutex_lock(&audio_job_lock);
        while (!audio_job_head) {
            pthread_cond_wait(&audio_job_condition, &audio_job_lock);
        }
        struct audio_callback_job *job = audio_job_head;
        audio_job_head = job->next;
        if (!audio_job_head) audio_job_tail = NULL;
        pthread_mutex_unlock(&audio_job_lock);

        struct audio_callback_context *context = job->context;
        pthread_mutex_lock(&context->lock);
        const uint32_t guest_arguments[] = {
            context->guest_refcon,
            context->guest_flags,
            context->guest_timestamp,
            context->bus,
            context->frame_count,
            context->host_has_buffer_list ? context->guest_buffer_list : 0,
        };
        context->guest_status = (int32_t)compat_runtime32_call(
            context->guest_function, guest_arguments,
            sizeof(guest_arguments) / sizeof(guest_arguments[0]));
        if (compat_runtime32_last_call_trapped()) {
            fprintf(stderr,
                    "compat32: guest audio callback 0x%08x escaped\n",
                    context->guest_function);
            context->guest_status = kAudio_ParamError;
        }
        context->response_ready = true;
        pthread_cond_signal(&context->response_condition);
        pthread_mutex_unlock(&context->lock);
    }
}

static void start_audio_callback_worker(void)
{
    unsigned created = 0;
    for (unsigned index = 0; index < kAudioWorkerCount; ++index) {
        pthread_t worker;
        if (pthread_create(&worker, NULL, audio_callback_worker, NULL) == 0) {
            pthread_detach(worker);
            ++created;
        }
    }
    audio_worker_available = created != 0;
}

static uint32_t guest_handle_for_graph(AUGraph graph)
{
    if (!graph) return 0;
    pthread_mutex_lock(&audio_object_lock);
    for (uint32_t index = 0; index < graph_object_count; ++index) {
        if (graph_objects[index] == graph) {
            pthread_mutex_unlock(&audio_object_lock);
            return kGuestGraphHandleBase + index * kGuestAudioHandleStride;
        }
    }
    uint32_t index = 0;
    while (index < graph_object_count && graph_objects[index]) ++index;
    if (index == graph_object_count &&
        graph_object_count < kGuestAudioHandleCapacity) {
        ++graph_object_count;
    }
    if (index >= kGuestAudioHandleCapacity) {
        pthread_mutex_unlock(&audio_object_lock);
        return 0;
    }
    graph_objects[index] = graph;
    pthread_mutex_unlock(&audio_object_lock);
    return kGuestGraphHandleBase + index * kGuestAudioHandleStride;
}

static uint32_t guest_handle_for_unit(AudioUnit unit, AUGraph owner_graph)
{
    if (!unit) return 0;
    pthread_mutex_lock(&audio_object_lock);
    for (uint32_t index = 0; index < unit_object_count; ++index) {
        if (unit_objects[index] == unit) {
            pthread_mutex_unlock(&audio_object_lock);
            return kGuestUnitHandleBase + index * kGuestAudioHandleStride;
        }
    }
    uint32_t index = 0;
    while (index < unit_object_count && unit_objects[index]) ++index;
    if (index == unit_object_count &&
        unit_object_count < kGuestAudioHandleCapacity) {
        ++unit_object_count;
    }
    if (index >= kGuestAudioHandleCapacity) {
        pthread_mutex_unlock(&audio_object_lock);
        return 0;
    }
    unit_objects[index] = unit;
    unit_owner_graphs[index] = owner_graph;
    pthread_mutex_unlock(&audio_object_lock);
    return kGuestUnitHandleBase + index * kGuestAudioHandleStride;
}

static void retire_guest_audio_objects(uint32_t graph_handle, AUGraph graph)
{
    pthread_mutex_lock(&audio_object_lock);
    if (graph_handle >= kGuestGraphHandleBase) {
        uint32_t offset = graph_handle - kGuestGraphHandleBase;
        uint32_t index = offset / kGuestAudioHandleStride;
        if (offset % kGuestAudioHandleStride == 0 &&
            index < graph_object_count && graph_objects[index] == graph) {
            graph_objects[index] = NULL;
        }
    }
    for (uint32_t index = 0; index < unit_object_count; ++index) {
        if (unit_owner_graphs[index] == graph) {
            unit_objects[index] = NULL;
            unit_owner_graphs[index] = NULL;
        }
    }
    pthread_mutex_unlock(&audio_object_lock);
}

static AUGraph graph_for_guest(uint32_t handle)
{
    if (handle < kGuestGraphHandleBase) return NULL;
    uint32_t offset = handle - kGuestGraphHandleBase;
    if (offset % kGuestAudioHandleStride) return NULL;
    uint32_t index = offset / kGuestAudioHandleStride;
    return index < graph_object_count ? graph_objects[index] : NULL;
}

static struct audio_callback_context *new_audio_callback(uint32_t function,
                                                          uint32_t refcon,
                                                          const char *kind,
                                                          AUGraph owner_graph,
                                                          AUNode callback_node,
                                                          UInt32 callback_bus)
{
    if (!function) return NULL;
    pthread_once(&audio_worker_once, start_audio_callback_worker);
    if (!audio_worker_available) return NULL;

    pthread_mutex_lock(&audio_callback_registry_lock);
    for (uint32_t index = 0; index < audio_callback_count; ++index) {
        struct audio_callback_context *context = audio_callbacks[index];
        /* A busy context holds its lock for the whole guest render (several
           milliseconds); skip those without queueing behind them. */
        if (__atomic_load_n(&context->in_use, __ATOMIC_ACQUIRE)) continue;
        pthread_mutex_lock(&context->lock);
        if (!context->in_use) {
            context->callback_kind = kind;
            context->owner_graph = owner_graph;
            context->callback_node = callback_node;
            context->callback_bus = callback_bus;
            __atomic_store_n(&context->in_use, true, __ATOMIC_RELEASE);
            context->callback_attached = true;
            context->muted = false;
            context->guest_function = function;
            context->guest_refcon = refcon;
            context->guest_status = noErr;
            context->host_call_count = 0;
            context->next_sample_time_valid = false;
            context->request_pending = false;
            context->response_ready = false;
            context->host_has_buffer_list = false;
            pthread_mutex_unlock(&context->lock);
            pthread_mutex_unlock(&audio_callback_registry_lock);
            if (trace_audio()) {
                fprintf(stderr,
                        "compat32: audio reused callback[%u] for %s\n",
                        context->callback_index, kind);
            }
            return context;
        }
        pthread_mutex_unlock(&context->lock);
    }

    uint32_t index = audio_callback_count;
    if (index >= kAudioCallbackCapacity) {
        pthread_mutex_unlock(&audio_callback_registry_lock);
        fprintf(stderr,
                "compat32: audio callback capacity exhausted (%u active)\n",
                audio_callback_count);
        return NULL;
    }

    struct audio_callback_context *context = calloc(1, sizeof(*context));
    if (!context) {
        pthread_mutex_unlock(&audio_callback_registry_lock);
        return NULL;
    }
    context->callback_index = index;
    context->callback_kind = kind;
    context->owner_graph = owner_graph;
    context->callback_node = callback_node;
    context->callback_bus = callback_bus;
    __atomic_store_n(&context->in_use, true, __ATOMIC_RELEASE);
    context->callback_attached = true;
    context->muted = false;
    context->guest_function = function;
    context->guest_refcon = refcon;
    context->guest_flags = compat_runtime32_allocate(sizeof(uint32_t), 1);
    context->guest_timestamp = compat_runtime32_allocate(sizeof(AudioTimeStamp), 1);
    context->guest_buffer_list = compat_runtime32_allocate(
        sizeof(uint32_t) + kAudioCallbackBufferCapacity *
                               sizeof(struct guest_audio_buffer),
        1);
    if (!context->guest_flags || !context->guest_timestamp ||
        !context->guest_buffer_list) {
        compat_runtime32_deallocate(context->guest_flags);
        compat_runtime32_deallocate(context->guest_timestamp);
        compat_runtime32_deallocate(context->guest_buffer_list);
        free(context);
        pthread_mutex_unlock(&audio_callback_registry_lock);
        return NULL;
    }
    pthread_mutex_init(&context->lock, NULL);
    pthread_cond_init(&context->request_condition, NULL);
    pthread_cond_init(&context->response_condition, NULL);
    if (trace_audio_latency()) {
        fprintf(stderr, "compat32: audio callback[%u] %s guest fn=0x%08x refcon=0x%08x\n",
                index, kind, function, refcon);
    }
    audio_callbacks[index] = context;
    /* Entries are append-only and never freed, so a reader that loads the
       count with acquire may walk the table without the registry lock. */
    __atomic_store_n(&audio_callback_count, index + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&audio_callback_registry_lock);
    return context;
}

static void release_audio_callback(struct audio_callback_context *context)
{
    if (!context) return;
    pthread_mutex_lock(&context->lock);
    context->in_use = false;
    context->callback_attached = false;
    context->owner_graph = NULL;
    context->callback_node = 0;
    context->callback_bus = 0;
    context->guest_function = 0;
    context->guest_refcon = 0;
    context->request_pending = false;
    context->response_ready = false;
    context->host_has_buffer_list = false;
    pthread_mutex_unlock(&context->lock);
}

static uint32_t release_audio_callbacks_for_graph(AUGraph graph)
{
    uint32_t released = 0;
    pthread_mutex_lock(&audio_callback_registry_lock);
    for (uint32_t index = 0; index < audio_callback_count; ++index) {
        struct audio_callback_context *context = audio_callbacks[index];
        pthread_mutex_lock(&context->lock);
        if (context->in_use && context->owner_graph == graph) {
            context->in_use = false;
            context->callback_attached = false;
            context->owner_graph = NULL;
            context->callback_node = 0;
            context->callback_bus = 0;
            context->guest_function = 0;
            context->guest_refcon = 0;
            context->request_pending = false;
            context->response_ready = false;
            context->host_has_buffer_list = false;
            ++released;
        }
        pthread_mutex_unlock(&context->lock);
    }
    pthread_mutex_unlock(&audio_callback_registry_lock);
    return released;
}

/*
 * Diagnostic view of the engine's stream ring (NuSound streamer).  The render
 * callback consumes fixed chunks from a ring and posts "refill chunk k" to a
 * worker thread that polls every 5 ms; if the worker falls a whole ring
 * behind, the callback plays stale chunk contents and the audio timeline
 * slips.  Offsets are the engine's (shared by both titles); the callback
 * address comes from the profile.
 */
static uint64_t stream_probe_pending_max;
static uint64_t stream_probe_behind_renders;
static uint32_t stream_probe_last_pending;

static inline uint32_t guest_u32(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static void probe_stream_ring(struct audio_callback_context *context,
                              UInt32 frame_count, uint64_t round_trip_ns)
{
    uint32_t refcon = context->guest_refcon;
    if (refcon < 0x1000 || (guest_u32(refcon + 8) & 0xF) != 1) return;
    uint32_t stream = guest_u32(refcon + 4);
    if (stream < 0x1000) return;
    uint32_t sound = guest_u32(stream + 12);
    if (sound < 0x1000) return;
    uint32_t desc = guest_u32(sound + 108);
    if (!desc) desc = sound + 4;
    uint32_t chunk_count = guest_u32(desc + 8);
    uint32_t chunk_index = guest_u32(stream + 364);
    uint32_t worker = guest_u32(stream + 16);
    if (worker < 0x1000 || chunk_count == 0 || chunk_count > 4096) return;
    uint32_t produced = guest_u32(worker + 1812);
    uint32_t consumed = guest_u32(worker + 1808);
    uint32_t pending = produced - consumed;
    if (pending > stream_probe_pending_max) stream_probe_pending_max = pending;
    if (pending >= chunk_count) ++stream_probe_behind_renders;
    if (trace_audio_latency()) {
        /* Ring 1 carries load/open/close jobs for every sound (one-shots
           included); the worker serves it only when ring 2 is empty. */
        static uint32_t last_load_produced, last_load_consumed;
        uint32_t load_produced = guest_u32(worker + 908);
        uint32_t load_consumed = guest_u32(worker + 904);
        if (load_produced != last_load_produced ||
            load_consumed != last_load_consumed) {
            fprintf(stderr,
                    "compat32: audio loader t=%.3f jobs produced=%u consumed=%u "
                    "pending=%u (stream refills pending=%u)\n",
                    monotonic_ns() / 1e9, load_produced, load_consumed,
                    load_produced - load_consumed, pending);
            last_load_produced = load_produced;
            last_load_consumed = load_consumed;
        }
        static uint32_t traced_streams;
        if (context->host_call_count == 0 && traced_streams++ < 8) {
            uint64_t total = guest_u32(desc) | ((uint64_t)guest_u32(desc + 4) << 32);
            fprintf(stderr,
                    "compat32: audio stream cb[%u] stream=0x%08x chunks=%u "
                    "bytes=%llu chunk=%u frames/render=%u\n",
                    context->callback_index, stream, chunk_count,
                    (unsigned long long)total, chunk_index, frame_count);
        }
        if (pending != stream_probe_last_pending) {
            fprintf(stderr,
                    "compat32: audio stream cb[%u] t=%.3f refill backlog %u of %u "
                    "(produced=%u consumed=%u chunk=%u len0=%u len1=%u "
                    "done=%u pause=%u src=0x%08x rt=%.2f ms)\n",
                    context->callback_index, monotonic_ns() / 1e9, pending,
                    chunk_count, produced, consumed, chunk_index,
                    guest_u32(desc + 12), guest_u32(desc + 20),
                    *(const uint8_t *)(uintptr_t)(stream + 356),
                    *(const uint8_t *)(uintptr_t)(stream + 20),
                    guest_u32(stream + 236), round_trip_ns / 1e6);
        }
    }
    stream_probe_last_pending = pending;
}

static OSStatus host_audio_callback(void *refcon,
                                    AudioUnitRenderActionFlags *flags,
                                    const AudioTimeStamp *timestamp,
                                    UInt32 bus,
                                    UInt32 frame_count,
                                    AudioBufferList *host_list)
{
    struct audio_callback_context *context = refcon;
    if (!context) return kAudio_ParamError;
    pthread_mutex_lock(&context->lock);
    bool held = false;
    if (context->in_use && context->guest_function &&
        !__atomic_load_n(&context->muted, __ATOMIC_ACQUIRE)) {
        /* The guest is never entered while held, so its stream positions and
           one-shot progress stand still with the frozen picture. */
        held = audio_hold_active();
        if (held && strcmp(context->callback_kind, "render-notify") != 0) {
            __atomic_fetch_add(&hold_renders, 1, __ATOMIC_RELAXED);
            if (timestamp && (timestamp->mFlags & kAudioTimeStampSampleTimeValid)) {
                /* Silence still occupies host sample time; don't report the
                   held cycles as HAL discontinuities. */
                context->next_sample_time = timestamp->mSampleTime + frame_count;
                context->next_sample_time_valid = true;
            }
        }
    }
    if (!context->in_use || !context->guest_function || held ||
        __atomic_load_n(&context->muted, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&context->lock);
        if (host_list) {
            for (UInt32 index = 0; index < host_list->mNumberBuffers; ++index) {
                AudioBuffer *host_buffer = &host_list->mBuffers[index];
                if (host_buffer->mData && host_buffer->mDataByteSize) {
                    memset(host_buffer->mData, 0, host_buffer->mDataByteSize);
                }
            }
        }
        return noErr;
    }

    uint32_t guest_flags_value = flags ? *flags : 0;
    memcpy((void *)(uintptr_t)context->guest_flags, &guest_flags_value,
           sizeof(guest_flags_value));
    if (timestamp) {
        memcpy((void *)(uintptr_t)context->guest_timestamp, timestamp,
               sizeof(*timestamp));
    } else {
        memset((void *)(uintptr_t)context->guest_timestamp, 0,
               sizeof(AudioTimeStamp));
    }

    uint32_t buffer_count = host_list ? host_list->mNumberBuffers : 0;
    if (buffer_count > kAudioCallbackBufferCapacity) {
        buffer_count = kAudioCallbackBufferCapacity;
    }
    uint32_t *guest_count = (void *)(uintptr_t)context->guest_buffer_list;
    *guest_count = buffer_count;
    struct guest_audio_buffer *guest_buffers = (void *)(guest_count + 1);

    for (uint32_t index = 0; index < buffer_count; ++index) {
        const AudioBuffer *host_buffer = &host_list->mBuffers[index];
        size_t byte_size = host_buffer->mDataByteSize;
        if (byte_size > context->guest_data_capacity[index]) {
            uint32_t storage = compat_runtime32_reallocate(
                context->guest_data[index], byte_size);
            if (!storage) {
                pthread_mutex_unlock(&context->lock);
                return kAudio_MemFullError;
            }
            context->guest_data[index] = storage;
            context->guest_data_capacity[index] = byte_size;
        }
        guest_buffers[index].channels = host_buffer->mNumberChannels;
        guest_buffers[index].byte_size = (uint32_t)byte_size;
        guest_buffers[index].data = context->guest_data[index];
        if (byte_size && host_buffer->mData) {
            memcpy((void *)(uintptr_t)guest_buffers[index].data,
                   host_buffer->mData, byte_size);
        }
    }

    /*
     * Apple invokes audio callbacks on a special real-time worker.  Rosetta's
     * compatibility-mode far jump is not safe from that thread, so hand the
     * synchronous request to a normal pthread dedicated to this guest
     * callback.  The audio thread still observes the callback synchronously.
     */
    context->bus = bus;
    context->frame_count = frame_count;
    context->host_has_buffer_list = host_list != NULL;
    context->response_ready = false;
    struct audio_callback_job job = {
        .context = context,
        .next = NULL,
    };
    uint64_t round_trip_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&audio_job_lock);
    if (audio_job_tail) audio_job_tail->next = &job;
    else audio_job_head = &job;
    audio_job_tail = &job;
    pthread_cond_signal(&audio_job_condition);
    pthread_mutex_unlock(&audio_job_lock);
    while (!context->response_ready) {
        pthread_cond_wait(&context->response_condition, &context->lock);
    }
    int32_t status = context->guest_status;
    uint64_t round_trip_ns =
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - round_trip_start;
    if (round_trip_ns > __atomic_load_n(&render_callback_max_ns, __ATOMIC_RELAXED)) {
        __atomic_store_n(&render_callback_max_ns, round_trip_ns, __ATOMIC_RELAXED);
    }

    bool is_input_callback = strcmp(context->callback_kind, "render-notify") != 0;
    if (is_input_callback) {
        uint64_t requested = __atomic_exchange_n(&context->start_requested_ns, 0,
                                                 __ATOMIC_ACQ_REL);
        if (requested && trace_audio_latency()) {
            fprintf(stderr,
                    "compat32: audio callback[%u] first render %.1f ms after "
                    "AUGraphStart (fn=0x%08x)\n",
                    context->callback_index,
                    (monotonic_ns() - requested) / 1e6, context->guest_function);
        }
    }
    if (is_input_callback && context->guest_function &&
        context->guest_function == lp32_profile()->stream_input_callback) {
        probe_stream_ring(context, frame_count, round_trip_ns);
    }
    if (is_input_callback && host_list) {
        uint32_t returned_flags = 0;
        memcpy(&returned_flags, (const void *)(uintptr_t)context->guest_flags,
               sizeof(returned_flags));
        if (returned_flags & kAudioUnitRenderAction_OutputIsSilence) {
            __atomic_fetch_add(&render_silent_count, 1, __ATOMIC_RELAXED);
        }
        if (timestamp && (timestamp->mFlags & kAudioTimeStampSampleTimeValid)) {
            if (context->next_sample_time_valid) {
                double gap = timestamp->mSampleTime - context->next_sample_time;
                if (gap > 1.0) {
                    __atomic_fetch_add(&render_gap_count, 1, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&render_gap_frames, (uint64_t)gap,
                                       __ATOMIC_RELAXED);
                    if (trace_audio_latency()) {
                        fprintf(stderr,
                                "compat32: audio callback[%u] sample-time gap "
                                "%.0f frames (round trip %.2f ms)\n",
                                context->callback_index, gap,
                                round_trip_ns / 1e6);
                    }
                }
            }
            context->next_sample_time = timestamp->mSampleTime + frame_count;
            context->next_sample_time_valid = true;
        }
    }

    uint64_t call_count = ++context->host_call_count;
    if (trace_audio() && host_list &&
        (call_count <= 8 || (call_count % 1000) == 0)) {
        size_t total_bytes = 0;
        size_t nonzero_bytes = 0;
        for (uint32_t index = 0; index < buffer_count; ++index) {
            size_t byte_size = guest_buffers[index].byte_size;
            const unsigned char *bytes =
                (const void *)(uintptr_t)guest_buffers[index].data;
            total_bytes += byte_size;
            for (size_t offset = 0; bytes && offset < byte_size; ++offset) {
                if (bytes[offset]) ++nonzero_bytes;
            }
        }
        fprintf(stderr,
                "compat32: audio callback[%u] %s call=%llu fn=0x%08x "
                "bus=%u frames=%u buffers=%u bytes=%zu nonzero=%zu status=%d\n",
                context->callback_index, context->callback_kind,
                (unsigned long long)call_count, context->guest_function,
                bus, frame_count, buffer_count, total_bytes, nonzero_bytes,
                status);
    }

    if (status != kAudio_ParamError) {
        if (flags) {
            memcpy(&guest_flags_value,
                   (const void *)(uintptr_t)context->guest_flags,
                   sizeof(guest_flags_value));
            *flags = guest_flags_value;
        }
        for (uint32_t index = 0; index < buffer_count; ++index) {
            AudioBuffer *host_buffer = &host_list->mBuffers[index];
            size_t copy_size = guest_buffers[index].byte_size;
            if (copy_size > host_buffer->mDataByteSize) {
                copy_size = host_buffer->mDataByteSize;
            }
            host_buffer->mDataByteSize = (UInt32)copy_size;
            if (copy_size && host_buffer->mData && guest_buffers[index].data) {
                memcpy(host_buffer->mData,
                       (const void *)(uintptr_t)guest_buffers[index].data,
                       copy_size);
            }
        }
    }

    /* Keep the complete guest audio path running during unattended tests,
       while ensuring that it cannot interrupt the user's system audio. */
    if (mute_audio_output() && host_list) {
        for (uint32_t index = 0; index < buffer_count; ++index) {
            AudioBuffer *host_buffer = &host_list->mBuffers[index];
            if (host_buffer->mData && host_buffer->mDataByteSize) {
                memset(host_buffer->mData, 0, host_buffer->mDataByteSize);
            }
        }
    }

    pthread_mutex_unlock(&context->lock);
    return status;
}

static uint32_t detach_audio_callbacks_for_graph(AUGraph graph,
                                                 bool *detach_failed)
{
    uint32_t detached = 0;
    if (detach_failed) *detach_failed = false;
    pthread_mutex_lock(&audio_callback_registry_lock);
    for (uint32_t index = 0; index < audio_callback_count; ++index) {
        struct audio_callback_context *context = audio_callbacks[index];
        pthread_mutex_lock(&context->lock);
        if (context->in_use && context->callback_attached &&
            context->owner_graph == graph) {
            OSStatus status;
            if (strcmp(context->callback_kind, "render-notify") == 0) {
                status = AUGraphRemoveRenderNotify(
                    graph, host_audio_callback, context);
            } else {
                status = AUGraphDisconnectNodeInput(
                    graph, context->callback_node, context->callback_bus);
            }
            if (trace_audio()) {
                trace_audio_status(
                    strcmp(context->callback_kind, "render-notify") == 0 ?
                    "AUGraphRemoveRenderNotify" :
                    "AUGraphDisconnectNodeInput",
                    status);
            }
            if (status == noErr) {
                context->callback_attached = false;
                ++detached;
            } else if (detach_failed) {
                *detach_failed = true;
            }
        }
        pthread_mutex_unlock(&context->lock);
    }
    pthread_mutex_unlock(&audio_callback_registry_lock);
    return detached;
}

static bool quarantine_audio_graphs(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("LP32_QUARANTINE_AUDIO_GRAPHS") != NULL;
    }
    return enabled != 0;
}

/*
 * Deferred graph teardown.
 *
 * AUGraphStop blocks until the device IO cycle in flight completes (7-12 ms
 * with the default HAL buffer), and the game stops and disposes a graph on
 * the render thread for nearly every sound effect.  Each one was a dropped
 * frame.  Stop/Uninitialize/Close/Dispose therefore return immediately: the
 * graph's guest callbacks are muted at that instant (the guest is never
 * entered again for a graph it believes is stopped), and the host work runs
 * in FIFO order on a dedicated thread.  Any later synchronous operation on a
 * graph with pending work drains that work first, so Stop followed by Start
 * (music pause/resume) or Uninitialize followed by Initialize keeps its
 * ordering.  LP32_SYNC_AUDIO_TEARDOWN restores the synchronous path.
 */
enum deferred_graph_kind {
    /* Lifecycle steps that unit-level property/parameter writes need not
       wait for: they are legal on an open unit in any run state. */
    kDeferredGraphInitialize,
    kDeferredGraphAddRenderNotify,
    kDeferredGraphStart,
    kDeferredGraphStop,
    /* A mixer volume write issued while the graph's Initialize is still
       queued: the mixer rejects parameters until it is initialized, so the
       write rides the queue behind Initialize instead of failing. */
    kDeferredUnitSetParameter,
    /* Builds one pre-opened graph for the pool (graph is NULL). */
    kDeferredPoolRefill,
    /* Teardown steps every later operation on the graph must wait for. */
    kDeferredGraphUninitialize,
    kDeferredGraphClose,
    kDeferredGraphDispose,
};

struct deferred_graph_op {
    AUGraph graph;
    enum deferred_graph_kind kind;
    struct audio_callback_context *context;
    AudioUnit unit;
    AudioUnitParameterID parameter;
    AudioUnitScope scope;
    AudioUnitElement element;
    AudioUnitParameterValue value;
    UInt32 offset;
    uint64_t enqueued_ns;
    struct deferred_graph_op *next;
};

/* Queue-latency statistics for the frame-stats line: how long AUGraphStart
   requests wait behind other work before the graph actually runs, and how
   long the slowest op took.  Reset on each read. */
static uint64_t deferred_start_count;
static uint64_t deferred_start_wait_total_ns;
static uint64_t deferred_start_wait_max_ns;
static uint64_t deferred_op_run_max_ns;
static uint32_t deferred_queue_depth_max;
static uint32_t deferred_queue_depth;

static pthread_mutex_t deferred_graph_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t deferred_graph_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t deferred_graph_done = PTHREAD_COND_INITIALIZER;
static struct deferred_graph_op *deferred_graph_head;
static struct deferred_graph_op *deferred_graph_tail;
static AUGraph deferred_graph_in_progress;
static enum deferred_graph_kind deferred_graph_in_progress_kind;
static int deferred_graph_outstanding;
static pthread_once_t deferred_graph_worker_once = PTHREAD_ONCE_INIT;
static bool deferred_graph_worker_available;

static bool synchronous_audio_teardown(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("LP32_SYNC_AUDIO_TEARDOWN") != NULL ||
                  quarantine_audio_graphs();
    }
    return enabled != 0;
}

static void set_graph_callbacks_muted(AUGraph graph, bool muted)
{
    /* Runs on the render thread for every sound start/stop, so it takes no
       locks: the registry lock is held by the worker across CoreAudio
       detach calls, and each context lock across a guest render.  The
       table is append-only, and owner_graph/muted are accessed atomically.
       A render already in flight simply completes; the graph's host
       teardown is queued behind AUGraphStop, which waits for it. */
    uint32_t count = __atomic_load_n(&audio_callback_count, __ATOMIC_ACQUIRE);
    for (uint32_t index = 0; index < count; ++index) {
        struct audio_callback_context *context = audio_callbacks[index];
        if (__atomic_load_n(&context->in_use, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&context->owner_graph, __ATOMIC_ACQUIRE) == graph) {
            __atomic_store_n(&context->muted, muted, __ATOMIC_RELEASE);
            if (!muted) {
                __atomic_store_n(&context->start_requested_ns, monotonic_ns(),
                                 __ATOMIC_RELEASE);
            }
        }
    }
}

static const char *deferred_graph_kind_name(enum deferred_graph_kind kind)
{
    switch (kind) {
    case kDeferredGraphInitialize: return "AUGraphInitialize";
    case kDeferredGraphAddRenderNotify: return "AUGraphAddRenderNotify";
    case kDeferredGraphStart: return "AUGraphStart";
    case kDeferredGraphStop: return "AUGraphStop";
    case kDeferredUnitSetParameter: return "AudioUnitSetParameter";
    case kDeferredPoolRefill: return "pool refill";
    case kDeferredGraphUninitialize: return "AUGraphUninitialize";
    case kDeferredGraphClose: return "AUGraphClose";
    case kDeferredGraphDispose: return "DisposeAUGraph";
    }
    return "?";
}

static bool deferred_kind_is_teardown(enum deferred_graph_kind kind)
{
    return kind >= kDeferredGraphUninitialize;
}

static void perform_graph_pool_refill(void);

/* Teardown follows the same sequence the synchronous path used: stop, detach
   the bridge callbacks, then let CoreAudio tear the graph down natively. */
static void perform_graph_op(const struct deferred_graph_op *op)
{
    AUGraph graph = op->graph;
    enum deferred_graph_kind kind = op->kind;
    OSStatus status = noErr;
    switch (kind) {
    case kDeferredGraphInitialize:
        status = AUGraphInitialize(graph);
        if (trace_audio()) trace_audio_status("AUGraphInitialize(deferred)", status);
        return;
    case kDeferredGraphAddRenderNotify:
        status = AUGraphAddRenderNotify(graph, host_audio_callback, op->context);
        if (status != noErr) release_audio_callback(op->context);
        if (trace_audio()) trace_audio_status("AUGraphAddRenderNotify(deferred)", status);
        return;
    case kDeferredGraphStart:
        status = AUGraphStart(graph);
        if (trace_audio()) trace_audio_status("AUGraphStart(deferred)", status);
        return;
    case kDeferredGraphStop:
        status = AUGraphStop(graph);
        if (trace_audio()) trace_audio_status("AUGraphStop(deferred)", status);
        return;
    case kDeferredUnitSetParameter:
        status = AudioUnitSetParameter(op->unit, op->parameter, op->scope,
                                       op->element, op->value, op->offset);
        if (trace_audio() && status != noErr) {
            trace_audio_status("AudioUnitSetParameter(deferred)", status);
        }
        return;
    case kDeferredPoolRefill:
        perform_graph_pool_refill();
        return;
    default:
        break;
    }
    (void)AUGraphStop(graph);
    bool detach_failed = false;
    uint32_t detached = detach_audio_callbacks_for_graph(graph, &detach_failed);
    if (detach_failed) {
        if (kind == kDeferredGraphDispose) release_audio_callbacks_for_graph(graph);
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio quarantined graph %p via %s after callback "
                    "detach failure\n", (void *)graph,
                    deferred_graph_kind_name(kind));
        }
        return;
    }
    if (kind == kDeferredGraphUninitialize) {
        status = AUGraphUninitialize(graph);
    } else if (kind == kDeferredGraphClose) {
        status = AUGraphClose(graph);
    } else {
        /* Release before disposing: once CoreAudio frees the graph, a new
           graph may be allocated at the same address and register callbacks
           that must not be swept up here. */
        release_audio_callbacks_for_graph(graph);
        status = DisposeAUGraph(graph);
    }
    if (trace_audio()) {
        fprintf(stderr,
                "compat32: audio native teardown graph=%p via %s "
                "detachedCallbacks=%u status=%d\n", (void *)graph,
                deferred_graph_kind_name(kind), detached, (int)status);
    }
}

static void *deferred_graph_worker(void *opaque)
{
    (void)opaque;
    pthread_setname_np("lp32 audio teardown");
    for (;;) {
        pthread_mutex_lock(&deferred_graph_lock);
        while (!deferred_graph_head) {
            pthread_cond_wait(&deferred_graph_wake, &deferred_graph_lock);
        }
        struct deferred_graph_op *op = deferred_graph_head;
        deferred_graph_head = op->next;
        if (!deferred_graph_head) deferred_graph_tail = NULL;
        deferred_graph_in_progress = op->graph;
        deferred_graph_in_progress_kind = op->kind;
        --deferred_queue_depth;
        pthread_mutex_unlock(&deferred_graph_lock);

        uint64_t started_ns = monotonic_ns();
        perform_graph_op(op);
        uint64_t finished_ns = monotonic_ns();

        pthread_mutex_lock(&deferred_graph_lock);
        uint64_t wait_ns = started_ns - op->enqueued_ns;
        uint64_t run_ns = finished_ns - started_ns;
        if (op->kind == kDeferredGraphStart) {
            ++deferred_start_count;
            deferred_start_wait_total_ns += wait_ns;
            if (wait_ns > deferred_start_wait_max_ns) {
                deferred_start_wait_max_ns = wait_ns;
            }
        }
        if (run_ns > deferred_op_run_max_ns) deferred_op_run_max_ns = run_ns;
        if (trace_audio_latency() && op->kind == kDeferredGraphStart &&
            wait_ns > 15000000ull) {
            fprintf(stderr,
                    "compat32: audio AUGraphStart graph=%p waited %.1f ms in "
                    "queue (op ran %.1f ms)\n", (void *)op->graph,
                    wait_ns / 1e6, run_ns / 1e6);
        }
        deferred_graph_in_progress = NULL;
        __atomic_fetch_sub(&deferred_graph_outstanding, 1, __ATOMIC_SEQ_CST);
        pthread_cond_broadcast(&deferred_graph_done);
        pthread_mutex_unlock(&deferred_graph_lock);
        free(op);
    }
}

static void start_deferred_graph_worker(void)
{
    pthread_t worker;
    deferred_graph_worker_available =
        pthread_create(&worker, NULL, deferred_graph_worker, NULL) == 0;
    if (deferred_graph_worker_available) pthread_detach(worker);
}

/* teardown_only restricts the check to uninitialize/close/dispose work, the
   steps after which the graph's units cease to exist. */
static bool graph_has_deferred_work_locked(AUGraph graph, bool teardown_only)
{
    if (deferred_graph_in_progress == graph &&
        (!teardown_only ||
         deferred_kind_is_teardown(deferred_graph_in_progress_kind))) {
        return true;
    }
    for (struct deferred_graph_op *op = deferred_graph_head; op; op = op->next) {
        if (op->graph == graph &&
            (!teardown_only || deferred_kind_is_teardown(op->kind))) {
            return true;
        }
    }
    return false;
}

static void drain_deferred_graph_work(AUGraph graph, bool teardown_only)
{
    if (!graph || !__atomic_load_n(&deferred_graph_outstanding, __ATOMIC_SEQ_CST)) return;
    pthread_mutex_lock(&deferred_graph_lock);
    while (graph_has_deferred_work_locked(graph, teardown_only)) {
        pthread_cond_wait(&deferred_graph_done, &deferred_graph_lock);
    }
    pthread_mutex_unlock(&deferred_graph_lock);
}

static struct deferred_graph_op *new_deferred_graph_op(
    AUGraph graph, enum deferred_graph_kind kind)
{
    pthread_once(&deferred_graph_worker_once, start_deferred_graph_worker);
    struct deferred_graph_op *op = deferred_graph_worker_available ?
        calloc(1, sizeof(*op)) : NULL;
    if (op) {
        op->graph = graph;
        op->kind = kind;
    }
    return op;
}

static void append_deferred_graph_op_locked(struct deferred_graph_op *op)
{
    op->enqueued_ns = monotonic_ns();
    if (deferred_graph_tail) deferred_graph_tail->next = op;
    else deferred_graph_head = op;
    deferred_graph_tail = op;
    if (++deferred_queue_depth > deferred_queue_depth_max) {
        deferred_queue_depth_max = deferred_queue_depth;
    }
    __atomic_fetch_add(&deferred_graph_outstanding, 1, __ATOMIC_SEQ_CST);
    pthread_cond_signal(&deferred_graph_wake);
}

void audio_bridge32_worker_statistics(struct audio_bridge32_worker_stats *stats)
{
    pthread_mutex_lock(&deferred_graph_lock);
    stats->starts = deferred_start_count;
    stats->start_wait_avg_ms = deferred_start_count ?
        deferred_start_wait_total_ns / 1e6 / (double)deferred_start_count : 0.0;
    stats->start_wait_max_ms = deferred_start_wait_max_ns / 1e6;
    stats->op_run_max_ms = deferred_op_run_max_ns / 1e6;
    stats->queue_depth_max = deferred_queue_depth_max;
    deferred_start_count = 0;
    deferred_start_wait_total_ns = 0;
    deferred_start_wait_max_ns = 0;
    deferred_op_run_max_ns = 0;
    deferred_queue_depth_max = deferred_queue_depth;
    pthread_mutex_unlock(&deferred_graph_lock);
    stats->silent_renders = __atomic_exchange_n(&render_silent_count, 0, __ATOMIC_RELAXED);
    stats->timestamp_gaps = __atomic_exchange_n(&render_gap_count, 0, __ATOMIC_RELAXED);
    stats->timestamp_gap_frames = __atomic_exchange_n(&render_gap_frames, 0, __ATOMIC_RELAXED);
    stats->callback_max_ms =
        __atomic_exchange_n(&render_callback_max_ns, 0, __ATOMIC_RELAXED) / 1e6;
    stats->stream_pending_max =
        __atomic_exchange_n(&stream_probe_pending_max, 0, __ATOMIC_RELAXED);
    stats->stream_behind_renders =
        __atomic_exchange_n(&stream_probe_behind_renders, 0, __ATOMIC_RELAXED);
    stats->hold_episodes = __atomic_exchange_n(&hold_episodes, 0, __ATOMIC_RELAXED);
    stats->hold_ms = __atomic_exchange_n(&hold_total_ns, 0, __ATOMIC_RELAXED) / 1e6;
    stats->hold_renders = __atomic_exchange_n(&hold_renders, 0, __ATOMIC_RELAXED);
}

static void enqueue_graph_teardown(AUGraph graph, enum deferred_graph_kind kind,
                                   struct audio_callback_context *context)
{
    struct deferred_graph_op *op = new_deferred_graph_op(graph, kind);
    if (!op) {
        struct deferred_graph_op inline_op = {
            .graph = graph, .kind = kind, .context = context,
        };
        perform_graph_op(&inline_op);
        return;
    }
    op->context = context;
    pthread_mutex_lock(&deferred_graph_lock);
    append_deferred_graph_op_locked(op);
    pthread_mutex_unlock(&deferred_graph_lock);
}

/*
 * Pre-opened graph pool.
 *
 * Every sound effect builds the same graph (default output <- mixer <-
 * converter), and AUGraphOpen, which instantiates those units, was the last
 * synchronous CoreAudio cost left on the render thread at 1-3 ms per sound.
 * The topology of each guest graph is recorded as it is built.  The first
 * opened graph becomes the template; the worker keeps a few identical graphs
 * built and opened in advance, and a guest graph whose topology matches the
 * template is swapped for a pooled one at AUGraphOpen.  The swap is only
 * transparent if CoreAudio numbered the pooled graph's nodes the same way,
 * which is checked rather than assumed.  If the game switches to a different
 * topology for a run of graphs the template follows it.
 */
enum {
    kGraphTopologyNodeCapacity = 4,
    kGraphTopologyConnectionCapacity = 4,
    /* Sound bursts (a stud shower, a combat exchange) start several graphs
       within a few frames, faster than the worker refills; four covers the
       bursts seen in the hub stress with 3 pool misses per 1,000 opens. */
    kGraphPoolTarget = 4,
    kGraphTemplateRelearnMismatches = 3,
};

struct graph_connection {
    AUNode source;
    UInt32 source_bus;
    AUNode destination;
    UInt32 destination_bus;
};

struct graph_topology {
    uint32_t node_count;
    uint32_t connection_count;
    /* Something happened before Open that a pooled graph would not carry. */
    bool unpoolable;
    AudioComponentDescription nodes[kGraphTopologyNodeCapacity];
    AUNode node_ids[kGraphTopologyNodeCapacity];
    struct graph_connection connections[kGraphTopologyConnectionCapacity];
};

static struct graph_topology graph_topologies[kGuestAudioHandleCapacity];
static pthread_mutex_t graph_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct graph_topology graph_template;
static bool graph_template_valid;
static uint32_t graph_template_mismatches;
static uint64_t graph_template_matches;
static AUGraph graph_pool[kGraphPoolTarget];
static uint32_t graph_pool_count;
static uint32_t graph_pool_refills_pending;
static uint64_t graph_pool_hits, graph_pool_misses;

static bool graph_pool_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("LP32_NO_AUDIO_GRAPH_POOL") == NULL &&
                  !synchronous_audio_teardown();
    }
    return enabled != 0;
}

static struct graph_topology *topology_for_guest(uint32_t handle)
{
    if (handle < kGuestGraphHandleBase) return NULL;
    uint32_t offset = handle - kGuestGraphHandleBase;
    if (offset % kGuestAudioHandleStride) return NULL;
    uint32_t index = offset / kGuestAudioHandleStride;
    return index < kGuestAudioHandleCapacity ? &graph_topologies[index] : NULL;
}

static void reset_graph_topology(uint32_t handle)
{
    struct graph_topology *topology = topology_for_guest(handle);
    if (topology) memset(topology, 0, sizeof(*topology));
}

static void mark_graph_unpoolable(uint32_t handle)
{
    struct graph_topology *topology = topology_for_guest(handle);
    if (topology) topology->unpoolable = true;
}

static void record_graph_node(uint32_t handle,
                              const AudioComponentDescription *description,
                              AUNode node)
{
    struct graph_topology *topology = topology_for_guest(handle);
    if (!topology || topology->unpoolable) return;
    if (!description || topology->node_count >= kGraphTopologyNodeCapacity) {
        topology->unpoolable = true;
        return;
    }
    topology->nodes[topology->node_count] = *description;
    topology->node_ids[topology->node_count] = node;
    ++topology->node_count;
}

static void record_graph_connection(uint32_t handle, AUNode source,
                                    UInt32 source_bus, AUNode destination,
                                    UInt32 destination_bus)
{
    struct graph_topology *topology = topology_for_guest(handle);
    if (!topology || topology->unpoolable) return;
    if (topology->connection_count >= kGraphTopologyConnectionCapacity) {
        topology->unpoolable = true;
        return;
    }
    struct graph_connection *connection =
        &topology->connections[topology->connection_count++];
    connection->source = source;
    connection->source_bus = source_bus;
    connection->destination = destination;
    connection->destination_bus = destination_bus;
}

static bool graph_topologies_match(const struct graph_topology *a,
                                   const struct graph_topology *b)
{
    if (a->node_count != b->node_count ||
        a->connection_count != b->connection_count) {
        return false;
    }
    return memcmp(a->nodes, b->nodes, a->node_count * sizeof(a->nodes[0])) == 0 &&
           memcmp(a->node_ids, b->node_ids,
                  a->node_count * sizeof(a->node_ids[0])) == 0 &&
           memcmp(a->connections, b->connections,
                  a->connection_count * sizeof(a->connections[0])) == 0;
}

/* Builds and opens one graph shaped like the template.  Returns NULL if
   CoreAudio numbered its nodes differently from the template. */
static AUGraph build_pooled_graph(const struct graph_topology *topology)
{
    AUGraph graph = NULL;
    if (NewAUGraph(&graph) != noErr || !graph) return NULL;
    bool ok = true;
    for (uint32_t index = 0; ok && index < topology->node_count; ++index) {
        AUNode node = 0;
        ok = AUGraphAddNode(graph, &topology->nodes[index], &node) == noErr &&
             node == topology->node_ids[index];
    }
    for (uint32_t index = 0; ok && index < topology->connection_count; ++index) {
        const struct graph_connection *connection = &topology->connections[index];
        ok = AUGraphConnectNodeInput(graph, connection->source,
                                     connection->source_bus,
                                     connection->destination,
                                     connection->destination_bus) == noErr;
    }
    if (ok) ok = AUGraphOpen(graph) == noErr;
    if (!ok) {
        DisposeAUGraph(graph);
        return NULL;
    }
    return graph;
}

static void schedule_graph_pool_refills_locked(void)
{
    while (graph_pool_count + graph_pool_refills_pending < kGraphPoolTarget) {
        struct deferred_graph_op *op =
            new_deferred_graph_op(NULL, kDeferredPoolRefill);
        if (!op) return;
        ++graph_pool_refills_pending;
        pthread_mutex_lock(&deferred_graph_lock);
        append_deferred_graph_op_locked(op);
        pthread_mutex_unlock(&deferred_graph_lock);
    }
}

static void perform_graph_pool_refill(void)
{
    pthread_mutex_lock(&graph_pool_lock);
    bool valid = graph_template_valid;
    struct graph_topology topology = graph_template;
    pthread_mutex_unlock(&graph_pool_lock);

    AUGraph graph = valid ? build_pooled_graph(&topology) : NULL;

    pthread_mutex_lock(&graph_pool_lock);
    if (graph_pool_refills_pending) --graph_pool_refills_pending;
    if (graph && graph_template_valid && graph_pool_count < kGraphPoolTarget &&
        graph_topologies_match(&topology, &graph_template)) {
        graph_pool[graph_pool_count++] = graph;
        graph = NULL;
    }
    pthread_mutex_unlock(&graph_pool_lock);
    if (graph) DisposeAUGraph(graph);
}

static void discard_graph_pool_locked(void)
{
    for (uint32_t index = 0; index < graph_pool_count; ++index) {
        enqueue_graph_teardown(graph_pool[index], kDeferredGraphDispose, NULL);
    }
    graph_pool_count = 0;
}

/* Called at AUGraphOpen with the guest graph's recorded topology.  Returns a
   pooled open graph when one matches, otherwise NULL, in which case the
   caller opens the guest's own graph and this topology may become (or
   reinforce) the template. */
static AUGraph take_pooled_graph(const struct graph_topology *topology)
{
    if (!graph_pool_enabled() || !topology || topology->unpoolable ||
        !topology->node_count) {
        return NULL;
    }
    AUGraph graph = NULL;
    pthread_mutex_lock(&graph_pool_lock);
    if (graph_template_valid && graph_topologies_match(topology, &graph_template)) {
        graph_template_mismatches = 0;
        ++graph_template_matches;
        if (graph_pool_count) {
            graph = graph_pool[--graph_pool_count];
            ++graph_pool_hits;
        } else {
            ++graph_pool_misses;
        }
    } else if (!graph_template_valid || !graph_template_matches ||
               ++graph_template_mismatches >= kGraphTemplateRelearnMismatches) {
        /* A template nothing has matched yet (the game's one-off startup
           probe graph) is replaced at once; an established one only after a
           run of mismatches. */
        graph_template_matches = 0;
        discard_graph_pool_locked();
        graph_template = *topology;
        graph_template_valid = true;
        graph_template_mismatches = 0;
        ++graph_pool_misses;
    } else {
        ++graph_pool_misses;
    }
    schedule_graph_pool_refills_locked();
    pthread_mutex_unlock(&graph_pool_lock);
    return graph;
}

void audio_bridge32_pool_statistics(uint64_t *hits, uint64_t *misses)
{
    pthread_mutex_lock(&graph_pool_lock);
    *hits = graph_pool_hits;
    *misses = graph_pool_misses;
    pthread_mutex_unlock(&graph_pool_lock);
}

static void swap_guest_graph(uint32_t handle, AUGraph old_graph, AUGraph new_graph)
{
    pthread_mutex_lock(&audio_object_lock);
    uint32_t index = (handle - kGuestGraphHandleBase) / kGuestAudioHandleStride;
    if (index < graph_object_count && graph_objects[index] == old_graph) {
        graph_objects[index] = new_graph;
    }
    pthread_mutex_unlock(&audio_object_lock);
}

/* Graph/unit lookups for operations that must observe any pending work on
   the same graph before they run. */
static AUGraph synchronized_graph_for_guest(uint32_t handle)
{
    AUGraph graph = graph_for_guest(handle);
    drain_deferred_graph_work(graph, false);
    return graph;
}

static AudioUnit unit_for_guest(uint32_t handle, AUGraph *owner_graph)
{
    if (handle < kGuestUnitHandleBase) return NULL;
    uint32_t offset = handle - kGuestUnitHandleBase;
    if (offset % kGuestAudioHandleStride) return NULL;
    uint32_t index = offset / kGuestAudioHandleStride;
    if (index >= unit_object_count) return NULL;
    if (owner_graph) *owner_graph = unit_owner_graphs[index];
    return unit_objects[index];
}

static AudioUnit synchronized_unit_for_guest(uint32_t handle)
{
    AUGraph owner_graph = NULL;
    AudioUnit unit = unit_for_guest(handle, &owner_graph);
    if (unit) drain_deferred_graph_work(owner_graph, false);
    return unit;
}

/* Parameter writes are the one unit-level call the game issues between
   AUGraphInitialize and AUGraphStart (mixer volumes), so they must not wait
   for the queue: with the graph's Initialize pending they join the queue
   behind it, otherwise they run directly.  A graph whose teardown is pending
   is going away; the write is dropped as it would be moot. */
static OSStatus set_unit_parameter_for_guest(const uint32_t *arguments,
                                             bool *deferred)
{
    AUGraph owner_graph = NULL;
    AudioUnit unit = unit_for_guest(arguments[0], &owner_graph);
    AudioUnitParameterValue value;
    memcpy(&value, &arguments[4], sizeof(value));
    *deferred = false;
    if (!unit) return kAudio_ParamError;

    if (owner_graph && __atomic_load_n(&deferred_graph_outstanding, __ATOMIC_SEQ_CST)) {
        pthread_mutex_lock(&deferred_graph_lock);
        if (graph_has_deferred_work_locked(owner_graph, true)) {
            pthread_mutex_unlock(&deferred_graph_lock);
            *deferred = true;
            return noErr;
        }
        if (graph_has_deferred_work_locked(owner_graph, false)) {
            struct deferred_graph_op *op =
                new_deferred_graph_op(owner_graph, kDeferredUnitSetParameter);
            if (op) {
                op->unit = unit;
                op->parameter = arguments[1];
                op->scope = arguments[2];
                op->element = arguments[3];
                op->value = value;
                op->offset = arguments[5];
                append_deferred_graph_op_locked(op);
                pthread_mutex_unlock(&deferred_graph_lock);
                *deferred = true;
                return noErr;
            }
            pthread_mutex_unlock(&deferred_graph_lock);
            drain_deferred_graph_work(owner_graph, false);
        } else {
            pthread_mutex_unlock(&deferred_graph_lock);
        }
    }
    return AudioUnitSetParameter(unit, arguments[1], arguments[2],
                                 arguments[3], value, arguments[5]);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#include "peggle_audio.inc"

int audio_bridge32_dispatch(const char *import_name, const uint32_t *arguments,
                            uint64_t *result)
{
    if (peggle_audio_dispatch(import_name, arguments, result)) return 1;
    const size_t import_length = strlen(import_name);
    if (LP32_NAME_IS(import_name, import_length, "_NewAUGraph")) {
        AUGraph graph = NULL;
        OSStatus status = NewAUGraph(&graph);
        uint32_t handle = status == noErr ? guest_handle_for_graph(graph) : 0;
        if (status == noErr && !handle) {
            DisposeAUGraph(graph);
            status = kAudio_MemFullError;
        }
        if (handle) reset_graph_topology(handle);
        if (arguments[0]) *(uint32_t *)(uintptr_t)arguments[0] = handle;
        if (trace_audio()) {
            fprintf(stderr, "compat32: audio NewAUGraph handle=0x%08x\n",
                    handle);
            trace_audio_status("NewAUGraph", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AUGraphAddNode")) {
        AUGraph graph = synchronized_graph_for_guest(arguments[0]);
        const AudioComponentDescription *description =
            (const void *)(uintptr_t)arguments[1];
        OSStatus status = graph ? AUGraphAddNode(
            graph,
            description,
            (AUNode *)(uintptr_t)arguments[2]) : kAudio_ParamError;
        if (status == noErr && arguments[2]) {
            record_graph_node(arguments[0], description,
                              *(const AUNode *)(uintptr_t)arguments[2]);
        } else {
            mark_graph_unpoolable(arguments[0]);
        }
        if (trace_audio()) {
            char type[5], subtype[5], manufacturer[5];
            AUNode node = arguments[2] ?
                *(const AUNode *)(uintptr_t)arguments[2] : 0;
            fprintf(stderr,
                    "compat32: audio AddNode graph=0x%08x node=%d "
                    "component='%s'/'%s'/'%s'\n",
                    arguments[0], (int)node,
                    fourcc_text(description ? description->componentType : 0,
                                type),
                    fourcc_text(description ? description->componentSubType : 0,
                                subtype),
                    fourcc_text(description ? description->componentManufacturer : 0,
                                manufacturer));
            trace_audio_status("AUGraphAddNode", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AUGraphOpen") ||
        LP32_NAME_IS(import_name, import_length, "_AUGraphInitialize") ||
        LP32_NAME_IS(import_name, import_length, "_AUGraphStart") ||
        LP32_NAME_IS(import_name, import_length, "_AUGraphStop")) {
        bool synchronous = synchronous_audio_teardown();
        AUGraph graph = synchronous || LP32_NAME_IS(import_name, import_length, "_AUGraphOpen") ?
            synchronized_graph_for_guest(arguments[0]) : graph_for_guest(arguments[0]);
        OSStatus status = kAudio_ParamError;
        if (graph) {
            if (LP32_NAME_IS(import_name, import_length, "_AUGraphOpen")) {
                AUGraph pooled = take_pooled_graph(topology_for_guest(arguments[0]));
                if (pooled) {
                    /* The guest's never-opened graph is disposed off-thread;
                       its handle now names the pre-opened one. */
                    swap_guest_graph(arguments[0], graph, pooled);
                    enqueue_graph_teardown(graph, kDeferredGraphDispose, NULL);
                    graph = pooled;
                    status = noErr;
                    if (trace_audio()) {
                        fprintf(stderr,
                                "compat32: audio AUGraphOpen graph=0x%08x served "
                                "from pool (hits=%llu misses=%llu)\n",
                                arguments[0],
                                (unsigned long long)graph_pool_hits,
                                (unsigned long long)graph_pool_misses);
                    }
                } else {
                    status = AUGraphOpen(graph);
                }
            } else if (LP32_NAME_IS(import_name, import_length, "_AUGraphInitialize")) {
                if (synchronous) {
                    status = AUGraphInitialize(graph);
                } else {
                    enqueue_graph_teardown(graph, kDeferredGraphInitialize, NULL);
                    status = noErr;
                }
            } else if (LP32_NAME_IS(import_name, import_length, "_AUGraphStart")) {
                set_graph_callbacks_muted(graph, false);
                if (synchronous) {
                    status = AUGraphStart(graph);
                } else {
                    enqueue_graph_teardown(graph, kDeferredGraphStart, NULL);
                    status = noErr;
                }
            } else if (synchronous) {
                status = AUGraphStop(graph);
            } else {
                set_graph_callbacks_muted(graph, true);
                enqueue_graph_teardown(graph, kDeferredGraphStop, NULL);
                status = noErr;
            }
        }
        trace_audio_status(import_name + 1, status);
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AUGraphUninitialize") ||
        LP32_NAME_IS(import_name, import_length, "_AUGraphClose") ||
        LP32_NAME_IS(import_name, import_length, "_DisposeAUGraph")) {
        AUGraph graph = graph_for_guest(arguments[0]);
        if (!graph) {
            *result = (uint32_t)kAudio_ParamError;
            return 1;
        }
        if (!synchronous_audio_teardown()) {
            enum deferred_graph_kind kind = kDeferredGraphDispose;
            if (LP32_NAME_IS(import_name, import_length, "_AUGraphUninitialize")) {
                kind = kDeferredGraphUninitialize;
            } else if (LP32_NAME_IS(import_name, import_length, "_AUGraphClose")) {
                kind = kDeferredGraphClose;
            }
            set_graph_callbacks_muted(graph, true);
            if (kind == kDeferredGraphDispose) {
                /* The guest handle is free for reuse immediately; the worker
                   owns the host graph from here on. */
                retire_guest_audio_objects(arguments[0], graph);
            }
            enqueue_graph_teardown(graph, kind, NULL);
            *result = noErr;
            return 1;
        }

        /*
         * Current CoreAudio can spin forever in its caulk allocator if an
         * Intel AUGraph containing mixed 32/64-bit bridge callbacks is
         * uninitialized directly.  AUGraphStop drains those callbacks, after
         * which explicitly disconnecting the input callback and removing the
         * render notification makes native teardown safe.  This is important
         * here because the game creates a new graph for virtually every sound
         * effect.  LP32_QUARANTINE_AUDIO_GRAPHS retains the older leak-safe
         * fallback for diagnosis on a future CoreAudio implementation.
        */
        (void)AUGraphStop(graph);
        if (!quarantine_audio_graphs()) {
            OSStatus status = noErr;
            bool detach_failed = false;
            uint32_t detached_callbacks = detach_audio_callbacks_for_graph(
                graph, &detach_failed);
            if (detach_failed) {
                if (LP32_NAME_IS(import_name, import_length, "_DisposeAUGraph")) {
                    release_audio_callbacks_for_graph(graph);
                    retire_guest_audio_objects(arguments[0], graph);
                }
                if (trace_audio()) {
                    fprintf(stderr,
                            "compat32: audio quarantined graph=0x%08x via %s "
                            "after callback detach failure\n",
                            arguments[0], import_name + 1);
                }
                *result = noErr;
                return 1;
            }
            if (LP32_NAME_IS(import_name, import_length, "_AUGraphUninitialize")) {
                status = AUGraphUninitialize(graph);
            } else if (LP32_NAME_IS(import_name, import_length, "_AUGraphClose")) {
                status = AUGraphClose(graph);
            } else {
                status = DisposeAUGraph(graph);
                release_audio_callbacks_for_graph(graph);
                retire_guest_audio_objects(arguments[0], graph);
            }
            if (trace_audio()) {
                fprintf(stderr,
                        "compat32: audio native teardown graph=0x%08x via %s "
                        "detachedCallbacks=%u status=%d\n",
                        arguments[0], import_name + 1, detached_callbacks,
                        (int)status);
            }
            *result = noErr;
            return 1;
        }
        uint32_t released_callbacks = 0;
        if (LP32_NAME_IS(import_name, import_length, "_DisposeAUGraph")) {
            released_callbacks = release_audio_callbacks_for_graph(graph);
            retire_guest_audio_objects(arguments[0], graph);
        }
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio quarantined graph=0x%08x via %s "
                    "releasedCallbacks=%u\n",
                    arguments[0], import_name + 1, released_callbacks);
        }
        *result = noErr;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AUGraphNodeInfo")) {
        AUGraph graph = synchronized_graph_for_guest(arguments[0]);
        AudioComponentDescription description;
        AudioUnit unit = NULL;
        AudioComponentDescription *description_pointer =
            arguments[2] ? &description : NULL;
        AudioUnit *unit_pointer = arguments[3] ? &unit : NULL;
        OSStatus status = graph ? AUGraphNodeInfo(
            graph, (AUNode)arguments[1], description_pointer, unit_pointer)
            : kAudio_ParamError;
        if (status == noErr && arguments[2]) {
            memcpy((void *)(uintptr_t)arguments[2], &description,
                   sizeof(description));
        }
        if (arguments[3]) {
            *(uint32_t *)(uintptr_t)arguments[3] =
                status == noErr ? guest_handle_for_unit(unit, graph) : 0;
        }
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio NodeInfo graph=0x%08x node=%u unit=0x%08x\n",
                    arguments[0], arguments[1], arguments[3] ?
                    *(const uint32_t *)(uintptr_t)arguments[3] : 0);
            trace_audio_status("AUGraphNodeInfo", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AUGraphConnectNodeInput")) {
        AUGraph graph = synchronized_graph_for_guest(arguments[0]);
        OSStatus status = graph ? AUGraphConnectNodeInput(
            graph, (AUNode)arguments[1], arguments[2],
            (AUNode)arguments[3], arguments[4]) : kAudio_ParamError;
        if (status == noErr) {
            record_graph_connection(arguments[0], (AUNode)arguments[1],
                                    arguments[2], (AUNode)arguments[3],
                                    arguments[4]);
        } else {
            mark_graph_unpoolable(arguments[0]);
        }
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio Connect graph=0x%08x %u:%u -> %u:%u\n",
                    arguments[0], arguments[1], arguments[2], arguments[3],
                    arguments[4]);
            trace_audio_status("AUGraphConnectNodeInput", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AUGraphSetNodeInputCallback")) {
        AUGraph graph = synchronized_graph_for_guest(arguments[0]);
        mark_graph_unpoolable(arguments[0]);
        const uint32_t *guest_callback =
            (const void *)(uintptr_t)arguments[3];
        struct audio_callback_context *context =
            guest_callback ? new_audio_callback(guest_callback[0],
                                                guest_callback[1],
                                                "node-input", graph,
                                                (AUNode)arguments[1],
                                                arguments[2]) : NULL;
        AURenderCallbackStruct callback = {
            .inputProc = context ? host_audio_callback : NULL,
            .inputProcRefCon = context,
        };
        OSStatus status = graph && context ? AUGraphSetNodeInputCallback(
            graph, (AUNode)arguments[1], arguments[2], &callback)
            : kAudio_ParamError;
        if (status != noErr) release_audio_callback(context);
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio SetInputCallback graph=0x%08x node=%u "
                    "bus=%u callback=%u fn=0x%08x refcon=0x%08x\n",
                    arguments[0], arguments[1], arguments[2],
                    context ? context->callback_index : UINT32_MAX,
                    guest_callback ? guest_callback[0] : 0,
                    guest_callback ? guest_callback[1] : 0);
            trace_audio_status("AUGraphSetNodeInputCallback", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AUGraphAddRenderNotify")) {
        bool synchronous = synchronous_audio_teardown();
        AUGraph graph = synchronous ? synchronized_graph_for_guest(arguments[0]) :
            graph_for_guest(arguments[0]);
        mark_graph_unpoolable(arguments[0]);
        struct audio_callback_context *context = new_audio_callback(
            arguments[1], arguments[2], "render-notify", graph, 0, 0);
        OSStatus status = kAudio_ParamError;
        if (graph && context) {
            if (synchronous) {
                status = AUGraphAddRenderNotify(graph, host_audio_callback, context);
            } else {
                /* The worker releases the context if the host call fails. */
                enqueue_graph_teardown(graph, kDeferredGraphAddRenderNotify, context);
                status = noErr;
            }
        }
        if (status != noErr) release_audio_callback(context);
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio AddRenderNotify graph=0x%08x callback=%u "
                    "fn=0x%08x refcon=0x%08x\n",
                    arguments[0], context ? context->callback_index : UINT32_MAX,
                    arguments[1], arguments[2]);
            trace_audio_status("AUGraphAddRenderNotify", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AudioUnitGetProperty")) {
        AudioUnit unit = synchronized_unit_for_guest(arguments[0]);
        OSStatus status = unit ? AudioUnitGetProperty(
            unit, arguments[1], arguments[2], arguments[3],
            (void *)(uintptr_t)arguments[4],
            (UInt32 *)(uintptr_t)arguments[5]) : kAudio_ParamError;
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio GetProperty unit=0x%08x id=%u scope=%u "
                    "element=%u size=%u\n",
                    arguments[0], arguments[1], arguments[2], arguments[3],
                    arguments[5] ?
                    *(const UInt32 *)(uintptr_t)arguments[5] : 0);
            trace_audio_status("AudioUnitGetProperty", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AudioUnitSetProperty")) {
        AudioUnit unit = synchronized_unit_for_guest(arguments[0]);
        OSStatus status = unit ? AudioUnitSetProperty(
            unit, arguments[1], arguments[2], arguments[3],
            (const void *)(uintptr_t)arguments[4], arguments[5])
            : kAudio_ParamError;
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio SetProperty unit=0x%08x id=%u scope=%u "
                    "element=%u size=%u\n",
                    arguments[0], arguments[1], arguments[2], arguments[3],
                    arguments[5]);
            if (arguments[1] == kAudioUnitProperty_StreamFormat &&
                arguments[4] && arguments[5] >= sizeof(AudioStreamBasicDescription)) {
                const AudioStreamBasicDescription *format =
                    (const void *)(uintptr_t)arguments[4];
                char format_id[5];
                fprintf(stderr,
                        "compat32: audio ASBD rate=%.3f format='%s' flags=0x%x "
                        "packetBytes=%u frames=%u frameBytes=%u channels=%u bits=%u\n",
                        format->mSampleRate,
                        fourcc_text(format->mFormatID, format_id),
                        format->mFormatFlags, format->mBytesPerPacket,
                        format->mFramesPerPacket, format->mBytesPerFrame,
                        format->mChannelsPerFrame, format->mBitsPerChannel);
            }
            trace_audio_status("AudioUnitSetProperty", status);
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AudioUnitSetParameter")) {
        bool deferred = false;
        OSStatus status = set_unit_parameter_for_guest(arguments, &deferred);
        if (trace_audio()) {
            static uint64_t parameter_calls;
            static uint64_t deferred_parameter_calls;
            uint64_t call = ++parameter_calls;
            if (deferred) ++deferred_parameter_calls;
            if (call <= 32 || (call % 100000) == 0 || status != noErr) {
                AudioUnitParameterValue value;
                memcpy(&value, &arguments[4], sizeof(value));
                fprintf(stderr,
                        "compat32: audio SetParameter call=%llu deferred=%llu "
                        "unit=0x%08x id=%u scope=%u element=%u value=%g offset=%u\n",
                        (unsigned long long)call,
                        (unsigned long long)deferred_parameter_calls,
                        arguments[0], arguments[1], arguments[2], arguments[3],
                        (double)value, arguments[5]);
                trace_audio_status("AudioUnitSetParameter", status);
            }
        }
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AudioDeviceGetPropertyInfo")) {
        Boolean writable = false;
        Boolean *writable_pointer = arguments[5] ? &writable : NULL;
        UInt32 host_size = 0;
        OSStatus status = AudioDeviceGetPropertyInfo(
            arguments[0], arguments[1], (Boolean)arguments[2], arguments[3],
            &host_size, writable_pointer);
        if (status == noErr && arguments[4]) {
            UInt32 guest_size = host_size;
            if (arguments[3] == kAudioDevicePropertyStreamConfiguration) {
                AudioBufferList *host_list = NULL;
                UInt32 actual_size = 0;
                OSStatus configuration_status = get_host_stream_configuration(
                    arguments[0], arguments[1], (Boolean)arguments[2],
                    &host_list, &actual_size);
                if (configuration_status == noErr) {
                    guest_size = guest_audio_buffer_list_size(
                        bounded_host_audio_buffer_count(host_list, actual_size));
                    free(host_list);
                } else {
                    status = configuration_status;
                }
            }
            *(UInt32 *)(uintptr_t)arguments[4] = guest_size;
        }
        if (arguments[5]) {
            *(Boolean *)(uintptr_t)arguments[5] = writable;
        }
        trace_audio_status("AudioDeviceGetPropertyInfo", status);
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AudioDeviceGetProperty")) {
        OSStatus status;
        if (arguments[3] == kAudioDevicePropertyStreamConfiguration &&
            arguments[4] && arguments[5]) {
            UInt32 *guest_size = (void *)(uintptr_t)arguments[4];
            UInt32 guest_capacity = *guest_size;
            AudioBufferList *host_list = NULL;
            UInt32 host_size = 0;
            status = get_host_stream_configuration(
                arguments[0], arguments[1], (Boolean)arguments[2],
                &host_list, &host_size);
            if (status == noErr) {
                uint32_t buffer_count =
                    bounded_host_audio_buffer_count(host_list, host_size);
                UInt32 required_size =
                    guest_audio_buffer_list_size(buffer_count);
                *guest_size = required_size;
                if (guest_capacity < required_size) {
                    status = kAudio_ParamError;
                } else {
                    uint32_t *guest_count =
                        (void *)(uintptr_t)arguments[5];
                    *guest_count = buffer_count;
                    struct guest_audio_buffer *guest_buffers =
                        (void *)(guest_count + 1);
                    for (uint32_t index = 0; index < buffer_count; ++index) {
                        guest_buffers[index].channels =
                            host_list->mBuffers[index].mNumberChannels;
                        guest_buffers[index].byte_size =
                            host_list->mBuffers[index].mDataByteSize;
                        /* Stream-configuration buffers describe topology;
                           their host data pointers are neither used by this
                           guest nor representable in its address space. */
                        guest_buffers[index].data = 0;
                    }
                    if (trace_audio()) {
                        uint32_t channels = 0;
                        for (uint32_t index = 0; index < buffer_count; ++index) {
                            channels += guest_buffers[index].channels;
                        }
                        fprintf(stderr,
                                "compat32: audio converted stream layout "
                                "buffers=%u channels=%u hostBytes=%u guestBytes=%u\n",
                                buffer_count, channels, host_size, required_size);
                    }
                }
                free(host_list);
            }
        } else {
            status = AudioDeviceGetProperty(
                arguments[0], arguments[1], (Boolean)arguments[2], arguments[3],
                (UInt32 *)(uintptr_t)arguments[4],
                (void *)(uintptr_t)arguments[5]);
        }
        trace_audio_status("AudioDeviceGetProperty", status);
        *result = (uint32_t)status;
        return 1;
    }
    if (LP32_NAME_IS(import_name, import_length, "_AudioHardwareGetProperty")) {
        OSStatus status = AudioHardwareGetProperty(
            arguments[0], (UInt32 *)(uintptr_t)arguments[1],
            (void *)(uintptr_t)arguments[2]);
        trace_audio_status("AudioHardwareGetProperty", status);
        *result = (uint32_t)status;
        return 1;
    }
    return 0;
}

#pragma clang diagnostic pop
