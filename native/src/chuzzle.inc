/*
 * Chuzzle Deluxe is not a "Sexy" engine title: Raptisoft built it on SDL 1.2,
 * with SDL_image for its GIF/JPEG textures and BASS (the 2.3-era API) for
 * audio.  Both SDL frameworks ship inside the app as i386 dylibs the loader
 * cannot use, so what follows is SDL itself: the video surface, the event
 * pump, threads, mutexes and image loading, expressed with the same Cocoa
 * window and NSOpenGLContext that peggle_window.inc already manages.
 *
 * Structure layouts below are SDL 1.2's on i386 and are what the game reads
 * and writes directly, so they are fixed by the binary, not by us.
 */

enum {
    /* SDL_Surface */
    kSdlSurfaceFlags = 0, kSdlSurfaceFormat = 4, kSdlSurfaceW = 8,
    kSdlSurfaceH = 12, kSdlSurfacePitch = 16, kSdlSurfacePixels = 20,
    kSdlSurfaceOffset = 24, kSdlSurfaceClipRect = 32, kSdlSurfaceRefcount = 56,
    kSdlSurfaceSize = 60,
    /* SDL_PixelFormat */
    kSdlFormatPalette = 0, kSdlFormatBits = 4, kSdlFormatBytes = 5,
    kSdlFormatLoss = 6, kSdlFormatShift = 10, kSdlFormatRmask = 16,
    kSdlFormatColorkey = 32, kSdlFormatAlpha = 36, kSdlFormatSize = 40,
    /* SDL_Event is a 20-byte union; only the members the game reads matter. */
    kSdlEventSize = 20,
    /* surface flags */
    kSdlSwSurface = 0x00000000, kSdlHwSurface = 0x00000001,
    kSdlOpenGL = 0x00000002, kSdlSrcColorKey = 0x00001000,
    kSdlSrcAlpha = 0x00010000, kSdlPrealloc = 0x01000000,
    kSdlAnyFormat = 0x10000000, kSdlDoubleBuf = 0x40000000,
    kSdlFullScreen = 0x80000000,
    /* event types */
    kSdlNoEvent = 0, kSdlActiveEvent = 1, kSdlKeyDown = 2, kSdlKeyUp = 3,
    kSdlMouseMotion = 4, kSdlMouseButtonDown = 5, kSdlMouseButtonUp = 6,
    kSdlQuit = 12, kSdlVideoResize = 16, kSdlVideoExpose = 17,
    /* app state */
    kSdlAppMouseFocus = 1, kSdlAppInputFocus = 2, kSdlAppActive = 4,
    /* key count: SDLK_LAST */
    kSdlKeyCount = 323,
};

/* --- guest memory helpers ---------------------------------------------- */

static uint32_t cz_read32(uint32_t address)
{
    uint32_t value = 0;
    if (address >= 0x1000) memcpy(&value, (const void *)(uintptr_t)address, 4);
    return value;
}

static void cz_write32(uint32_t address, uint32_t value)
{
    if (address >= 0x1000) memcpy((void *)(uintptr_t)address, &value, 4);
}

static void cz_write16(uint32_t address, uint16_t value)
{
    if (address >= 0x1000) memcpy((void *)(uintptr_t)address, &value, 2);
}

static void cz_write8(uint32_t address, uint8_t value)
{
    if (address >= 0x1000) *(uint8_t *)(uintptr_t)address = value;
}

/* --- surfaces ----------------------------------------------------------- */

/* Loss and shift for one channel mask, as SDL derives them. */
static void cz_mask_fields(uint32_t mask, uint8_t *loss, uint8_t *shift)
{
    uint8_t s = 0, bits = 0;
    if (mask) {
        while (!((mask >> s) & 1)) ++s;
        for (uint32_t m = mask >> s; m & 1; m >>= 1) ++bits;
    }
    *shift = s;
    *loss = (uint8_t)(8 - bits);
}

static uint32_t cz_create_surface(uint32_t width, uint32_t height, uint32_t bits,
                                  uint32_t rmask, uint32_t gmask, uint32_t bmask,
                                  uint32_t amask, uint32_t pixels, uint32_t flags)
{
    uint32_t bytes = (bits + 7) / 8;
    uint32_t pitch = width * bytes;
    uint32_t surface = compat_runtime32_allocate(kSdlSurfaceSize, 1);
    uint32_t format = compat_runtime32_allocate(kSdlFormatSize, 1);
    if (!surface || !format) return 0;
    /* An OpenGL video surface has no pixels of its own, exactly as SDL
       returns it; every other surface either owns or borrows a buffer. */
    if (!pixels && width && height && !(flags & kSdlOpenGL)) {
        pixels = compat_runtime32_allocate(pitch * height, 1);
        if (!pixels) return 0;
    } else if (pixels) {
        flags |= kSdlPrealloc;
    }
    cz_write32(surface + kSdlSurfaceFlags, flags);
    cz_write32(surface + kSdlSurfaceFormat, format);
    cz_write32(surface + kSdlSurfaceW, width);
    cz_write32(surface + kSdlSurfaceH, height);
    cz_write16(surface + kSdlSurfacePitch, (uint16_t)pitch);
    cz_write32(surface + kSdlSurfacePixels, pixels);
    cz_write32(surface + kSdlSurfaceRefcount, 1);
    /* clip_rect covers the whole surface */
    cz_write16(surface + kSdlSurfaceClipRect + 4, (uint16_t)width);
    cz_write16(surface + kSdlSurfaceClipRect + 6, (uint16_t)height);
    cz_write8(format + kSdlFormatBits, (uint8_t)bits);
    cz_write8(format + kSdlFormatBytes, (uint8_t)bytes);
    const uint32_t masks[4] = {rmask, gmask, bmask, amask};
    for (int i = 0; i < 4; ++i) {
        uint8_t loss, shift;
        cz_mask_fields(masks[i], &loss, &shift);
        cz_write8(format + kSdlFormatLoss + i, loss);
        cz_write8(format + kSdlFormatShift + i, shift);
        cz_write32(format + kSdlFormatRmask + i * 4, masks[i]);
    }
    cz_write8(format + kSdlFormatAlpha, 255);
    return surface;
}

static void cz_free_surface(uint32_t surface)
{
    if (surface < 0x1000) return;
    uint32_t flags = cz_read32(surface + kSdlSurfaceFlags);
    uint32_t pixels = cz_read32(surface + kSdlSurfacePixels);
    uint32_t format = cz_read32(surface + kSdlSurfaceFormat);
    if (pixels && !(flags & kSdlPrealloc)) compat_runtime32_deallocate(pixels);
    if (format) compat_runtime32_deallocate(format);
    compat_runtime32_deallocate(surface);
}

/* Reads one pixel as 8-bit RGBA through a surface's masks. */
static void cz_unpack_pixel(uint32_t format, uint32_t pixel, uint8_t out[4])
{
    for (int i = 0; i < 4; ++i) {
        uint32_t mask = cz_read32(format + kSdlFormatRmask + i * 4);
        uint8_t loss = *(const uint8_t *)(uintptr_t)(format + kSdlFormatLoss + i);
        uint8_t shift = *(const uint8_t *)(uintptr_t)(format + kSdlFormatShift + i);
        if (!mask) { out[i] = i == 3 ? 255 : 0; continue; }
        /* Widen the channel back to 8 bits the way SDL does, replicating the
           high bits into the ones the format does not carry. */
        uint32_t value = ((pixel & mask) >> shift) << loss;
        if (loss && loss < 8) value |= value >> (8 - loss);
        out[i] = (uint8_t)value;
    }
}

/*
 * SDL_UpperBlit for the shapes this game produces: any source format into a
 * 32-bit destination, whole-surface or rectangle, no colour key.  The game
 * uses it once, to widen a sub-32-bit image to RGBA8888.
 */
static uint32_t cz_blit(uint32_t source, uint32_t source_rect,
                        uint32_t destination, uint32_t destination_rect)
{
    if (source < 0x1000 || destination < 0x1000) return (uint32_t)-1;
    uint32_t source_format = cz_read32(source + kSdlSurfaceFormat);
    uint32_t destination_format = cz_read32(destination + kSdlSurfaceFormat);
    uint32_t source_pixels = cz_read32(source + kSdlSurfacePixels);
    uint32_t destination_pixels = cz_read32(destination + kSdlSurfacePixels);
    if (!source_pixels || !destination_pixels) return (uint32_t)-1;
    uint32_t source_bytes = *(const uint8_t *)(uintptr_t)(source_format + kSdlFormatBytes);
    uint32_t destination_bytes = *(const uint8_t *)(uintptr_t)(destination_format + kSdlFormatBytes);
    if (destination_bytes != 4) return (uint32_t)-1;
    int32_t sx = 0, sy = 0, dx = 0, dy = 0;
    int32_t width = (int32_t)cz_read32(source + kSdlSurfaceW);
    int32_t height = (int32_t)cz_read32(source + kSdlSurfaceH);
    if (source_rect >= 0x1000) {
        const int16_t *rect = (const void *)(uintptr_t)source_rect;
        sx = rect[0]; sy = rect[1];
        width = (uint16_t)rect[2]; height = (uint16_t)rect[3];
    }
    if (destination_rect >= 0x1000) {
        const int16_t *rect = (const void *)(uintptr_t)destination_rect;
        dx = rect[0]; dy = rect[1];
    }
    int32_t destination_width = (int32_t)cz_read32(destination + kSdlSurfaceW);
    int32_t destination_height = (int32_t)cz_read32(destination + kSdlSurfaceH);
    if (dx + width > destination_width) width = destination_width - dx;
    if (dy + height > destination_height) height = destination_height - dy;
    if (width <= 0 || height <= 0) return 0;
    uint16_t source_pitch, destination_pitch;
    memcpy(&source_pitch, (const void *)(uintptr_t)(source + kSdlSurfacePitch), 2);
    memcpy(&destination_pitch, (const void *)(uintptr_t)(destination + kSdlSurfacePitch), 2);
    for (int32_t row = 0; row < height; ++row) {
        const uint8_t *in = (const void *)(uintptr_t)
            (source_pixels + (uint32_t)(sy + row) * source_pitch + (uint32_t)sx * source_bytes);
        uint8_t *out = (void *)(uintptr_t)
            (destination_pixels + (uint32_t)(dy + row) * destination_pitch + (uint32_t)dx * 4);
        for (int32_t column = 0; column < width; ++column) {
            uint32_t pixel = 0;
            memcpy(&pixel, in, source_bytes < 4 ? source_bytes : 4);
            uint8_t channels[4];
            cz_unpack_pixel(source_format, pixel, channels);
            uint32_t packed = 0;
            for (int i = 0; i < 4; ++i) {
                uint32_t mask = cz_read32(destination_format + kSdlFormatRmask + i * 4);
                uint8_t shift = *(const uint8_t *)(uintptr_t)(destination_format + kSdlFormatShift + i);
                if (mask) packed |= ((uint32_t)channels[i] << shift) & mask;
            }
            memcpy(out, &packed, 4);
            in += source_bytes;
            out += 4;
        }
    }
    return 0;
}

/* --- image loading ------------------------------------------------------ */

/* IMG_Load over AppKit's decoders: every texture this game ships is a GIF or
   a JPEG.  The result is always RGBA8888 with straight (un-premultiplied)
   alpha and a tightly packed pitch, which is what SDL_image would have
   produced and what the game memcpy's straight out of the surface. */
static uint32_t cz_image_load(const char *path)
{
    if (!path) return 0;
    NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    NSBitmapImageRep *rep = data ? [NSBitmapImageRep imageRepWithData:data] : nil;
    CGImageRef image = [rep CGImage];
    if (!image) {
        fprintf(stderr, "compat32: IMG_Load could not decode %s\n", path);
        return 0;
    }
    size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    uint32_t surface = cz_create_surface((uint32_t)width, (uint32_t)height, 32,
                                         0x000000ff, 0x0000ff00, 0x00ff0000,
                                         0xff000000, 0, kSdlSwSurface | kSdlSrcAlpha);
    if (!surface) return 0;
    uint32_t pixels = cz_read32(surface + kSdlSurfacePixels);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        (void *)(uintptr_t)pixels, width, height, 8, width * 4, space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space);
    if (!context) { cz_free_surface(surface); return 0; }
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    /* CoreGraphics can only write premultiplied alpha; SDL_image hands the
       game straight alpha, and the game multiplies by its own _Alpha maps. */
    uint8_t *bytes = (void *)(uintptr_t)pixels;
    for (size_t i = 0; i < width * height; ++i) {
        uint8_t alpha = bytes[i * 4 + 3];
        if (alpha == 0 || alpha == 255) continue;
        for (int channel = 0; channel < 3; ++channel) {
            uint32_t value = bytes[i * 4 + channel] * 255u / alpha;
            bytes[i * 4 + channel] = (uint8_t)(value > 255 ? 255 : value);
        }
    }
    return surface;
}

/* --- events ------------------------------------------------------------- */

struct cz_event { uint8_t bytes[kSdlEventSize]; };
static struct cz_event cz_events[64];
static unsigned cz_event_head, cz_event_tail;
static uint32_t cz_key_state;      /* guest SDLK_LAST byte array */
static uint32_t cz_mouse_x, cz_mouse_y, cz_mouse_buttons;
static bool cz_has_focus = true;

static void cz_push_event(const struct cz_event *event)
{
    unsigned next = (cz_event_tail + 1) % 64;
    if (next == cz_event_head) return; /* queue full: drop, as SDL would */
    cz_events[cz_event_tail] = *event;
    cz_event_tail = next;
}

/* macOS virtual key code -> SDLKey for the keys that carry no character. */
static uint32_t cz_key_for_code(uint16_t code)
{
    switch (code) {
    case 36: return 13;   /* return */
    case 76: return 271;  /* keypad enter */
    case 48: return 9;    /* tab */
    case 49: return 32;   /* space */
    case 51: return 8;    /* backspace */
    case 53: return 27;   /* escape */
    case 123: return 276; /* left */
    case 124: return 275; /* right */
    case 125: return 274; /* down */
    case 126: return 273; /* up */
    case 116: return 280; /* page up */
    case 121: return 281; /* page down */
    case 115: return 278; /* home */
    case 119: return 279; /* end */
    case 117: return 127; /* delete */
    case 56: return 304;  /* left shift */
    case 60: return 303;  /* right shift */
    case 59: return 306;  /* left control */
    case 62: return 305;  /* right control */
    case 58: return 308;  /* left alt */
    case 61: return 307;  /* right alt */
    case 55: return 310;  /* left meta */
    case 54: return 309;  /* right meta */
    case 57: return 301;  /* caps lock */
    default: break;
    }
    static const uint16_t function_codes[] = {122, 120, 99, 118, 96, 97, 98, 100,
                                              101, 109, 103, 111};
    for (unsigned i = 0; i < sizeof(function_codes) / sizeof(function_codes[0]); ++i) {
        if (function_codes[i] == code) return 282 + i;
    }
    return 0;
}

static uint32_t cz_sdl_modifiers(NSEventModifierFlags flags)
{
    uint32_t modifiers = 0;
    if (flags & NSEventModifierFlagShift) modifiers |= 0x0003;
    if (flags & NSEventModifierFlagControl) modifiers |= 0x00c0;
    if (flags & NSEventModifierFlagOption) modifiers |= 0x0300;
    if (flags & NSEventModifierFlagCommand) modifiers |= 0x0c00;
    if (flags & NSEventModifierFlagCapsLock) modifiers |= 0x2000;
    return modifiers;
}

static void cz_set_key_state(uint32_t key, bool down)
{
    if (cz_key_state && key < kSdlKeyCount) {
        *(uint8_t *)(uintptr_t)(cz_key_state + key) = down ? 1 : 0;
    }
}

static void cz_queue_key(NSEvent *native, bool down)
{
    NSString *characters = [native charactersIgnoringModifiers];
    uint32_t key = cz_key_for_code([native keyCode]);
    uint16_t unicode = 0;
    if (!key && [characters length]) {
        unichar first = [characters characterAtIndex:0];
        if (first < 0x80) key = (uint32_t)tolower((int)first);
    }
    NSString *typed = [native characters];
    if (down && [typed length]) {
        unichar first = [typed characterAtIndex:0];
        if (first < 0xF700) unicode = first;
    }
    if (!key) return;
    cz_set_key_state(key, down);
    struct cz_event event = {{0}};
    event.bytes[0] = down ? kSdlKeyDown : kSdlKeyUp;
    event.bytes[2] = down ? 1 : 0;               /* state */
    event.bytes[4] = (uint8_t)[native keyCode];  /* keysym.scancode */
    uint32_t modifiers = cz_sdl_modifiers([native modifierFlags]);
    memcpy(event.bytes + 8, &key, 4);
    memcpy(event.bytes + 12, &modifiers, 4);
    memcpy(event.bytes + 16, &unicode, 2);
    cz_push_event(&event);
}

/* The modifier keys arrive as one flags-changed event; SDL reports them as
   ordinary key presses, which is how the game sees Shift. */
static void cz_queue_modifiers(NSEvent *native)
{
    static NSEventModifierFlags previous;
    NSEventModifierFlags flags = [native modifierFlags];
    static const struct { NSEventModifierFlags flag; uint32_t key; } pairs[] = {
        {NSEventModifierFlagShift, 304}, {NSEventModifierFlagControl, 306},
        {NSEventModifierFlagOption, 308}, {NSEventModifierFlagCommand, 310},
        {NSEventModifierFlagCapsLock, 301},
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
        bool now = (flags & pairs[i].flag) != 0;
        if (now == ((previous & pairs[i].flag) != 0)) continue;
        cz_set_key_state(pairs[i].key, now);
        struct cz_event event = {{0}};
        event.bytes[0] = now ? kSdlKeyDown : kSdlKeyUp;
        event.bytes[2] = now ? 1 : 0;
        memcpy(event.bytes + 8, &pairs[i].key, 4);
        uint32_t modifiers = cz_sdl_modifiers(flags);
        memcpy(event.bytes + 12, &modifiers, 4);
        cz_push_event(&event);
    }
    previous = flags;
}

static void cz_update_mouse(NSPoint local)
{
    int32_t x = (int32_t)local.x, y = (int32_t)local.y;
    cz_mouse_x = (uint32_t)(x < 0 ? 0 : x);
    cz_mouse_y = (uint32_t)(y < 0 ? 0 : y);
}

static void cz_queue_mouse(NSEvent *native, uint8_t type, uint8_t button)
{
    struct pg_win *window = pg_main_win();
    NSPoint local = window ? pg_logical_mouse_point(window, pg_event_screen_point(native))
                           : NSMakePoint(0, 0);
    uint32_t previous_x = cz_mouse_x, previous_y = cz_mouse_y;
    cz_update_mouse(local);
    struct cz_event event = {{0}};
    event.bytes[0] = type;
    uint16_t x = (uint16_t)cz_mouse_x, y = (uint16_t)cz_mouse_y;
    if (type == kSdlMouseMotion) {
        event.bytes[2] = (uint8_t)cz_mouse_buttons;
        memcpy(event.bytes + 4, &x, 2);
        memcpy(event.bytes + 6, &y, 2);
        int16_t dx = (int16_t)((int32_t)cz_mouse_x - (int32_t)previous_x);
        int16_t dy = (int16_t)((int32_t)cz_mouse_y - (int32_t)previous_y);
        memcpy(event.bytes + 8, &dx, 2);
        memcpy(event.bytes + 10, &dy, 2);
    } else {
        event.bytes[2] = button;
        event.bytes[3] = type == kSdlMouseButtonDown ? 1 : 0;
        memcpy(event.bytes + 4, &x, 2);
        memcpy(event.bytes + 6, &y, 2);
        uint32_t bit = 1u << (button - 1);
        if (type == kSdlMouseButtonDown) cz_mouse_buttons |= bit;
        else cz_mouse_buttons &= ~bit;
    }
    cz_push_event(&event);
}

static void cz_queue_simple(uint8_t type)
{
    struct cz_event event = {{0}};
    event.bytes[0] = type;
    cz_push_event(&event);
}

/* Drains the Cocoa queue into SDL events.  Called from SDL_PollEvent, which
   is the game's only pump. */
static void cz_pump_native_events(void)
{
    struct pg_win *window = pg_main_win();
    /* The window delegate reports a close request as a Carbon window event;
       for an SDL game that is SDL_QUIT, which lets it save and shut down. */
    for (unsigned i = 0; i < pg_pending_count; ++i) {
        if (pg_pending[i].cls == kEventClassWindow &&
            pg_pending[i].kind == kEventWindowClose) {
            cz_queue_simple(kSdlQuit);
        }
        [pg_pending[i].native release];
    }
    pg_pending_count = 0;
    if (window) {
        bool focus = [window->window isKeyWindow];
        if (focus != cz_has_focus) {
            cz_has_focus = focus;
            struct cz_event event = {{0}};
            event.bytes[0] = kSdlActiveEvent;
            event.bytes[1] = focus ? 1 : 0;                       /* gain */
            event.bytes[2] = kSdlAppInputFocus | kSdlAppActive;   /* state */
            cz_push_event(&event);
        }
    }
    for (unsigned drained = 0; drained < 32; ++drained) {
        NSEvent *native = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
        if (!native) break;
        switch ([native type]) {
        case NSEventTypeLeftMouseDown: cz_queue_mouse(native, kSdlMouseButtonDown, 1); break;
        case NSEventTypeLeftMouseUp: cz_queue_mouse(native, kSdlMouseButtonUp, 1); break;
        case NSEventTypeRightMouseDown: cz_queue_mouse(native, kSdlMouseButtonDown, 3); break;
        case NSEventTypeRightMouseUp: cz_queue_mouse(native, kSdlMouseButtonUp, 3); break;
        case NSEventTypeOtherMouseDown: cz_queue_mouse(native, kSdlMouseButtonDown, 2); break;
        case NSEventTypeOtherMouseUp: cz_queue_mouse(native, kSdlMouseButtonUp, 2); break;
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged: cz_queue_mouse(native, kSdlMouseMotion, 0); break;
        case NSEventTypeScrollWheel: {
            CGFloat delta = [native scrollingDeltaY];
            if (delta != 0) {
                uint8_t button = delta > 0 ? 4 : 5;
                cz_queue_mouse(native, kSdlMouseButtonDown, button);
                cz_queue_mouse(native, kSdlMouseButtonUp, button);
            }
            break;
        }
        case NSEventTypeKeyDown: cz_queue_key(native, true); break;
        case NSEventTypeKeyUp: cz_queue_key(native, false); break;
        case NSEventTypeFlagsChanged: cz_queue_modifiers(native); break;
        default: break;
        }
        [NSApp sendEvent:native];
    }
    [NSApp updateWindows];
}

/* --- threads and mutexes ------------------------------------------------ */

enum { kCzThreadCapacity = 32, kCzMutexCapacity = 64,
       kCzThreadBase = 0x7f0d0000, kCzMutexBase = 0x7f0e0000 };
static struct { pthread_t thread; bool live; } cz_threads[kCzThreadCapacity];
static pthread_mutex_t cz_mutexes[kCzMutexCapacity];
static bool cz_mutex_live[kCzMutexCapacity];
static pthread_mutex_t cz_table_lock = PTHREAD_MUTEX_INITIALIZER;

struct cz_thread_start { uint32_t function, argument; };

static void *cz_run_thread(void *opaque)
{
    struct cz_thread_start start = *(struct cz_thread_start *)opaque;
    free(opaque);
    uint32_t argument = start.argument;
    return (void *)(uintptr_t)compat_runtime32_call(start.function, &argument, 1);
}

/* --- cursors ------------------------------------------------------------ */

enum { kCzCursorCapacity = 16, kCzCursorBase = 0x7f0f0000 };
static NSCursor *cz_cursors[kCzCursorCapacity];

/* SDL cursors are 1-bit data/mask pairs, MSB first, in rows of (w+7)/8 bytes:
   data 1 + mask 1 = black, data 0 + mask 1 = white, mask 0 = transparent. */
static uint32_t cz_create_cursor(const uint8_t *data, const uint8_t *mask,
                                 uint32_t width, uint32_t height,
                                 int32_t hot_x, int32_t hot_y)
{
    unsigned slot = 0;
    while (slot < kCzCursorCapacity && cz_cursors[slot]) ++slot;
    if (slot == kCzCursorCapacity || !data || !mask || !width || !height) return 0;
    NSBitmapImageRep *rep = [[[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
        colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width * 4
        bitsPerPixel:32] autorelease];
    uint8_t *pixels = [rep bitmapData];
    uint32_t stride = (width + 7) / 8;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t bit = (uint8_t)(0x80 >> (x & 7));
            bool set = (data[y * stride + x / 8] & bit) != 0;
            bool opaque = (mask[y * stride + x / 8] & bit) != 0;
            uint8_t *pixel = pixels + (y * width + x) * 4;
            uint8_t level = set ? 0 : 255;
            pixel[0] = pixel[1] = pixel[2] = opaque ? level : 0;
            pixel[3] = opaque ? 255 : 0;
        }
    }
    NSImage *cursor_image = [[[NSImage alloc] initWithSize:NSMakeSize(width, height)] autorelease];
    [cursor_image addRepresentation:rep];
    cz_cursors[slot] = [[NSCursor alloc] initWithImage:cursor_image
                                              hotSpot:NSMakePoint(hot_x, hot_y)];
    return kCzCursorBase + slot;
}

/* --- video -------------------------------------------------------------- */

static NSSize cz_video_size;
static uint32_t cz_screen_surface;
static uint32_t cz_video_info;
static uint32_t cz_mode_list;
static uint64_t cz_start_ticks;

static uint32_t cz_ticks(void)
{
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    uint64_t milliseconds =
        mach_absolute_time() * timebase.numer / timebase.denom / 1000000ull;
    if (!cz_start_ticks) cz_start_ticks = milliseconds;
    return (uint32_t)(milliseconds - cz_start_ticks);
}

static uint32_t cz_set_video_mode(uint32_t width, uint32_t height,
                                  uint32_t bits, uint32_t flags)
{
    if (!width || !height) return 0;
    struct pg_win *window = pg_main_win();
    NSRect content = NSMakeRect(0, 0, width, height);
    if (!window) {
        pg_create_window(content, false);
        window = pg_main_win();
        if (!window) return 0;
        /* pg_create_window is shaped for the 4:3 Sexy titles; hold this one to
           the size SDL was asked for instead. */
        [window->window setContentAspectRatio:NSMakeSize(width, height)];
        [window->window setMinSize:NSMakeSize(width, height)];
        [window->window setTitle:@"Chuzzle"];
    }
    if (!NSEqualSizes(cz_video_size, content.size)) {
        window->logical_content_size = content.size;
        pg_set_bounds(window, content, true);
        cz_video_size = content.size;
    }
    [window->window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    pg_attach_gl();
    if (cz_screen_surface) cz_free_surface(cz_screen_surface);
    /* An OpenGL video surface carries no pixels, exactly as SDL returns it. */
    cz_screen_surface = cz_create_surface(width, height, bits ? bits : 32,
                                          0x00ff0000, 0x0000ff00, 0x000000ff, 0,
                                          0, flags | kSdlOpenGL);
    return cz_screen_surface;
}

/* --- dispatch ------------------------------------------------------------ */

static int chuzzle_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define OUT(i, v) do { if (a[i]) *(uint32_t *)P(i) = (uint32_t)(v); } while (0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    if (lp32_profile()->title != LP32_TITLE_CHUZZLE) return 0;

    /* --- SDL: startup and time --- */
    if (IS("_SDL_Init") || IS("_SDL_InitSubSystem")) { cz_ticks(); pg_start_app(); RET(0); }
    if (IS("_SDL_QuitSubSystem") || IS("_SDL_Quit")) RET(0);
    if (IS("_SDL_GetError")) {
        static uint32_t text;
        if (!text) text = compat_runtime32_copy_cstring("");
        RET(text);
    }
    if (IS("_SDL_GetTicks")) RET(cz_ticks());
    if (IS("_SDL_Delay")) { if (a[0]) usleep(a[0] * 1000u); RET(0); }
    if (IS("_SDL_NumJoysticks")) RET(0);

    /* --- SDL: video --- */
    if (IS("_SDL_SetVideoMode")) RET(cz_set_video_mode(a[0], a[1], a[2], a[3]));
    if (IS("_SDL_GL_SwapBuffers")) { pg_present(); RET(0); }
    if (IS("_SDL_GetVideoInfo")) {
        if (!cz_video_info) {
            cz_video_info = compat_runtime32_allocate(32, 1);
            uint32_t format = compat_runtime32_allocate(kSdlFormatSize, 1);
            if (!cz_video_info) RET(0);
            cz_write32(cz_video_info + 0, 0x00000003); /* hw_available, wm_available */
            cz_write32(cz_video_info + 8, format);
            if (format) {
                cz_write8(format + kSdlFormatBits, 32);
                cz_write8(format + kSdlFormatBytes, 4);
            }
        }
        NSSize screen = [[NSScreen screens][0] frame].size;
        cz_write32(cz_video_info + 12, (uint32_t)screen.width);
        cz_write32(cz_video_info + 16, (uint32_t)screen.height);
        RET(cz_video_info);
    }
    if (IS("_SDL_ListModes")) {
        /* A NULL-terminated array of SDL_Rect pointers, largest first, as SDL
           reports the fullscreen modes of the current display. */
        if (!cz_mode_list) {
            static const uint16_t sizes[][2] = {
                {1600, 1200}, {1280, 1024}, {1024, 768}, {800, 600}, {640, 480},
            };
            unsigned count = sizeof(sizes) / sizeof(sizes[0]);
            cz_mode_list = compat_runtime32_allocate((count + 1) * 4, 1);
            if (!cz_mode_list) RET(0);
            for (unsigned i = 0; i < count; ++i) {
                uint32_t rect = compat_runtime32_allocate(8, 1);
                if (!rect) break;
                cz_write16(rect + 4, sizes[i][0]);
                cz_write16(rect + 6, sizes[i][1]);
                cz_write32(cz_mode_list + i * 4, rect);
            }
        }
        RET(cz_mode_list);
    }
    if (IS("_SDL_WM_SetCaption")) {
        struct pg_win *window = pg_main_win();
        if (window && a[0] >= 0x1000) {
            NSString *title = [NSString stringWithUTF8String:P(0)];
            if (title) [window->window setTitle:title];
        }
        RET(0);
    }
    if (IS("_SDL_WM_IconifyWindow")) {
        struct pg_win *window = pg_main_win();
        if (window) [window->window miniaturize:nil];
        RET(1);
    }

    /* --- SDL: surfaces --- */
    if (IS("_SDL_CreateRGBSurface")) {
        RET(cz_create_surface(a[1], a[2], a[3], a[4], a[5], a[6], a[7], 0,
                              a[0] | (a[7] ? kSdlSrcAlpha : 0)));
    }
    if (IS("_SDL_CreateRGBSurfaceFrom")) {
        uint32_t surface = cz_create_surface(a[1], a[2], a[3], a[5], a[6], a[7], a[8],
                                             a[0], a[8] ? kSdlSrcAlpha : 0);
        if (surface && a[4]) cz_write16(surface + kSdlSurfacePitch, (uint16_t)a[4]);
        RET(surface);
    }
    if (IS("_SDL_FreeSurface")) { cz_free_surface(a[0]); RET(0); }
    if (IS("_SDL_LockSurface")) RET(0);
    if (IS("_SDL_UnlockSurface")) RET(0);
    if (IS("_SDL_UpperBlit") || IS("_SDL_BlitSurface")) RET(cz_blit(a[0], a[1], a[2], a[3]));
    if (IS("_SDL_GetRGBA")) {
        uint8_t channels[4] = {0, 0, 0, 255};
        if (a[1] >= 0x1000) cz_unpack_pixel(a[1], a[0], channels);
        for (int i = 0; i < 4; ++i) if (a[2 + i]) cz_write8(a[2 + i], channels[i]);
        RET(0);
    }
    if (IS("_IMG_Load")) RET(cz_image_load(a[0] >= 0x1000 ? P(0) : NULL));

    /* --- SDL: input --- */
    if (IS("_SDL_PollEvent")) {
        if (cz_event_head == cz_event_tail) cz_pump_native_events();
        if (cz_event_head == cz_event_tail) RET(0);
        if (a[0] >= 0x1000) memcpy(P(0), cz_events[cz_event_head].bytes, kSdlEventSize);
        cz_event_head = (cz_event_head + 1) % 64;
        RET(1);
    }
    if (IS("_SDL_PushEvent")) {
        if (a[0] >= 0x1000) {
            struct cz_event event;
            memcpy(event.bytes, P(0), kSdlEventSize);
            cz_push_event(&event);
        }
        RET(0);
    }
    if (IS("_SDL_GetKeyState")) {
        if (!cz_key_state) cz_key_state = compat_runtime32_allocate(kSdlKeyCount, 1);
        OUT(0, kSdlKeyCount);
        RET(cz_key_state);
    }
    if (IS("_SDL_GetMouseState")) {
        struct pg_win *window = pg_main_win();
        if (window) cz_update_mouse(pg_logical_mouse_point(window, [NSEvent mouseLocation]));
        OUT(0, cz_mouse_x);
        OUT(1, cz_mouse_y);
        RET(cz_mouse_buttons);
    }
    if (IS("_SDL_GetAppState")) {
        RET(kSdlAppActive | (cz_has_focus ? kSdlAppInputFocus | kSdlAppMouseFocus : 0));
    }
    if (IS("_SDL_WarpMouse")) {
        struct pg_win *window = pg_main_win();
        if (window) {
            NSRect content = pg_content_rect(window);
            NSSize logical = window->logical_content_size;
            double scale_x = logical.width > 0 ? content.size.width / logical.width : 1;
            double scale_y = logical.height > 0 ? content.size.height / logical.height : 1;
            CGWarpMouseCursorPosition(CGPointMake(
                content.origin.x + (uint16_t)a[0] * scale_x,
                pg_screen_top() - NSMaxY(content) + (uint16_t)a[1] * scale_y));
            cz_mouse_x = (uint16_t)a[0];
            cz_mouse_y = (uint16_t)a[1];
        }
        RET(0);
    }
    if (IS("_SDL_CreateCursor")) {
        RET(cz_create_cursor(a[0] >= 0x1000 ? P(0) : NULL, a[1] >= 0x1000 ? P(1) : NULL,
                             a[2], a[3], (int32_t)a[4], (int32_t)a[5]));
    }
    if (IS("_SDL_SetCursor")) {
        uint32_t index = a[0] - kCzCursorBase;
        if (a[0] >= kCzCursorBase && index < kCzCursorCapacity && cz_cursors[index]) {
            [cz_cursors[index] set];
        }
        RET(0);
    }
    if (IS("_SDL_FreeCursor")) {
        uint32_t index = a[0] - kCzCursorBase;
        if (a[0] >= kCzCursorBase && index < kCzCursorCapacity) {
            [cz_cursors[index] release];
            cz_cursors[index] = nil;
        }
        RET(0);
    }

    /* --- SDL: threads and mutexes --- */
    if (IS("_SDL_CreateThread")) {
        pthread_mutex_lock(&cz_table_lock);
        unsigned slot = 0;
        while (slot < kCzThreadCapacity && cz_threads[slot].live) ++slot;
        if (slot < kCzThreadCapacity) cz_threads[slot].live = true;
        pthread_mutex_unlock(&cz_table_lock);
        if (slot == kCzThreadCapacity) RET(0);
        struct cz_thread_start *start = malloc(sizeof(*start));
        *start = (struct cz_thread_start){a[0], a[1]};
        if (pthread_create(&cz_threads[slot].thread, NULL, cz_run_thread, start)) {
            free(start);
            cz_threads[slot].live = false;
            RET(0);
        }
        RET(kCzThreadBase + slot);
    }
    if (IS("_SDL_GetThreadID")) RET(a[0] >= kCzThreadBase ? a[0] - kCzThreadBase + 1 : 0);
    if (IS("_SDL_WaitThread")) {
        uint32_t index = a[0] - kCzThreadBase;
        if (a[0] >= kCzThreadBase && index < kCzThreadCapacity && cz_threads[index].live) {
            void *result = NULL;
            pthread_join(cz_threads[index].thread, &result);
            cz_threads[index].live = false;
            OUT(1, (uint32_t)(uintptr_t)result);
        }
        RET(0);
    }
    if (IS("_SDL_CreateMutex")) {
        pthread_mutex_lock(&cz_table_lock);
        unsigned slot = 0;
        while (slot < kCzMutexCapacity && cz_mutex_live[slot]) ++slot;
        if (slot < kCzMutexCapacity) {
            pthread_mutexattr_t attributes;
            pthread_mutexattr_init(&attributes);
            pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&cz_mutexes[slot], &attributes);
            pthread_mutexattr_destroy(&attributes);
            cz_mutex_live[slot] = true;
        }
        pthread_mutex_unlock(&cz_table_lock);
        RET(slot < kCzMutexCapacity ? kCzMutexBase + slot : 0);
    }
    if (IS("_SDL_DestroyMutex") || IS("_SDL_mutexP") || IS("_SDL_mutexV")) {
        uint32_t index = a[0] - kCzMutexBase;
        if (a[0] < kCzMutexBase || index >= kCzMutexCapacity || !cz_mutex_live[index]) RET(-1);
        if (IS("_SDL_mutexP")) pthread_mutex_lock(&cz_mutexes[index]);
        else if (IS("_SDL_mutexV")) pthread_mutex_unlock(&cz_mutexes[index]);
        else { cz_mutex_live[index] = false; pthread_mutex_destroy(&cz_mutexes[index]); }
        RET(0);
    }

    /* --- the odds and ends SDLMain and the game reach for --- */
    /* The Core Process Services calls SDLMain uses to become a foreground
       app; the bridge's own NSApplication setup already did that. */
    if (IS("_CPSGetCurrentProcess")) { OUT(0, 0); RET(0); }
    if (IS("_CPSEnableForegroundOperation")) { pg_start_app(); RET(0); }
    if (IS("_CPSSetFrontProcess")) { [NSApp activateIgnoringOtherApps:YES]; RET(0); }
    if (IS("_NSAllocateMemoryPages")) RET(compat_runtime32_allocate(a[0], 1));
    if (IS("_NSDeallocateMemoryPages")) { compat_runtime32_deallocate(a[0]); RET(0); }
    if (IS("_GetDateTime")) { OUT(0, (uint32_t)(time(NULL) + 2082844800u)); RET(0); }
    if (IS("_GetMouse")) {
        struct pg_win *window = pg_main_win();
        NSPoint local = window ? pg_logical_mouse_point(window, [NSEvent mouseLocation])
                               : NSMakePoint(0, 0);
        int16_t point[2] = {(int16_t)local.y, (int16_t)local.x};
        if (a[0] >= 0x1000) memcpy(P(0), point, sizeof(point));
        RET(0);
    }
    if (IS("_GetCurrentEventButtonState")) RET(cz_mouse_buttons);
    return 0;
#undef RET
#undef OUT
#undef P
#undef IS
}
