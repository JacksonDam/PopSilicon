/* OpenGL entry points the shared bridge lacks, display geometry that follows
   the emulated display mode, and the CoreGraphics image calls Peggle uses for
   its cursor images, dock tile and About box. */
static double pg_double(const uint32_t *a) { double v; memcpy(&v, a, 8); return v; }
static float pg_float(uint32_t a) { float v; memcpy(&v, &a, 4); return v; }
static CGRect pg_cgrect(const uint32_t *a) { return CGRectMake(pg_float(a[0]), pg_float(a[1]), pg_float(a[2]), pg_float(a[3])); }
static int peggle_graphics_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define O(i) object_for_argument(a[i])
#define CG(i) pg_cf_object(a[i])
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    /* These five are on the memoized fast path; call it rather than repeating
       the implementation, so the two can never drift apart. */
    if (IS("_glColor4ub")) RET(fast_glColor4ub(a, 0));
    if (IS("_glColor4ubv")) RET(fast_glColor4ubv(a, 0));
    if (IS("_glVertex2f")) RET(fast_glVertex2f(a, 0));
    if (IS("_glVertex2i")) RET(fast_glVertex2i(a, 0));
    if (IS("_glTexCoord2f")) RET(fast_glTexCoord2f(a, 0));
    if (IS("_glLineWidth")) { glLineWidth(pg_float(a[0])); RET(0); }
    if (IS("_glPointSize")) { glPointSize(pg_float(a[0])); RET(0); }
    if (IS("_glOrtho")) {
        double ortho_left = pg_double(a), ortho_right = pg_double(a + 2);
        double ortho_bottom = pg_double(a + 4), ortho_top = pg_double(a + 6);
        glOrtho(ortho_left, ortho_right, ortho_bottom, ortho_top,
                pg_double(a + 8), pg_double(a + 10));
        RET(0);
    }
    if (IS("_glPixelStorei")) { glPixelStorei(a[0], a[1]); RET(0); }
    if (IS("_glShadeModel")) { glShadeModel(a[0]); RET(0); }
    if (IS("_glTexEnvi")) { glTexEnvi(a[0], a[1], (GLint)a[2]); RET(0); }
    if (IS("_glTexEnvf")) { glTexEnvf(a[0], a[1], pg_float(a[2])); RET(0); }
    if (IS("_glTexEnvfv")) { glTexEnvfv(a[0], a[1], P(2)); RET(0); }
    if (IS("_glReadPixels")) { glReadPixels(a[0], a[1], a[2], a[3], a[4], a[5], P(6)); RET(0); }
    if (IS("_gluErrorString") || IS("_CGLErrorString")) { static uint32_t text; if (!text) text = compat_runtime32_copy_cstring("OpenGL error"); RET(text); }
    if (IS("_CGDisplayBitsPerPixel")) RET(32);
    if (IS("_CGDisplayPixelsWide")) RET(guest_display_mode_active() ? (uint32_t)guest_display_mode.width : (uint32_t)CGDisplayPixelsWide(a[0]));
    if (IS("_CGDisplayPixelsHigh")) RET(guest_display_mode_active() ? (uint32_t)guest_display_mode.height : (uint32_t)CGDisplayPixelsHigh(a[0]));
    if (IS("_CGDisplayBounds")) {
        CGRect b = guest_display_mode_active() ?
            CGRectMake(0, 0, guest_display_mode.width, guest_display_mode.height) : CGDisplayBounds(a[1]);
        float f[4] = {(float)b.origin.x, (float)b.origin.y, (float)b.size.width, (float)b.size.height};
        if (a[0]) memcpy(P(0), f, sizeof(f));
        compat_runtime32_struct_return();
        RET(a[0]);
    }
    if (IS("_CGGetDisplaysWithRect")) RET(CGGetDisplaysWithRect(pg_cgrect(a), a[4], P(5), P(6)));
    /* The bridge presents one fixed display whose mode it controls, so a
       reconfiguration callback would never have anything to report: accept
       the registration and never call back.  Unregistering is the same. */
    if (IS("_CGDisplayRegisterReconfigurationCallback") ||
        IS("_CGDisplayRemoveReconfigurationCallback")) RET(0);
    if (IS("_CGGetActiveDisplayList") || IS("_CGGetOnlineDisplayList")) {
        uint32_t capacity = a[0];
        uint32_t *displays = a[1] ? P(1) : NULL;
        if (displays && capacity) displays[0] = CGMainDisplayID();
        /* The count is reported even when no array was supplied, which is how
           the caller asks how many displays there are: the bridge shows one. */
        if (a[2]) *(uint32_t *)P(2) = 1;
        RET(0);
    }
    if (IS("_CGCursorIsVisible")) RET(CGCursorIsVisible());
    if (IS("_CGRectContainsPoint")) {
        /* CGRect is four floats and CGPoint two, all passed by value. */
        RET(CGRectContainsPoint(pg_cgrect(a),
                                CGPointMake(pg_float(a[4]), pg_float(a[5]))));
    }
    if (IS("_CGContextConcatCTM")) {
        CGContextRef context = (CGContextRef)CG(0);
        CGAffineTransform t = CGAffineTransformMake(
            pg_float(a[1]), pg_float(a[2]), pg_float(a[3]),
            pg_float(a[4]), pg_float(a[5]), pg_float(a[6]));
        if (context) CGContextConcatCTM(context, t);
        RET(0);
    }
    /* Returned by value: the hidden buffer comes first and the callee pops
       it, exactly as for CGDisplayBounds above. */
    if (IS("_CGContextGetCTM")) {
        CGContextRef context = (CGContextRef)CG(1);
        CGAffineTransform t = context ? CGContextGetCTM(context)
                                      : CGAffineTransformIdentity;
        if (a[0]) {
            float out[6] = {(float)t.a, (float)t.b, (float)t.c,
                            (float)t.d, (float)t.tx, (float)t.ty};
            memcpy(P(0), out, sizeof(out));
        }
        compat_runtime32_struct_return();
        RET(a[0]);
    }
    if (IS("_CGContextSetInterpolationQuality")) {
        CGContextRef context = (CGContextRef)CG(0);
        if (context) CGContextSetInterpolationQuality(context, (int32_t)a[1]);
        RET(0);
    }
    if (IS("_CGBitmapContextCreate"))
        RET(pg_cf_register(CGBitmapContextCreate(a[0] ? P(0) : NULL, a[1], a[2],
                a[3], a[4], (CGColorSpaceRef)CG(5), a[6])));
    if (IS("_CGImageGetAlphaInfo")) RET(CGImageGetAlphaInfo((CGImageRef)CG(0)));
    /* GetColorSpace hands back a borrowed reference; retain it so the handle
       the guest keeps stays valid for as long as it holds it. */
    if (IS("_CGImageGetColorSpace")) {
        CGColorSpaceRef space = CGImageGetColorSpace((CGImageRef)CG(0));
        RET(space ? pg_cf_register(CGColorSpaceRetain(space)) : 0);
    }
    if (IS("_CGImageSourceCreateWithURL"))
        RET(pg_cf_register(CGImageSourceCreateWithURL((CFURLRef)CG(0),
                                                      (CFDictionaryRef)CG(1))));
    if (IS("_CGImageSourceCreateWithData"))
        RET(pg_cf_register(CGImageSourceCreateWithData((CFDataRef)CG(0),
                                                       (CFDictionaryRef)CG(1))));
    if (IS("_CGImageSourceCreateImageAtIndex"))
        RET(pg_cf_register(CGImageSourceCreateImageAtIndex(
                (CGImageSourceRef)CG(0), a[1], (CFDictionaryRef)CG(2))));
    /* Games detach the pointer for relative mouse motion and re-attach when
       they are done; ignoring the call left it in whatever state the last warp
       put it, so absolute reads kept returning the warped position. */
    if (IS("_CGAssociateMouseAndMouseCursorPosition")) {
        if (a[0] == 0) {
            static bool refused;
            if (!refused) {
                refused = true;
                fprintf(stderr, "compat32: refusing CGAssociateMouseAndMouseCursor"
                        "Position(false) -- it locks the user's pointer;"
                        " the guest polls absolute position anyway\n");
            }
            RET(0);                       /* kCGErrorSuccess */
        }
        RET((uint32_t)CGAssociateMouseAndMouseCursorPosition(true));
    }
    /* CoreGraphics images: real host objects behind proxy handles.  Pixel
       data providers copy the guest bytes because the game frees them
       before the image is drawn. */
    if (IS("_CGColorSpaceCreateDeviceRGB")) RET(pg_cf_register(CGColorSpaceCreateDeviceRGB()));
    if (IS("_CGColorSpaceRelease") || IS("_CGDataProviderRelease") || IS("_CGImageRelease") || IS("_CGContextRelease")) { pg_cf_release(a[0]); RET(0); }
    if (IS("_CFRelease") && pg_cf_release(a[0])) RET(0);
    if (IS("_CFRetain") && pg_cf_retain(a[0])) RET(a[0]);
    if (IS("_CGDataProviderCreateWithData")) {
        if (a[1] < 0x1000) RET(0);
        CFDataRef data = CFDataCreate(NULL, P(1), a[2]);
        CGDataProviderRef provider = data ? CGDataProviderCreateWithCFData(data) : NULL;
        if (data) CFRelease(data);
        RET(pg_cf_register(provider));
    }
    if (IS("_CGDataProviderCreateWithURL")) RET(pg_cf_register(CGDataProviderCreateWithURL((CFURLRef)O(0))));
    if (IS("_CGImageCreate")) {
        CGImageRef image = CGImageCreate(a[0], a[1], a[2], a[3], a[4], (CGColorSpaceRef)CG(5), a[6],
                                         (CGDataProviderRef)CG(7), NULL, a[9] != 0, a[10]);
        RET(pg_cf_register(image));
    }
    if (IS("_CGImageCreateWithPNGDataProvider")) RET(pg_cf_register(CGImageCreateWithPNGDataProvider((CGDataProviderRef)CG(0), NULL, a[2] != 0, a[3])));
    if (IS("_CGImageGetWidth")) RET(CGImageGetWidth((CGImageRef)CG(0)));
    if (IS("_CGImageGetHeight")) RET(CGImageGetHeight((CGImageRef)CG(0)));
    if (IS("_CGContextClearRect")) { { CGContextRef c = (CGContextRef)CG(0); if (c) CGContextClearRect(c, pg_cgrect(a + 1)); } RET(0); }
    if (IS("_CGContextDrawImage")) { { CGContextRef c = (CGContextRef)CG(0); CGImageRef image = (CGImageRef)CG(5); if (c && image) CGContextDrawImage(c, pg_cgrect(a + 1), image); } RET(0); }
    if (IS("_CGContextScaleCTM")) { { CGContextRef c = (CGContextRef)CG(0); if (c) CGContextScaleCTM(c, pg_float(a[1]), pg_float(a[2])); } RET(0); }
    if (IS("_CGContextFlush")) { { CGContextRef c = (CGContextRef)CG(0); if (c) CGContextFlush(c); } RET(0); }
    return 0;
#undef RET
#undef CG
#undef O
#undef P
#undef IS
}
