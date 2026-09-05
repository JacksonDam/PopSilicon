/* Peggle's CoreAudio path: Component Manager lookups, per-unit render
   callbacks and an AudioConverter fed by a guest input procedure.  Units
   share the AUGraph bridge's handle table and its callback worker threads. */
enum {
    kPgComponentBase = 0x7f0c0000,
    kPgComponentCapacity = 64,
    kPgConverterBase = 0x7f0d0000,
    kPgConverterCapacity = 256,
    kPgConverterBuffers = 8,
};
static AudioComponent pg_components[kPgComponentCapacity];
static uint32_t pg_component_count;
static AudioConverterRef pg_converters[kPgConverterCapacity];
static struct {
    uint32_t unit, scope, element;
    struct audio_callback_context *context;
} pg_unit_callbacks[kAudioCallbackCapacity];
static uint32_t pg_unit_callback_count;

static AudioComponent pg_component(uint32_t handle)
{
    uint32_t index = handle - kPgComponentBase;
    return handle >= kPgComponentBase && index < pg_component_count ?
        pg_components[index] : NULL;
}
static AudioConverterRef pg_converter(uint32_t handle)
{
    uint32_t index = handle - kPgConverterBase;
    return handle >= kPgConverterBase && index < kPgConverterCapacity ?
        pg_converters[index] : NULL;
}
/* The host unit keeps the previous callback context alive until the property
   write replaces it, so the old context is released only after a success. */
static void pg_replace_unit_callback(uint32_t unit, uint32_t scope, uint32_t element,
                                     struct audio_callback_context *context)
{
    for (uint32_t i = 0; i < pg_unit_callback_count; ++i) {
        if (pg_unit_callbacks[i].unit == unit && pg_unit_callbacks[i].scope == scope &&
            pg_unit_callbacks[i].element == element) {
            if (pg_unit_callbacks[i].context) release_audio_callback(pg_unit_callbacks[i].context);
            pg_unit_callbacks[i].context = context;
            return;
        }
    }
    if (pg_unit_callback_count < kAudioCallbackCapacity) {
        pg_unit_callbacks[pg_unit_callback_count].unit = unit;
        pg_unit_callbacks[pg_unit_callback_count].scope = scope;
        pg_unit_callbacks[pg_unit_callback_count].element = element;
        pg_unit_callbacks[pg_unit_callback_count].context = context;
        ++pg_unit_callback_count;
    }
}

/* AudioConverterFillComplexBuffer pulls input through this host procedure,
   which relays to the guest's; the guest fills a guest-layout buffer list
   (12-byte AudioBuffers) that is copied into the host's. */
struct pg_converter_call {
    uint32_t handle, proc, user, guest_list, guest_packets, guest_desc;
};
static OSStatus pg_converter_input(AudioConverterRef converter, UInt32 *io_packets,
                                   AudioBufferList *io_data,
                                   AudioStreamPacketDescription **out_desc, void *opaque)
{
    (void)converter;
    struct pg_converter_call *call = opaque;
    uint32_t *list = (void *)(uintptr_t)call->guest_list;
    uint32_t count = io_data->mNumberBuffers;
    if (count > kPgConverterBuffers) count = kPgConverterBuffers;
    list[0] = count;
    for (uint32_t i = 0; i < count; ++i) {
        list[1 + 3 * i] = io_data->mBuffers[i].mNumberChannels;
        list[2 + 3 * i] = io_data->mBuffers[i].mDataByteSize;
        list[3 + 3 * i] = 0;
    }
    *(uint32_t *)(uintptr_t)call->guest_packets = *io_packets;
    *(uint32_t *)(uintptr_t)call->guest_desc = 0;
    const uint32_t arguments[] = {
        call->handle, call->guest_packets, call->guest_list,
        out_desc ? call->guest_desc : 0, call->user,
    };
    OSStatus status = (OSStatus)compat_runtime32_call(call->proc, arguments, 5);
    if (compat_runtime32_last_call_trapped()) return kAudio_ParamError;
    *io_packets = *(const uint32_t *)(uintptr_t)call->guest_packets;
    if (list[0] < count) count = list[0];
    io_data->mNumberBuffers = count;
    for (uint32_t i = 0; i < count; ++i) {
        io_data->mBuffers[i].mNumberChannels = list[1 + 3 * i];
        io_data->mBuffers[i].mDataByteSize = list[2 + 3 * i];
        io_data->mBuffers[i].mData = (void *)(uintptr_t)list[3 + 3 * i];
    }
    if (out_desc) {
        *out_desc = (void *)(uintptr_t)*(const uint32_t *)(uintptr_t)call->guest_desc;
    }
    return status;
}

static int peggle_audio_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define OUT(i, v) do { if (a[i]) *(uint32_t *)P(i) = (uint32_t)(v); } while (0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    if (lp32_profile()->title != LP32_TITLE_PEGGLE) return 0;
    if (IS("_FindNextComponent")) {
        AudioComponent found = AudioComponentFindNext(pg_component(a[0]), P(1));
        if (!found) RET(0);
        for (uint32_t i = 0; i < pg_component_count; ++i) {
            if (pg_components[i] == found) RET(kPgComponentBase + i);
        }
        if (pg_component_count >= kPgComponentCapacity) RET(0);
        pg_components[pg_component_count] = found;
        RET(kPgComponentBase + pg_component_count++);
    }
    if (IS("_OpenAComponent")) {
        AudioComponent component = pg_component(a[0]);
        AudioUnit unit = NULL;
        OSStatus status = component ? AudioComponentInstanceNew(component, &unit) : (OSStatus)-3000;
        uint32_t handle = status == noErr ? guest_handle_for_unit(unit, NULL) : 0;
        if (status == noErr && !handle) {
            AudioComponentInstanceDispose(unit);
            status = kAudio_MemFullError;
        }
        OUT(1, handle);
        if (trace_audio()) {
            const AudioComponentDescription *description = NULL;
            AudioComponentDescription storage;
            if (component && AudioComponentGetDescription(component, &storage) == noErr) description = &storage;
            char type[5], subtype[5];
            fprintf(stderr, "compat32: audio OpenAComponent '%s'/'%s' -> unit=0x%08x\n",
                    fourcc_text(description ? description->componentType : 0, type),
                    fourcc_text(description ? description->componentSubType : 0, subtype), handle);
            trace_audio_status("OpenAComponent", status);
        }
        RET((uint16_t)status);
    }
    if (IS("_AudioUnitInitialize") || IS("_AudioUnitUninitialize") ||
        IS("_AudioOutputUnitStart") || IS("_AudioOutputUnitStop")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        OSStatus status = kAudio_ParamError;
        if (unit) {
            status = n[6] == 'U' ? (n[10] == 'I' ? AudioUnitInitialize(unit) : AudioUnitUninitialize(unit))
                                 : (n[16] == 'S' && n[17] == 't' && n[18] == 'a' ? AudioOutputUnitStart(unit)
                                                                                  : AudioOutputUnitStop(unit));
        }
        if (trace_audio()) {
            fprintf(stderr, "compat32: audio %s unit=0x%08x\n", n + 1, a[0]);
            trace_audio_status(n + 1, status);
        }
        RET(status);
    }
    if (IS("_AudioUnitAddRenderNotify")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        struct audio_callback_context *context =
            new_audio_callback(a[1], a[2], "render-notify", NULL, 0, 0);
        OSStatus status = unit && context ?
            AudioUnitAddRenderNotify(unit, host_audio_callback, context) : kAudio_ParamError;
        if (status != noErr && context) release_audio_callback(context);
        if (trace_audio()) {
            fprintf(stderr, "compat32: audio AddRenderNotify unit=0x%08x fn=0x%08x\n", a[0], a[1]);
            trace_audio_status("AudioUnitAddRenderNotify", status);
        }
        RET(status);
    }
    if (IS("_AudioUnitSetProperty")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        if (!unit) RET(kAudio_ParamError);
        OSStatus status;
        if (a[1] == kAudioUnitProperty_SetRenderCallback) {
            const uint32_t *guest = a[4] >= 0x1000 && a[5] >= 8 ? P(4) : NULL;
            struct audio_callback_context *context = guest && guest[0] ?
                new_audio_callback(guest[0], guest[1], "node-input", NULL, 0, a[3]) : NULL;
            AURenderCallbackStruct callback = {
                .inputProc = context ? host_audio_callback : NULL,
                .inputProcRefCon = context,
            };
            status = AudioUnitSetProperty(unit, a[1], a[2], a[3], &callback, sizeof(callback));
            if (status == noErr) pg_replace_unit_callback(a[0], a[2], a[3], context);
            else if (context) release_audio_callback(context);
            if (trace_audio()) {
                fprintf(stderr, "compat32: audio SetRenderCallback unit=0x%08x scope=%u bus=%u fn=0x%08x\n",
                        a[0], a[2], a[3], guest ? guest[0] : 0);
                trace_audio_status("AudioUnitSetProperty(SetRenderCallback)", status);
            }
        } else if (a[1] == kAudioUnitProperty_MakeConnection) {
            const uint32_t *guest = a[4] >= 0x1000 && a[5] >= 12 ? P(4) : NULL;
            AudioUnitConnection connection = {
                .sourceAudioUnit = guest ? unit_for_guest(guest[0], NULL) : NULL,
                .sourceOutputNumber = guest ? guest[1] : 0,
                .destInputNumber = guest ? guest[2] : 0,
            };
            status = AudioUnitSetProperty(unit, a[1], a[2], a[3], &connection, sizeof(connection));
            if (trace_audio()) {
                fprintf(stderr, "compat32: audio MakeConnection unit=0x%08x bus=%u <- unit=0x%08x out=%u\n",
                        a[0], a[3], guest ? guest[0] : 0, guest ? guest[1] : 0);
                trace_audio_status("AudioUnitSetProperty(MakeConnection)", status);
            }
        } else {
            status = AudioUnitSetProperty(unit, a[1], a[2], a[3], P(4), a[5]);
            if (trace_audio()) {
                fprintf(stderr, "compat32: audio SetProperty unit=0x%08x id=%u scope=%u element=%u size=%u\n",
                        a[0], a[1], a[2], a[3], a[5]);
                trace_audio_status("AudioUnitSetProperty", status);
            }
        }
        RET(status);
    }
    if (IS("_AudioUnitGetProperty")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        OSStatus status = unit ? AudioUnitGetProperty(unit, a[1], a[2], a[3], P(4), P(5)) : kAudio_ParamError;
        if (trace_audio()) {
            fprintf(stderr, "compat32: audio GetProperty unit=0x%08x id=%u scope=%u element=%u\n",
                    a[0], a[1], a[2], a[3]);
            trace_audio_status("AudioUnitGetProperty", status);
        }
        RET(status);
    }
    if (IS("_AudioUnitSetParameter")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        AudioUnitParameterValue value;
        memcpy(&value, &a[4], sizeof(value));
        RET(unit ? AudioUnitSetParameter(unit, a[1], a[2], a[3], value, a[5]) : kAudio_ParamError);
    }
    if (IS("_AudioUnitGetParameter")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        RET(unit ? AudioUnitGetParameter(unit, a[1], a[2], a[3], P(4)) : kAudio_ParamError);
    }
    if (IS("_AudioConverterNew")) {
        AudioConverterRef converter = NULL;
        OSStatus status = AudioConverterNew(P(0), P(1), &converter);
        uint32_t handle = 0;
        if (status == noErr) {
            for (uint32_t i = 0; i < kPgConverterCapacity; ++i) {
                if (!pg_converters[i]) { pg_converters[i] = converter; handle = kPgConverterBase + i; break; }
            }
            if (!handle) { AudioConverterDispose(converter); status = kAudio_MemFullError; }
        }
        OUT(2, handle);
        if (trace_audio()) trace_audio_status("AudioConverterNew", status);
        RET(status);
    }
    if (IS("_AudioConverterDispose")) {
        AudioConverterRef converter = pg_converter(a[0]);
        if (converter) pg_converters[a[0] - kPgConverterBase] = NULL;
        RET(converter ? AudioConverterDispose(converter) : kAudio_ParamError);
    }
    if (IS("_AudioConverterFillComplexBuffer")) {
        AudioConverterRef converter = pg_converter(a[0]);
        uint32_t *guest_list = a[4] >= 0x1000 ? P(4) : NULL;
        if (!converter || !guest_list || a[3] < 0x1000) RET(kAudio_ParamError);
        struct { UInt32 count; AudioBuffer buffers[kPgConverterBuffers]; } host;
        host.count = guest_list[0] < kPgConverterBuffers ? guest_list[0] : kPgConverterBuffers;
        for (uint32_t i = 0; i < host.count; ++i) {
            host.buffers[i].mNumberChannels = guest_list[1 + 3 * i];
            host.buffers[i].mDataByteSize = guest_list[2 + 3 * i];
            host.buffers[i].mData = (void *)(uintptr_t)guest_list[3 + 3 * i];
        }
        uint32_t scratch = compat_runtime32_allocate(4 + 12 * kPgConverterBuffers + 8, 1);
        if (!scratch) RET(kAudio_MemFullError);
        struct pg_converter_call call = {
            a[0], a[1], a[2], scratch, scratch + 4 + 12 * kPgConverterBuffers,
            scratch + 4 + 12 * kPgConverterBuffers + 4,
        };
        UInt32 packets = *(const uint32_t *)P(3);
        OSStatus status = AudioConverterFillComplexBuffer(
            converter, pg_converter_input, &call, &packets, (AudioBufferList *)&host,
            a[5] >= 0x1000 ? P(5) : NULL);
        *(uint32_t *)P(3) = packets;
        for (uint32_t i = 0; i < host.count; ++i) guest_list[2 + 3 * i] = host.buffers[i].mDataByteSize;
        compat_runtime32_deallocate(scratch);
        if (trace_audio()) {
            fprintf(stderr, "compat32: audio FillComplexBuffer converter=0x%08x packets=%u\n", a[0], packets);
            trace_audio_status("AudioConverterFillComplexBuffer", status);
        }
        RET(status);
    }
    return 0;
#undef RET
#undef OUT
#undef P
#undef IS
}
