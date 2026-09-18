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
/* Property listeners are kept apart from the render callbacks above: those
   are keyed by (unit, scope, element) and own the context in each entry. */
enum { kPgPropertyListenerCapacity = 64 };
static struct {
    uint32_t unit, property, function, refcon;
} pg_property_listeners[kPgPropertyListenerCapacity];
static uint32_t pg_property_listener_count;
/* CoreAudio identifies a render notification by (unit, proc, refcon). Zuma's
   Revenge registers the same proc for every sound source on one mixer. Each
   refcon needs its own pre/post-render lifecycle, including independent removal. */
enum { kPgRenderNotifyCapacity = 64 };
static struct {
    uint32_t unit, function, refcon;
    struct audio_callback_context *context;
} pg_render_notifies[kPgRenderNotifyCapacity];
static uint32_t pg_render_notify_count;

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
            /* This releases inline, unlike the render-notify path which defers.
               If it can ever run on the worker thread that is inside this same
               context's guest render, it is the level-entry deadlock shape all
               over again -- so count that case rather than assume either way.
               SmartDX retires a node-input callback per sound (27 detaches for
               18 contexts in a short run), so this is a hot path, not a rare
               one. */
            if (pg_unit_callbacks[i].context) {
                pthread_t render = __atomic_load_n(
                    &pg_unit_callbacks[i].context->render_thread,
                    __ATOMIC_ACQUIRE);
                if (render && pthread_equal(render, pthread_self())) {
                    fprintf(stderr,
                            "compat32: audio INLINE RELEASE on the rendering "
                            "thread (unit=0x%08x scope=%u element=%u) -- "
                            "deadlock shape\n", unit, scope, element);
                }
                release_audio_callback(pg_unit_callbacks[i].context);
            }
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
    if (!lp32_title_is_peggle_engine(lp32_profile()->title)) return 0;
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
    if (IS("_OpenComponent")) {
        AudioComponent component = pg_component(a[0]);
        AudioUnit unit = NULL;
        OSStatus status = component ? AudioComponentInstanceNew(component, &unit) : (OSStatus)-3000;
        RET(status == noErr ? guest_handle_for_unit(unit, NULL) : 0);
    }
    if (IS("_OpenDefaultComponent")) {
        AudioComponentDescription description = {0};
        description.componentType = a[0];
        description.componentSubType = a[1];
        AudioComponent component = AudioComponentFindNext(NULL, &description);
        AudioUnit unit = NULL;
        OSStatus status = component ? AudioComponentInstanceNew(component, &unit) : (OSStatus)-3000;
        RET(status == noErr ? guest_handle_for_unit(unit, NULL) : 0);
    }
    if (IS("_CloseComponent")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        OSStatus status = unit ? AudioComponentInstanceDispose(unit) : noErr;
        if (trace_audio()) {
            fprintf(stderr, "compat32: audio CloseComponent unit=0x%08x\n", a[0]);
            trace_audio_status("CloseComponent", status);
        }
        RET(status);
    }
    if (IS("_AudioUnitSetProperty") && a[1] == kAudioUnitProperty_StreamFormat &&
        a[4] >= 0x1000 && trace_audio()) {
        const AudioStreamBasicDescription *format = P(4);
        fprintf(stderr,
                "compat32: audio STREAMFORMAT unit=0x%08x scope=%u element=%u "
                "rate=%.0f fmt=%.4s flags=0x%x bytesPerPacket=%u framesPerPacket=%u "
                "bytesPerFrame=%u channels=%u bits=%u%s\n",
                a[0], a[2], a[3], format->mSampleRate,
                (const char *)&format->mFormatID, (unsigned)format->mFormatFlags,
                (unsigned)format->mBytesPerPacket, (unsigned)format->mFramesPerPacket,
                (unsigned)format->mBytesPerFrame, (unsigned)format->mChannelsPerFrame,
                (unsigned)format->mBitsPerChannel,
                (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved)
                    ? " NON-INTERLEAVED" : " interleaved");
    }
    /* Component version, as the Component Manager's 0xMMMMmmbb.  The older
       titles never looked at this, but Zuma's Revenge does: SmartDX opens the
       default output component, asks its version and refuses to use it unless
       it is above 0x000102FF, then reports having no audio device at all.
       Report the 1.3 the QuickTime-era audio components did. */
    if (IS("_CallComponentVersion")) RET(0x00030000);
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
    /*
     * Property listeners.  SmartDX registers one for the output unit's sample
     * rate and only looks at whether the registration succeeded -- its handler
     * re-reads the unit's settings, which it also does directly straight
     * afterwards.  Record the registration so a matching removal succeeds, but
     * do not attach a host listener: the render callback machinery below
     * marshals a completely different signature, and a property notification
     * arriving on an arbitrary thread has nowhere safe to land.  The guest is
     * therefore never called back, which costs it a re-read it performs anyway.
     */
    if (IS("_AudioUnitAddPropertyListener")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        if (!unit) RET(kAudio_ParamError);
        /* Its own table: pg_unit_callbacks keys render callbacks by
           (unit, scope, element) and owns the context in each entry, so a
           listener recorded there would be matched -- and released -- as if
           it were one. */
        for (uint32_t i = 0; i < pg_property_listener_count; ++i) {
            if (pg_property_listeners[i].unit == a[0] &&
                pg_property_listeners[i].property == a[1] &&
                pg_property_listeners[i].function == a[2]) RET(noErr);
        }
        if (pg_property_listener_count < kPgPropertyListenerCapacity) {
            pg_property_listeners[pg_property_listener_count].unit = a[0];
            pg_property_listeners[pg_property_listener_count].property = a[1];
            pg_property_listeners[pg_property_listener_count].function = a[2];
            pg_property_listeners[pg_property_listener_count].refcon = a[3];
            ++pg_property_listener_count;
        }
        if (trace_audio()) {
            fprintf(stderr, "compat32: audio AddPropertyListener unit=0x%08x "
                    "property=%u fn=0x%08x (recorded, not delivered)\n",
                    a[0], a[1], a[2]);
        }
        RET(noErr);
    }
    if (IS("_AudioUnitRemovePropertyListener") ||
        IS("_AudioUnitRemovePropertyListenerWithUserData")) {
        for (uint32_t i = 0; i < pg_property_listener_count; ++i) {
            if (pg_property_listeners[i].unit != a[0] ||
                pg_property_listeners[i].property != a[1] ||
                pg_property_listeners[i].function != a[2]) continue;
            pg_property_listeners[i] =
                pg_property_listeners[--pg_property_listener_count];
            break;
        }
        RET(noErr);
    }
    if (IS("_AudioUnitRemoveRenderNotify")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        if (!unit) RET(kAudio_ParamError);
        /* The notify was installed with its own context as the refcon, so that
           same context has to be handed back for the removal to match; only
           then is the guest callback free to release. */
        uint32_t slot = pg_render_notify_count;
        for (uint32_t i = 0; i < pg_render_notify_count; ++i) {
            if (pg_render_notifies[i].unit == a[0] &&
                pg_render_notifies[i].function == a[1] &&
                pg_render_notifies[i].refcon == a[2]) { slot = i; break; }
        }
        if (slot == pg_render_notify_count) RET(noErr); /* never installed */
        struct audio_callback_context *context = pg_render_notifies[slot].context;
        OSStatus status = AudioUnitRemoveRenderNotify(unit, host_audio_callback,
                                                      context);
        if (status == noErr) {
            pg_render_notifies[slot] = pg_render_notifies[--pg_render_notify_count];
            /* SmartDX removes a notify from inside the render it belongs to,
               when the sound completes.  The worker running that guest code
               holds context->lock for the duration of the call, so the release
               has to happen off this thread. */
            enqueue_callback_release(context);
        }
        if (trace_audio()) trace_audio_status("AudioUnitRemoveRenderNotify", status);
        RET(status);
    }
    if (IS("_AudioUnitRender")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        RET(unit ? AudioUnitRender(unit, (AudioUnitRenderActionFlags *)P(1),
                                   P(2), a[3], a[4], P(5))
                 : kAudio_ParamError);
    }
    if (IS("_AudioConverterConvertBuffer")) {
        AudioConverterRef converter = pg_converter(a[0]);
        RET(converter ? AudioConverterConvertBuffer(converter, a[1], P(2),
                                                    (UInt32 *)P(3), P(4))
                      : kAudio_ParamError);
    }
    if (IS("_AudioConverterReset")) {
        AudioConverterRef converter = pg_converter(a[0]);
        RET(converter ? AudioConverterReset(converter) : kAudio_ParamError);
    }
    if (IS("_AudioUnitAddRenderNotify")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        if (!unit) RET(kAudio_ParamError);
        /* Only an exact duplicate is already installed. Different refcons
           with the same proc are separate sources and must all be called. */
        for (uint32_t i = 0; i < pg_render_notify_count; ++i) {
            if (pg_render_notifies[i].unit == a[0] &&
                pg_render_notifies[i].function == a[1] &&
                pg_render_notifies[i].refcon == a[2]) {
                if (trace_audio()) {
                    fprintf(stderr, "compat32: audio AddRenderNotify unit=0x%08x "
                            "fn=0x%08x (already installed)\n", a[0], a[1]);
                }
                RET(noErr);
            }
        }
        if (pg_render_notify_count >= kPgRenderNotifyCapacity) RET(kAudio_ParamError);
        struct audio_callback_context *context =
            new_audio_callback(a[1], a[2], "render-notify", NULL, 0, 0);
        OSStatus status = context ?
            AudioUnitAddRenderNotify(unit, host_audio_callback, context) : kAudio_ParamError;
        if (status == noErr) {
            pg_render_notifies[pg_render_notify_count].unit = a[0];
            pg_render_notifies[pg_render_notify_count].function = a[1];
            pg_render_notifies[pg_render_notify_count].refcon = a[2];
            pg_render_notifies[pg_render_notify_count].context = context;
            ++pg_render_notify_count;
        } else {
            /* Reachable from inside a render as well, so it carries the same
               self-deadlock hazard as the removal path. */
            enqueue_callback_release(context);
        }
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
            static int force_passthrough = -1;
            if (force_passthrough < 0) {
                force_passthrough = getenv("LP32_FORCE_STEREO_PASSTHROUGH") != NULL;
            }
            UInt32 passthrough = kSpatializationAlgorithm_StereoPassThrough;
            if (force_passthrough &&
                a[1] == kAudioUnitProperty_SpatializationAlgorithm) {
                status = AudioUnitSetProperty(unit, a[1], a[2], a[3],
                                              &passthrough, sizeof(passthrough));
                if (trace_audio()) {
                    fprintf(stderr, "compat32: audio SpatializationAlgorithm "
                            "element=%u forced to StereoPassThrough\n", a[3]);
                }
                RET(status);
            }
            status = AudioUnitSetProperty(unit, a[1], a[2], a[3], P(4), a[5]);
            if (trace_audio()) {
                fprintf(stderr, "compat32: audio SetProperty unit=0x%08x id=%u scope=%u element=%u size=%u\n",
                        a[0], a[1], a[2], a[3], a[5]);
                /* The id and size alone cannot say how the mixer was actually
                   configured.  "EqualPowerPanning on elements 1-63,
                   StereoPassThrough on element 0, distance attenuation on" has
                   been carried in a comment and reasoned from all session, but
                   never measured in this build -- and the mixer is what eats
                   the audio.  These are 4-byte scalars; print them so the
                   configuration is evidence rather than folklore. */
                if ((a[1] == kAudioUnitProperty_SpatializationAlgorithm ||
                     a[1] == kAudioUnitProperty_SpatialMixerRenderingFlags) &&
                    a[4] >= 0x1000 && a[5] >= sizeof(UInt32)) {
                    UInt32 scalar = 0;
                    memcpy(&scalar, P(4), sizeof(scalar));
                    fprintf(stderr,
                            "compat32: audio   -> %s element=%u value=%u\n",
                            a[1] == kAudioUnitProperty_SpatializationAlgorithm ?
                                "SpatializationAlgorithm" :
                                "SpatialMixerRenderingFlags",
                            a[3], scalar);
                }
                /* The ids alone cannot say whether a mixer was configured into
                   silence: a 3D mixer consumes valid input and emits nothing
                   when its distance/gain or stream format are wrong.  Dump the
                   payload as words and as floats so the values are visible. */
                const uint32_t *payload = a[4] >= 0x1000 ? P(4) : NULL;
                if (payload && a[5] >= 4) {
                    unsigned words = a[5] / 4;
                    if (words > 10) words = 10;
                    for (unsigned i = 0; i < words; ++i) {
                        float as_float;
                        memcpy(&as_float, &payload[i], sizeof(as_float));
                        fprintf(stderr,
                                "compat32: audio   value[%u]=0x%08x (%.4f)\n",
                                i, payload[i], (double)as_float);
                    }
                }
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
            /* SmartDX reads SpatialMixerDistanceParams and never writes it, so
               the mixer's own defaults decide how far away every voice sounds.
               Those defaults are what this port inherits from a modern
               AUSpatialMixer rather than the 2009 AU3DMixer the game expects,
               and they have been assumed all session without once being read.
               MixerDistanceParams is three floats. */
            if (status == noErr &&
                a[1] == kAudioUnitProperty_SpatialMixerDistanceParams &&
                a[4] >= 0x1000) {
                const float *params = P(4);
                fprintf(stderr,
                        "compat32: audio   -> DistanceParams element=%u "
                        "reference=%.4f max=%.4f maxAttenuation=%.4f\n",
                        a[3], (double)params[0], (double)params[1],
                        (double)params[2]);
            }
            trace_audio_status("AudioUnitGetProperty", status);
        }
        RET(status);
    }
    if (IS("_AudioUnitSetParameter")) {
        AudioUnit unit = unit_for_guest(a[0], NULL);
        AudioUnitParameterValue value;
        memcpy(&value, &a[4], sizeof(value));
        OSStatus status = unit ?
            AudioUnitSetParameter(unit, a[1], a[2], a[3], value, a[5]) :
            kAudio_ParamError;
        if (trace_audio()) {
            fprintf(stderr,
                    "compat32: audio SetParameter unit=0x%08x id=%u scope=%u "
                    "element=%u value=%.4f\n",
                    a[0], a[1], a[2], a[3], (double)value);
            trace_audio_status("AudioUnitSetParameter", status);
        }
        RET(status);
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
    if (IS("_AudioConverterSetProperty")) {
        AudioConverterRef converter = pg_converter(a[0]);
        RET(converter ? AudioConverterSetProperty(converter, a[1], a[2],
                                                  a[3] >= 0x1000 ? P(3) : NULL)
                      : kAudio_ParamError);
    }
    if (IS("_AudioConverterGetProperty")) {
        AudioConverterRef converter = pg_converter(a[0]);
        RET(converter ? AudioConverterGetProperty(converter, a[1],
                                                  a[2] >= 0x1000 ? P(2) : NULL,
                                                  a[3] >= 0x1000 ? P(3) : NULL)
                      : kAudio_ParamError);
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
        /* Record what this converter actually produced, for the render callback
           in progress: a short fill is what leaves a stale tail in the guest's
           shared scratch buffer. */
        ++converter_calls_this_call;
        converter_frames_this_call = packets;
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
