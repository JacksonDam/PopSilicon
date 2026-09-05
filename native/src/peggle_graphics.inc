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
    if (IS("_glColor4ub")) { glColor4ub(a[0], a[1], a[2], a[3]); RET(0); }
    if (IS("_glColor4ubv")) { glColor4ubv(P(0)); RET(0); }
    if (IS("_glVertex2f")) { glVertex2f(pg_float(a[0]), pg_float(a[1])); RET(0); }
    if (IS("_glVertex2i")) { glVertex2i(a[0], a[1]); RET(0); }
    if (IS("_glTexCoord2f")) { glTexCoord2f(pg_float(a[0]), pg_float(a[1])); RET(0); }
    if (IS("_glLineWidth")) { glLineWidth(pg_float(a[0])); RET(0); }
    if (IS("_glPointSize")) { glPointSize(pg_float(a[0])); RET(0); }
    if (IS("_glOrtho")) { glOrtho(pg_double(a), pg_double(a + 2), pg_double(a + 4), pg_double(a + 6), pg_double(a + 8), pg_double(a + 10)); RET(0); }
    if (IS("_glPixelStorei")) { glPixelStorei(a[0], a[1]); RET(0); }
    if (IS("_glShadeModel")) { glShadeModel(a[0]); RET(0); }
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
    if (IS("_CGAssociateMouseAndMouseCursorPosition")) RET(0);
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
