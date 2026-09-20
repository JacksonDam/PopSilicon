/* These checks run with the Bejeweled 3 image mapped, before its entry point,
   so they exercise the original sizing method without changing preferences. */
@interface LP32SuperProbeBase : NSObject
- (NSUInteger)probe:(NSNumber *)value;
- (NSUInteger)offset;
@end
@implementation LP32SuperProbeBase
- (NSUInteger)probe:(NSNumber *)value { return [value unsignedIntegerValue] + [self offset]; }
- (NSUInteger)offset { return 1; }
@end
@interface LP32SuperProbe : LP32SuperProbeBase
@end
@implementation LP32SuperProbe
- (NSUInteger)probe:(NSNumber *)value { (void)value; return 999; }
- (NSUInteger)offset { return 7; }
@end

int objc_bridge32_run_guest_class_self_test(void)
{
    @autoreleasepool {
        guest_synthesize_all_classes();
        struct guest_objc_class *window = guest_class_named("SexyWindow");
        if (!window) {
            fputs("Guest class self-test requires the Bejeweled 3 image\n", stderr);
            return -1;
        }
        uint32_t storage = compat_runtime32_allocate(5 * sizeof(float), 1);
        if (!storage) return -1;
        float *rect = (float *)(uintptr_t)storage;
        static const struct {
            float width, height, max_width, max_height, expected_width, expected_height;
        } cases[] = {
            {800, 600, 1920, 1080, 800, 600},
            {800, 600, 1920, 480, 640, 480},
            {800, 600, 512, 1080, 512, 384},
            {320, 240, 1920, 1080, 320, 240},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            uint32_t args[17] = {storage, window->class_addr,
                guest_selector("contentRectForContent:preferred:safe:min:max:")};
            float geometry[14] = {1600, 1200, 100, 100,
                cases[i].width, cases[i].height, 0, 0, 1920, 1080,
                320, 240, cases[i].max_width, cases[i].max_height};
            memcpy(args + 3, geometry, sizeof(geometry));
            rect[4] = 12345;
            uint64_t result = 0;
            bool handled = guest_objc_msgsend_stret(args, &result);
            if (!handled || rect[2] != cases[i].expected_width ||
                rect[3] != cases[i].expected_height || rect[4] != 12345) {
                fprintf(stderr, "Guest class self-test: sizing case %zu got %.0fx%.0f\n",
                        i, rect[2], rect[3]);
                compat_runtime32_deallocate(storage);
                return -1;
            }
        }
        compat_runtime32_deallocate(storage);

        /* Native super lookup must skip the override but preserve normal
           virtual dispatch for a callback made by the superclass. */
        struct guest_objc_class probe_class = {.host_class = [LP32SuperProbe class]};
        struct guest_objc_instance probe = {
            .companion = [[[LP32SuperProbe alloc] init] autorelease], .cls = &probe_class};
        uint32_t value = proxy_for_object(@35);
        uint64_t result = 0;
        if (!guest_objc_super_message(&probe, "probe:", &value, &result) || result != 42) {
            fputs("Guest class self-test: superclass dispatch failed\n", stderr);
            return -1;
        }

        uint32_t live_before = proxy_count - proxy_free_count;
        for (unsigned i = 0; i < 10000; ++i) {
            @autoreleasepool {
                NSNotification *note = [NSNotification notificationWithName:@"frame" object:nil];
                uint32_t borrowed = 0;
                uint32_t handle = proxy_for_callback_argument(note, &borrowed);
                uint32_t nested = 0;
                if (!borrowed || object_for_receiver(handle) != note ||
                    proxy_for_callback_argument(note, &nested) != handle || nested) {
                    fputs("Guest class self-test: notification identity failed\n", stderr);
                    return -1;
                }
                release_callback_argument(nested);
                release_callback_argument(borrowed);
                if (object_for_receiver(handle)) {
                    fputs("Guest class self-test: notification handle outlived callback\n", stderr);
                    return -1;
                }
            }
        }
        if (proxy_count - proxy_free_count != live_before) {
            fputs("Guest class self-test: notification proxies leaked\n", stderr);
            return -1;
        }
        NSNotification *note = [NSNotification notificationWithName:@"retained" object:nil];
        uint32_t borrowed = 0;
        uint32_t retain_args[] = {proxy_for_callback_argument(note, &borrowed), guest_selector("retain")};
        if (!objc_bridge32_dispatch("_objc_msgSend", retain_args, &result)) return -1;
        release_callback_argument(borrowed);
        if (object_for_receiver(retain_args[0]) != note) {
            fputs("Guest class self-test: retained notification lost\n", stderr);
            return -1;
        }
        [note release];
        recycle_proxy((uint32_t)proxy_index_for_handle(retain_args[0]));
        puts("Guest class self-test: PASS (saved geometry, superclass dispatch, 10000 notifications)");
        return 0;
    }
}
