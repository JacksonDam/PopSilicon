/* Peggle's Carbon/CoreFoundation API translated to the host Cocoa ABI. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static uint64_t pg_fp_double(double v)
{
    extern _Thread_local int lp32_return_fp_kind;
    extern _Thread_local union { float f; double d; uint64_t bits; } lp32_fp_result;
    lp32_fp_result.d = v;
    lp32_return_fp_kind = 2;
    return 0;
}
#include "peggle_window.inc"
#include "peggle_graphics.inc"

/* A small printf for CFStringCreateWithFormat over i386 varargs (%@ takes a
   proxied CFString, %f a two-word double). */
static NSString *pg_format(NSString *format, const uint32_t *args)
{
    NSMutableString *out = [NSMutableString string];
    NSUInteger length = [format length], i = 0;
    while (i < length) {
        unichar c = [format characterAtIndex:i++];
        if (c != '%' || i >= length) { [out appendFormat:@"%C", c]; continue; }
        char spec[32] = "%";
        size_t n = 1;
        while (i < length && n < 30) {
            unichar d = [format characterAtIndex:i++];
            spec[n++] = (char)d;
            spec[n] = '\0';
            if (d == '%') { [out appendString:@"%"]; break; }
            if (d == '@') { id object = object_for_argument(*args++); [out appendString:object ? [object description] : @"(null)"]; break; }
            if (strchr("diouxXc", d)) {
                if (strchr(spec, 'l') && strstr(spec, "ll")) { int64_t v = (uint64_t)args[0] | ((uint64_t)args[1] << 32); args += 2; [out appendFormat:@"%lld", (long long)v]; }
                else { char *end = spec + n - 1; if (*(end - 1) == 'l') { memmove(end - 1, end, 2); } [out appendFormat:[NSString stringWithUTF8String:spec], (int)*args++]; }
                break;
            }
            if (strchr("feEgG", d)) { double v; memcpy(&v, args, 8); args += 2; [out appendFormat:[NSString stringWithUTF8String:spec], v]; break; }
            if (d == 's') { const char *s = (const char *)(uintptr_t)*args++; [out appendFormat:@"%s", s ? s : "(null)"]; break; }
            if (d == 'p') { [out appendFormat:@"0x%08x", *args++]; break; }
        }
    }
    return out;
}

static int peggle_mac_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define O(i) object_for_argument(a[i])
#define OUT(i, v) do { if (a[i]) *(uint32_t *)P(i) = (uint32_t)(v); } while (0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    if (lp32_profile()->title != LP32_TITLE_PEGGLE) return 0;
    if (peggle_window_dispatch(n, a, r) || peggle_graphics_dispatch(n, a, r)) return 1;
    if (IS("_CFBundleGetValueForInfoDictionaryKey")) RET(proxy_for_object([O(0) objectForInfoDictionaryKey:O(1)]));
    if (IS("_CFBundleGetVersionNumber")) RET(0x01058000);
    if (IS("_CFBundleCopyLocalizedString")) RET(pg_cf_register((CFTypeRef)[[O(0) localizedStringForKey:O(1) value:O(2) table:O(3)] retain]));
    if (IS("_CFStringCreateWithCString")) RET(pg_cf_register(a[1] >= 0x1000 ? CFStringCreateWithCString(NULL, P(1), a[2]) : NULL));
    if (IS("_CFStringCreateWithCharacters")) RET(pg_cf_register(a[1] >= 0x1000 ? CFStringCreateWithCharacters(NULL, P(1), a[2]) : NULL));
    if (IS("_CFStringCreateWithFormat")) RET(pg_cf_register((CFTypeRef)[pg_format(O(2) ?: @"", a + 3) retain]));
    if (IS("_CFStringGetCStringPtr")) RET(pg_cf_cstring(a[0], a[1]));
    if (IS("_CFStringGetCharacters")) { [O(0) getCharacters:P(3) range:NSMakeRange(a[1], a[2])]; RET(0); }
    if (IS("_CFStringGetTypeID")) RET(CFStringGetTypeID());
    if (IS("_CFDataGetTypeID")) RET(CFDataGetTypeID());
    if (IS("_CFNumberGetTypeID")) RET(CFNumberGetTypeID());
    if (IS("_CFDataCreate")) RET(pg_cf_register(CFDataCreate(NULL, P(1), a[2])));
    if (IS("_CFDataGetLength")) RET([O(0) length]);
    if (IS("_CFDataGetBytes")) { [O(0) getBytes:P(3) range:NSMakeRange(a[1], a[2])]; RET(0); }
    if (IS("_CFBooleanGetValue")) RET([O(0) boolValue]);
    if (IS("_CFNumberCreate")) RET(pg_cf_register(CFNumberCreate(NULL, a[1], P(2))));
    if (IS("_CFPreferencesCopyAppValue")) RET(pg_cf_register(CFPreferencesCopyAppValue((CFStringRef)O(0), CFSTR("local.peggle.compat"))));
    if (IS("_CFPreferencesSetAppValue")) { CFPreferencesSetAppValue((CFStringRef)O(0), (CFTypeRef)O(1), CFSTR("local.peggle.compat")); RET(0); }
    if (IS("_CFPreferencesAppSynchronize")) RET(CFPreferencesAppSynchronize(CFSTR("local.peggle.compat")));
    if (IS("_CFLocaleCopyCurrent")) RET(pg_cf_register(CFLocaleCopyCurrent()));
    if (IS("_CFNumberFormatterCreate")) RET(pg_cf_register(CFNumberFormatterCreate(NULL, (CFLocaleRef)O(1), a[2])));
    if (IS("_CFNumberFormatterCreateStringWithValue")) RET(pg_cf_register(CFNumberFormatterCreateStringWithValue(NULL, (CFNumberFormatterRef)O(1), a[2], P(3))));
    if (IS("_CFURLCreateWithBytes")) RET(pg_cf_register(CFURLCreateWithBytes(NULL, P(1), a[2], a[3], (CFURLRef)O(4))));
    if (IS("_CFURLGetFSRef")) RET(CFURLGetFSRef((CFURLRef)O(0), P(1)));
    if (IS("_CFUserNotificationDisplayAlert")) {
        NSString *title = O(6), *message = O(7);
        fprintf(stderr, "compat32: alert \"%s\": %s\n", [title UTF8String] ?: "", [message UTF8String] ?: "");
        NSAlert *alert = [[[NSAlert alloc] init] autorelease];
        [alert setMessageText:title ?: @"Peggle"];
        [alert setInformativeText:message ?: @""];
        [alert addButtonWithTitle:O(8) ?: @"OK"];
        if (O(9)) [alert addButtonWithTitle:O(9)];
        if (O(10)) [alert addButtonWithTitle:O(10)];
        pg_start_app();
        NSModalResponse response = [alert runModal];
        OUT(11, response == NSAlertSecondButtonReturn ? 1 : response == NSAlertThirdButtonReturn ? 2 : 0);
        RET(0);
    }
    if (IS("_LSOpenCFURLRef")) { if (O(0)) [[NSWorkspace sharedWorkspace] openURL:O(0)]; OUT(1, 0); RET(0); }
    if (IS("_LSOpenFromRefSpec")) RET(0);
    if (IS("_SysBeep")) { NSBeep(); RET(0); }
    if (IS("_DebugStr")) { const uint8_t *p = P(0); if (p) fprintf(stderr, "compat32: DebugStr %.*s\n", p[0], p + 1); RET(0); }
    /* Scrap Manager (clipboard) over NSPasteboard, text flavors only. */
    if (IS("_GetCurrentScrap")) { OUT(0, 1); RET(0); }
    if (IS("_ClearCurrentScrap")) { [[NSPasteboard generalPasteboard] clearContents]; RET(0); }
    if (IS("_GetScrapFlavorFlags") || IS("_GetScrapFlavorSize") || IS("_GetScrapFlavorData")) {
        NSString *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (!text || (a[1] != 'TEXT' && a[1] != 'utxt')) RET(-102); /* noTypeErr */
        NSData *data = a[1] == 'TEXT' ? [text dataUsingEncoding:NSMacOSRomanStringEncoding allowLossyConversion:YES]
                                      : [text dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
        if (IS("_GetScrapFlavorFlags")) { OUT(2, 0); RET(0); }
        if (IS("_GetScrapFlavorSize")) { OUT(2, [data length]); RET(0); }
        uint32_t *size = P(2);
        uint32_t copy = size && *size < [data length] ? *size : (uint32_t)[data length];
        if (a[3]) memcpy(P(3), [data bytes], copy);
        if (size) *size = copy;
        RET(0);
    }
    if (IS("_PutScrapFlavor")) {
        if (a[1] == 'TEXT' && a[4]) {
            NSString *text = [[[NSString alloc] initWithBytes:P(4) length:a[3] encoding:NSMacOSRomanStringEncoding] autorelease];
            [[NSPasteboard generalPasteboard] clearContents];
            if (text) [[NSPasteboard generalPasteboard] setString:text forType:NSPasteboardTypeString];
        }
        RET(0);
    }
    if (IS("_FSPathMakeRef")) RET((uint32_t)FSPathMakeRef(P(0), P(1), P(2)));
    if (IS("_FSRefMakePath")) RET((uint32_t)FSRefMakePath(P(0), P(1), a[2]));
    if (IS("_FSFindFolder")) RET((uint32_t)FSFindFolder(a[0], a[1], a[2], P(3)));
    if (IS("_FSMakeFSRefUnicode")) RET((uint32_t)FSMakeFSRefUnicode(P(0), a[1], P(2), a[3], P(4)));
    if (IS("_FSCreateDirectoryUnicode")) RET((uint32_t)FSCreateDirectoryUnicode(P(0), a[1], P(2), a[3], P(4), P(5), NULL, NULL));
    if (IS("_FSCreateFileUnicode")) RET((uint32_t)FSCreateFileUnicode(P(0), a[1], P(2), a[3], P(4), P(5), NULL));
    if (IS("_GetScriptManagerVariable") || IS("_GetScriptVariable")) RET(0);
    if (IS("_GetResource")) { static uint32_t resource; if (!resource) resource = compat_runtime32_allocate(8, 1); *(uint32_t *)(uintptr_t)resource = resource + 4; RET(resource); }
    if (IS("_KeyTranslate")) {
        TISInputSourceRef input = TISCopyCurrentKeyboardLayoutInputSource();
        CFDataRef data = input ? TISGetInputSourceProperty(input, kTISPropertyUnicodeKeyLayoutData) : NULL;
        UniChar chars[8] = {0};
        UniCharCount count = 0;
        UInt32 dead = 0;
        if (data) UCKeyTranslate((const UCKeyboardLayout *)CFDataGetBytePtr(data), a[1] & 0x7f, kUCKeyActionDown, (a[1] >> 8) & 255, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &dead, 8, &count, chars);
        if (input) CFRelease(input);
        OUT(2, 0);
        RET(chars[0]);
    }
    if (IS("_CopyCStringToPascal")) { size_t len = strlen(P(0)); if (len > 255) len = 255; *(uint8_t *)P(1) = len; memcpy((uint8_t *)P(1) + 1, P(0), len); RET(0); }
    if (IS("_Delay")) { usleep(a[0] * 1000000u / 60); OUT(1, (uint32_t)(CFAbsoluteTimeGetCurrent() * 60)); RET(0); }
    if (IS("_AddNanosecondsToAbsolute")) { mach_timebase_info_data_t info; mach_timebase_info(&info); uint64_t ns = (uint64_t)a[0] | ((uint64_t)a[1] << 32), time = (uint64_t)a[2] | ((uint64_t)a[3] << 32); RET(time + ns * info.denom / info.numer); }
    if (IS("_MPDelayUntil")) { uint64_t target; memcpy(&target, P(0), 8); mach_wait_until(target); RET(0); }
    if (IS("_GetApplicationTextEncoding")) RET(0);
    if (IS("_GetCurrentEventTime")) RET(pg_fp_double(CFAbsoluteTimeGetCurrent()));
    if (IS("_UpTime")) RET(mach_absolute_time());
    if (IS("_Microseconds")) { UnsignedWide t; Microseconds(&t); memcpy(P(0), &t, 8); RET(0); }
    if (IS("_NewAEEventHandlerUPP") || IS("_NewEventHandlerUPP") || IS("_NewEventLoopTimerUPP")) RET(a[0]);
    if (IS("_AEInstallEventHandler") || IS("_AEProcessAppleEvent")) RET(0);
    if (IS("_CreateNibReference")) { OUT(1, 1); RET(0); }
    if (IS("_GetMainEventLoop") || IS("_GetApplicationEventTarget") || IS("_GetEventDispatcherTarget")) RET(1);
    return 0;
#undef RET
#undef OUT
#undef O
#undef P
#undef IS
}
#pragma clang diagnostic pop
