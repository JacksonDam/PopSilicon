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
#include "gli_context.inc"
#include "peggle_window.inc"
#include "peggle_graphics.inc"
#include "classic_toolbox.inc"
#include "chuzzle.inc"

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
    if (!lp32_title_is_peggle_engine(lp32_profile()->title)) return 0;
    if (gli_dispatch(n, a, r) ||
        peggle_window_dispatch(n, a, r) || peggle_graphics_dispatch(n, a, r) ||
        classic_toolbox_dispatch(n, a, r) || chuzzle_dispatch(n, a, r)) return 1;
    if (IS("_CFBundleGetValueForInfoDictionaryKey")) RET(proxy_for_object([O(0) objectForInfoDictionaryKey:O(1)]));
    if (IS("_CFBundleGetVersionNumber")) {
        switch (lp32_profile()->title) {
        case LP32_TITLE_PEGGLE_NIGHTS: RET(0x01048000); /* 1.0.4 final */
        case LP32_TITLE_BEJEWELED2: RET(0x01008000);    /* 1.0.0 final */
        default: RET(0x01058000);                       /* 1.0.5 final */
        }
    }
    if (IS("_CFBundleCopyLocalizedString")) RET(pg_cf_register((CFTypeRef)[[O(0) localizedStringForKey:O(1) value:O(2) table:O(3)] retain]));
    if (IS("_CFStringCreateWithCString")) RET(pg_cf_register(a[1] >= 0x1000 ? CFStringCreateWithCString(NULL, P(1), a[2]) : NULL));
    if (IS("_CFStringCreateWithCharacters")) RET(pg_cf_register(a[1] >= 0x1000 ? CFStringCreateWithCharacters(NULL, P(1), a[2]) : NULL));
    if (IS("_CFStringCreateWithFormat")) RET(pg_cf_register((CFTypeRef)[pg_format(O(2) ?: @"", a + 3) retain]));
    if (IS("_CFStringGetCStringPtr")) RET(pg_cf_cstring(a[0], a[1]));
    if (IS("_CFStringGetCharacters")) { [O(0) getCharacters:P(3) range:NSMakeRange(a[1], a[2])]; RET(0); }
    if (IS("_CFStringGetTypeID")) RET(CFStringGetTypeID());
    if (IS("_CFDataGetTypeID")) RET(CFDataGetTypeID());
    if (IS("_CFNumberGetTypeID")) RET(CFNumberGetTypeID());
    if (IS("_CFBooleanGetTypeID")) RET(CFBooleanGetTypeID());
    if (IS("_CFURLCreateFromFileSystemRepresentation")) RET(pg_cf_register(CFURLCreateFromFileSystemRepresentation(NULL, P(1), a[2], a[3])));
    if (IS("_CFDataCreate")) RET(pg_cf_register(CFDataCreate(NULL, P(1), a[2])));
    if (IS("_CFDataGetLength")) RET([O(0) length]);
    if (IS("_CFDataGetBytes")) { [O(0) getBytes:P(3) range:NSMakeRange(a[1], a[2])]; RET(0); }
    if (IS("_CFBooleanGetValue")) RET([O(0) boolValue]);
    if (IS("_CFNumberCreate")) RET(pg_cf_register(CFNumberCreate(NULL, a[1], P(2))));
    if (IS("_CFPreferencesCopyAppValue")) {
        CFTypeRef value = CFPreferencesCopyAppValue((CFStringRef)O(0),
                                                    CFSTR("local.peggle.compat"));
        if (getenv("LP32_TRACE_PREFS")) {
            fprintf(stderr, "compat32: prefs copyApp %s -> %s\n",
                    [(NSString *)O(0) UTF8String] ?: "(null)",
                    value ? [[(id)value description] UTF8String] : "(absent)");
        }
        RET(pg_cf_register(value));
    }
    if (IS("_CFPreferencesSetAppValue")) {
        if (getenv("LP32_TRACE_PREFS")) {
            fprintf(stderr, "compat32: prefs setApp %s = %s\n",
                    [(NSString *)O(0) UTF8String] ?: "(null)",
                    O(1) ? [[(id)O(1) description] UTF8String] : "(nil)");
        }
        CFPreferencesSetAppValue((CFStringRef)O(0), (CFTypeRef)O(1), CFSTR("local.peggle.compat")); RET(0);
    }
    if (IS("_CFPreferencesAppSynchronize")) RET(CFPreferencesAppSynchronize(CFSTR("local.peggle.compat")));
    /* Bejeweled 3 uses the lower-level CFPreferences API (with explicit
       user/host domains) rather than Peggle's App* helpers.  Pin every call
       to the same private application domain so all three games share one
       isolated preferences store. */
    if (IS("_CFPreferencesCopyKeyList")) RET(pg_cf_register(CFPreferencesCopyKeyList(CFSTR("local.peggle.compat"), kCFPreferencesCurrentUser, kCFPreferencesAnyHost)));
    if (IS("_CFPreferencesCopyValue")) {
        /* LP32_TRACE_PREFS names every key the guest reads.  Zuma's Revenge
           decides its 3-D path from RegReadBool("Is3D") and its art set from
           "HiRes", and neither key exists in this domain -- an absent key reads
           false, which switches off the screen-space shader effects while the
           hi-res art still loads.  These calls carried no tracing at all, so
           "no Is3D lookup in the logs" was a missing instrument, not evidence. */
        CFTypeRef value = CFPreferencesCopyValue((CFStringRef)O(0),
                                                 CFSTR("local.peggle.compat"),
                                                 kCFPreferencesCurrentUser,
                                                 kCFPreferencesAnyHost);
        if (getenv("LP32_TRACE_PREFS")) {
            fprintf(stderr, "compat32: prefs read %s -> %s\n",
                    [(NSString *)O(0) UTF8String] ?: "(null)",
                    value ? [[(id)value description] UTF8String] : "(absent)");
        }
        RET(pg_cf_register(value));
    }
    if (IS("_CFPreferencesSetValue")) {
        if (getenv("LP32_TRACE_PREFS")) {
            fprintf(stderr, "compat32: prefs set %s = %s\n",
                    [(NSString *)O(0) UTF8String] ?: "(null)",
                    O(1) ? [[(id)O(1) description] UTF8String] : "(nil)");
        }
        CFPreferencesSetValue((CFStringRef)O(0), (CFTypeRef)O(1), CFSTR("local.peggle.compat"), kCFPreferencesCurrentUser, kCFPreferencesAnyHost); RET(0);
    }
    if (IS("_CFPreferencesSynchronize")) RET(CFPreferencesSynchronize(CFSTR("local.peggle.compat"), kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
    if (IS("_CFLocaleCopyCurrent")) RET(pg_cf_register(CFLocaleCopyCurrent()));
    if (IS("_CFNumberFormatterCreate")) RET(pg_cf_register(CFNumberFormatterCreate(NULL, (CFLocaleRef)O(1), a[2])));
    if (IS("_CFNumberFormatterCreateStringWithValue")) RET(pg_cf_register(CFNumberFormatterCreateStringWithValue(NULL, (CFNumberFormatterRef)O(1), a[2], P(3))));
    if (IS("_CFURLCreateWithBytes")) RET(pg_cf_register(CFURLCreateWithBytes(NULL, P(1), a[2], a[3], (CFURLRef)O(4))));
    if (IS("_CFURLCopyAbsoluteURL")) RET(pg_cf_register((CFTypeRef)[[O(0) absoluteURL] retain]));
    if (IS("_CFURLCreateCopyDeletingLastPathComponent"))
        RET(pg_cf_register((CFTypeRef)[[O(1) URLByDeletingLastPathComponent] retain]));
    if (IS("_CFBundleGetInfoDictionary")) RET(proxy_for_object([O(0) infoDictionary]));
    if (IS("_CFBundleCopyBundleURL")) RET(pg_cf_register((CFTypeRef)[[O(0) bundleURL] retain]));
    if (IS("_CFStringCreateWithBytes"))
        RET(pg_cf_register(CFStringCreateWithBytes(NULL, P(1), (int32_t)a[2], a[3], a[4] != 0)));
    /* CFRange is two words on i386, so the buffer arguments start at 6. */
    if (IS("_CFStringGetBytes")) {
        CFIndex used = 0;
        CFIndex converted = CFStringGetBytes((CFStringRef)O(0),
            CFRangeMake((int32_t)a[1], (int32_t)a[2]), a[3], (uint8_t)a[4],
            a[5] != 0, a[6] ? P(6) : NULL, (int32_t)a[7], &used);
        if (a[8]) *(int32_t *)P(8) = (int32_t)used;
        RET((uint32_t)converted);
    }
    if (IS("_CFStringGetPascalString"))
        RET(CFStringGetPascalString((CFStringRef)O(0), P(1), (int32_t)a[2], a[3]));
    if (IS("_CopyPascalStringToC")) {
        const uint8_t *source = P(0);
        if (source && a[1]) { memcpy(P(1), source + 1, source[0]); ((char *)P(1))[source[0]] = '\0'; }
        RET(0);
    }
    if (IS("_CFPreferencesGetAppBooleanValue") || IS("_CFPreferencesGetAppIntegerValue")) {
        Boolean exists = false;
        uint32_t value = IS("_CFPreferencesGetAppBooleanValue")
            ? (uint32_t)CFPreferencesGetAppBooleanValue((CFStringRef)pg_cf_object(a[0]), CFSTR("local.peggle.compat"), &exists)
            : (uint32_t)CFPreferencesGetAppIntegerValue((CFStringRef)pg_cf_object(a[0]), CFSTR("local.peggle.compat"), &exists);
        /* Same instrument on the App* helpers: this is the pair that answers a
           RegReadBool, and an absent key returns 0 with exists=false. */
        if (getenv("LP32_TRACE_PREFS")) {
            fprintf(stderr, "compat32: prefs %s %s -> %u (exists=%d)\n",
                    IS("_CFPreferencesGetAppBooleanValue") ? "bool" : "int",
                    [(NSString *)pg_cf_object(a[0]) UTF8String] ?: "(null)",
                    value, (int)exists);
        }
        OUT(2, exists);
        RET(value);
    }
    if (IS("_CFURLGetFSRef")) RET(CFURLGetFSRef((CFURLRef)O(0), P(1)));

    if (IS("_CFStringConvertWindowsCodepageToEncoding"))
        RET(CFStringConvertWindowsCodepageToEncoding(a[0]));
    if (IS("_CFStringConvertEncodingToWindowsCodepage"))
        RET(CFStringConvertEncodingToWindowsCodepage(a[0]));
    if (IS("_CFStringCreateMutable"))
        RET(pg_cf_register(CFStringCreateMutable(NULL, (int32_t)a[1])));
    if (IS("_CFStringCreateMutableCopy"))
        RET(pg_cf_register(CFStringCreateMutableCopy(NULL, (int32_t)a[1],
                                                     (CFStringRef)O(2))));
    if (IS("_CFStringCreateCopy"))
        RET(pg_cf_register(CFStringCreateCopy(NULL, (CFStringRef)O(1))));
    if (IS("_CFStringCreateWithSubstring"))
        RET(pg_cf_register(CFStringCreateWithSubstring(NULL, (CFStringRef)O(1),
                CFRangeMake((int32_t)a[2], (int32_t)a[3]))));
    /* The guest's buffer can be freed or reused while CoreFoundation still
       holds it, so take a copy rather than honouring the no-copy request. */
    if (IS("_CFStringCreateWithCStringNoCopy"))
        RET(pg_cf_register(a[1] >= 0x1000
                ? CFStringCreateWithCString(NULL, P(1), a[2]) : NULL));
    if (IS("_CFStringCreateWithPascalString"))
        RET(pg_cf_register(a[1] >= 0x1000
                ? CFStringCreateWithPascalString(NULL, P(1), a[2]) : NULL));
    if (IS("_CFStringCreateWithFormatAndArguments")) {
        /* An i386 va_list is just a pointer to the argument words, which is
           exactly what pg_format already walks. */
        RET(pg_cf_register((CFTypeRef)[pg_format(O(2) ?: @"",
                (const uint32_t *)(uintptr_t)a[3]) retain]));
    }
    if (IS("_CFStringCreateArrayBySeparatingStrings"))
        RET(pg_cf_register(CFStringCreateArrayBySeparatingStrings(NULL,
                (CFStringRef)O(1), (CFStringRef)O(2))));
    if (IS("_CFStringAppend")) {
        if (O(0) && O(1)) CFStringAppend((CFMutableStringRef)O(0), (CFStringRef)O(1));
        RET(0);
    }
    if (IS("_CFStringAppendCString")) {
        if (O(0) && a[1] >= 0x1000) {
            CFStringAppendCString((CFMutableStringRef)O(0), P(1), a[2]);
        }
        RET(0);
    }
    if (IS("_CFStringReplace")) {
        if (O(0) && O(3)) {
            CFStringReplace((CFMutableStringRef)O(0),
                            CFRangeMake((int32_t)a[1], (int32_t)a[2]),
                            (CFStringRef)O(3));
        }
        RET(0);
    }
    if (IS("_CFStringDelete")) {
        if (O(0)) {
            CFStringDelete((CFMutableStringRef)O(0),
                           CFRangeMake((int32_t)a[1], (int32_t)a[2]));
        }
        RET(0);
    }
    if (IS("_CFStringFindAndReplace"))
        RET(O(0) ? (uint32_t)CFStringFindAndReplace((CFMutableStringRef)O(0),
                (CFStringRef)O(1), (CFStringRef)O(2),
                CFRangeMake((int32_t)a[3], (int32_t)a[4]), a[5]) : 0);
    if (IS("_CFStringFindWithOptions")) {
        CFRange found = CFRangeMake(kCFNotFound, 0);
        Boolean ok = O(0) && O(1) &&
            CFStringFindWithOptions((CFStringRef)O(0), (CFStringRef)O(1),
                    CFRangeMake((int32_t)a[2], (int32_t)a[3]), a[4], &found);
        if (a[5]) {
            int32_t *out = P(5);
            out[0] = (int32_t)found.location;
            out[1] = (int32_t)found.length;
        }
        RET(ok);
    }
    if (IS("_CFStringGetCharacterAtIndex"))
        RET(CFStringGetCharacterAtIndex((CFStringRef)O(0), (int32_t)a[1]));
    if (IS("_CFStringGetIntValue"))
        RET((uint32_t)CFStringGetIntValue((CFStringRef)O(0)));

    if (IS("_CFArrayAppendValue")) {
        id value = O(1);
        if (O(0) && value) [(NSMutableArray *)O(0) addObject:value];
        RET(0);
    }
    if (IS("_CFArrayRemoveAllValues")) {
        if (O(0)) [(NSMutableArray *)O(0) removeAllObjects];
        RET(0);
    }
    if (IS("_CFDictionaryGetKeysAndValues")) {
        NSDictionary *dictionary = O(0);
        uint32_t *keys = a[1] ? P(1) : NULL;
        uint32_t *values = a[2] ? P(2) : NULL;
        NSUInteger index = 0;
        for (id key in dictionary) {
            /* Borrowed, like the real call: interned proxies, not new refs. */
            if (keys) keys[index] = proxy_for_object(key);
            if (values) values[index] = proxy_for_object([dictionary objectForKey:key]);
            ++index;
        }
        RET(0);
    }
    /* Both of these dereference the numbers they are handed, so a lookup the
       guest made and missed has to come back as a result rather than a
       fault. */
    if (IS("_CFNumberCompare")) {
        id left = O(0), right = O(1);
        if (!left || !right) RET(left ? 1 : (right ? (uint32_t)(int32_t)-1 : 0));
        RET((uint32_t)CFNumberCompare((CFNumberRef)left, (CFNumberRef)right, P(2)));
    }
    if (IS("_CFNumberGetType")) RET(O(0) ? (uint32_t)CFNumberGetType((CFNumberRef)O(0)) : 0);

    if (IS("_CFTimeZoneCopyDefault")) RET(pg_cf_register(CFTimeZoneCopyDefault()));
    if (IS("_CFTimeZoneGetSecondsFromGMT")) {
        double at;
        memcpy(&at, a + 1, sizeof(at));
        RET(pg_fp_double(CFTimeZoneGetSecondsFromGMT((CFTimeZoneRef)O(0), at)));
    }
    if (IS("_CFTimeZoneIsDaylightSavingTime")) {
        double at;
        memcpy(&at, a + 1, sizeof(at));
        RET(CFTimeZoneIsDaylightSavingTime((CFTimeZoneRef)O(0), at));
    }
    if (IS("_CFAbsoluteTimeGetDayOfWeek")) {
        double at;
        memcpy(&at, a + 0, sizeof(at));
        RET((uint32_t)CFAbsoluteTimeGetDayOfWeek(at, (CFTimeZoneRef)O(2)));
    }
    /* CFGregorianDate is returned by value: on i386 the caller passes a
       hidden buffer pointer first and the callee pops it.  The struct is
       sixteen bytes -- year, then four single-byte fields, then the seconds
       double at offset eight -- which is how the guest copies it back. */
    if (IS("_CFAbsoluteTimeGetGregorianDate")) {
        double at;
        memcpy(&at, a + 1, sizeof(at));
        CFGregorianDate date = CFAbsoluteTimeGetGregorianDate(at, (CFTimeZoneRef)O(3));
        if (a[0]) {
            uint8_t *out = P(0);
            int32_t year = (int32_t)date.year;
            memcpy(out, &year, 4);
            out[4] = (uint8_t)date.month;
            out[5] = (uint8_t)date.day;
            out[6] = (uint8_t)date.hour;
            out[7] = (uint8_t)date.minute;
            memcpy(out + 8, &date.second, 8);
        }
        compat_runtime32_struct_return();
        RET(a[0]);
    }
    if (IS("_CFGregorianDateGetAbsoluteTime")) {
        /* Passed by value: the sixteen bytes occupy a[0]..a[3]. */
        const uint8_t *in = (const uint8_t *)(a + 0);
        CFGregorianDate date;
        int32_t year = 0;
        memcpy(&year, in, 4);
        date.year = year;
        date.month = in[4];
        date.day = in[5];
        date.hour = in[6];
        date.minute = in[7];
        memcpy(&date.second, in + 8, 8);
        RET(pg_fp_double(CFGregorianDateGetAbsoluteTime(date, (CFTimeZoneRef)O(4))));
    }

    if (IS("_CFURLCreateWithFileSystemPath"))
        RET(pg_cf_register(CFURLCreateWithFileSystemPath(NULL, (CFStringRef)O(1),
                                                         a[2], a[3] != 0)));
    if (IS("_CFURLCreateWithString"))
        RET(pg_cf_register(CFURLCreateWithString(NULL, (CFStringRef)O(1),
                                                 (CFURLRef)O(2))));
    if (IS("_CFURLCreateFromFSRef"))
        RET(pg_cf_register(a[1] ? CFURLCreateFromFSRef(NULL, P(1)) : NULL));
    if (IS("_CFURLCreateCopyAppendingPathComponent"))
        RET(pg_cf_register(CFURLCreateCopyAppendingPathComponent(NULL,
                (CFURLRef)O(1), (CFStringRef)O(2), a[3] != 0)));
    if (IS("_CFURLCopyFileSystemPath"))
        RET(pg_cf_register(CFURLCopyFileSystemPath((CFURLRef)O(0), a[1])));
    if (IS("_CFURLCopyPath"))
        RET(pg_cf_register(CFURLCopyPath((CFURLRef)O(0))));
    if (IS("_CFURLCreateDataAndPropertiesFromResource")) {
        CFDataRef data = NULL;
        CFDictionaryRef properties = NULL;
        SInt32 error = 0;
        Boolean ok = CFURLCreateDataAndPropertiesFromResource(NULL,
                (CFURLRef)O(1), a[2] ? &data : NULL, a[3] ? &properties : NULL,
                (CFArrayRef)O(4), &error);
        /* The last untraced whole-file read.  If GamePreferences.xml is loaded
           at all, it is most likely here -- and the error code says whether the
           file is simply absent. */
        NSString *read_path = O(1) ? [(NSURL *)O(1) path] : nil;
        if (getenv("LP32_TRACE_FILES")) {
            fprintf(stderr, "compat32: CFURLCreateDataAndProperties %s -> ok=%d err=%d bytes=%ld\n",
                    read_path ? [read_path UTF8String] : "(no url)", (int)ok, (int)error,
                    data ? (long)CFDataGetLength(data) : 0L);
        }
        /* The registry coming back in is the signal that the guest's tree is
           populated, and the only place the bridge learns the file's real
           path.  Both matter to the flush: before this, serialising the tree
           writes an empty registry over the player's settings. */
        if (ok && data && read_path &&
            [[read_path lastPathComponent] isEqualToString:@"RegistryData.xml"]) {
            compat_runtime32_note_registry_loaded([read_path fileSystemRepresentation],
                                                  (long)CFDataGetLength(data));
        }
        if (a[2]) *(uint32_t *)P(2) = pg_cf_register(data);
        if (a[3]) *(uint32_t *)P(3) = pg_cf_register(properties);
        if (a[5]) *(int32_t *)P(5) = error;
        RET(ok);
    }

    if (IS("_CFPropertyListCreateFromXMLData")) {
        CFStringRef error = NULL;
        CFPropertyListRef list = CFPropertyListCreateFromXMLData(NULL,
                (CFDataRef)O(1), a[2], a[3] ? &error : NULL);
        /* The settings read path: which keys come back tells us directly
           whether Is3D / HARDWARE_ACCEL_* were ever persisted. */
        if (getenv("LP32_TRACE_FILES")) {
            fprintf(stderr, "compat32: CFPropertyListCreateFromXMLData %ld bytes -> %s%s\n",
                    O(1) ? (long)[(NSData *)O(1) length] : 0L,
                    list ? [[(id)list description] UTF8String] : "NULL",
                    error ? " (parse error)" : "");
        }
        if (a[3]) *(uint32_t *)P(3) = pg_cf_register(error);
        RET(pg_cf_register(list));
    }
    if (IS("_CFPropertyListCreateXMLData"))
        RET(pg_cf_register(CFPropertyListCreateXMLData(NULL, (CFPropertyListRef)O(1))));
    if (IS("_CFPropertyListWriteToStream")) {
        CFStringRef error = NULL;
        CFIndex written = CFPropertyListWriteToStream((CFPropertyListRef)O(0),
                (CFWriteStreamRef)O(1), a[2], a[3] ? &error : NULL);
        /* Zero bytes written means the settings were silently discarded. */
        if (getenv("LP32_TRACE_FILES")) {
            fprintf(stderr, "compat32: CFPropertyListWriteToStream -> %ld bytes%s%s\n",
                    (long)written, error ? " error=" : "",
                    error ? [(NSString *)error UTF8String] : "");
        }
        if (a[3]) *(uint32_t *)P(3) = pg_cf_register(error);
        RET((uint32_t)written);
    }
    if (IS("_CFWriteStreamCreateWithFile")) {
        CFWriteStreamRef stream = CFWriteStreamCreateWithFile(NULL, (CFURLRef)O(1));
        /* The settings save path.  GamePreferences.xml holds Is3D and the
           HARDWARE_ACCEL_* consent flags, and it exists nowhere on disk -- so
           whether this stream is ever created, and whether it opens, decides
           if 3-D acceleration can ever be remembered between launches. */
        if (getenv("LP32_TRACE_FILES")) {
            NSString *path = O(1) ? [(NSURL *)O(1) path] : nil;
            fprintf(stderr, "compat32: CFWriteStreamCreateWithFile %s -> %s\n",
                    path ? [path UTF8String] : "(no url)", stream ? "ok" : "NULL");
        }
        RET(pg_cf_register(stream));
    }
    if (IS("_CFWriteStreamOpen")) {
        Boolean opened = CFWriteStreamOpen((CFWriteStreamRef)O(0));
        if (getenv("LP32_TRACE_FILES")) {
            fprintf(stderr, "compat32: CFWriteStreamOpen -> %d\n", (int)opened);
        }
        RET(opened);
    }
    if (IS("_CFWriteStreamClose")) {
        if (O(0)) CFWriteStreamClose((CFWriteStreamRef)O(0));
        RET(0);
    }

    if (IS("_CFUUIDGetConstantUUIDWithBytes")) {
        /* Sixteen UInt8 parameters, each a word on the i386 stack. */
        RET(proxy_for_object((id)CFUUIDGetConstantUUIDWithBytes(NULL,
                (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4],
                (uint8_t)a[5], (uint8_t)a[6], (uint8_t)a[7], (uint8_t)a[8],
                (uint8_t)a[9], (uint8_t)a[10], (uint8_t)a[11], (uint8_t)a[12],
                (uint8_t)a[13], (uint8_t)a[14], (uint8_t)a[15], (uint8_t)a[16])));
    }
    if (IS("_CFUUIDGetUUIDBytes")) {
        /* Also returned by value: sixteen bytes into the hidden buffer. */
        CFUUIDBytes bytes = CFUUIDGetUUIDBytes((CFUUIDRef)O(1));
        if (a[0]) memcpy(P(0), &bytes, sizeof(bytes));
        compat_runtime32_struct_return();
        RET(a[0]);
    }
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
    /* The only Carbon entry point here that takes a UTF-8 path, and the route a
       Carbon title uses to reach its saved games.  Route it through the same
       LP32_APPLICATION_SUPPORT_DIR rewrite as the libc file calls, or an
       "isolated" test run still lands in the player's real profile. */
    if (IS("_FSPathMakeRef")) {
        char redirected[PATH_MAX];
        const UInt8 *path = (const UInt8 *)compat_runtime32_redirect_path(
            (const char *)P(0), redirected, sizeof(redirected));
        RET((uint32_t)FSPathMakeRef(path, P(1), P(2)));
    }
    if (IS("_FSRefMakePath")) {
        OSStatus status = FSRefMakePath(P(0), P(1), a[2]);
        /* The startup path resolves ~5,000 paths through FSRef and none of it
           was traced, which is why the settings store never appeared in any
           file log. */
        if (getenv("LP32_TRACE_FILES") && status == noErr && a[1]) {
            fprintf(stderr, "compat32: FSRefMakePath -> %s\n", (const char *)P(1));
        }
        RET((uint32_t)status);
    }
    if (IS("_FSFindFolder")) {
        OSStatus status = FSFindFolder(a[0], a[1], a[2], P(3));
        if (getenv("LP32_TRACE_FILES")) {
            fprintf(stderr, "compat32: FSFindFolder domain=%d type=%.4s -> %d\n",
                    (int)a[0], (const char *)&a[1], (int)status);
        }
        RET((uint32_t)status);
    }
    if (IS("_FSMakeFSRefUnicode")) RET((uint32_t)FSMakeFSRefUnicode(P(0), a[1], P(2), a[3], P(4)));
    if (IS("_FSCreateDirectoryUnicode")) RET((uint32_t)FSCreateDirectoryUnicode(P(0), a[1], P(2), a[3], P(4), P(5), NULL, NULL));
    if (IS("_FSCreateFileUnicode")) {
        OSStatus status = FSCreateFileUnicode(P(0), a[1], P(2), a[3], P(4), P(5), NULL);
        /* If the settings file is missing, this is where it would be created;
           a failure here means Is3D/HARDWARE_ACCEL_* can never persist. */
        if (getenv("LP32_TRACE_FILES")) {
            char parent[PATH_MAX] = "(unresolved)";
            if (P(0)) FSRefMakePath(P(0), (UInt8 *)parent, sizeof(parent));
            fprintf(stderr, "compat32: FSCreateFileUnicode in %s -> %d\n",
                    parent, (int)status);
        }
        RET((uint32_t)status);
    }
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
    if (IS("_MPDelayUntil")) {
        /* mach_wait_until returns KERN_ABORTED on any signal, and the guest
           ignores the result.  This is Zuma's frame limiter, so an interrupted
           wait would shorten its frame -- which the sampling profiler, firing
           SIGPROF hundreds of times a second, would do to every frame and so
           change the very timing being measured.  Wait out the full deadline,
           as _usleep already does. */
        uint64_t target;
        memcpy(&target, P(0), 8);
        while (mach_absolute_time() < target && mach_wait_until(target) == KERN_ABORTED) {
        }
        RET(0);
    }
    if (IS("_GetApplicationTextEncoding")) RET(0);
    if (IS("_GetCurrentEventTime")) RET(pg_fp_double(CFAbsoluteTimeGetCurrent()));
    if (IS("_UpTime")) RET(mach_absolute_time());
    if (IS("_Microseconds")) { UnsignedWide t; Microseconds(&t); memcpy(P(0), &t, 8); RET(0); }
    if (IS("_NewAEEventHandlerUPP") || IS("_NewEventHandlerUPP") || IS("_NewEventLoopTimerUPP")) RET(a[0]);
    if (IS("_AEInstallEventHandler") || IS("_AEProcessAppleEvent")) RET(0);
    if (IS("_CreateNibReference")) { OUT(1, 1); RET(0); }
    if (IS("_GetMainEventLoop") || IS("_GetApplicationEventTarget") || IS("_GetEventDispatcherTarget")) RET(1);
    if (IS("_GetMainEventQueue")) RET(1);
    /* The bridge pumps Cocoa events itself, so nothing is ever parked in a
       Carbon queue for the game to find or remove. */
    if (IS("_IsEventInQueue")) RET(0);
    if (IS("_RemoveEventFromQueue")) RET(0);
    if (IS("_EnableSecureEventInput") || IS("_DisableSecureEventInput")) RET(0);
    if (IS("_SetKeyboardFocus")) RET(0);
    if (IS("_GetControlData")) { OUT(5, 0); RET(-30580); } /* errDataNotSupported */
    /* System UI mode (hiding the menu bar for full screen) is the presentation
       policy this bridge decides for itself. */
    if (IS("_SetSystemUIMode") || IS("_GetSystemUIMode")) RET(0);
    /* NSApplicationLoad exists to bring up NSApp for a Carbon app; the bridge
       has already done that. */
    if (IS("_NSApplicationLoad")) { pg_start_app(); RET(1); }
    /* Carbon's standard alert, over NSAlert.  The parameter block is only
       ever the defaults here, so the text and the three buttons are what
       matter. */
    if (IS("_GetStandardAlertDefaultParams")) {
        if (a[0]) memset(P(0), 0, 32);
        RET(0);
    }
    if (IS("_CreateStandardAlert")) {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:pg_cf_object(a[1]) ?: @""];
        [alert setInformativeText:pg_cf_object(a[2]) ?: @""];
        OUT(4, pg_cf_register((CFTypeRef)alert));
        RET(0);
    }
    if (IS("_RunStandardAlert")) {
        NSAlert *alert = (NSAlert *)pg_cf_object(a[0]);
        NSModalResponse response = NSAlertFirstButtonReturn;
        if ([alert isKindOfClass:[NSAlert class]]) {
            pg_start_app();
            response = [alert runModal];
        }
        OUT(2, response == NSAlertSecondButtonReturn ? 2 :
               response == NSAlertThirdButtonReturn ? 3 : 1);
        pg_cf_release(a[0]);
        RET(0);
    }
    /* IOKit handles the bridge never created. */
    if (IS("_IONotificationPortDestroy") || IS("_IOServiceClose")) RET(0);
    return 0;
#undef RET
#undef OUT
#undef O
#undef P
#undef IS
}
#pragma clang diagnostic pop
