/*
 * Guest Objective-C class synthesis (old fragile ABI).
 *
 * Bejeweled 3 is a Cocoa app whose executable DEFINES its own Objective-C
 * classes (SexyApplicationDelegate, SexyWindow : NSWindow, SexyOpenGLView :
 * NSOpenGLView, ...) in the __OBJC segment.  dyld would normally register
 * these when it maps the image, but our loader maps the image by hand, so the
 * host runtime never sees them and objc_getClass() returns nil.
 *
 * We synthesize a real host class for each guest class so that (a) the guest's
 * own objc_msgSend on its class references resolves, and (b) host AppKit can
 * message instances (as an app delegate / window / view).  The 32-bit guest
 * ivar layout is incompatible with the 64-bit host object layout, so each
 * instance is a COMPANION PAIR: a guest-memory block with the guest's 32-bit
 * ivar layout (what the guest IMPs read/write via self), plus a host object of
 * the synthesized class (what host Cocoa messages).  A generic trampoline is
 * installed for every guest method; when host code messages the companion it
 * forwards to the guest IMP with self = the guest block, and when guest code
 * messages the block it is routed to the guest IMP directly (see the
 * _objc_msgSend handler).  Only int/pointer/object argument and return types
 * are marshalled; float/double/struct-by-value method arguments are not yet
 * handled (noted where they block progress).
 */

/* Old fragile-ABI objc_class field offsets (32-bit words). */
enum {
    OC_ISA = 0, OC_SUPER = 1, OC_NAME = 2, OC_VERSION = 3, OC_INFO = 4,
    OC_INSTANCE_SIZE = 5, OC_IVARS = 6, OC_METHODLISTS = 7, OC_CACHE = 8,
    OC_PROTOCOLS = 9,
};

struct guest_objc_method {
    const char *selector;
    const char *types;
    uint32_t imp;
};

struct guest_objc_class {
    uint32_t class_addr;
    uint32_t meta_addr;
    const char *name;
    uint32_t instance_size;
    Class host_class;
    bool use_forwarder;        /* NSObject-rooted, non-NSResponder: forward */
    struct guest_objc_method instance_methods[96];
    uint32_t instance_method_count;
    struct guest_objc_method class_methods[32];
    uint32_t class_method_count;
};

static struct guest_objc_class g_guest_classes[32];
static uint32_t g_guest_class_count;
static bool g_guest_classes_ready;
/* Set while dispatching an objc_msgSendSuper so an instance message skips the
   guest override and reaches the host superclass via the companion. */
static int g_force_super;

/* Companion instances: guest block <-> host object of the synthesized class. */
struct guest_objc_instance {
    uint32_t block;            /* guest-memory instance (32-bit ivar layout) */
    id companion;              /* host object of the synthesized class */
    struct guest_objc_class *cls;
};
static struct guest_objc_instance g_guest_instances[1024];
static uint32_t g_guest_instance_count;
static pthread_mutex_t g_guest_instance_lock = PTHREAD_MUTEX_INITIALIZER;

static struct guest_objc_class *guest_class_for_host(Class c)
{
    for (uint32_t i = 0; i < g_guest_class_count; ++i) {
        if (g_guest_classes[i].host_class == c) return &g_guest_classes[i];
    }
    return NULL;
}

static struct guest_objc_class *guest_class_named(const char *name)
{
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_guest_class_count; ++i) {
        if (strcmp(g_guest_classes[i].name, name) == 0) return &g_guest_classes[i];
    }
    return NULL;
}

/* Match a receiver that is a guest class object address (the class or its
   metaclass struct), used when the guest holds a direct class pointer. */
static struct guest_objc_class *guest_class_for_addr(uint32_t addr)
{
    if (!addr) return NULL;
    for (uint32_t i = 0; i < g_guest_class_count; ++i) {
        if (g_guest_classes[i].class_addr == addr ||
            g_guest_classes[i].meta_addr == addr) return &g_guest_classes[i];
    }
    return NULL;
}

/* Is this guest address one of our synthesized instance blocks? */
static struct guest_objc_instance *guest_instance_for_block(uint32_t block)
{
    if (!block) return NULL;
    struct guest_objc_instance *found = NULL;
    pthread_mutex_lock(&g_guest_instance_lock);
    for (uint32_t i = 0; i < g_guest_instance_count; ++i) {
        if (g_guest_instances[i].block == block) { found = &g_guest_instances[i]; break; }
    }
    pthread_mutex_unlock(&g_guest_instance_lock);
    return found;
}

static struct guest_objc_instance *guest_instance_for_companion(id companion)
{
    if (!companion) return NULL;
    struct guest_objc_instance *found = NULL;
    pthread_mutex_lock(&g_guest_instance_lock);
    for (uint32_t i = 0; i < g_guest_instance_count; ++i) {
        if (g_guest_instances[i].companion == companion) { found = &g_guest_instances[i]; break; }
    }
    pthread_mutex_unlock(&g_guest_instance_lock);
    return found;
}

/* Accessors used by object_for_receiver / proxy_for_object (forward-declared
   near the top of objc_bridge.m so those earlier functions can call them). */
id guest_companion_for_block(uint32_t block)
{
    struct guest_objc_instance *inst = guest_instance_for_block(block);
    return inst ? inst->companion : nil;
}

uint32_t guest_block_for_companion(id obj)
{
    struct guest_objc_instance *inst = guest_instance_for_companion(obj);
    return inst ? inst->block : 0;
}

static const struct guest_objc_method *guest_find_method(
    const struct guest_objc_class *cls, const char *selector, bool class_method)
{
    if (!cls || !selector) return NULL;
    const struct guest_objc_method *list = class_method ? cls->class_methods : cls->instance_methods;
    uint32_t count = class_method ? cls->class_method_count : cls->instance_method_count;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(list[i].selector, selector) == 0) return &list[i];
    }
    return NULL;
}

/* Parse one objc_method_list (obsolete, count, {name,types,imp}[count]). */
static void guest_collect_methods(uint32_t list_addr, struct guest_objc_method *out,
                                  uint32_t *out_count, uint32_t capacity)
{
    if (!list_addr) return;
    const uint32_t *list = (const void *)(uintptr_t)list_addr;
    uint32_t count = list[1];
    for (uint32_t i = 0; i < count && *out_count < capacity; ++i) {
        const uint32_t *m = list + 2 + i * 3;
        out[*out_count].selector = (const char *)(uintptr_t)m[0];
        out[*out_count].types = (const char *)(uintptr_t)m[1];
        out[*out_count].imp = m[2];
        ++(*out_count);
    }
}

/* Skip ObjC type qualifiers and one complete type; return pointer past it. */
static const char *guest_skip_one_type(const char *t)
{
    while (*t && strchr("rnNoORV", *t)) ++t;
    if (!*t) return t;
    char c = *t++;
    if (c == '^') return guest_skip_one_type(t);
    if (c == '{' || c == '(') {
        int depth = 1;
        while (*t && depth) { if (*t == '{' || *t == '(') ++depth; else if (*t == '}' || *t == ')') --depth; ++t; }
        return t;
    }
    if (c == '[') { while (*t && *t != ']') ++t; if (*t) ++t; return t; }
    if (c == 'b') { while (isdigit((unsigned char)*t)) ++t; return t; }
    return t;
}

enum guest_arg_kind { GA_INT, GA_OBJECT, GA_POINTER, GA_UNSUPPORTED };

static enum guest_arg_kind guest_arg_kind(const char *t)
{
    while (*t && strchr("rnNoORV", *t)) ++t;
    switch (*t) {
    case '@': return GA_OBJECT;
    case '#': return GA_OBJECT;   /* Class */
    case ':': return GA_POINTER;  /* SEL */
    case '^': case '*': return GA_POINTER;
    case 'c': case 'C': case 's': case 'S': case 'i': case 'I':
    case 'l': case 'L': case 'B': return GA_INT;
    case 'q': case 'Q': return GA_INT; /* 64-bit: only low word forwarded */
    default: return GA_UNSUPPORTED;    /* f d { ( [ etc. */
    }
}

/*
 * Invoke a guest instance/class method IMP.  `self_value` is the guest block
 * (instance) or guest class addr (class method).  Host `id` object arguments
 * are converted to guest handles; pointers/ints pass through.  Returns the
 * guest 32-bit result; the caller converts it per the return encoding.
 */
static uint32_t guest_invoke(uint32_t imp, uint32_t self_value, const char *selector,
                             const char *types, intptr_t *args, uint32_t host_argc)
{
    uint32_t words[10];
    uint32_t n = 0;
    words[n++] = self_value;
    words[n++] = guest_selector(selector);
    /* Walk the method arg types after return, size, self(@), _cmd(:). */
    const char *t = types ? types : "";
    t = guest_skip_one_type(t);              /* return type */
    while (isdigit((unsigned char)*t)) ++t;  /* frame size */
    t = guest_skip_one_type(t);              /* self */
    while (isdigit((unsigned char)*t)) ++t;
    t = guest_skip_one_type(t);              /* _cmd */
    while (isdigit((unsigned char)*t)) ++t;
    for (uint32_t i = 0; i < host_argc && n < 10; ++i) {
        enum guest_arg_kind kind = *t ? guest_arg_kind(t) : GA_INT;
        intptr_t raw = args[i];
        if (kind == GA_OBJECT) {
            words[n++] = raw ? proxy_for_object((id)raw) : 0;
        } else {
            words[n++] = (uint32_t)raw;      /* pointer / int / SEL */
        }
        if (*t) { t = guest_skip_one_type(t); while (isdigit((unsigned char)*t)) ++t; }
    }
    if (getenv("LP32_TRACE_OBJC_SELECTORS"))
        fprintf(stderr, "compat32: guest-call -[%s] imp=0x%x words=%u\n", selector, imp, n);
    uint32_t result = compat_runtime32_call(imp, words, n);
    if (compat_runtime32_last_call_trapped()) {
        if (getenv("LP32_TRACE_OBJC_SELECTORS")) fprintf(stderr, "compat32: guest ObjC method -[%s] trapped\n", selector);
    }
    return result;
}

/* Number of leading integer/pointer arguments a selector takes (host regs). */
static uint32_t guest_selector_argc(const char *types)
{
    if (!types) return 0;
    const char *t = types;
    t = guest_skip_one_type(t); while (isdigit((unsigned char)*t)) ++t; /* ret */
    t = guest_skip_one_type(t); while (isdigit((unsigned char)*t)) ++t; /* self */
    t = guest_skip_one_type(t); while (isdigit((unsigned char)*t)) ++t; /* _cmd */
    uint32_t argc = 0;
    while (*t) { ++argc; t = guest_skip_one_type(t); while (isdigit((unsigned char)*t)) ++t; }
    return argc;
}

/* Convert a guest method result to a host value per the return type. */
static intptr_t guest_result_to_host(uint32_t result, const char *types)
{
    const char *t = types ? types : "";
    while (*t && strchr("rnNoORV", *t)) ++t;
    if (*t == '@' || *t == '#') {
        if (guest_instance_for_block(result)) {
            struct guest_objc_instance *inst = guest_instance_for_block(result);
            return (intptr_t)inst->companion;
        }
        id obj = object_for_receiver(result);
        return (intptr_t)obj;
    }
    return (intptr_t)(int32_t)result;
}

/*
 * Generic host IMP trampoline.  Installed for every synthesized method; host
 * AppKit calls it with the companion as self.  The SysV x86_64 integer arg
 * registers give us self, _cmd and up to four leading int/pointer/object args.
 */
static intptr_t guest_method_trampoline(id self, SEL _cmd,
                                        intptr_t a0, intptr_t a1,
                                        intptr_t a2, intptr_t a3)
{
    const char *selector = sel_getName(_cmd);
    /* renewGState is a graphics-state renewal hint AppKit sends very often; the
       guest override traps (and a per-call trap plus its diagnostic I/O showed
       up as gameplay choppiness that scaled with on-screen activity).  A no-op
       is safe here and avoids the trap entirely. */
    if (strcmp(selector, "renewGState") == 0) return 0;
    struct guest_objc_instance *inst = guest_instance_for_companion(self);
    struct guest_objc_class *cls;
    uint32_t self_value;
    bool class_method = false;
    if (inst) {
        cls = inst->cls;
        self_value = inst->block;
    } else {
        /* class method: self is the synthesized (meta)class's class object */
        cls = guest_class_for_host((Class)self);
        if (!cls) cls = guest_class_named(class_getName((Class)self));
        self_value = cls ? cls->class_addr : 0;
        class_method = true;
    }
    const struct guest_objc_method *m = guest_find_method(cls, selector, class_method);
    if (!m) m = guest_find_method(cls, selector, !class_method);
    if (!m) return 0;
    intptr_t args[4] = { a0, a1, a2, a3 };
    uint32_t argc = guest_selector_argc(m->types);
    if (argc > 4) argc = 4;
    uint32_t result = guest_invoke(m->imp, self_value, selector, m->types, args, argc);
    return guest_result_to_host(result, m->types);
}

static void guest_add_methods(Class target, struct guest_objc_method *methods, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        SEL sel = sel_registerName(methods[i].selector);
        const char *types = methods[i].types ? methods[i].types : "@@:";
        class_addMethod(target, sel, (IMP)guest_method_trampoline, types);
    }
}

/*
 * For NSObject-rooted guest classes (the app delegate, helper singletons) the
 * companion is a real, statically-compiled forwarder object rather than a
 * synthesized-class instance: AppKit stores its delegate as a zeroing weak
 * reference, and libobjc's weak machinery faults on objc_allocateClassPair
 * instances here.  A normal NSObject subclass with forwardInvocation: is
 * weak-referenceable and forwards each message to the guest IMP.
 */
@interface LP32GuestForwarder : NSObject {
@public
    uint32_t block;
    struct guest_objc_class *cls;
}
@end

@implementation LP32GuestForwarder
- (BOOL)respondsToSelector:(SEL)aSelector
{
    if (guest_find_method(cls, sel_getName(aSelector), false)) return YES;
    return [super respondsToSelector:aSelector];
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)aSelector
{
    const struct guest_objc_method *m = guest_find_method(cls, sel_getName(aSelector), false);
    if (m && m->types) {
        NSMethodSignature *sig = [NSMethodSignature signatureWithObjCTypes:m->types];
        if (sig) return sig;
    }
    return [super methodSignatureForSelector:aSelector];
}
- (void)forwardInvocation:(NSInvocation *)invocation
{
    const char *selector = sel_getName([invocation selector]);
    const struct guest_objc_method *m = guest_find_method(cls, selector, false);
    if (!m) { [super forwardInvocation:invocation]; return; }
    NSMethodSignature *sig = [invocation methodSignature];
    NSUInteger argc = [sig numberOfArguments];
    intptr_t args[4] = {0,0,0,0};
    uint32_t forwarded = 0;
    for (NSUInteger i = 2; i < argc && forwarded < 4; ++i, ++forwarded) {
        const char *t = [sig getArgumentTypeAtIndex:i];
        while (*t && strchr("rnNoORV", *t)) ++t;
        intptr_t raw = 0;
        if (*t == '@' || *t == '#') { id o = nil; [invocation getArgument:&o atIndex:i]; raw = (intptr_t)o; }
        else if (*t == '^' || *t == '*' || *t == ':') { void *p = NULL; [invocation getArgument:&p atIndex:i]; raw = (intptr_t)p; }
        else { int v = 0; [invocation getArgument:&v atIndex:i]; raw = v; }
        args[forwarded] = raw;
    }
    uint32_t result = guest_invoke(m->imp, block, selector, m->types, args, forwarded);
    const char *rt = [sig methodReturnType];
    while (*rt && strchr("rnNoORV", *rt)) ++rt;
    if (*rt == '@' || *rt == '#') { id o = (id)guest_result_to_host(result, m->types); [invocation setReturnValue:&o]; }
    else if (*rt == 'v') { /* no return */ }
    else if (*rt == '^' || *rt == '*') { void *p = (void *)(uintptr_t)result; [invocation setReturnValue:&p]; }
    else { int v = (int)result; [invocation setReturnValue:&v]; }
}
@end

static void guest_synthesize_class(uint32_t class_addr)
{
    if (g_guest_class_count >= 32) return;
    const uint32_t *c = (const void *)(uintptr_t)class_addr;
    const char *name = (const char *)(uintptr_t)c[OC_NAME];
    const char *super_name = (const char *)(uintptr_t)c[OC_SUPER];
    if (!name || !super_name) return;
    if (objc_getClass(name)) return;            /* already registered */
    Class super = objc_getClass(super_name);
    if (!super) {
        fprintf(stderr, "compat32: guest class %s: unknown superclass %s\n", name, super_name);
        return;
    }
    Class host = objc_allocateClassPair(super, name, 0);
    if (!host) {
        fprintf(stderr, "compat32: guest class %s: objc_allocateClassPair failed\n", name);
        return;
    }
    struct guest_objc_class *gc = &g_guest_classes[g_guest_class_count++];
    memset(gc, 0, sizeof(*gc));
    gc->class_addr = class_addr;
    gc->meta_addr = c[OC_ISA];
    gc->name = name;
    gc->instance_size = c[OC_INSTANCE_SIZE];
    gc->host_class = host;
    gc->use_forwarder = ![host isSubclassOfClass:[NSResponder class]];
    guest_collect_methods(c[OC_METHODLISTS], gc->instance_methods,
                          &gc->instance_method_count, 96);
    if (gc->meta_addr) {
        const uint32_t *mc = (const void *)(uintptr_t)gc->meta_addr;
        guest_collect_methods(mc[OC_METHODLISTS], gc->class_methods,
                              &gc->class_method_count, 32);
    }
    guest_add_methods(host, gc->instance_methods, gc->instance_method_count);
    guest_add_methods(object_getClass((id)host), gc->class_methods, gc->class_method_count);
    objc_registerClassPair(host);
    fprintf(stderr, "compat32: synthesized guest class %s : %s (isize=%u, %u+%u methods)\n",
            name, super_name, gc->instance_size,
            gc->instance_method_count, gc->class_method_count);
}

/* Walk __OBJC,__module_info -> symtabs -> class defs and synthesize each. */
static void guest_synthesize_all_classes(void)
{
    if (g_guest_classes_ready) return;
    g_guest_classes_ready = true;
    const struct mach_header *header = bridge_image ? bridge_image->header : NULL;
    if (!header) return;
    const uint8_t *cursor = (const uint8_t *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const struct load_command *lc = (const void *)cursor;
        if (lc->cmd == LC_SEGMENT) {
            const struct segment_command *seg = (const void *)cursor;
            const struct section *sect = (const void *)(seg + 1);
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                if (strcmp(sect[s].sectname, "__module_info") == 0 &&
                    strcmp(sect[s].segname, "__OBJC") == 0) {
                    uint32_t addr = sect[s].addr, size = sect[s].size;
                    for (uint32_t off = 0; off + 16 <= size; off += 16) {
                        const uint32_t *mod = (const void *)(uintptr_t)(addr + off);
                        uint32_t symtab = mod[3];
                        if (!symtab) continue;
                        const uint32_t *st = (const void *)(uintptr_t)symtab;
                        uint16_t cls_def_cnt = ((const uint16_t *)st)[4];
                        const uint32_t *defs = st + 3;
                        for (uint16_t k = 0; k < cls_def_cnt; ++k) {
                            guest_synthesize_class(defs[k]);
                        }
                    }
                }
            }
        }
        cursor += lc->cmdsize;
    }
}

/*
 * Allocate a companion instance for a synthesized class: a guest-memory block
 * with the 32-bit ivar layout (isa = the guest class struct) plus a host
 * object of the synthesized class.  Returns the guest block address, which the
 * guest uses as the object; object_for_receiver maps it to the companion.
 */
static uint32_t guest_class_alloc(struct guest_objc_class *cls)
{
    uint32_t block = compat_runtime32_allocate(cls->instance_size < 4 ? 4 : cls->instance_size, 1);
    if (!block) return 0;
    *(uint32_t *)(uintptr_t)block = cls->class_addr;     /* isa */
    id companion;
    if (cls->use_forwarder) {
        LP32GuestForwarder *fwd = [[LP32GuestForwarder alloc] init];
        fwd->block = block;
        fwd->cls = cls;
        /* The guest autoreleases the object it just created; since this bridge
           drains host temporaries per dispatch (guest NSAutoreleasePools are
           no-ops), retain the companion so it survives as long as the guest
           holds the block.  These are long-lived game objects (delegate,
           window, view); the retain is intentionally never balanced. */
        [fwd retain];
        companion = fwd;
    } else {
        /* NSWindow/NSOpenGLView subclass: create the real AppKit instance
           lazily, when the guest runs its designated initializer through
           objc_msgSendSuper (guest_super_init).  Until then there is no
           companion; a freshly alloc'd, un-init'd NSWindow is not safe for
           AppKit to weak-reference. */
        companion = nil;
    }
    pthread_mutex_lock(&g_guest_instance_lock);
    if (g_guest_instance_count < 1024) {
        g_guest_instances[g_guest_instance_count].block = block;
        g_guest_instances[g_guest_instance_count].companion = companion;
        g_guest_instances[g_guest_instance_count].cls = cls;
        ++g_guest_instance_count;
    }
    pthread_mutex_unlock(&g_guest_instance_lock);
    return block;
}

/* Number of 32-bit argument words the guest pushes for a method (from the
   encoding's frame size, minus self+_cmd). */
static uint32_t guest_frame_arg_words(const char *types)
{
    const char *t = types ? types : "";
    t = guest_skip_one_type(t);                 /* return type */
    uint32_t frame = (uint32_t)strtoul(t, NULL, 10);
    return frame >= 8 ? (frame - 8) / 4 : 0;
}

/* Call a guest IMP with self plus the guest's own already-32-bit arg words. */
static uint32_t guest_call_with_frame(uint32_t imp, uint32_t self_value,
                                      const char *selector, const char *types,
                                      const uint32_t *guest_args, uint32_t avail)
{
    uint32_t words[18];
    uint32_t n = 0;
    words[n++] = self_value;
    words[n++] = guest_selector(selector);
    uint32_t argw = guest_frame_arg_words(types);
    if (argw > avail) argw = avail;
    if (argw > 16 - n) argw = 16 - n;
    for (uint32_t i = 0; i < argw; ++i) words[n++] = guest_args[i];
    if (getenv("LP32_TRACE_OBJC_SELECTORS"))
        fprintf(stderr, "compat32: guest-call +[%s] imp=0x%x words=%u\n", selector, imp, n);
    uint32_t result = compat_runtime32_call(imp, words, n);
    if (compat_runtime32_last_call_trapped()) {
        if (getenv("LP32_TRACE_OBJC_SELECTORS")) fprintf(stderr, "compat32: guest ObjC method +[%s] trapped\n", selector);
    }
    return result;
}

/*
 * Create the real AppKit companion for a window/view guest instance when its
 * guest designated initializer calls the host initializer via objc_msgSendSuper.
 * `init_args` points at the initializer arguments (a leading NSRect is the
 * first four float words).  The returned object is fully initialized and safe
 * for AppKit to weak-reference; it is mapped to the guest block (the ivar
 * shadow), so both guest ivar access and host messaging resolve correctly.
 */
static bool guest_super_init(struct guest_objc_instance *inst,
                             const char *selector, const uint32_t *init_args)
{
    if (!inst || inst->companion) return false;
    Class host = inst->cls->host_class;
    const float *r = (const float *)init_args;
    NSRect rect = NSMakeRect(r[0], r[1], r[2], r[3]);
    id obj = [host alloc];
    if (!obj) return false;
    /* Map the companion to the guest block BEFORE running the initializer.
       AppKit messages the window/view while it initializes (Bejeweled 3's
       NSWindow init does), and those sends hit the guest's overridden methods;
       with the mapping in place guest_method_trampoline dispatches them to the
       right instance instead of misreading a half-built object. */
    inst->companion = obj;
    /* Invoke the *superclass* designated initializer via objc_msgSendSuper.
       host is the synthesized subclass (e.g. SexyWindow : NSWindow) and it
       overrides this very initializer, so a plain [obj init...] would re-enter
       the guest override and recurse; super-dispatch reaches NSWindow. */
    struct objc_super sup = { obj, class_getSuperclass(host) };
    SEL sel = sel_registerName(selector);
    id done = nil;
    if (strcmp(selector, "initWithContentRect:styleMask:backing:defer:") == 0) {
        typedef id (*fn)(struct objc_super *, SEL, NSRect, NSUInteger, NSUInteger, BOOL);
        done = ((fn)objc_msgSendSuper)(&sup, sel, rect, init_args[4], init_args[5],
                                       init_args[6] != 0);
    } else if (strcmp(selector, "initWithContentRect:styleMask:backing:defer:screen:") == 0) {
        id screen = object_for_receiver(init_args[7]);
        if (screen && ![screen isKindOfClass:[NSScreen class]]) screen = nil;
        typedef id (*fn)(struct objc_super *, SEL, NSRect, NSUInteger, NSUInteger, BOOL, id);
        done = ((fn)objc_msgSendSuper)(&sup, sel, rect, init_args[4], init_args[5],
                                       init_args[6] != 0, screen);
    } else if (strcmp(selector, "initWithFrame:pixelFormat:") == 0) {
        typedef id (*fn)(struct objc_super *, SEL, NSRect, id);
        done = ((fn)objc_msgSendSuper)(&sup, sel, rect, object_for_receiver(init_args[4]));
    } else if (strcmp(selector, "initWithFrame:") == 0) {
        typedef id (*fn)(struct objc_super *, SEL, NSRect);
        done = ((fn)objc_msgSendSuper)(&sup, sel, rect);
    } else {
        inst->companion = nil;
        [obj release];
        return false;
    }
    if (!done) { inst->companion = nil; return false; }
    inst->companion = done;   /* the initializer may substitute the instance */
    [done retain];            /* long-lived; the guest may autorelease the block */
    fprintf(stderr, "compat32: created %s companion %p for guest block 0x%08x via %s\n",
            inst->cls->name, (void *)done, inst->block, selector);
    return true;
}

/* objc_msgSendSuper entry: if a window/view guest instance is running its
   designated initializer, create the companion now and keep the guest block as
   the object identity.  Returns true when handled. */
bool guest_objc_super_init(uint32_t receiver, const char *selector,
                           const uint32_t *init_args, uint64_t *out)
{
    struct guest_objc_instance *inst = guest_instance_for_block(receiver);
    if (!inst || inst->companion || inst->cls->use_forwarder) return false;
    if (guest_super_init(inst, selector, init_args)) { *out = receiver; return true; }
    return false;
}

/*
 * Guest-side objc_msgSend hook for synthesized classes and their instances.
 * `arguments` is the full guest msgSend frame (0=receiver, 1=selector,
 * 2..=method args, already guest 32-bit values).  `avail` is how many arg
 * words are safely readable.  Returns true and sets *out when handled.
 */
static bool guest_objc_msgsend(const uint32_t *arguments, uint32_t avail, uint64_t *out)
{
    guest_synthesize_all_classes();
    uint32_t receiver = arguments[0];
    const char *selector = (const char *)(uintptr_t)arguments[1];
    if (!selector) return false;
    const uint32_t *method_args = arguments + 2;

    /* Class message to a synthesized class: the receiver is either the
       class-name ref (in the cstring section, pre-fixup) or the guest class
       object address itself (when the guest holds a direct class pointer). */
    struct guest_objc_class *cls = NULL;
    if (receiver >= bridge_image->cstring_start && receiver < bridge_image->cstring_end) {
        cls = guest_class_named((const char *)(uintptr_t)receiver);
    }
    if (!cls) cls = guest_class_for_addr(receiver);
    if (cls) {
        if (strcmp(selector, "alloc") == 0 || strcmp(selector, "new") == 0 ||
            strcmp(selector, "allocWithZone:") == 0) {
            *out = guest_class_alloc(cls);
            return true;
        }
        if (strcmp(selector, "class") == 0) { *out = cls->class_addr; return true; }
        const struct guest_objc_method *m = guest_find_method(cls, selector, true);
        if (m) {
            *out = guest_call_with_frame(m->imp, cls->class_addr, selector,
                                         m->types, method_args, avail);
            return true;
        }
        return false; /* unknown class message: let normal dispatch try */
    }

    /* Instance message to a synthesized instance block. */
    struct guest_objc_instance *inst = guest_instance_for_block(receiver);
    if (inst) {
        /* A super-send must reach the host superclass, not the guest override:
           fall through so normal dispatch sends to the companion. */
        if (g_force_super) return false;
        const struct guest_objc_method *m = guest_find_method(inst->cls, selector, false);
        if (m) {
            *out = guest_call_with_frame(m->imp, receiver, selector, m->types,
                                         method_args, avail);
            return true;
        }
        if (strcmp(selector, "init") == 0 || strcmp(selector, "self") == 0 ||
            strcmp(selector, "retain") == 0) {
            *out = receiver;   /* inherited no-ops: keep the guest block identity */
            return true;
        }
        /* Inherited host method: send to the companion via normal dispatch
           (object_for_receiver maps the block to the companion). */
        return false;
    }
    return false;
}

/*
 * Struct-return (objc_msgSend_stret) dispatch to a synthesized guest method.
 * arguments: 0 = hidden struct-return pointer, 1 = receiver, 2 = selector,
 * 3.. = guest arg words.  On i386 a struct-returning method is compiled as
 *   void imp(struct* hidden, id self, SEL _cmd, args...)
 * and float/double arguments are ordinary stack words, so the caller's frame
 * is forwarded verbatim -- no per-argument-kind marshalling is needed.  The
 * IMP writes the result through the hidden pointer; the caller's callee-pop of
 * that pointer is arranged by compat_runtime32_struct_return() at the msgSend
 * site (set AFTER this call so the nested guest call does not consume it).
 * Returns true and sets *out (the returned struct pointer) when handled.
 */
static bool guest_objc_msgsend_stret(const uint32_t *arguments, uint32_t avail,
                                     uint64_t *out)
{
    guest_synthesize_all_classes();
    uint32_t hidden = arguments[0];
    uint32_t receiver = arguments[1];
    const char *selector = (const char *)(uintptr_t)arguments[2];
    if (!selector) return false;
    const uint32_t *method_args = arguments + 3;
    uint32_t arg_avail = avail > 3 ? avail - 3 : 0;

    const struct guest_objc_method *m = NULL;
    uint32_t self_value = 0;
    struct guest_objc_class *cls = NULL;
    if (receiver >= bridge_image->cstring_start &&
        receiver < bridge_image->cstring_end)
        cls = guest_class_named((const char *)(uintptr_t)receiver);
    if (!cls) cls = guest_class_for_addr(receiver);
    if (cls) {
        m = guest_find_method(cls, selector, true);
        self_value = cls->class_addr;
    } else {
        struct guest_objc_instance *inst = guest_instance_for_block(receiver);
        if (!inst || g_force_super) return false;
        m = guest_find_method(inst->cls, selector, false);
        self_value = receiver;
    }
    if (!m) return false;

    uint32_t words[16];
    uint32_t n = 0;
    words[n++] = hidden;
    words[n++] = self_value;
    words[n++] = guest_selector(selector);
    uint32_t argw = arg_avail;
    if (argw > 16 - n) argw = 16 - n;
    for (uint32_t i = 0; i < argw; ++i) words[n++] = method_args[i];
    if (getenv("LP32_TRACE_OBJC_SELECTORS"))
        fprintf(stderr, "compat32: guest-stret [%s] imp=0x%x words=%u hidden=0x%08x\n",
                selector, m->imp, n, hidden);
    uint32_t result = compat_runtime32_call(m->imp, words, n);
    if (compat_runtime32_last_call_trapped()) {
        if (getenv("LP32_TRACE_OBJC_SELECTORS")) fprintf(stderr, "compat32: guest stret method [%s] trapped\n", selector);
        return false;
    }
    *out = result;
    return true;
}
