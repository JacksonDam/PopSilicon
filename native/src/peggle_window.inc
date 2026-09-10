/* Peggle's Carbon window, event and AGL/CGL surface translated to Cocoa.
   The game window is an NSWindow with one NSOpenGLContext that both the
   AGL (windowed) and CGL (emulated fullscreen) paths share; overlay windows
   (the custom cursor) are borderless image windows that follow the game's
   QuickDraw drawing through a bitmap CGContext. */
@interface PeggleView : NSView
@end
@implementation PeggleView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
- (void)keyDown:(NSEvent *)event { (void)event; } /* the game reads keys itself; no beep */
- (void)keyUp:(NSEvent *)event { (void)event; }
@end

/* The game draws in a 4:3 surface even when AppKit makes its window fill an
   ultrawide display.  Keeping that surface as a child view lets native full
   screen scale it to the largest undistorted size and leaves the remainder
   black, just like a classic display mode. */
@interface PeggleLetterboxView : NSView
@end
@implementation PeggleLetterboxView
- (BOOL)isOpaque { return YES; }
- (void)drawRect:(NSRect)dirty
{
    [[NSColor blackColor] setFill];
    NSRectFill(dirty);
}
- (void)layout
{
    [super layout];
    if (![[self subviews] count]) return;
    NSRect bounds = [self bounds];
    CGFloat scale = fmin(bounds.size.width / 4.0, bounds.size.height / 3.0);
    NSSize size = NSMakeSize(floor(4.0 * scale), floor(3.0 * scale));
    NSRect frame = NSMakeRect(floor((bounds.size.width - size.width) / 2.0),
                              floor((bounds.size.height - size.height) / 2.0),
                              size.width, size.height);
    [[[self subviews] objectAtIndex:0] setFrame:frame];
}
@end

enum {
    kPgWindowCapacity = 8, kPgWindowBase = 0x7f0a0000, kPgWindowStride = 16,
    kPgEventBase = 0x7f090000, kPgEventCapacity = 64, kPgEventStride = 16,
    kPgCfBase = 0x7f0b0000, kPgCfCapacity = 4096, kPgCfStride = 16,
};
/* CoreFoundation objects the game creates (score strings and formatters
   every frame, cursor images, the overlay bitmap contexts) live in this
   reference-counted table rather than the persistent proxy pool, so the
   game's CFRelease actually frees them.  The shared bridge's object lookup
   consults it too (peggle_object_for_handle), so these handles work with
   every CF import upstream implements. */
static struct { CFTypeRef object; uint32_t refs; uint32_t cstring; } pg_cf_table[kPgCfCapacity];
static uint32_t pg_cf_cursor;
static uint32_t pg_cf_register(CFTypeRef object) /* takes over the caller's reference */
{
    if (!object) return 0;
    for (uint32_t step = 0; step < kPgCfCapacity; ++step) {
        uint32_t i = (pg_cf_cursor + step) % kPgCfCapacity;
        if (pg_cf_table[i].object) continue;
        pg_cf_table[i].object = object;
        pg_cf_table[i].refs = 1;
        pg_cf_table[i].cstring = 0;
        pg_cf_cursor = i + 1;
        return kPgCfBase + i * kPgCfStride;
    }
    static bool reported;
    if (!reported) { reported = true; fprintf(stderr, "compat32: CoreFoundation handle table exhausted\n"); }
    CFRelease(object);
    return 0;
}
static uint32_t pg_cf_index(uint32_t handle)
{
    uint32_t index = (handle - kPgCfBase) / kPgCfStride;
    return handle >= kPgCfBase && index < kPgCfCapacity && (handle - kPgCfBase) % kPgCfStride == 0 ? index : UINT32_MAX;
}
static CFTypeRef pg_cf_object(uint32_t handle)
{
    uint32_t index = pg_cf_index(handle);
    if (index != UINT32_MAX) return pg_cf_table[index].object;
    return (CFTypeRef)object_for_argument(handle); /* constants and shared-bridge proxies */
}
static id peggle_object_for_handle(uint32_t handle)
{
    uint32_t index = pg_cf_index(handle);
    return index != UINT32_MAX ? (id)pg_cf_table[index].object : nil;
}
static bool pg_cf_retain(uint32_t handle)
{
    uint32_t index = pg_cf_index(handle);
    if (index == UINT32_MAX || !pg_cf_table[index].object) return false;
    ++pg_cf_table[index].refs;
    return true;
}
static bool pg_cf_release(uint32_t handle)
{
    uint32_t index = pg_cf_index(handle);
    if (index == UINT32_MAX) return false;
    if (!pg_cf_table[index].object) return true;
    if (--pg_cf_table[index].refs) return true;
    CFRelease(pg_cf_table[index].object);
    if (pg_cf_table[index].cstring) compat_runtime32_deallocate(pg_cf_table[index].cstring);
    memset(&pg_cf_table[index], 0, sizeof(pg_cf_table[index]));
    return true;
}
/* CFStringGetCStringPtr: a C copy that lives as long as the string handle. */
static uint32_t pg_cf_cstring(uint32_t handle, CFStringEncoding encoding)
{
    uint32_t index = pg_cf_index(handle);
    NSString *string = (NSString *)pg_cf_object(handle);
    if (![string isKindOfClass:[NSString class]]) return 0;
    const char *text = [string cStringUsingEncoding:CFStringConvertEncodingToNSStringEncoding(encoding)];
    if (!text) text = [string UTF8String];
    if (index == UINT32_MAX) return compat_runtime32_copy_cstring(text);
    if (pg_cf_table[index].cstring) compat_runtime32_deallocate(pg_cf_table[index].cstring);
    pg_cf_table[index].cstring = compat_runtime32_copy_cstring(text);
    return pg_cf_table[index].cstring;
}
struct pg_win {
    uint32_t handle;
    NSWindow *window;
    NSView *render_view;
    NSImageView *image_view;
    /* The size the Carbon game requested.  The render view may be larger
       while the window is resized or full screen, so input must be projected
       back into this coordinate space. */
    NSSize logical_content_size;
    uint32_t refcon;
    bool overlay;
    CGContextRef context;
    uint32_t context_handle;
};
static struct pg_win pg_wins[kPgWindowCapacity];
static unsigned pg_win_count;
/* The game disposes and recreates its cursor overlay window on every cursor
   change; keep the last one (window, view and bitmap context) for reuse. */
static struct pg_win pg_overlay_cache;
static NSOpenGLContext *pg_gl;
static uint32_t pg_gl_handle;
static uint32_t pg_current_port;
static bool pg_app_started, pg_cursor_hidden, pg_cursor_applied;
static struct { uint32_t cls, kind; NSEvent *native; uint32_t command; } pg_pending[16];
static unsigned pg_pending_count;
static struct { uint32_t target, callback, user, count; EventTypeSpec types[64]; } pg_handlers[64];
static uint32_t pg_handler_count;
static uint32_t pg_timer_callback, pg_timer_user;
static double pg_timer_interval, pg_timer_next;
struct pg_event {
    uint32_t cls, kind;
    NSEvent *native;
    unsigned param_count;
    struct { uint32_t name, type, size; uint8_t bytes[64]; } params[4];
};
static struct pg_event pg_events[kPgEventCapacity];
static unsigned pg_event_next;
/* Set when the title bar's close button produced a kEventWindowClose the game
   did not handle.  The quit that answers it is queued from the event pump
   rather than from inside the guest call that declined the close, so the game
   receives it the way it receives any other event. */
static bool pg_close_unhandled;
static int pg_trace_events = -1;
static bool pg_tracing(void)
{
    if (pg_trace_events < 0) pg_trace_events = getenv("PEGGLE_TRACE_EVENTS") != NULL;
    return pg_trace_events;
}
/* PEGGLE_SCRIPT="12:text:Claude;14:key:36;16:click:400,375": synthesized
   input posted to the Cocoa queue at the given seconds after launch, so the
   whole event path can be exercised without assistive access. */
static const char *pg_script;
static double pg_script_start;
static uint16_t pg_keycode_for(char c)
{
    static const char *const layout = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0 `";
    if (c == '\n') return 36;
    if (c == '\t') return 48;
    if (c == ' ') return 49;
    if (c == 8) return 51;
    if (c == 27) return 53;
    char lower = (char)tolower((unsigned char)c);
    for (unsigned i = 0; layout[i]; ++i) if (layout[i] == lower && layout[i] != ' ') return (uint16_t)i;
    return 0;
}
static void pg_post_key(NSWindow *window, char c, uint16_t keycode)
{
    NSString *text = c ? [NSString stringWithFormat:@"%c", c] : @"";
    NSEventModifierFlags flags = isupper((unsigned char)c) ? NSEventModifierFlagShift : 0;
    for (int up = 0; up < 2; ++up) {
        NSEvent *event = [NSEvent keyEventWithType:up ? NSEventTypeKeyUp : NSEventTypeKeyDown location:NSMakePoint(0, 0)
                                     modifierFlags:flags timestamp:[NSDate timeIntervalSinceReferenceDate]
                                      windowNumber:[window windowNumber] context:nil characters:text
                       charactersIgnoringModifiers:[text lowercaseString] isARepeat:NO keyCode:keycode];
        [NSApp postEvent:event atStart:NO];
    }
}
static void pg_post_click(NSWindow *window, double x, double y, bool press)
{
    NSRect content = [[window contentView] frame];
    NSPoint location = NSMakePoint(x, content.size.height - y);
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    NSEventType types[] = {NSEventTypeMouseMoved, NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp};
    for (unsigned i = 0; i < (press ? 3u : 1u); ++i) {
        NSEvent *event = [NSEvent mouseEventWithType:types[i] location:location modifierFlags:0 timestamp:now
                                        windowNumber:[window windowNumber] context:nil eventNumber:0
                                          clickCount:i ? 1 : 0 pressure:i == 1 ? 1 : 0];
        [NSApp postEvent:event atStart:NO];
    }
}
static void pg_run_script(NSWindow *window)
{
    if (!pg_script) {
        const char *script = getenv("PEGGLE_SCRIPT");
        pg_script = script ? script : "";
        pg_script_start = CFAbsoluteTimeGetCurrent();
    }
    while (*pg_script) {
        char *end = NULL;
        double at = strtod(pg_script, &end);
        if (end == pg_script || *end != ':') { pg_script = ""; return; }
        if (CFAbsoluteTimeGetCurrent() - pg_script_start < at) return;
        const char *action = end + 1;
        const char *colon = strchr(action, ':');
        const char *stop = strchr(action, ';');
        if (!colon || (stop && colon > stop)) { pg_script = ""; return; }
        const char *argument = colon + 1;
        size_t length = stop ? (size_t)(stop - argument) : strlen(argument);
        fprintf(stderr, "compat32: script t=%.0f %.*s\n", at, (int)(stop ? stop - action : (long)strlen(action)), action);
        if (!strncmp(action, "text:", 5)) {
            for (size_t i = 0; i < length; ++i) pg_post_key(window, argument[i], pg_keycode_for(argument[i]));
        } else if (!strncmp(action, "key:", 4)) {
            pg_post_key(window, 0, (uint16_t)strtoul(argument, NULL, 0));
        } else if (!strncmp(action, "click:", 6) || !strncmp(action, "move:", 5)) {
            char *comma = NULL;
            double x = strtod(argument, &comma);
            pg_post_click(window, x, comma && *comma == ',' ? strtod(comma + 1, NULL) : 0, action[0] == 'c');
        }
        pg_script = stop ? stop + 1 : "";
    }
}

static void pg_queue_event(uint32_t cls, uint32_t kind, NSEvent *native)
{
    if (pg_pending_count < 16) {
        pg_pending[pg_pending_count].cls = cls;
        pg_pending[pg_pending_count].kind = kind;
        pg_pending[pg_pending_count].native = [native retain];
        pg_pending[pg_pending_count].command = 0;
        ++pg_pending_count;
    }
}
/* A menu command, as Carbon's own menu bar would deliver it: the command id
   travels in the event's direct object as an HICommand. */
static void pg_queue_command(uint32_t command)
{
    pg_queue_event(kEventClassCommand, kEventCommandProcess, nil);
    if (pg_pending_count) pg_pending[pg_pending_count - 1].command = command;
}
static void pg_post_window_event(uint32_t kind) { pg_queue_event(kEventClassWindow, kind, nil); }
/* Carbon's toolbox turns an unhandled raw key down into a Unicode text-input
   event; the game's character input (name entry, cheats) listens to that. */
static void pg_queue_text_input(NSEvent *event)
{
    NSString *characters = [event characters];
    if (![characters length] || ([event modifierFlags] & NSEventModifierFlagCommand)) return;
    unichar first = [characters characterAtIndex:0];
    if (first >= 0xF700) return; /* function keys carry no text */
    pg_queue_event(kEventClassTextInput, kEventTextInputUnicodeForKeyEvent, event);
}
@interface PeggleWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation PeggleWindowDelegate
- (BOOL)windowShouldClose:(id)sender { (void)sender; pg_post_window_event(kEventWindowClose); return NO; }
- (void)windowDidBecomeKey:(NSNotification *)note { (void)note; pg_post_window_event(kEventWindowActivated); }
- (void)windowDidResignKey:(NSNotification *)note { (void)note; pg_post_window_event(kEventWindowDeactivated); }
- (void)windowDidResize:(NSNotification *)note
{
    (void)note;
    /* The legacy renderer learns about the drawable's size lazily.  Updating
       the context here makes a user resize or native full-screen transition
       take effect immediately, instead of waiting for the next AGL update. */
    if (pg_gl) [pg_gl update];
}
@end

static CGFloat pg_screen_top(void)
{
    NSArray *screens = [NSScreen screens];
    return [screens count] ? NSMaxY([screens[0] frame]) : 0;
}
static NSRect pg_rect_from_qd(const int16_t *q) { return NSMakeRect(q[1], pg_screen_top() - q[2], q[3] - q[1], q[2] - q[0]); }
static void pg_qd_from_rect(NSRect r, int16_t *q)
{
    q[0] = (int16_t)(pg_screen_top() - NSMaxY(r));
    q[1] = (int16_t)r.origin.x;
    q[2] = (int16_t)(pg_screen_top() - r.origin.y);
    q[3] = (int16_t)NSMaxX(r);
}
static NSPoint pg_qd_point(NSPoint screen) { return NSMakePoint(screen.x, pg_screen_top() - screen.y); }
static struct pg_win *pg_main_win(void)
{
    for (unsigned i = 0; i < pg_win_count; ++i) if (pg_wins[i].window && !pg_wins[i].overlay) return &pg_wins[i];
    return NULL;
}
static struct pg_win *pg_win_for(uint32_t handle)
{
    uint32_t index = (handle - kPgWindowBase) / kPgWindowStride;
    if (handle >= kPgWindowBase && index < kPgWindowCapacity && (handle - kPgWindowBase) % kPgWindowStride == 0 &&
        pg_wins[index].window) return &pg_wins[index];
    return pg_main_win();
}
static NSRect pg_content_rect(struct pg_win *w)
{
    if (w->render_view && [w->render_view superview]) {
        NSRect frame = [w->render_view frame];
        return [w->window convertRectToScreen:frame];
    }
    return [w->window contentRectForFrameRect:[w->window frame]];
}
static NSPoint pg_logical_mouse_point(struct pg_win *w, NSPoint screen)
{
    NSRect content = pg_content_rect(w);
    NSSize logical = w->logical_content_size;
    if (logical.width <= 0 || logical.height <= 0 ||
        content.size.width <= 0 || content.size.height <= 0) {
        return NSMakePoint(screen.x - content.origin.x, NSMaxY(content) - screen.y);
    }
    return NSMakePoint((screen.x - content.origin.x) * logical.width / content.size.width,
                       (NSMaxY(content) - screen.y) * logical.height / content.size.height);
}
/* Carbon hands the game both window-local and global points.  While the
   drawable is scaled, expose a virtual global point whose origin remains at
   the real render surface but whose offsets are in the game's 800x600 space.
   That makes direct event reads and GetGlobalMouse/GlobalToLocal agree. */
static NSPoint pg_virtual_screen_mouse_point(struct pg_win *w, NSPoint screen)
{
    if (!w) return screen;
    NSRect content = pg_content_rect(w);
    NSPoint local = pg_logical_mouse_point(w, screen);
    return NSMakePoint(content.origin.x + local.x, NSMaxY(content) - local.y);
}
static void pg_start_app(void)
{
    if (pg_app_started) return;
    pg_app_started = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSMenu *bar = [[[NSMenu alloc] init] autorelease];
    NSMenuItem *app_item = [[[NSMenuItem alloc] init] autorelease];
    [bar addItem:app_item];
    NSMenu *menu = [[[NSMenu alloc] initWithTitle:@"Peggle"] autorelease];
    NSMenuItem *fullscreen = [[[NSMenuItem alloc]
        initWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:)
        keyEquivalent:@"f"] autorelease];
    [fullscreen setKeyEquivalentModifierMask:NSEventModifierFlagControl |
                                               NSEventModifierFlagCommand];
    [menu addItem:fullscreen];
    [menu addItemWithTitle:@"Quit Peggle" action:@selector(terminate:) keyEquivalent:@"q"];
    [app_item setSubmenu:menu];
    [NSApp setMainMenu:bar];
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
}
static uint32_t pg_create_window(NSRect content, bool overlay)
{
    pg_start_app();
    struct pg_win *slot = NULL;
    for (unsigned i = 0; i < pg_win_count; ++i) if (!pg_wins[i].window) { slot = &pg_wins[i]; break; }
    if (!slot) {
        if (pg_win_count >= kPgWindowCapacity) return 0;
        slot = &pg_wins[pg_win_count++];
    }
    uint32_t handle = kPgWindowBase + (uint32_t)(slot - pg_wins) * kPgWindowStride;
    memset(slot, 0, sizeof(*slot));
    static unsigned overlay_creations;
    if (overlay && (++overlay_creations == 1 || overlay_creations % 200 == 0)) {
        fprintf(stderr, "compat32: cursor overlay window created %u times (cached=%s)\n",
                overlay_creations, pg_overlay_cache.window ? "yes" : "no");
    }
    if (overlay && pg_overlay_cache.window) {
        *slot = pg_overlay_cache;
        memset(&pg_overlay_cache, 0, sizeof(pg_overlay_cache));
        slot->handle = handle;
        slot->refcon = 0;
        [slot->window setFrame:content display:NO];
        return handle;
    }
    if (overlay) {
        NSWindow *window = [[NSWindow alloc] initWithContentRect:content styleMask:NSWindowStyleMaskBorderless
                                                          backing:NSBackingStoreBuffered defer:NO];
        [window setOpaque:NO];
        [window setBackgroundColor:[NSColor clearColor]];
        [window setHasShadow:NO];
        [window setIgnoresMouseEvents:YES];
        [window setLevel:NSPopUpMenuWindowLevel];
        [window setReleasedWhenClosed:NO];
        NSImageView *view = [[[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, content.size.width, content.size.height)] autorelease];
        [view setImageScaling:NSImageScaleNone];
        [view setImageAlignment:NSImageAlignTopLeft];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [window setContentView:view];
        slot->window = window;
        slot->image_view = view;
    } else {
        NSWindow *window = [[NSWindow alloc] initWithContentRect:content
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                         backing:NSBackingStoreBuffered defer:NO];
        [window setTitle:@"PeggleSilicon"];
        [window setReleasedWhenClosed:NO];
        [window setAcceptsMouseMovedEvents:YES];
        /* Peggle renders at 4:3.  Let AppKit enlarge the drawable, but keep
           the game's native geometry intact and offer the standard full-screen
           transition (Control-Command-F or the Peggle menu). */
        [window setContentAspectRatio:NSMakeSize(4, 3)];
        [window setMinSize:NSMakeSize(800, 600)];
        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        PeggleLetterboxView *container = [[[PeggleLetterboxView alloc]
            initWithFrame:NSMakeRect(0, 0, content.size.width, content.size.height)] autorelease];
        PeggleView *view = [[[PeggleView alloc]
            initWithFrame:NSMakeRect(0, 0, content.size.width, content.size.height)] autorelease];
        [container addSubview:view];
        [container layout];
        [window setContentView:container];
        [window makeFirstResponder:view];
        static PeggleWindowDelegate *delegate;
        if (!delegate) delegate = [[PeggleWindowDelegate alloc] init];
        [window setDelegate:delegate];
        [window center];
        slot->window = window;
        slot->render_view = view;
        slot->logical_content_size = content.size;
    }
    slot->overlay = overlay;
    slot->handle = handle;
    return handle;
}
static void pg_dispose_window(struct pg_win *w)
{
    [w->window orderOut:nil];
    if (w->overlay) {
        if (pg_overlay_cache.window) {
            if (pg_overlay_cache.context_handle) pg_cf_release(pg_overlay_cache.context_handle);
            [pg_overlay_cache.window release];
        }
        pg_overlay_cache = *w;
    } else {
        if (w->context_handle) pg_cf_release(w->context_handle);
        [w->window setDelegate:nil];
        [w->window release];
    }
    memset(w, 0, sizeof(*w));
}
static void pg_set_bounds(struct pg_win *w, NSRect rect, bool is_content)
{
    if (!w->overlay && is_content && rect.size.width > 0 && rect.size.height > 0) {
        w->logical_content_size = rect.size;
    }
    NSRect frame = is_content ? [w->window frameRectForContentRect:rect] : rect;
    if (!w->overlay && guest_display_mode_active()) {
        /* Emulated fullscreen: the game pins its window to the display origin;
           keep it centred on the desktop instead. */
        NSRect screen = [[NSScreen screens][0] visibleFrame];
        frame.origin.x = floor(NSMidX(screen) - frame.size.width / 2);
        frame.origin.y = floor(NSMidY(screen) - frame.size.height / 2);
    }
    [w->window setFrame:frame display:YES];
    if (!w->overlay) {
        [pg_gl update];
        NSRect c = pg_content_rect(w);
        fprintf(stderr, "compat32: window frame %.0f,%.0f,%.0f,%.0f\n", c.origin.x, pg_screen_top() - NSMaxY(c), c.size.width, c.size.height);
    }
}
static NSOpenGLContext *pg_ensure_gl(void)
{
    if (pg_gl) return pg_gl;
    NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated, NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8, NSOpenGLPFADepthSize, 16, 0,
    };
    NSOpenGLPixelFormat *format = [[[NSOpenGLPixelFormat alloc] initWithAttributes:attributes] autorelease];
    pg_gl = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
    pg_gl_handle = proxy_for_object(pg_gl);
    return pg_gl;
}
/* The handle the game gets for "the GL context".  Titles that call GL through
   <OpenGL/CGLMacro.h> read the dispatch table out of it, so they get the
   synthesized context; everyone else gets the Cocoa proxy they always had. */
static uint32_t pg_gl_context_handle(void)
{
    if (lp32_profile()->title == LP32_TITLE_PLANTSVSZOMBIES) {
        uint32_t context = pg_gli_context_object();
        if (context) return context;
    }
    return pg_gl_handle;
}

static void pg_attach_gl(void)
{
    struct pg_win *w = pg_main_win();
    if (!w) {
        pg_create_window(NSMakeRect(0, 0, 800, 600), false);
        w = pg_main_win();
    }
    pg_ensure_gl();
    [pg_gl setView:w->render_view ? w->render_view : [w->window contentView]];
    [pg_gl makeCurrentContext];
    [pg_gl update];
    GLint one = 1;
    [pg_gl setValues:&one forParameter:NSOpenGLContextParameterSwapInterval];
}
static void pg_update_cursor(void)
{
    struct pg_win *w = pg_main_win();
    bool inside = false;
    if (w) {
        NSPoint p = [w->window mouseLocationOutsideOfEventStream];
        inside = [w->window isKeyWindow] && NSPointInRect(p, [[w->window contentView] frame]);
    }
    bool want_hidden = pg_cursor_hidden && inside;
    if (want_hidden != pg_cursor_applied) {
        if (want_hidden) [NSCursor hide]; else [NSCursor unhide];
        pg_cursor_applied = want_hidden;
    }
}
static void pg_present(void)
{
    [pg_gl flushBuffer];
    audio_bridge32_note_frame_presented();
}
static struct pg_event *pg_event_for(uint32_t handle)
{
    uint32_t index = (handle - kPgEventBase) / kPgEventStride;
    return handle >= kPgEventBase && index < kPgEventCapacity && (handle - kPgEventBase) % kPgEventStride == 0 ?
        &pg_events[index] : NULL;
}
static uint32_t pg_make_event(uint32_t cls, uint32_t kind, NSEvent *native)
{
    unsigned index = pg_event_next++ % kPgEventCapacity;
    struct pg_event *event = &pg_events[index];
    [event->native release];
    event->cls = cls;
    event->kind = kind;
    event->native = [native retain];
    event->param_count = 0;
    return kPgEventBase + index * kPgEventStride;
}
static uint32_t pg_dispatch_event(uint32_t handle, uint32_t target)
{
    struct pg_event *event = pg_event_for(handle);
    if (!event) return (uint32_t)eventNotHandledErr;
    for (uint32_t i = 0; i < pg_handler_count; ++i) {
        if (!pg_handlers[i].callback) continue;
        if (pg_handlers[i].target != target && target != 1) continue;
        for (uint32_t j = 0; j < pg_handlers[i].count; ++j) {
            if (pg_handlers[i].types[j].eventClass != event->cls || pg_handlers[i].types[j].eventKind != event->kind) continue;
            uint32_t arguments[] = { 0, handle, pg_handlers[i].user };
            uint32_t status = compat_runtime32_call(pg_handlers[i].callback, arguments, 3);
            if (status == 0) return 0;
        }
    }
    /* The close box did nothing because Peggle registers no handler for
       kEventWindowClose and Carbon's standard window handler -- which would
       have turned it into a quit -- is not here to fall back on.  It does
       register kEventCommandProcess, which is how the Apple menu's Quit
       reaches a Carbon app, so hand it that instead and let the game shut
       itself down the way it always has.  [NSApp terminate:] is no use: this
       app never enters [NSApp run], so there is no loop for it to unwind. */
    if (event->cls == kEventClassWindow && event->kind == kEventWindowClose) {
        pg_close_unhandled = true;
    }
    /* If the game will not take the quit command either, nothing else will
       end the process, so do it here. */
    if (event->cls == kEventClassCommand && event->kind == kEventCommandProcess &&
        event->param_count && event->params[0].name == kEventParamDirectObject) {
        uint32_t command;
        memcpy(&command, event->params[0].bytes + 4, sizeof(command));
        if (command == kHICommandQuit) exit(0);
    }
    return (uint32_t)eventNotHandledErr;
}
static uint32_t pg_carbon_modifiers(NSEventModifierFlags flags)
{
    uint32_t modifiers = 0;
    if (flags & NSEventModifierFlagCommand) modifiers |= cmdKey;
    if (flags & NSEventModifierFlagShift) modifiers |= shiftKey;
    if (flags & NSEventModifierFlagOption) modifiers |= optionKey;
    if (flags & NSEventModifierFlagControl) modifiers |= controlKey;
    if (flags & NSEventModifierFlagCapsLock) modifiers |= alphaLock;
    return modifiers;
}
static NSPoint pg_event_screen_point(NSEvent *event)
{
    if (event && [event window]) return [[event window] convertPointToScreen:[event locationInWindow]];
    return [NSEvent mouseLocation];
}

static int peggle_window_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define O(i) object_for_argument(a[i])
#define OUT(i, v) do { if (a[i]) *(uint32_t *)P(i) = (uint32_t)(v); } while (0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    /* --- windows --- */
    if (IS("_CreateNewWindow")) {
        const int16_t *q = a[2] >= 0x1000 ? P(2) : NULL;
        NSRect rect = q ? pg_rect_from_qd(q) : NSMakeRect(0, 0, 800, 600);
        bool overlay = a[0] == kOverlayWindowClass || (a[1] & kWindowIgnoreClicksAttribute) != 0;
        uint32_t handle = pg_create_window(rect, overlay);
        if (!overlay && q) pg_set_bounds(pg_win_for(handle), rect, true);
        OUT(3, handle);
        RET(handle ? 0 : -1);
    }
    if (IS("_CreateWindowFromNib")) { OUT(2, 0); RET(-1); } /* the About box nib is not loadable */
    if (IS("_DisposeWindow")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w && w->handle == a[0]) pg_dispose_window(w);
        RET(0);
    }
    if (IS("_ShowWindow") || IS("_SelectWindow") || IS("_BringToFront") || IS("_ActivateWindow")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w && w->overlay) [w->window orderFront:nil];
        else if (w) { [w->window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; }
        RET(0);
    }
    if (IS("_HideWindow")) { struct pg_win *w = pg_win_for(a[0]); if (w) [w->window orderOut:nil]; RET(0); }
    if (IS("_SetWindowTitleWithCFString")) { struct pg_win *w = pg_win_for(a[0]); if (w) [w->window setTitle:@"PeggleSilicon"]; RET(0); }
    if (IS("_SizeWindow")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w) {
            NSSize size = NSMakeSize((int16_t)a[1], (int16_t)a[2]);
            if (!w->overlay && size.width > 0 && size.height > 0) w->logical_content_size = size;
            [w->window setContentSize:size];
            if (!w->overlay) [pg_gl update];
        }
        RET(0);
    }
    if (IS("_GetWindowBounds")) {
        struct pg_win *w = pg_win_for(a[0]);
        int16_t q[4] = {0, 0, 600, 800};
        if (w) {
            /* The engine was written for a borderless full-screen window and
               assumes the structure and content regions coincide: it takes its
               drawing and hit-testing origin from the structure region.
               Reporting the real frame, which includes the title bar, put that
               origin a title bar above the content and pushed every hit test
               down by the same amount, so buttons only responded below
               themselves.  Report the content region for both. */
            pg_qd_from_rect(pg_content_rect(w), q);
            if (!w->overlay && w->logical_content_size.width > 0 && w->logical_content_size.height > 0) {
                q[2] = q[0] + (int16_t)w->logical_content_size.height;
                q[3] = q[1] + (int16_t)w->logical_content_size.width;
            }
        }
        if (a[2]) memcpy(P(2), q, sizeof(q));
        RET(0);
    }
    if (IS("_SetWindowBounds")) {
        struct pg_win *w = pg_win_for(a[0]);
        /* Both regions are the content region here, so a bounds the engine
           read back and set again round-trips instead of creeping upwards. */
        if (w && a[2] >= 0x1000) pg_set_bounds(w, pg_rect_from_qd(P(2)), true);
        RET(0);
    }
    if (IS("_GetPortBounds")) {
        struct pg_win *w = pg_win_for(a[0]);
        NSSize size = w && w->logical_content_size.width > 0 ? w->logical_content_size : NSMakeSize(800, 600);
        int16_t q[4] = {0, 0, (int16_t)size.height, (int16_t)size.width};
        if (a[1]) memcpy(P(1), q, sizeof(q));
        RET(a[1]);
    }
    if (IS("_GetWindowPort") || IS("_GetWindowEventTarget")) RET(a[0]);
    if (IS("_FrontWindow") || IS("_ActiveNonFloatingWindow")) { struct pg_win *w = pg_main_win(); RET(w ? w->handle : 0); }
    if (IS("_GetPort")) { struct pg_win *w = pg_main_win(); OUT(0, pg_current_port ? pg_current_port : (w ? w->handle : 0)); RET(0); }
    if (IS("_SetPort") || IS("_SetPortWindowPort")) { pg_current_port = a[0]; RET(0); }
    if (IS("_SetWRefCon")) { struct pg_win *w = pg_win_for(a[0]); if (w) w->refcon = a[1]; RET(0); }
    if (IS("_GetWRefCon")) { struct pg_win *w = pg_win_for(a[0]); RET(w ? w->refcon : 0); }
    if (IS("_FindWindow")) {
        /* The argument is a global QuickDraw Point, measured down from the top
           of the screen.  pg_qd_point already turns it into the screen
           coordinates pg_content_rect uses; flipping it a second time put the
           point back into QuickDraw space, so every click outside a vertically
           centred window read as inDesk and the engine dropped it.  It stayed
           hidden while the emulated display mode centred the window, which made
           the two flips cancel out. */
        NSPoint screen = pg_qd_point(NSMakePoint((int16_t)(a[0] >> 16),
                                                 (int16_t)(a[0] & 0xffff)));
        struct pg_win *w = pg_main_win();
        if (w && NSPointInRect(screen, pg_content_rect(w))) { OUT(1, w->handle); RET(inContent); }
        OUT(1, 0);
        RET(inDesk);
    }
    if (IS("_GetGlobalMouse")) {
        NSPoint screen = pg_virtual_screen_mouse_point(pg_main_win(), [NSEvent mouseLocation]);
        NSPoint p = pg_qd_point(screen);
        int16_t q[2] = {(int16_t)p.y, (int16_t)p.x};
        if (a[0]) memcpy(P(0), q, sizeof(q));
        RET(0);
    }
    if (IS("_GlobalToLocal")) {
        struct pg_win *w = pg_main_win();
        int16_t *q = P(0);
        if (q && w) {
            int16_t bounds[4];
            pg_qd_from_rect(pg_content_rect(w), bounds);
            q[0] -= bounds[0];
            q[1] -= bounds[1];
        }
        RET(0);
    }
    if (IS("_SetRect")) { int16_t q[4] = {(int16_t)a[2], (int16_t)a[1], (int16_t)a[4], (int16_t)a[3]}; if (a[0]) memcpy(P(0), q, sizeof(q)); RET(0); }
    if (IS("_OffsetRect")) { int16_t *q = P(0); if (q) { q[0] += (int16_t)a[2]; q[2] += (int16_t)a[2]; q[1] += (int16_t)a[1]; q[3] += (int16_t)a[1]; } RET(0); }
    if (IS("_CreateWindowGroup")) { OUT(1, 2); RET(0); }
    if (IS("_GetWindowGroupOfClass")) RET(2);
    if (IS("_HideCursor")) { pg_cursor_hidden = true; pg_update_cursor(); RET(0); }
    if (IS("_ShowCursor") || IS("_InitCursor")) { pg_cursor_hidden = false; pg_update_cursor(); RET(0); }
    if (IS("_DMGetGDeviceByDisplayID")) { OUT(1, 0x1000); RET(0); }
    /* --- QuickDraw ports drawn through CoreGraphics (cursor overlay) --- */
    if (IS("_QDBeginCGContext")) {
        struct pg_win *w = a[0] ? pg_win_for(a[0]) : NULL;
        if (!w) { OUT(1, 0); RET(-50); }
        NSRect c = pg_content_rect(w);
        size_t width = (size_t)c.size.width, height = (size_t)c.size.height;
        if (w->context && (CGBitmapContextGetWidth(w->context) != width || CGBitmapContextGetHeight(w->context) != height)) {
            pg_cf_release(w->context_handle); /* the table owned the reference */
            w->context = NULL;
            w->context_handle = 0;
        }
        if (!w->context && width && height) {
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            w->context = CGBitmapContextCreate(NULL, width, height, 8, width * 4, space,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
            CGColorSpaceRelease(space);
            w->context_handle = pg_cf_register(w->context);
            if (!w->context_handle) w->context = NULL;
        }
        OUT(1, w->context ? w->context_handle : 0);
        RET(w->context ? 0 : -50);
    }
    if (IS("_QDEndCGContext")) {
        struct pg_win *w = a[0] ? pg_win_for(a[0]) : NULL;
        if (w && w->context && w->overlay && w->image_view) {
            CGImageRef image = CGBitmapContextCreateImage(w->context);
            if (image) {
                NSImage *ns = [[[NSImage alloc] initWithCGImage:image size:NSZeroSize] autorelease];
                [w->image_view setImage:ns];
                CGImageRelease(image);
            }
        }
        OUT(1, 0);
        RET(0);
    }
    if (IS("_CreateQDContextForCollapsedWindowDockTile")) RET(0);
    if (IS("_ReleaseQDContextForCollapsedWindowDockTile")) RET(0);
    /* --- Carbon events --- */
    if (IS("_InstallEventHandler")) {
        uint32_t i = pg_handler_count;
        if (i >= 64) RET(-1);
        ++pg_handler_count;
        pg_handlers[i].target = a[0];
        pg_handlers[i].callback = a[1];
        pg_handlers[i].count = a[2] < 64 ? a[2] : 64;
        if (a[3]) memcpy(pg_handlers[i].types, P(3), pg_handlers[i].count * sizeof(EventTypeSpec));
        pg_handlers[i].user = a[4];
        OUT(5, i + 10);
        RET(0);
    }
    if (IS("_RemoveEventHandler")) { if (a[0] >= 10 && a[0] - 10 < pg_handler_count) pg_handlers[a[0] - 10].callback = 0; RET(0); }
    if (IS("_InstallEventLoopTimer")) {
        double first, interval;
        memcpy(&first, a + 1, sizeof(first));
        memcpy(&interval, a + 3, sizeof(interval));
        pg_timer_callback = a[5];
        pg_timer_user = a[6];
        pg_timer_interval = interval > 0 ? interval : 0;
        pg_timer_next = CFAbsoluteTimeGetCurrent() + (first > 0 ? first : 0);
        OUT(7, 3);
        RET(0);
    }
    if (IS("_RemoveEventLoopTimer")) { pg_timer_callback = 0; RET(0); }
    if (IS("_CreateEvent")) { OUT(6, pg_make_event(a[1], a[2], nil)); RET(0); }
    if (IS("_ReleaseEvent") || IS("_RetainEvent")) RET(IS("_RetainEvent") ? a[0] : 0);
    if (IS("_GetEventClass")) { struct pg_event *e = pg_event_for(a[0]); RET(e ? e->cls : 0); }
    if (IS("_GetEventKind")) { struct pg_event *e = pg_event_for(a[0]); RET(e ? e->kind : 0); }
    if (IS("_SetEventParameter")) {
        struct pg_event *e = pg_event_for(a[0]);
        if (!e || a[3] > 64) RET(-9870);
        unsigned slot = e->param_count;
        for (unsigned i = 0; i < e->param_count; ++i) if (e->params[i].name == a[1]) slot = i;
        if (slot >= 4) RET(-9870);
        if (slot == e->param_count) ++e->param_count;
        e->params[slot].name = a[1];
        e->params[slot].type = a[2];
        e->params[slot].size = a[3];
        if (a[4]) memcpy(e->params[slot].bytes, P(4), a[3]);
        RET(0);
    }
    if (IS("_GetEventParameter")) {
        struct pg_event *e = pg_event_for(a[0]);
        if (!e) RET(-9870);
        NSEvent *native = e->native;
        uint8_t temp[64] = {0};
        size_t length = 0;
        const void *bytes = temp;
        bool found = false;
        for (unsigned i = 0; i < e->param_count; ++i) {
            if (e->params[i].name == a[1]) { bytes = e->params[i].bytes; length = e->params[i].size; found = true; }
        }
        if (!found) {
            found = true;
            if (a[1] == kEventParamDirectObject ||
                a[1] == kEventParamWindowRef) {
                /* Carbon tags every mouse event with the window it happened in
                   and the engine asks for it; answering "missing" left it with
                   no window to route the click to.  There is one game window. */
                struct pg_win *w = pg_main_win();
                uint32_t handle = w ? w->handle : 0;
                memcpy(temp, &handle, 4);
                length = 4;
            } else if (a[1] == kEventParamMouseLocation) {
                NSPoint screen = pg_virtual_screen_mouse_point(pg_main_win(),
                                                                 pg_event_screen_point(native));
                NSPoint p = pg_qd_point(screen);
                if (a[2] == typeQDPoint) { int16_t q[2] = {(int16_t)p.y, (int16_t)p.x}; memcpy(temp, q, 4); length = 4; }
                else { float q[2] = {(float)p.x, (float)p.y}; memcpy(temp, q, 8); length = 8; }
            } else if (a[1] == kEventParamWindowMouseLocation) {
                struct pg_win *w = pg_main_win();
                NSPoint p = pg_event_screen_point(native);
                NSPoint local = w ? pg_logical_mouse_point(w, p) : NSZeroPoint;
                /* Honour the requested type, as the global location above
                   already does.  The engine asks for this one as a QuickDraw
                   Point (two 16-bit words, vertical first); always answering
                   with the HIPoint pair handed it the bytes of a float. */
                if (a[2] == typeQDPoint) {
                    int16_t q[2] = {(int16_t)local.y, (int16_t)local.x};
                    memcpy(temp, q, 4);
                    length = 4;
                } else {
                    float q[2] = {(float)local.x, (float)local.y};
                    memcpy(temp, q, 8);
                    length = 8;
                }
            } else if (a[1] == kEventParamMouseButton) {
                uint16_t button = (uint16_t)(native ? [native buttonNumber] + 1 : 1);
                memcpy(temp, &button, 2);
                length = 2;
            } else if (a[1] == kEventParamClickCount) {
                uint32_t count = native ? (uint32_t)[native clickCount] : 1;
                memcpy(temp, &count, 4);
                length = 4;
            } else if (a[1] == kEventParamKeyCode) {
                uint32_t code = native ? [native keyCode] : 0;
                memcpy(temp, &code, 4);
                length = 4;
            } else if (a[1] == kEventParamKeyMacCharCodes) {
                NSString *characters = native && [native type] != NSEventTypeFlagsChanged ? [native characters] : nil;
                temp[0] = [characters length] ? (uint8_t)[characters characterAtIndex:0] : 0;
                length = 1;
            } else if (a[1] == kEventParamKeyUnicodes) {
                NSString *characters = native && [native type] != NSEventTypeFlagsChanged ? [native characters] : nil;
                NSUInteger count = [characters length] < 16 ? [characters length] : 16;
                [characters getCharacters:(unichar *)temp range:NSMakeRange(0, count)];
                length = count * 2;
            } else if (a[1] == kEventParamTextInputSendText) {
                NSMutableString *characters = [[[native characters] mutableCopy] autorelease];
                [characters replaceOccurrencesOfString:@"\x7f" withString:@"\b" options:0 range:NSMakeRange(0, [characters length])];
                NSData *data = a[2] == typeUnicodeText ? [characters dataUsingEncoding:NSUTF16LittleEndianStringEncoding]
                                                       : [characters dataUsingEncoding:NSMacOSRomanStringEncoding allowLossyConversion:YES];
                length = [data length] < 64 ? [data length] : 64;
                memcpy(temp, [data bytes], length);
            } else if (a[1] == kEventParamKeyModifiers) {
                uint32_t modifiers = pg_carbon_modifiers(native ? [native modifierFlags] : [NSEvent modifierFlags]);
                memcpy(temp, &modifiers, 4);
                length = 4;
            } else if (a[1] == kEventParamMouseWheelAxis) {
                uint16_t axis = native && fabs([native scrollingDeltaX]) > fabs([native scrollingDeltaY]) ? kEventMouseWheelAxisX : kEventMouseWheelAxisY;
                memcpy(temp, &axis, 2);
                length = 2;
            } else if (a[1] == kEventParamMouseWheelDelta) {
                int32_t delta = 0;
                if (native) {
                    CGFloat d = fabs([native scrollingDeltaX]) > fabs([native scrollingDeltaY]) ? [native scrollingDeltaX] : [native scrollingDeltaY];
                    delta = d > 0 ? (int32_t)ceil(d) : (int32_t)floor(d);
                }
                memcpy(temp, &delta, 4);
                length = 4;
            } else {
                found = false;
            }
        }
        if (pg_tracing()) {
            fprintf(stderr, "compat32: GetEventParameter '%.4s' type '%.4s' -> %s (%zu bytes)\n",
                    (const char[]){a[1] >> 24, a[1] >> 16, a[1] >> 8, a[1]}, (const char[]){a[2] >> 24, a[2] >> 16, a[2] >> 8, a[2]},
                    found ? "ok" : "missing", length);
        }
        if (!found) RET(-9870);
        OUT(3, a[2]);
        OUT(5, length);
        if (a[6]) memcpy(P(6), bytes, a[4] < length ? a[4] : length);
        RET(0);
    }
    if (IS("_SendEventToEventTarget")) {
        uint32_t status = pg_dispatch_event(a[0], a[1]);
        if (pg_tracing()) {
            struct pg_event *e = pg_event_for(a[0]);
            fprintf(stderr, "compat32: event '%.4s'/%u -> target 0x%08x status=%d\n",
                    e ? (const char[]){e->cls >> 24, e->cls >> 16, e->cls >> 8, e->cls} : "????", e ? e->kind : 0, a[1], (int)status);
        }
        RET(status);
    }
    if (IS("_CallNextEventHandler")) RET(eventNotHandledErr);
    if (IS("_ReceiveNextEvent")) {
        double now = CFAbsoluteTimeGetCurrent();
        if (pg_timer_callback && now >= pg_timer_next) {
            pg_timer_next = pg_timer_interval > 0 ? now + pg_timer_interval : now + 3600;
            uint32_t arguments[] = {3, pg_timer_user};
            compat_runtime32_call(pg_timer_callback, arguments, 2);
        }
        pg_update_cursor();
        if (pg_close_unhandled) { pg_close_unhandled = false; pg_queue_command(kHICommandQuit); }
        { struct pg_win *w = pg_main_win(); if (w) pg_run_script(w->window); }
        if (pg_pending_count) {
            uint32_t cls = pg_pending[0].cls, kind = pg_pending[0].kind;
            NSEvent *native = [pg_pending[0].native autorelease];
            uint32_t command = pg_pending[0].command;
            memmove(pg_pending, pg_pending + 1, (--pg_pending_count) * sizeof(pg_pending[0]));
            uint32_t handle = pg_make_event(cls, kind, native);
            if (command) {
                /* i386 HICommand: attributes, commandID, then the menu the
                   command came from -- none, here. */
                struct pg_event *e = pg_event_for(handle);
                uint32_t hi_command[4] = {kHICommandFromMenu, command, 0, 0};
                if (!e) { OUT(5, handle); RET(0); }
                e->params[0].name = kEventParamDirectObject;
                e->params[0].type = typeHICommand;
                e->params[0].size = 14;
                memcpy(e->params[0].bytes, hi_command, sizeof(hi_command));
                e->param_count = 1;
            }
            OUT(5, handle);
            RET(0);
        }
        double timeout;
        memcpy(&timeout, a + 2, sizeof(timeout));
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:timeout > 0 ? fmin(timeout, 0.01) : 0];
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
        if (!e) { OUT(5, 0); RET(eventLoopTimedOutErr); }
        uint32_t cls = 0, kind = 0;
        switch ([e type]) {
        case NSEventTypeLeftMouseDown: case NSEventTypeRightMouseDown: case NSEventTypeOtherMouseDown: cls = kEventClassMouse; kind = kEventMouseDown; break;
        case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp: cls = kEventClassMouse; kind = kEventMouseUp; break;
        case NSEventTypeMouseMoved: cls = kEventClassMouse; kind = kEventMouseMoved; break;
        case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged: cls = kEventClassMouse; kind = kEventMouseDragged; break;
        case NSEventTypeScrollWheel: cls = kEventClassMouse; kind = kEventMouseWheelMoved; break;
        case NSEventTypeKeyDown: cls = kEventClassKeyboard; kind = [e isARepeat] ? kEventRawKeyRepeat : kEventRawKeyDown; break;
        case NSEventTypeKeyUp: cls = kEventClassKeyboard; kind = kEventRawKeyUp; break;
        case NSEventTypeFlagsChanged: cls = kEventClassKeyboard; kind = kEventRawKeyModifiersChanged; break;
        default: break;
        }
        if (pg_tracing() && [e type] != NSEventTypeMouseMoved) {
            fprintf(stderr, "compat32: NSEvent type=%lu window=%ld key=%u chars=\"%s\" -> '%.4s'/%u\n",
                    (unsigned long)[e type], (long)[e windowNumber],
                    [e type] == NSEventTypeKeyDown || [e type] == NSEventTypeKeyUp ? [e keyCode] : 0,
                    [e type] == NSEventTypeKeyDown || [e type] == NSEventTypeKeyUp ? [[e characters] UTF8String] : "",
                    (const char[]){cls >> 24, cls >> 16, cls >> 8, cls}, kind);
        }
        [NSApp sendEvent:e];
        [NSApp updateWindows];
        if (!cls) { OUT(5, 0); RET(eventLoopTimedOutErr); }
        if (cls == kEventClassKeyboard && (kind == kEventRawKeyDown || kind == kEventRawKeyRepeat)) pg_queue_text_input(e);
        OUT(5, pg_make_event(cls, kind, e));
        RET(0);
    }
    /* --- AGL (windowed) and CGL (emulated fullscreen) share one context --- */
    if (IS("_aglChoosePixelFormat")) RET(1);
    /* A title built with CGLMacro.h calls GL straight through whichever
       context handle it kept, so every handle it can obtain has to be the
       synthesized dispatch context; the AGL/CGL calls themselves ignore it. */
    if (IS("_aglCreateContext")) { pg_ensure_gl(); RET(pg_gl_context_handle()); }
    if (IS("_aglSetDrawable")) { pg_attach_gl(); RET(1); }
    if (IS("_aglSetCurrentContext")) { if (a[0]) [pg_gl makeCurrentContext]; else [NSOpenGLContext clearCurrentContext]; RET(1); }
    if (IS("_aglUpdateContext")) { [pg_gl update]; RET(1); }
    if (IS("_aglSwapBuffers")) { pg_present(); RET(1); }
    if (IS("_aglGetError")) RET(0);
    /* Titles built with CGLMacro.h call GL through the context's own dispatch
       table, so they get the synthesized one rather than a proxy handle. */
    if (IS("_aglGetCGLContext")) { pg_ensure_gl(); OUT(1, pg_gl_context_handle()); RET(1); }
    if (IS("_aglErrorString")) { static uint32_t text; if (!text) text = compat_runtime32_copy_cstring("no error"); RET(text); }
    if (IS("_aglDestroyPixelFormat") || IS("_aglDestroyContext")) RET(1);
    if (IS("_CGLChoosePixelFormat")) { OUT(1, 1); OUT(2, 1); RET(0); }
    if (IS("_CGLDestroyPixelFormat")) RET(0);
    if (IS("_CGLCreateContext")) { pg_ensure_gl(); OUT(2, pg_gl_context_handle()); RET(0); }
    if (IS("_CGLDestroyContext")) RET(0);
    if (IS("_CGLSetCurrentContext")) { if (a[0]) [pg_gl makeCurrentContext]; else [NSOpenGLContext clearCurrentContext]; RET(0); }
    if (IS("_CGLGetCurrentContext")) RET([NSOpenGLContext currentContext] == pg_gl ? pg_gl_context_handle() : 0);
    if (IS("_CGLSetFullScreen") || IS("_CGLSetFullScreenOnDisplay")) { pg_attach_gl(); RET(0); }
    if (IS("_CGLClearDrawable")) { [pg_gl clearDrawable]; RET(0); }
    if (IS("_CGLFlushDrawable")) { pg_present(); RET(0); }
    if (IS("_CGLSetParameter")) {
        if (a[0] && a[1] == kCGLCPSwapInterval && a[2] >= 0x1000) [pg_gl setValues:P(2) forParameter:NSOpenGLContextParameterSwapInterval];
        RET(0);
    }
    if (IS("_CGLGetParameter")) {
        if (a[1] == kCGLCPSwapInterval && a[2] >= 0x1000) [pg_gl getValues:P(2) forParameter:NSOpenGLContextParameterSwapInterval];
        RET(0);
    }
    static const char *const noops[] = {
        "_DrawMenuBar", "_HideMenuBar", "_ShowMenuBar", "_BeginUpdate", "_EndUpdate",
        "_ChangeWindowGroupAttributes", "_SetWindowGroup", "_SetWindowGroupParent", "_ReleaseWindowGroup",
        "_SendBehind", "_SetWindowAlpha", "_SetThemeCursor", "_HiliteMenu", "_MenuSelect",
        "_ConvertEventRefToEventRecord", "_HIViewSetNeedsDisplay", "_HIViewAddSubview", "_HIImageViewSetImage",
        "_HIImageViewSetOpaque", "_SetControlData", "_Draw1Control", "_QuitAppModalLoopForWindow",
        "_RunAppModalLoopForWindow", "_DisposeNibReference", "_SetMenuBarFromNib",
    };
    for (size_t i = 0; i < sizeof(noops) / sizeof(noops[0]); ++i) if (IS(noops[i])) RET(0);
    if (IS("_HIViewFindByID") || IS("_GetControlByID")) { OUT(2, 0); RET(-1); }
    if (IS("_HIViewGetRoot")) RET(0);
    return 0;
#undef RET
#undef OUT
#undef O
#undef P
#undef IS
}
