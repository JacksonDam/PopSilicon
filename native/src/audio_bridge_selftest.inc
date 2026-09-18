/* Run through real CoreAudio notifications and i386 callback workers, without
   opening an output device. Sources share a proc but have distinct refcons. */
static bool audio_test_notify(const char *operation, uint32_t unit,
                              uint32_t proc, uint32_t refcon)
{
    const uint32_t args[] = {unit, proc, refcon};
    uint64_t status = 0;
    bool handled = peggle_audio_dispatch(operation, args, &status);
    if (!handled || (int32_t)status != noErr) {
        fprintf(stderr, "audio self-test: %s failed: handled=%d status=%d\n",
                operation, handled, (int32_t)status);
        return false;
    }
    return true;
}

int audio_bridge32_run_self_test(void)
{
    AudioComponentDescription description = {
        .componentType = kAudioUnitType_Mixer,
        .componentSubType = kAudioUnitSubType_MultiChannelMixer,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
    };
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    AudioUnit unit = NULL;
    OSStatus status = component ? AudioComponentInstanceNew(component, &unit) : kAudio_ParamError;
    if (status != noErr) {
        fprintf(stderr, "audio self-test: create mixer failed: %d\n", (int)status);
        return -1;
    }
    uint32_t handle = guest_handle_for_unit(unit, NULL);
    uint32_t storage = compat_runtime32_allocate(64, 1);
    int result = -1;
    if (!handle || !storage) goto done;
    /* void callback(void *refcon, ...): ++*(uint32_t *)refcon; return noErr. */
    const unsigned char code[] = {0x8b,0x44,0x24,0x04,0xff,0x00,0x31,0xc0,0xc3};
    memcpy((void *)(uintptr_t)storage, code, sizeof(code));
    uint32_t refs[] = {storage + 32, storage + 36, storage + 40};
    uint32_t *counts = (void *)(uintptr_t)refs[0];
    uint32_t expected[3] = {0};
    AudioStreamBasicDescription format = {
        .mSampleRate = 44100,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
        .mBytesPerPacket = 4, .mFramesPerPacket = 1,
        .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 32,
    };
    status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Output, 0, &format, sizeof(format));
    if (status == noErr) status = AudioUnitInitialize(unit);
    if (status != noErr) {
        fprintf(stderr, "audio self-test: initialize mixer failed: %d\n", (int)status);
        goto done;
    }
    double sample_time = 0;
    for (unsigned cycle = 0; cycle < 96; ++cycle) {
        bool active[3] = {true, true, true};
        for (unsigned i = 0; i < 3; ++i) {
            if (!audio_test_notify("_AudioUnitAddRenderNotify", handle, storage, refs[i])) goto done;
        }
        /* Adding the exact same registration twice must remain idempotent. */
        if (!audio_test_notify("_AudioUnitAddRenderNotify", handle, storage, refs[0])) goto done;
        for (unsigned step = 0; step < 4; ++step) {
            float samples[2][64] = {{0}};
            struct { UInt32 count; AudioBuffer buffers[2]; } buffers = {
                2, {{1, sizeof(samples[0]), samples[0]},
                    {1, sizeof(samples[1]), samples[1]}},
            };
            AudioTimeStamp timestamp = {.mSampleTime = sample_time,
                                        .mFlags = kAudioTimeStampSampleTimeValid};
            AudioUnitRenderActionFlags flags = 0;
            OSStatus status = AudioUnitRender(unit, &flags, &timestamp, 0, 64,
                                             (AudioBufferList *)&buffers);
            sample_time += 64;
            if (status != noErr) {
                fprintf(stderr, "audio self-test: render failed: %d\n", (int)status);
                goto done;
            }
            for (unsigned i = 0; i < 3; ++i) {
                if (active[i]) expected[i] += 2; /* pre-render and post-render */
                if (counts[i] != expected[i]) {
                    fprintf(stderr, "audio self-test: cycle=%u step=%u source=%u "
                            "callbacks=%u expected=%u\n", cycle, step, i, counts[i], expected[i]);
                    goto done;
                }
            }
            if (step < 3) {
                unsigned source = (cycle + step + 1) % 3;
                if (!audio_test_notify("_AudioUnitRemoveRenderNotify", handle, storage, refs[source])) goto done;
                active[source] = false;
                /* Removing it again must not remove a different source. */
                if (!audio_test_notify("_AudioUnitRemoveRenderNotify", handle, storage, refs[source])) goto done;
            }
        }
    }
    fprintf(stderr, "audio self-test: PASS (96 shared-proc source lifecycles, "
            "duplicate adds, independent removals, callback reuse)\n");
    result = 0;
done:
    if (storage) {
        for (unsigned i = 0; i < 3; ++i) {
            audio_test_notify("_AudioUnitRemoveRenderNotify", handle, storage, storage + 32 + 4 * i);
        }
    }
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    compat_runtime32_deallocate(storage);
    return result;
}
