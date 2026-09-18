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

/* The game draws in a fixed-shape surface even when AppKit makes its window
   fill an ultrawide display.  Keeping that surface as a child view lets native
   full screen scale it to the largest undistorted size and leaves the
   remainder black, just like a classic display mode.

   The shape is 4:3 for the Sexy titles but follows the video mode for Chuzzle,
   whose options screen switches between 4:3 (800x600) and 5:4 (1280x1024)
   while the game runs. */
@interface PeggleLetterboxView : NSView
@property (nonatomic) NSSize contentAspect;
@end
@implementation PeggleLetterboxView
- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) _contentAspect = NSMakeSize(4, 3);
    return self;
}
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
    NSSize aspect = _contentAspect;
    if (aspect.width <= 0 || aspect.height <= 0) aspect = NSMakeSize(4, 3);
    CGFloat scale = fmin(bounds.size.width / aspect.width,
                         bounds.size.height / aspect.height);
    NSSize size = NSMakeSize(floor(aspect.width * scale),
                             floor(aspect.height * scale));
    NSRect frame = NSMakeRect(floor((bounds.size.width - size.width) / 2.0),
                              floor((bounds.size.height - size.height) / 2.0),
                              size.width, size.height);
    [[[self subviews] objectAtIndex:0] setFrame:frame];
}
- (void)resizeSubviewsWithOldSize:(NSSize)oldSize
{
    [super resizeSubviewsWithOldSize:oldSize];
    /* AppKit does not always schedule -layout for a content view whose frame
       changes during a Carbon SetWindowBounds call.  Recompute the child
       frame synchronously so a display-sized fullscreen window cannot leave
       the GL view at its old 4:3 size. */
    [self layout];
}
@end

enum {
    kPgWindowCapacity = 8, kPgWindowBase = 0x7f0a0000, kPgWindowStride = 16,
    /* Carbon WindowRegionCode values, confirmed from HIToolbox BridgeSupport
       (the Command Line Tools SDK no longer ships MacWindows.h): the title bar
       is 0, and those are the three the guest actually passes. */
    kPgWindowTitleBarRgn = 0, kPgWindowStructureRgn = 32, kPgWindowContentRgn = 33,
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
    /* Carbon window properties: the game hangs its own C++ window object off
       the WindowRef under ('PNMA','CWnd') and reads it back on every event. */
    struct { uint32_t creator, tag, size; uint8_t bytes[16]; } properties[8];
    unsigned property_count;
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
/* Depth of guest frame callbacks currently running inside the Carbon pump.  A
   widget can only be handed a mouse event while it is still being constructed
   if the pump runs re-entrantly during that construction, so this counts.
   Counters only: printing per event perturbs the timing badly enough to hide
   the crash under investigation. */
static int pg_timer_depth;
static unsigned long pg_pump_reentries;
static unsigned long pg_nested_mouse_dispatches;
static uint32_t pg_last_nested_mouse_kind;

/* Reported by the crash diagnostic and at exit, so the counts are visible
   without touching the hot path. */
void pg_report_pump_counters(void)
{
    /* Called only from the crash diagnostic today, so a run that does not
       fault reports nothing.  Do not describe this as reporting at exit
       without actually wiring an exit call -- eight runs were read as "no
       nesting observed" when the truth was "nothing was ever printed". */
    if (!pg_pump_reentries && !pg_nested_mouse_dispatches) return;
    fprintf(stderr,
            "compat32: pump re-entries=%lu nested-mouse-dispatches=%lu "
            "last-nested-kind=%u\n",
            pg_pump_reentries, pg_nested_mouse_dispatches,
            pg_last_nested_mouse_kind);
}
/* Set while RunApplicationEventLoop is pumping; QuitApplicationEventLoop
   clears it so that call returns, the way Carbon's own pair behaves. */
static bool pg_event_loop_running;
/* The token aglQueryRendererInfo hands back: there is one shared context, so
   nothing has to be enumerated behind it. */
enum { kPgRendererInfo = 0x7f0d0001 };
struct pg_event {
    uint32_t cls, kind;
    NSEvent *native;
    /* When the event was made, in the seconds-since-boot an EventTime is.
       The game timestamps every event it takes and compares them, so an
       event without one is an event it cannot order or act on. */
    double when;
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
    /* The US virtual-keycode table, indexed by keycode.  Its holes are the
       keycodes with no plain character (10 is ISO section, 36 Return, 48 Tab)
       and they are spelled as NULs, so the scan has to run to the table's
       length: stopping at the first one ended it after 'v' and left every
       letter past that silently typing keycode 0. */
    static const char layout[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0 `";
    if (c == '\n') return 36;
    if (c == '\t') return 48;
    if (c == ' ') return 49;
    if (c == 8) return 51;
    if (c == 27) return 53;
    char lower = (char)tolower((unsigned char)c);
    for (unsigned i = 0; i + 1 < sizeof(layout); ++i) {
        if (layout[i] && layout[i] == lower && layout[i] != ' ') return (uint16_t)i;
    }
    return 0;
}
static void pg_post_key(NSWindow *window, char c, uint16_t keycode,
                        NSEventModifierFlags extra_flags)
{
    NSString *text = c ? [NSString stringWithFormat:@"%c", c] : @"";
    NSEventModifierFlags flags =
        (isupper((unsigned char)c) ? NSEventModifierFlagShift : 0) | extra_flags;
    for (int up = 0; up < 2; ++up) {
        NSEvent *event = [NSEvent keyEventWithType:up ? NSEventTypeKeyUp : NSEventTypeKeyDown location:NSMakePoint(0, 0)
                                     modifierFlags:flags timestamp:[NSDate timeIntervalSinceReferenceDate]
                                      windowNumber:[window windowNumber] context:nil characters:text
                       charactersIgnoringModifiers:[text lowercaseString] isARepeat:NO keyCode:keycode];
        [NSApp postEvent:event atStart:NO];
    }
}
/* LP32_CG_EVENTS=1: deliver scripted clicks as real CGEvents instead of
   synthesised NSEvents.  Every layer of the NSEvent path measures correct --
   posted, dequeued 1:1, right class/kind, right coordinates, dispatched to
   matching handlers, every requested parameter answered -- and the guest still
   ignores them, so the remaining difference is that a synthesised NSEvent has
   no backing CGEvent.  Posted to our own pid, so the user's physical cursor is
   not moved.  Off by default; nothing changes for a normal run. */
static bool pg_use_cg_events(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("LP32_CG_EVENTS") != NULL;
    return enabled != 0;
}
static bool pg_post_cg_click(NSWindow *window, double x, double y, bool press)
{
    NSRect content = [[window contentView] frame];
    NSRect frame = [window frame];
    NSScreen *screen = [window screen] ?: [NSScreen mainScreen];
    if (!screen) return false;
    CGFloat screen_height = [screen frame].size.height;
    /* Cocoa screen coords are bottom-left origin; CGEvent wants top-left. */
    CGFloat content_top_cocoa = NSMaxY(frame) - (frame.size.height - content.size.height);
    CGPoint point = CGPointMake(frame.origin.x + x,
                                screen_height - content_top_cocoa + y);
    const CGEventType types[] = { kCGEventMouseMoved, kCGEventLeftMouseDown,
                                  kCGEventLeftMouseUp };
    for (unsigned i = 0; i < (press ? 3u : 1u); ++i) {
        CGEventRef event = CGEventCreateMouseEvent(NULL, types[i], point,
                                                   kCGMouseButtonLeft);
        if (!event) return false;
        if (i) CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
        CGEventPostToPid((pid_t)getpid(), event);
        CFRelease(event);
    }
    if (pg_tracing()) {
        fprintf(stderr, "compat32: CGEvent click at global=(%.0f,%.0f)\n",
                point.x, point.y);
    }
    return true;
}
/* Keys as real CGEvents, for the same reason pg_post_cg_click exists above:
   this guest ignores synthesised NSEvents.  alt+enter posted as an NSEvent
   dispatched correctly at every layer and still never reached the game's own
   fullscreen toggle -- no "Want Windowed" line appeared -- so the chord could
   not be exercised at all.  Posted to our own pid, so nothing is delivered to
   the user's session. */
static bool pg_post_cg_key(uint16_t keycode, CGEventFlags flags)
{
    CGEventSourceRef source =
        CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    for (int up = 0; up < 2; ++up) {
        CGEventRef event =
            CGEventCreateKeyboardEvent(source, (CGKeyCode)keycode, up == 0);
        if (!event) {
            if (source) CFRelease(source);
            return false;
        }
        if (flags) CGEventSetFlags(event, flags);
        CGEventPostToPid((pid_t)getpid(), event);
        CFRelease(event);
    }
    if (source) CFRelease(source);
    if (pg_tracing()) {
        fprintf(stderr, "compat32: CGEvent key %u flags=0x%llx\n",
                (unsigned)keycode, (unsigned long long)flags);
    }
    return true;
}
static void pg_report_guest_geometry(const char *when);
/* Where the script last aimed, in Cocoa screen coords, so a cursor poll can be
   answered with the scripted position rather than the live pointer. */
static NSPoint pg_script_cursor;
static bool pg_script_cursor_valid;
static void pg_post_click(NSWindow *window, double x, double y, bool press)
{
    {
        NSRect content = [[window contentView] frame];
        NSPoint local = NSMakePoint(x, content.size.height - y);
        pg_script_cursor = [window convertPointToScreen:local];
        pg_script_cursor_valid = true;
    }
    if (pg_use_cg_events() && pg_post_cg_click(window, x, y, press)) return;
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
            for (size_t i = 0; i < length; ++i) pg_post_key(window, argument[i], pg_keycode_for(argument[i]), 0);
        } else if (!strncmp(action, "key:", 4)) {
            /* key:<code>[:option|:command|:control|:shift].  The modifier
               matters for chords the game handles itself -- alt+enter is its
               own fullscreen toggle -- and without one this could only ever
               send Shift, so that chord was not expressible. */
            char *rest = NULL;
            uint16_t code = (uint16_t)strtoul(argument, &rest, 0);
            NSEventModifierFlags key_flags = 0;
            CGEventFlags cg_flags = 0;
            if (rest && *rest == ':') {
                const char *name = rest + 1;
                if (!strncmp(name, "option", 6)) {
                    key_flags |= NSEventModifierFlagOption;
                    cg_flags |= kCGEventFlagMaskAlternate;
                } else if (!strncmp(name, "command", 7)) {
                    key_flags |= NSEventModifierFlagCommand;
                    cg_flags |= kCGEventFlagMaskCommand;
                } else if (!strncmp(name, "control", 7)) {
                    key_flags |= NSEventModifierFlagControl;
                    cg_flags |= kCGEventFlagMaskControl;
                } else if (!strncmp(name, "shift", 5)) {
                    key_flags |= NSEventModifierFlagShift;
                    cg_flags |= kCGEventFlagMaskShift;
                }
            }
            /* Prefer a real CGEvent when LP32_CG_EVENTS is set; the NSEvent
               fallback is kept for the titles that do accept it. */
            if (!pg_use_cg_events() || !pg_post_cg_key(code, cg_flags)) {
                pg_post_key(window, 0, code, key_flags);
            }
        } else if (!strncmp(action, "click:", 6) || !strncmp(action, "move:", 5)) {
            char *comma = NULL;
            double x = strtod(argument, &comma);
            pg_post_click(window, x, comma && *comma == ',' ? strtod(comma + 1, NULL) : 0, action[0] == 'c');
        } else if (!strncmp(action, "switchmode:", 11)) {
            /* Call the game's own mode switch, which is what its in-game
               fullscreen option invokes.  Every synthesised input route is
               ignored by this guest (NSEvent clicks, NSEvent keys and real
               CGEvent keys were each tested and produce no reaction), and its
               ScreenMode registry value is written on the way out of a switch
               rather than read at startup, so this is the only way to reach
               that path without a person pressing the button.

               gSexyAppBase is a real heap pointer -- the layout trace read
               0x020012d0 through it -- and the argument shape comes from the
               guest's own log line, "Want Windowed / Want Hi-Res /
               restarting". */
            enum { kSexyAppBaseGlobal = 0x00534c30, kGuestHeapFloor = 0x02000000 };
            uint32_t app = *(const uint32_t *)(uintptr_t)kSexyAppBaseGlobal;
            uint32_t fn = compat_runtime32_guest_symbol(
                "__ZN4Sexy7GameApp16SwitchScreenModeEbbb");
            uint32_t windowed = (uint32_t)strtoul(argument, NULL, 0);
            if (app >= kGuestHeapFloor && fn) {
                uint32_t call[4] = {app, windowed, 1u, 0u};
                fprintf(stderr, "compat32: calling GameApp::SwitchScreenMode"
                        "(this=0x%08x, windowed=%u) at 0x%08x\n",
                        app, windowed, fn);
                pg_report_guest_geometry("before-switch");
                compat_runtime32_call(fn, call, 4);
                fprintf(stderr, "compat32: SwitchScreenMode returned%s\n",
                        compat_runtime32_last_call_trapped() ? " (TRAPPED)" : "");
                pg_report_guest_geometry("after-switch");
            } else {
                fprintf(stderr, "compat32: cannot switch mode; app=0x%08x fn=0x%08x\n",
                        app, fn);
            }
        } else if (!strncmp(action, "geom:", 5)) {
            /* Sample the same geometry later than the switch itself: the
               surface can be rebuilt a frame or more after ToggleFullscreen
               returns, and reading only at the call site is exactly how the
               previous attempt latched the windowed values. */
            pg_report_guest_geometry("sample");
        } else if (!strncmp(action, "fullscreen:", 11)) {
            /* Drive AppKit's native fullscreen the way the menu item does.
               The mode switch cannot be reached with synthesised input on
               every title -- Zuma's Revenge ignores posted NSEvents, and
               pg_post_key can only ever add Shift, so alt+enter is not
               expressible -- and this is the same transition the player
               triggers, so it exercises the real SetWindowBounds path. */
            bool want_full = strtol(argument, NULL, 0) != 0;
            bool is_full =
                ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
            if (want_full != is_full) [window toggleFullScreen:nil];
        }
        pg_script = stop ? stop + 1 : "";
    }
}

static void pg_queue_event(uint32_t cls, uint32_t kind, NSEvent *native)
{
    /* A full queue silently drops the event.  Measured 2026-09-15 during fast
       drags: zero overflows, so this bound is not an input-hitching source. */
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
    NSView *content = [[note object] contentView];
    if ([content respondsToSelector:@selector(layout)]) {
        [content setNeedsLayout:YES];
        [content layout];
    }
    if (pg_gl) [pg_gl update];
    if (getenv("LP32_TRACE_DISPLAY")) {
        NSView *render = [[content subviews] count] ? [content.subviews objectAtIndex:0] : nil;
        NSRect frame = [[note object] frame];
        fprintf(stderr, "compat32: windowDidResize frame=%.0fx%.0f content=%.0fx%.0f render=%.0fx%.0f fullscreen=%d\n",
                frame.size.width, frame.size.height, content.bounds.size.width,
                content.bounds.size.height, render ? render.frame.size.width : 0,
                render ? render.frame.size.height : 0,
                (int)(([[note object] styleMask] & NSWindowStyleMaskFullScreen) != 0));
    }
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
    /* Prefer a window that can actually be drawn into.  Zuma's Revenge builds
       a Win32-shim message window first -- created with an empty rect and
       never sized -- and taking the first non-overlay window attached the GL
       context to that 0x0 drawable, so every buffer swap went nowhere and the
       real window had nothing to show. */
    for (unsigned i = 0; i < pg_win_count; ++i) {
        struct pg_win *w = &pg_wins[i];
        if (!w->window || w->overlay) continue;
        NSSize size = [[w->window contentView] frame].size;
        if (size.width > 0 && size.height > 0) return w;
    }
    /* Nothing sized yet: fall back to the first, as before. */
    for (unsigned i = 0; i < pg_win_count; ++i) {
        if (pg_wins[i].window && !pg_wins[i].overlay) return &pg_wins[i];
    }
    return NULL;
}
#define PG_FOUR_THREE_MIN_ASPECT 1.30
#define PG_FOUR_THREE_MAX_ASPECT 1.36

static bool pg_adjust_fullscreen_viewport(GLint *x, GLint *y,
                                          GLsizei *width, GLsizei *height)
{
    /* This used to require NSWindowStyleMaskFullScreen, which meant it only
       ever fired for AppKit's own fullscreen.  Zuma's Revenge's in-game
       fullscreen option never enters that mode: it resizes its window to a
       zoomed 1920x986 and the letterbox grows the render view to 1272x954,
       so the expansion was skipped and the drawable stayed at the guest's
       1080x810 -- the game drew 1:1 in the corner of a much larger view.
       Key it on the drawable instead: the checks below already refuse an
       off-screen framebuffer, a non-4:3 viewport, and any backing that is not
       strictly larger than what the guest asked for, so a window that has not
       actually grown still cannot be rewritten. */
    struct pg_win *w = pg_main_win();
    if (!w || !w->window || !w->render_view || !x || !y || !width || !height ||
        *width <= 0 || *height <= 0) {
        return false;
    }

    /* Never rewrite an effect's off-screen viewport.  The default framebuffer
       is the only surface whose size follows the fullscreen child view. */
    GLint framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer) return false;

    NSRect backing = [w->render_view convertRectToBacking:[w->render_view bounds]];
    GLsizei target_width = (GLsizei)lrint(backing.size.width);
    GLsizei target_height = (GLsizei)lrint(backing.size.height);
    if (target_width <= 0 || target_height <= 0) return false;

    if (*width > target_width || *height > target_height) {
        GLsizei asked_w = *width, asked_h = *height;
        *x = 0; *y = 0;
        *width = target_width;
        *height = target_height;
        static GLsizei clamped_w, clamped_h;
        if (target_width != clamped_w || target_height != clamped_h) {
            clamped_w = target_width; clamped_h = target_height;
            fprintf(stderr, "compat32: viewport clamped to drawable %dx%d"
                    " (guest asked %dx%d, which would push %d rows off the"
                    " top)\n", (int)target_width, (int)target_height,
                    (int)asked_w, (int)asked_h, (int)(asked_h - target_height));
        }
        return true;
    }

    /* Below here: the original enlargement path, for a guest asking for a
       4:3 viewport smaller than the drawable. */
    double requested_aspect = (double)*width / (double)*height;
    if (requested_aspect < PG_FOUR_THREE_MIN_ASPECT ||
        requested_aspect > PG_FOUR_THREE_MAX_ASPECT) return false;
    if (target_width <= *width || target_height <= *height) return false;

    /* Keep the caller's 4:3 projection and enlarge only the default drawable.
       The child view has already been letterboxed, so (0,0) is its own lower
       left corner and no additional offset is needed. */
    /* Capture the request before overwriting it: reporting *width afterwards
       would print the expanded size twice and claim the guest asked for it. */
    GLsizei asked_width = *width, asked_height = *height;
    *x = 0;
    *y = 0;
    *width = target_width;
    *height = target_height;
    /* Report each distinct size rather than only the first: the drawable grows
       for a zoomed window as well as for AppKit fullscreen, and a one-shot
       report hides whichever happens second. */
    static GLsizei logged_width, logged_height;
    if (target_width != logged_width || target_height != logged_height) {
        logged_width = target_width;
        logged_height = target_height;
        fprintf(stderr, "compat32: viewport expanded to %dx%d (guest asked"
                " %dx%d)\n", (int)target_width, (int)target_height,
                (int)asked_width, (int)asked_height);
    }
    return true;
}
static void pg_report_guest_geometry(const char *when)
{
    enum { kSexyAppBaseGlobal = 0x00534c30, kGuestHeapFloor = 0x02000000 };
    uint32_t app = *(const uint32_t *)(uintptr_t)kSexyAppBaseGlobal;
    if (app < kGuestHeapFloor) {
        fprintf(stderr, "compat32: geom %s: gSexyAppBase not live (0x%08x)\n",
                when, app);
        return;
    }
    const uint8_t *base = (const uint8_t *)(uintptr_t)app;
    uint32_t gfx = *(const uint32_t *)(const void *)(base + 0x2fc);
    uint32_t surface_w = 0, surface_h = 0;
    if (gfx >= kGuestHeapFloor) {
        const uint8_t *g = (const uint8_t *)(uintptr_t)gfx;
        surface_w = *(const uint32_t *)(const void *)(g + 0x84);
        surface_h = *(const uint32_t *)(const void *)(g + 0x88);
    }
    struct pg_win *w = pg_main_win();
    NSSize logical = w ? w->logical_content_size : NSZeroSize;
    NSRect view = (w && w->render_view) ? [w->render_view frame] : NSZeroRect;
    fprintf(stderr, "compat32: geom %s: windowed=%u screen=%u offset=%d"
            " layout=%ux%u surface=%ux%u (gfx=0x%08x) logical=%.0fx%.0f"
            " view=%.0fx%.0f\n",
            when, (unsigned)base[0x2d7],
            *(const uint32_t *)(const void *)(base + 0x38),
            (int)*(const int32_t *)(const void *)(base + 0x578),
            *(const uint32_t *)(const void *)(base + 0x580),
            *(const uint32_t *)(const void *)(base + 0x584),
            surface_w, surface_h, gfx,
            logical.width, logical.height,
            view.size.width, view.size.height);
}
/* Put the letterbox on a given aspect and make the drawable follow it.
   Re-issuing glViewport here is required, not belt-and-braces: this guest
   calls glViewport exactly once, at startup, so nothing else would resize the
   viewport when the child view changes shape. */
static void pg_apply_letterbox_aspect(struct pg_win *w, NSSize aspect)
{
    if (!w || !w->window || w->overlay || aspect.width <= 0 || aspect.height <= 0) return;
    if (![[w->window contentView] isKindOfClass:[PeggleLetterboxView class]]) return;
    PeggleLetterboxView *letterbox = (PeggleLetterboxView *)[w->window contentView];
    if (!NSEqualSizes([letterbox contentAspect], aspect))
        [letterbox setContentAspect:aspect];
    [letterbox setNeedsLayout:YES];
    [letterbox layout];
    if (pg_gl) [pg_gl update];
    if (w->render_view) {
        NSRect backing = [w->render_view convertRectToBacking:[w->render_view bounds]];
        GLsizei bw = (GLsizei)lrint(backing.size.width);
        GLsizei bh = (GLsizei)lrint(backing.size.height);
        if (bw > 0 && bh > 0) glViewport(0, 0, bw, bh);
    }
}
/* The guest's layout span expressed in logical_content_size units, so the
   pointer can be mapped into the same space the guest hit-tests in.  Zero
   means "not in widescreen fullscreen; use the plain mapping". */
static double pg_mouse_span_x;
static double pg_mouse_origin_x;
static double pg_mouse_span_y;
static double pg_mouse_origin_y;
static NSSize pg_hit_test_size;
static void pg_sync_guest_layout(void)
{
    enum { kSexyAppBaseGlobal = 0x00534c30, kGuestHeapFloor = 0x02000000 };
    static NSSize applied;
    /* What logical_content_size was before LP32_FULLSCREEN_WIDE_LOGICAL
       replaced it.  It MUST be restored on the way out: an earlier attempt
       left logical stuck at the fullscreen value and windowed mode inherited
       16:10, drawing its 4:3 scene stretched. */
    static NSSize saved_logical;
    struct pg_win *w = pg_main_win();
    if (!w || !w->window || w->overlay) return;
    uint32_t app = *(const uint32_t *)(uintptr_t)kSexyAppBaseGlobal;
    if (app < kGuestHeapFloor) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)app;
    bool guest_windowed = base[0x2d7] != 0;
    uint32_t screen_w = *(const uint32_t *)(const void *)(base + 0x38);
    int32_t layout_x = *(const int32_t *)(const void *)(base + 0x578);
    uint32_t layout_w = *(const uint32_t *)(const void *)(base + 0x580);
    uint32_t layout_h = *(const uint32_t *)(const void *)(base + 0x584);
    NSSize want = (!guest_windowed && layout_w > 0 && layout_h > 0 &&
                   screen_w > 0 && layout_w > screen_w)
        ? NSMakeSize((CGFloat)layout_w, (CGFloat)layout_h) : NSZeroSize;
    if (NSEqualSizes(want, applied)) return;
    applied = want;
    if (want.width > 0) {
        pg_projection_x_scale = (double)screen_w / (double)layout_w;
        /* In NDC units: one layout pixel is 2/screen_w of the modelview's
           output, and the projection scale applies on top of that.  Measured
           need: -160 * (2/1600) * 0.8333 = -0.1667, which places the visible
           span at layout -160..1760 exactly.  Expressed in layout pixels this
           value blanks the screen. */
        pg_projection_x_translate =
            (double)layout_x * (2.0 / (double)screen_w) * pg_projection_x_scale;
        /* The pointer has to land in the same space or the cursor and the
           drawn UI disagree by exactly the ratio the projection was corrected
           by -- which is why the right-hand controls could not be reached.
           logical_content_size is the guest's window in its own units
           (1080 <-> screen_w), so carry the span and origin into those. */
        static int cursor_fix = -1;
        if (cursor_fix < 0)
            cursor_fix = getenv("LP32_FULLSCREEN_CURSOR_FIX") != NULL;
        pg_mouse_span_x = cursor_fix ? (double)layout_w : 0.0;
        pg_mouse_origin_x = 0.0;
        /* This older path only ever corrected X; leave Y on its original
           mapping here so the two knobs stay independently testable. */
        pg_mouse_span_y = 0.0;
        pg_mouse_origin_y = 0.0;
        static int wide_logical = -1;
        if (wide_logical < 0) {
            const char *want = getenv("LP32_FULLSCREEN_WIDE_LOGICAL");
            wide_logical = want ? strcmp(want, "0") != 0 : 1;
        }
        if (wide_logical) {
            if (saved_logical.width <= 0) saved_logical = w->logical_content_size;
            pg_hit_test_size = NSMakeSize((CGFloat)layout_w, (CGFloat)layout_h);
            pg_mouse_span_x = (double)layout_w;
            pg_mouse_origin_x = (double)layout_x;
            pg_mouse_span_y = (double)layout_h;
            pg_mouse_origin_y =
                (double)*(const int32_t *)(const void *)(base + 0x57c);
            uint32_t screen_h = *(const uint32_t *)(const void *)(base + 0x3c);
            int32_t layout_y = *(const int32_t *)(const void *)(base + 0x57c);
            if (screen_h > 0 && layout_h > 0) {
                pg_projection_y_scale = (double)screen_h / (double)layout_h;
                pg_projection_y_translate =
                    (double)layout_y * (2.0 / (double)screen_h) * pg_projection_y_scale;
            } else {
                pg_projection_y_scale = 1.0;
                pg_projection_y_translate = 0.0;
            }
            fprintf(stderr, "compat32: wide logical: window LEFT at %.0fx%.0f"
                    " (clicks only work at the true size); guest screen %ux%u,"
                    " layout %ux%u at (%d,%d); pointer span %.1f origin %.1f;"
                    " projection x %.4f/%+.4f y %.4f/%+.4f\n",
                    w->logical_content_size.width, w->logical_content_size.height,
                    *(const uint32_t *)(const void *)(base + 0x38), screen_h,
                    layout_w, layout_h, layout_x, layout_y,
                    pg_mouse_span_x, pg_mouse_origin_x,
                    pg_projection_x_scale, pg_projection_x_translate,
                    pg_projection_y_scale, pg_projection_y_translate);
        }
        pg_apply_letterbox_aspect(w, want);
        fprintf(stderr, "compat32: widescreen layout %ux%u at x=%d;"
                " projection ndc' = %.4f * ndc %+.4f; mouse span %.1f"
                " origin %.1f\n",
                layout_w, layout_h, layout_x, pg_projection_x_scale,
                pg_projection_x_translate, pg_mouse_span_x, pg_mouse_origin_x);
    } else {
        pg_projection_x_scale = 1.0;
        pg_projection_x_translate = 0.0;
        pg_projection_y_scale = 1.0;
        pg_projection_y_translate = 0.0;
        pg_mouse_span_x = 0.0;
        pg_mouse_origin_x = 0.0;
        pg_mouse_span_y = 0.0;
        pg_mouse_origin_y = 0.0;
        pg_hit_test_size = NSZeroSize;
        if (saved_logical.width > 0) {
            fprintf(stderr, "compat32: wide logical: restoring window to"
                    " %.0fx%.0f\n", saved_logical.width, saved_logical.height);
            w->logical_content_size = saved_logical;
            saved_logical = NSZeroSize;
        }
        pg_apply_letterbox_aspect(w, w->logical_content_size);
        fprintf(stderr, "compat32: layout back to windowed; projection scale 1.0\n");
    }
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
    double x = (screen.x - content.origin.x) * logical.width / content.size.width;
    if (pg_mouse_span_x > 0.0) {
        x = pg_mouse_origin_x +
            (screen.x - content.origin.x) * pg_mouse_span_x / content.size.width;
    }
    double y = (NSMaxY(content) - screen.y) * logical.height / content.size.height;
    if (pg_mouse_span_y > 0.0) {
        y = pg_mouse_origin_y +
            (NSMaxY(content) - screen.y) * pg_mouse_span_y / content.size.height;
    }
    return NSMakePoint(x, y);
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

/* Carbon fullscreen passes the physical display rectangle through
   SetWindowBounds even though the game still renders a 4:3 surface.  Keep the
   guest's logical drawable at its existing aspect ratio in that case; the
   host view and CGL backing surface perform the enlargement.  Chuzzle is the
   one title that intentionally switches to 5:4, so it is left free to accept
   either mode. */
static NSSize pg_logical_size_for_request(NSSize requested, NSSize previous)
{
    if (guest_display_mode_active()) return guest_display_mode;
    if (requested.width <= 0 || requested.height <= 0) return previous;


    bool preserve_four_three = lp32_profile()->title != LP32_TITLE_CHUZZLE;
    double aspect = requested.width / requested.height;
    if (!preserve_four_three ||
        (aspect >= PG_FOUR_THREE_MIN_ASPECT && aspect <= PG_FOUR_THREE_MAX_ASPECT)) {
        return requested;
    }
    if (previous.width > 0 && previous.height > 0) {
        static NSSize seen[8];
        static unsigned seen_count;
        bool known = false;
        for (unsigned i = 0; i < seen_count; ++i) {
            if (NSEqualSizes(seen[i], requested)) { known = true; break; }
        }
        if (!known) {
            if (seen_count < sizeof seen / sizeof seen[0]) {
                seen[seen_count++] = requested;
            }
            fprintf(stderr, "compat32: keeping logical %.0fx%.0f; ignored a "
                    "%.0fx%.0f request (aspect %.3f is not the 4:3 surface)\n",
                    previous.width, previous.height,
                    requested.width, requested.height, aspect);
        }
        return previous;
    }

    /* A first request can itself be the display's 16:9 fullscreen bounds.
       Select the largest 4:3 rectangle that fits rather than teaching the
       guest a stretched aspect ratio. */
    if (aspect > 1.45) {
        requested.width = floor(requested.height * 4.0 / 3.0);
    } else {
        requested.height = floor(requested.width * 3.0 / 4.0);
    }
    return requested;
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
    /* A Carbon/AGL title never reaches the NSOpenGLView path that installs
       this, so until now a Dock quit -- or the Quit item just added, which is
       bound to terminate: -- tore the process down through AppKit with no
       handler at all: no heap report, and no chance to persist anything. */
    install_termination_handler();
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
    if (!overlay) {
        fprintf(stderr, "compat32: create window handle=0x%08x request %.0fx%.0f at %.0f,%.0f\n",
                handle, content.size.width, content.size.height,
                content.origin.x, content.origin.y);
    }
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
        /* Peggle renders at 4:3.  Keep that constraint in our child view
           instead of on the NSWindow: AppKit applies a window-level aspect
           ratio while entering native fullscreen and consequently shrinks a
           1920x1080 display to a centred 1080x810 window.  The letterbox view
           below preserves the game's geometry while still letting fullscreen
           occupy the complete display. */
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
        w->logical_content_size = pg_logical_size_for_request(
            rect.size, w->logical_content_size);
    }
    NSRect frame = is_content ? [w->window frameRectForContentRect:rect] : rect;
    if (!w->overlay && guest_display_mode_active()) {
        /* Emulated fullscreen: the game pins its window to the display origin;
           keep it centred on the desktop instead. */
        NSRect screen = [[NSScreen screens][0] visibleFrame];
        frame.origin.x = floor(NSMidX(screen) - frame.size.width / 2);
        frame.origin.y = floor(NSMidY(screen) - frame.size.height / 2);
    }
    /* Native AppKit fullscreen owns the window frame.  Zuma continues to
       issue its old Carbon SetWindowBounds calls (1080x810) while the
       transition is active; applying them here makes the fullscreen window
       oscillate between 1920x1080 and 1080x832 and leaves the drawable at the
       low-resolution size.  Keep the guest's logical size for input and
       projection, but let AppKit retain the display-sized frame. */
    bool native_fullscreen = !w->overlay &&
        (([w->window styleMask] & NSWindowStyleMaskFullScreen) != 0);
    if (!native_fullscreen) [w->window setFrame:frame display:YES];
    if (!w->overlay) {
        [pg_gl update];
        if (native_fullscreen && [[w->window contentView] respondsToSelector:@selector(layout)]) {
            [[w->window contentView] layout];
        }
        NSRect c = pg_content_rect(w);
        NSRect actual = [w->window frame];
        fprintf(stderr, "compat32: window frame %.0f,%.0f,%.0f,%.0f"
                " (NSWindow #%ld frame %.0fx%.0f visible=%d logical %.0fx%.0f)\n",
                c.origin.x, pg_screen_top() - NSMaxY(c), c.size.width, c.size.height,
                (long)[w->window windowNumber], actual.size.width, actual.size.height,
                (int)[w->window isVisible],
                w->logical_content_size.width, w->logical_content_size.height);
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
/* SDL 1.2 destroys and rebuilds the GL context on every SDL_SetVideoMode, so
   a game written against it expects the context that comes back to start at
   its defaults with no textures, and reloads accordingly.  Ours survived the
   mode change, which left Chuzzle drawing with GL state and texture names it
   had already written off.  Drop the context so the next attach builds a new
   one; the proxy table still owns the old object, so nothing the guest holds
   dangles. */
static void pg_recreate_gl(void)
{
    if (!pg_gl) return;
    [NSOpenGLContext clearCurrentContext];
    [pg_gl clearDrawable];
    pg_gl = nil;
    pg_gl_handle = 0;
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

static void pg_apply_guest_display_mode(void);
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
    pg_apply_guest_display_mode();
    GLint one = 1;
    [pg_gl setValues:&one forParameter:NSOpenGLContextParameterSwapInterval];
}

/* AGL/CGL titles use the Carbon context in this file rather than the
   NSOpenGLView bridge.  Apply the same fixed-size backing surface used by the
   Cocoa path so an emulated 800x600 mode is scaled with letterboxing instead
   of rendering directly into the display's 16:9 drawable. */
static void pg_apply_guest_display_mode(void)
{
    if (!pg_gl) return;
    CGLContextObj context = [pg_gl CGLContextObj];
    if (!context) return;
    if (guest_display_mode_active()) {
        GLint dimensions[2] = {(GLint)guest_display_mode.width,
                               (GLint)guest_display_mode.height};
        CGLSetParameter(context, kCGLCPSurfaceBackingSize, dimensions);
        CGLEnable(context, kCGLCESurfaceBackingSize);
    } else {
        CGLDisable(context, kCGLCESurfaceBackingSize);
    }
    [pg_gl update];
    struct pg_win *w = pg_main_win();
    if (w && w->render_view && [w->render_view superview]) {
        NSView *container = [w->render_view superview];
        if ([container respondsToSelector:@selector(layout)]) {
            [container layout];
        }
    }
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
/* LP32_DUMP_FRAME=<prefix>: write the first few rendered frames straight out
   of the back buffer, before the swap.  A window that is not ordered on
   screen cannot be captured from outside the process, so this is the only way
   to see what the game actually drew rather than what the window server can
   image. */
static void pg_dump_frame_if_requested(void)
{
    static int enabled = -1;
    static unsigned dumped;
    static const char *prefix;
    static double next_dump;
    if (enabled < 0) {
        prefix = getenv("LP32_DUMP_FRAME");
        enabled = prefix != NULL;
    }
    if (!enabled || dumped >= 40) return;
    double dump_now = CFAbsoluteTimeGetCurrent();
    if (dumped > 0 && dump_now < next_dump) return;
    struct pg_win *w = pg_main_win();
    if (!w) return;
    NSSize size = [[w->window contentView] frame].size;
    int width = (int)size.width, height = (int)size.height;
    if (width <= 0 || height <= 0) return;
    unsigned char *pixels = malloc((size_t)width * height * 4);
    if (!pixels) return;
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s-%u.ppm", prefix, dumped);
    FILE *file = fopen(path, "wb");
    if (file) {
        /* GL hands back bottom-up rows; PPM wants them top-down. */
        fprintf(file, "P6\n%d %d\n255\n", width, height);
        for (int y = height - 1; y >= 0; --y) {
            for (int x = 0; x < width; ++x) {
                fwrite(pixels + ((size_t)y * width + x) * 4, 1, 3, file);
            }
        }
        fclose(file);
        unsigned long long sum = 0;
        for (size_t i = 0; i < (size_t)width * height * 4; ++i) sum += pixels[i];
        fprintf(stderr, "compat32: frame dump %s %dx%d mean=%.1f\n",
                path, width, height, (double)sum / ((double)width * height * 4));
    }
    free(pixels);
    next_dump = dump_now + 5.0;
    ++dumped;
}

/* LP32_PRESENT_STATS=<seconds>: report the presented frame rate.  The frame
   statistics in objc_bridge.m only cover present_frame_with_stats, which this
   path never reaches -- a title that presents through aglSwapBuffers or
   CGLFlushDrawable had no frame-rate measurement at all, so "it runs at 30
   FPS" could not be told apart from vsync halving a frame that just missed
   its deadline. */
/* Time the Carbon pump spends parked in nextEventMatchingMask, accumulated
   between reports.  A title whose event-loop timer fires on every pass runs
   exactly one guest frame per _ReceiveNextEvent, so an idle wait there is a
   direct per-frame tax rather than overlapped work. */
static double pg_pump_wait_total;
static double pg_pump_timeout_last;
static void pg_note_presented(void)
{
    static double interval = -1.0;
    static double window_start;
    static unsigned long frames;
    if (interval < 0.0) {
        const char *text = getenv("LP32_PRESENT_STATS");
        interval = text ? atof(text) : 0.0;
        if (text && interval <= 0.0) interval = 5.0;
    }
    if (interval == 0.0) return;
    double now = CFAbsoluteTimeGetCurrent();
    ++frames;
    if (!window_start) { window_start = now; return; }
    if (now - window_start < interval) return;
    fprintf(stderr,
            "compat32: present fps=%.1f frames=%lu over %.1fs "
            "pump-wait=%.2fms/frame guest-timeout=%.3fs\n",
            (double)frames / (now - window_start), frames, now - window_start,
            frames ? pg_pump_wait_total * 1000.0 / (double)frames : 0.0,
            pg_pump_timeout_last);
    window_start = now;
    frames = 0;
    pg_pump_wait_total = 0.0;
}
static void pg_present(void)
{
    /* Keep Carbon/AGL swaps on the same monotonic sequence as Cocoa swaps so
       frame diagnostics, pacing and the cached FPS query see every frame. */
    ++objc_bridge_swap_count;
    /* Persist the guest registry from the swap path rather than from a
       shutdown hook or the Carbon pump: this title quits through AppKit's
       terminate: (host exit(), no guest teardown), and it only calls
       RunApplicationEventLoop long after startup, so neither is dependable on
       its own.  The period is deliberately long.  Each flush walks the tree
       into roughly sixteen CoreFoundation objects (twelve node dictionaries
       and their values), the guest dutifully CFReleases every one, and the
       bridge's _CFRelease drops that on the floor -- so each flush costs
       sixteen permanent slots out of the 2048-entry proxy pool.  At a
       twenty-second period that exhausted the pool in about forty minutes of
       play, after which proxy_for_object returns 0 and the first CF call to
       dereference it killed the process.  Until _CFRelease reclaims slots,
       this interval is what keeps that bounded; the shutdown hooks are what
       actually make settings durable. */
    {
        static double next_registry_flush;
        static double registry_flush_period = -1.0;
        if (registry_flush_period < 0.0) {
            /*
             * Off unless asked for.  Every periodic flush measured so far
             * serialised an EMPTY tree over a populated RegistryData.xml --
             * the guest re-read 218 bytes after each one -- and only the
             * shrink guard in compat_runtime32_flush_guest_registry put the
             * real contents back.  Until that is understood, a timer that
             * writes garbage and relies on a rollback has no business running
             * for a player; the shutdown hooks are where durability lives.
             * LP32_REGISTRY_FLUSH_SECS=<n> re-enables it for investigation.
             */
            const char *text = getenv("LP32_REGISTRY_FLUSH_SECS");
            registry_flush_period = (text && text[0]) ? strtod(text, NULL) : 0.0;
            if (registry_flush_period < 0.0) registry_flush_period = 0.0;
        }
        double flush_now = CFAbsoluteTimeGetCurrent();
        if (registry_flush_period > 0.0 && flush_now >= next_registry_flush) {
            next_registry_flush = flush_now + registry_flush_period;
            compat_runtime32_flush_guest_registry("frame");
        }
    }
    /* The existing proxy census lives in the Cocoa view's swapBuffers, which a
       Carbon/AGL title never reaches; report it from the path this game does
       use so pool growth is observable here at all. */
    if (getenv("LP32_TRACE_OBJC_PROXIES") &&
        (objc_bridge_swap_count <= 10 || objc_bridge_swap_count % 300 == 0)) {
        /* free= matters as much as persistent=: proxy_count is a high-water
           mark that never falls, so without the free-list depth a pool that is
           recycling correctly looks identical to one that is not allocating. */
        fprintf(stderr, "compat32: Objective-C proxies swap=%llu persistent=%u "
                "free=%u event=%u\n", (unsigned long long)objc_bridge_swap_count,
                proxy_count, proxy_free_count, event_proxy_count);
    }
    pg_dump_frame_if_requested();
    struct pg_win *window = pg_main_win();
    /* Keep Carbon/AGL presents on the same accounting and pacing path as the
       Cocoa OpenGLView.  Previously this direct flush bypassed the frame
       profiler entirely, so an effect hitch could only be observed as a
       vague FPS drop and the bridge could not apply its pacing policy. */
    present_frame_with_stats(pg_gl, window ? window->window : nil, true);
    pg_note_presented();
}
/* An event's timestamp has to sit on the same clock the bridge already
   answers GetCurrentEventTime with, or a game that compares the two reads a
   difference of decades.  That answer is CFAbsoluteTime, so this is too --
   NSEvent's own timestamps are seconds since boot and would not compare. */
static double pg_event_time_now(void)
{
    return CFAbsoluteTimeGetCurrent();
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
    event->when = pg_event_time_now();
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

/* AppKit asserts when mouse-only accessors such as -clickCount or
   -buttonNumber are sent to a scroll-wheel event.  Carbon asks for those
   parameters from a shared event path, so keep the query safe for every
   NSEvent kind and let the caller receive the neutral value for events that
   do not carry a mouse button. */
static bool pg_event_has_mouse_button(NSEvent *event)
{
    if (!event) return false;
    switch ([event type]) {
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            return true;
        default:
            return false;
    }
}

static int peggle_window_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
#define IS(s) (strcmp(n, s) == 0)
#define P(i) ((void *)(uintptr_t)a[i])
#define O(i) object_for_argument(a[i])
#define OUT(i, v) do { if (a[i]) *(uint32_t *)P(i) = (uint32_t)(v); } while (0)
#define RET(x) do { *r = (uint64_t)(x); return 1; } while (0)
    /*
     * Carbon's modifier queries.  The engine's deferred-message pump asks for
     * these while processing every key, and an unimplemented import does not
     * merely answer zero: the trap sets lp32_leave_guest and abandons the
     * guest call, so the pump dies mid-keystroke and the character it had
     * already fetched is dropped.  That is the whole of "the keyboard does
     * nothing" -- mouse and window events never take this path.  The engine
     * reads the answer twice, testing cmdKey and then optionKey, so it has to
     * be the real modifier state rather than a constant.
     */
    if (IS("_GetCurrentEventKeyModifiers") || IS("_GetCurrentKeyModifiers")) {
        RET(pg_carbon_modifiers([NSEvent modifierFlags]));
    }
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
        /* Report the state after ordering: the frame line printed when the
           bounds were set runs before this, so it says nothing about whether
           the window ever reached the screen. */
        if (w && !w->overlay) {
            NSRect frame = [w->window frame];
            fprintf(stderr, "compat32: %s handle=0x%08x -> NSWindow #%ld "
                    "visible=%d onscreen=%d key=%d %.0fx%.0f at %.0f,%.0f "
                    "policy=%ld\n", n + 1, a[0], (long)[w->window windowNumber],
                    (int)[w->window isVisible], (int)[w->window isOnActiveSpace],
                    (int)[w->window isKeyWindow], frame.size.width,
                    frame.size.height, frame.origin.x, frame.origin.y,
                    (long)[NSApp activationPolicy]);
        }
        RET(0);
    }
    if (IS("_HideWindow")) { struct pg_win *w = pg_win_for(a[0]); if (w) [w->window orderOut:nil]; RET(0); }
    /*
     * The Carbon window calls this title uses that the earlier games did not.
     * Its resize path is gated behind IsWindowVisible, so these are what let
     * it reach its own SetWindowBounds and size the window past the degenerate
     * rect CreateNewWindow starts from.
     */
    if (IS("_SetWindowProperty")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (!w || a[3] > sizeof(w->properties[0].bytes)) RET(-1);
        unsigned slot = w->property_count;
        for (unsigned i = 0; i < w->property_count; ++i) {
            if (w->properties[i].creator == a[1] && w->properties[i].tag == a[2]) slot = i;
        }
        if (slot >= sizeof(w->properties) / sizeof(w->properties[0])) RET(-108);
        if (slot == w->property_count) ++w->property_count;
        w->properties[slot].creator = a[1];
        w->properties[slot].tag = a[2];
        w->properties[slot].size = a[3];
        if (a[4]) memcpy(w->properties[slot].bytes, P(4), a[3]);
        RET(0);
    }
    if (IS("_GetWindowProperty")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w) {
            for (unsigned i = 0; i < w->property_count; ++i) {
                if (w->properties[i].creator != a[1] || w->properties[i].tag != a[2]) continue;
                uint32_t size = w->properties[i].size < a[3] ? w->properties[i].size : a[3];
                if (a[5]) memcpy(P(5), w->properties[i].bytes, size);
                OUT(4, size);
                RET(0);
            }
        }
        OUT(4, 0);
        RET(-5602); /* errWindowPropertyNotFound */
    }
    if (IS("_GetWindowPortBounds")) {
        struct pg_win *w = pg_win_for(a[0]);
        /* Port bounds are content-relative: the origin is always zero. */
        int16_t q[4] = {0, 0, 600, 800};
        if (w) {
            NSSize size = w->logical_content_size.width > 0 ?
                w->logical_content_size : pg_content_rect(w).size;
            q[2] = (int16_t)size.height;
            q[3] = (int16_t)size.width;
        }
        if (a[1]) memcpy(P(1), q, sizeof(q));
        RET(0);
    }
    if (IS("_IsWindowVisible")) {
        struct pg_win *w = pg_win_for(a[0]);
        RET(w && [w->window isVisible]);
    }
    if (IS("_IsWindowActive")) {
        struct pg_win *w = pg_win_for(a[0]);
        RET(w && [w->window isKeyWindow]);
    }
    if (IS("_IsValidWindowPtr")) RET(a[0] != 0);
    if (IS("_IsWindowCollapsed")) RET(0);
    if (IS("_GetWindowClass")) { OUT(1, kDocumentWindowClass); RET(0); }
    if (IS("_GetUserFocusWindow")) {
        struct pg_win *w = pg_main_win();
        RET(w ? w->handle : 0);
    }
    if (IS("_SetUserFocusWindow")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w) [w->window makeKeyWindow];
        RET(0);
    }
    if (IS("_MoveWindow")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w) {
            NSRect frame = [w->window frame];
            /* Carbon places the structure region's top-left in screen
               coordinates that grow downwards. */
            frame.origin.x = (int16_t)a[1];
            frame.origin.y = pg_screen_top() - (int16_t)a[2] - frame.size.height;
            [w->window setFrameOrigin:frame.origin];
            if (a[3]) [w->window makeKeyAndOrderFront:nil];
        }
        RET(0);
    }
    if (IS("_RepositionWindow")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w && !w->overlay) [w->window center];
        RET(0);
    }
    if (IS("_GetWindowResizeLimits")) {
        /* Point is {v, h} as two 16-bit fields. */
        if (a[1]) { int16_t min[2] = {1, 1}; memcpy(P(1), min, sizeof(min)); }
        if (a[2]) { int16_t max[2] = {16384, 16384}; memcpy(P(2), max, sizeof(max)); }
        RET(0);
    }
    /* The bridge redraws from the game's own render loop, so an invalidation
       has nothing to schedule; the standard handler it wants to install does
       not exist here either, and the game registers its own handlers anyway. */
    if (IS("_InvalWindowRect")) RET(0);
    if (IS("_InstallStandardEventHandler")) RET(0);
    if (IS("_ChangeWindowAttributes")) RET(0);
    if (IS("_EqualRect")) {
        const int16_t *left = P(0), *right = P(1);
        RET(left && right && memcmp(left, right, 8) == 0);
    }
    if (IS("_HIPointConvert")) {
        enum { kSpaceScreen = 2, kSpaceWindow = 3 };
        float *point = a[0] ? P(0) : NULL;
        if (point && a[1] != a[3]) {
            struct pg_win *w = pg_win_for(a[4] ? a[4] : a[2]);
            if (w) {
                NSRect c = pg_content_rect(w);
                float left = (float)c.origin.x;
                float top = (float)(pg_screen_top() - NSMaxY(c));
                if (a[1] == kSpaceScreen && a[3] == kSpaceWindow) {
                    point[0] -= left; point[1] -= top;
                } else if (a[1] == kSpaceWindow && a[3] == kSpaceScreen) {
                    point[0] += left; point[1] += top;
                }
            }
        }
        RET(0);
    }
    if (IS("_SetWindowTitleWithCFString")) { struct pg_win *w = pg_win_for(a[0]); if (w) [w->window setTitle:@"PeggleSilicon"]; RET(0); }
    if (IS("_SizeWindow")) {
        struct pg_win *w = pg_win_for(a[0]);
        if (w) {
            NSSize size = NSMakeSize((int16_t)a[1], (int16_t)a[2]);
            if (!w->overlay && size.width > 0 && size.height > 0) {
                w->logical_content_size = pg_logical_size_for_request(
                    size, w->logical_content_size);
            }
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
            bool widen = pg_hit_test_size.width > 0 &&
                         (a[1] == kPgWindowStructureRgn ||
                          a[1] == kPgWindowContentRgn);
            NSSize reported = widen ? pg_hit_test_size : w->logical_content_size;
            if (a[1] == 0) {
                q[0] = q[1] = q[2] = q[3] = 0;
            } else if (!w->overlay && reported.width > 0 && reported.height > 0) {
                q[2] = q[0] + (int16_t)reported.height;
                q[3] = q[1] + (int16_t)reported.width;
            }
        }
        if (a[2]) memcpy(P(2), q, sizeof(q));
        RET(0);
    }
    if (IS("_SetWindowBounds")) {
        struct pg_win *w = pg_win_for(a[0]);
        /* Both regions are the content region here, so a bounds the engine
           read back and set again round-trips instead of creeping upwards.
           GetWindowBounds above reports the content region for both, for the
           hit-testing reason described there, and the Carbon titles set back
           what they read.

           Zuma's Revenge is the exception: it reaches Carbon through SmartDX,
           whose one call site passes kWindowStructureRgn and means it.  Its
           Win32 side calls SetWindowPos(x=0, y=-22, cx, cy=client+22) -- the
           standard idiom of lifting the frame by the caption so the client
           lands at the origin -- and SmartDX turns that straight into a
           structure Rect.  Taking it as a content rect kept the caption: the
           game asked for an 810-tall client and we recorded 832, and asked for
           a 1080-tall one and we recorded 1102.  Convert it here instead, so
           the logical size is the client the game actually wants. */
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
        NSRect hit = NSZeroRect;
        bool inside = false;
        if (w) {
            hit = pg_content_rect(w);
            if (pg_mouse_span_x > 0.0) {
                hit.origin.x += pg_mouse_origin_x;
                hit.size.width = pg_mouse_span_x;
            }
            if (pg_mouse_span_y > 0.0) {
                hit.origin.y = NSMaxY(hit) - (pg_mouse_origin_y + pg_mouse_span_y);
                hit.size.height = pg_mouse_span_y;
            }
            inside = NSPointInRect(screen, hit);
        }
        if (inside) { OUT(1, w->handle); RET(inContent); }
        OUT(1, 0);
        RET(inDesk);
    }
    if (IS("_GetGlobalMouse")) {
        NSPoint live = [NSEvent mouseLocation];
        static int script_cursor = -1;
        if (script_cursor < 0) script_cursor = getenv("LP32_SCRIPT_CURSOR") != NULL;
        if (script_cursor && pg_script_cursor_valid) live = pg_script_cursor;
        NSPoint screen = pg_virtual_screen_mouse_point(pg_main_win(), live);
        NSPoint p = pg_qd_point(screen);
        if (pg_tracing()) {
            static unsigned polled;
            if (polled < 40) {
                ++polled;
                NSPoint actual = [NSEvent mouseLocation];
                fprintf(stderr,
                        "compat32: GetGlobalMouse -> qd=(%.0f,%.0f) "
                        "livePointer=(%.0f,%.0f) scripted=%s\n",
                        p.x, p.y, actual.x, actual.y,
                        (script_cursor && pg_script_cursor_valid) ? "yes" : "no");
            }
        }
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
    /*
     * The caller dereferences this twice -- **gdh, then the screen bounds in
     * the GDevice's gdRect at +0x22 -- and uses them to place its window, so
     * neither a bogus handle (it faults) nor a failure (it leaves the rect
     * uninitialised and the window lands thousands of pixels off-screen) will
     * do.  Hand back a real one-screen GDevice whose gdRect is the display
     * the bridge presents.  Classic layout: Rect {top, left, bottom, right}
     * as four shorts starting at offset 0x22.
     */
    if (IS("_DMGetGDeviceByDisplayID")) {
        static uint32_t device_handle;
        if (!device_handle) {
            uint32_t device = compat_runtime32_allocate(0x40, 1);
            uint32_t master = compat_runtime32_allocate(4, 1);
            if (device && master) {
                NSSize size = guest_display_mode_active()
                    ? NSMakeSize(guest_display_mode.width, guest_display_mode.height)
                    : [[NSScreen screens][0] frame].size;
                int16_t *bounds = (void *)(uintptr_t)(device + 0x22);
                bounds[0] = 0;
                bounds[1] = 0;
                bounds[2] = (int16_t)size.height;
                bounds[3] = (int16_t)size.width;
                *(uint32_t *)(uintptr_t)master = device;
                device_handle = master;
            }
        }
        OUT(1, device_handle);
        RET(device_handle ? 0 : -1);
    }
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
        /* Which (class, kind) pairs the guest actually asks for, against which
           target: pg_dispatch_event matches both exactly, so a handler the game
           registered for a kind the bridge never queues is silently dead. */
        if (pg_tracing()) {
            for (uint32_t j = 0; j < pg_handlers[i].count; ++j) {
                uint32_t cls = pg_handlers[i].types[j].eventClass;
                fprintf(stderr,
                        "compat32: InstallEventHandler target=0x%08x '%.4s'/%u\n",
                        a[0], (const char[]){cls >> 24, cls >> 16, cls >> 8, cls},
                        pg_handlers[i].types[j].eventKind);
            }
        }
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
    /*
     * Zuma's Revenge hands control to Carbon rather than pumping events
     * itself: it installs a timer as its frame clock and calls
     * RunApplicationEventLoop, which only returns once the application
     * quits.  Carbon's loop is receive-then-dispatch, and ReceiveNextEvent
     * below already is the whole pump (guest timer, cursor, pending queue,
     * one NSEvent), so run that and send whatever it yields to the handlers
     * the game registered.  Reusing it keeps one pump rather than a second
     * one that could drift out of step with it.
     */
    if (IS("_RunApplicationEventLoop")) {
        pg_start_app();
        /* ReceiveNextEvent reports the event through a guest pointer, so the
           slot it writes to has to live in the guest's address space. */
        static uint32_t event_slot;
        if (!event_slot) event_slot = compat_runtime32_allocate(4, 1);
        if (!event_slot) RET(0);
        pg_event_loop_running = true;
        while (pg_event_loop_running) {
            double timeout = 0.01;
            uint32_t inner_arguments[6] = {0, 0, 0, 0, 1, event_slot};
            memcpy(inner_arguments + 2, &timeout, sizeof(timeout));
            *(uint32_t *)(uintptr_t)event_slot = 0;
            uint64_t inner_result = 0;
            if (!peggle_window_dispatch("_ReceiveNextEvent", inner_arguments,
                                        &inner_result)) {
                break;
            }
            if ((uint32_t)inner_result != 0) continue; /* timed out: no event */
            uint32_t handle = *(uint32_t *)(uintptr_t)event_slot;
            /* Target 1 is the application target, which pg_dispatch_event
               treats as "any registered handler". */
            if (handle) pg_dispatch_event(handle, 1);
        }
        /* The application quit.  Persist the registry here as well as at
           _exit: this is the boundary a Carbon title reaches on a normal
           quit, and the settings and selected profile are still only in
           memory until something calls the flush the game itself omits. */
        compat_runtime32_flush_guest_registry("event loop quit");
        RET(0);
    }
    if (IS("_QuitApplicationEventLoop")) { pg_event_loop_running = false; RET(0); }
    /* The game's frame timer is installed with a zero interval, which makes
       it one-shot; like any Carbon frame loop it re-arms itself from inside
       its own callback, so this is what actually paces rendering. */
    if (IS("_SetEventLoopTimerNextFireTime")) {
        double delay;
        memcpy(&delay, a + 1, sizeof(delay));
        pg_timer_next = CFAbsoluteTimeGetCurrent() + (delay > 0 ? delay : 0);
        RET(0);
    }
    if (IS("_CreateEvent")) { OUT(6, pg_make_event(a[1], a[2], nil)); RET(0); }
    if (IS("_ReleaseEvent") || IS("_RetainEvent")) RET(IS("_RetainEvent") ? a[0] : 0);
    if (IS("_GetEventClass")) { struct pg_event *e = pg_event_for(a[0]); RET(e ? e->cls : 0); }
    if (IS("_GetEventKind")) { struct pg_event *e = pg_event_for(a[0]); RET(e ? e->kind : 0); }
    /* EventTime is a double, so it leaves through the x87 result path rather
       than in eax.  pg_fp_double lives in the file that includes this one, so
       the runtime's own entry point is what is reachable here. */
    if (IS("_GetEventTime")) {
        struct pg_event *e = pg_event_for(a[0]);
        RET(compat_runtime32_return_double(e ? e->when : pg_event_time_now()));
    }
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
                uint16_t button = (uint16_t)(pg_event_has_mouse_button(native) ? [native buttonNumber] + 1 : 0);
                memcpy(temp, &button, 2);
                length = 2;
            } else if (a[1] == kEventParamClickCount) {
                uint32_t count = pg_event_has_mouse_button(native) ? (uint32_t)[native clickCount] : 0;
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
            } else if (a[1] == kEventParamMouseDelta) {
                /* Carbon reports a mouse delta on every mouse event, not just
                   wheel events, and the guest reads it.  A census of every
                   GetEventParameter call across the traced runs found this was
                   the ONE parameter answered "missing" -- 141 times -- while
                   all thirteen others returned ok.  Supply it from the event's
                   own deltas so a handler that treats a failed read as a
                   malformed event is not handed one. */
                CGFloat dx = native ? [native deltaX] : 0;
                CGFloat dy = native ? [native deltaY] : 0;
                if (a[2] == typeQDPoint) {
                    int16_t q[2] = {(int16_t)dy, (int16_t)dx};   /* vertical first */
                    memcpy(temp, q, 4);
                    length = 4;
                } else {
                    float q[2] = {(float)dx, (float)dy};
                    memcpy(temp, q, 8);
                    length = 8;
                }
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
            /* A zero interval means "one shot" in Carbon, and a game that
               wants another frame re-arms the timer from inside its own
               callback.  Zuma's Revenge never does: it installs the timer
               with a zero delay and a zero interval and expects to be called
               back for every frame, so honouring the one-shot rule left it
               idling in the pump forever after a single callback.  Treat a
               zero interval as "every pass" for it, and leave the rule alone
               for the titles that do re-arm. */
            bool repeat_without_interval =
                lp32_profile()->title == LP32_TITLE_ZUMAS_REVENGE;
            pg_timer_next = pg_timer_interval > 0 ? now + pg_timer_interval
                          : (repeat_without_interval ? now : now + 3600);
            uint32_t arguments[] = {3, pg_timer_user};
            if (pg_timer_depth > 0) ++pg_pump_reentries;
            ++pg_timer_depth;
            compat_runtime32_call(pg_timer_callback, arguments, 2);
            --pg_timer_depth;
        }
        pg_update_cursor();
        if (pg_close_unhandled) { pg_close_unhandled = false; pg_queue_command(kHICommandQuit); }
        pg_sync_guest_layout();
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
        pg_pump_timeout_last = timeout;
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:timeout > 0 ? fmin(timeout, 0.01) : 0];
        double wait_start = CFAbsoluteTimeGetCurrent();
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
        pg_pump_wait_total += CFAbsoluteTimeGetCurrent() - wait_start;
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
        if (pg_timer_depth > 0 && cls == kEventClassMouse) {
            ++pg_nested_mouse_dispatches;
            pg_last_nested_mouse_kind = kind;
            /* A frame callback is allowed to run a nested Carbon pump while
               it is updating the menu.  Queue every mouse event, including
               movement, and let the pending-event check above hold it until
               the callback has returned and the widget graph is stable. */
            pg_queue_event(cls, kind, e);
            OUT(5, 0);
            RET(eventLoopTimedOutErr);
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
    /*
     * SmartDX asks AGL what the renderer can do before it decides which
     * Direct3D capabilities to advertise.  It walks properties 0x64 upwards
     * into consecutive words and only records an error when a query returns
     * false, so every one of these has to succeed: failing them leaves it
     * believing it has no usable renderer.  There is a single shared context
     * here, so the renderer info is a token rather than a real list.
     */
    if (IS("_aglGetCurrentContext")) {
        RET([NSOpenGLContext currentContext] == pg_gl ? pg_gl_context_handle() : 0);
    }
    if (IS("_aglQueryRendererInfo")) RET(kPgRendererInfo);
    if (IS("_aglDestroyRendererInfo")) RET(1);
    if (IS("_aglDescribeRenderer")) {
        uint32_t *value = a[2] ? P(2) : NULL;
        if (!value) RET(0);
        /* AGL reports memory in bytes.  The rest are capability counts the
           shared context genuinely supports. */
        switch (a[1]) {
        case 0x64: *value = 256u * 1024 * 1024; break; /* texture memory */
        case 0x65: *value = 256u * 1024 * 1024; break; /* video memory */
        case 0x66: *value = 1; break;                  /* accelerated */
        case 0x67: *value = 1; break;                  /* compliant */
        case 0x68: *value = 32; break;                 /* colour bits */
        case 0x69: *value = 24; break;                 /* depth bits */
        default: *value = 1; break;
        }
        RET(1);
    }
    if (IS("_aglEnable") || IS("_aglDisable")) RET(1);
    if (IS("_aglSetFullScreen")) { pg_attach_gl(); RET(1); }
    if (IS("_aglGetVersion")) {
        /* AGL 3.x, which is what every renderer this bridge stands on is. */
        if (a[0]) *(uint32_t *)P(0) = 3;
        if (a[1]) *(uint32_t *)P(1) = 0;
        RET(0);
    }
    if (IS("_aglSurfaceTexture")) RET(0);
    if (IS("_aglUseFont")) RET(0);
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
