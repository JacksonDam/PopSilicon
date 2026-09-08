/*
 * A CGL context the guest can call OpenGL through.
 *
 * Plants vs. Zombies is compiled against <OpenGL/CGLMacro.h>, which rewrites
 * every GL call into a direct call through the current CGL context's
 * function-pointer table:
 *
 *     glDisable(cap)  ->  (*cgl_ctx->disp.disable)(cgl_ctx->rend, cap)
 *
 * so the game imports no gl* symbols at all and the import bridge never sees
 * those calls.  The host's own CGLContextObj is no use here: it is a 64-bit
 * structure full of 64-bit pointers, and the guest reads it with i386 offsets.
 *
 * So the runtime hands the game a context of its own, laid out as i386 code
 * expects -- { GLIContext rend; GLIFunctionDispatch disp; } -- whose every
 * dispatch slot holds a guest-callable thunk named for the GL entry point that
 * belongs there.  Calling one arrives at the import bridge as "_gli_glDisable",
 * and gli_dispatch drops the renderer argument and hands it to the ordinary GL
 * bridge.  gli_dispatch.inc says which entry point each slot is; it is
 * generated from the SDK's own headers by tools/gen_gli_dispatch.py.
 */

#include "gli_dispatch.inc"

enum { kGliRendererOffset = 0, kGliDispatchOffset = 4 };

static uint32_t pg_gli_context;

static uint32_t pg_gli_context_object(void)
{
    if (pg_gli_context) return pg_gli_context;
    /* rend, the dispatch table, and room for the priv/stak fields the header
       declares after it (the game never reads those). */
    uint32_t size = kGliDispatchOffset + kGliDispatchSlots * 4 + 8;
    uint32_t context = compat_runtime32_allocate(size, 1);
    if (!context) return 0;
    unsigned bound = 0, unmapped = 0;
    for (unsigned slot = 0; slot < kGliDispatchSlots; ++slot) {
        const char *name = kGliDispatchNames[slot];
        if (!name) { ++unmapped; continue; }
        char thunk[128];
        snprintf(thunk, sizeof(thunk), "_gli%s", name);
        uint32_t address = compat_runtime32_guest_callback(thunk);
        if (!address) {
            fprintf(stderr, "compat32: no thunk left for GL dispatch slot %u (%s)\n",
                    slot, name);
            break;
        }
        *(uint32_t *)(uintptr_t)(context + kGliDispatchOffset + slot * 4) = address;
        ++bound;
    }
    /* The renderer is passed back to us as the first argument of every call
       and then dropped, so it only has to be a plausible non-NULL token. */
    *(uint32_t *)(uintptr_t)(context + kGliRendererOffset) = context;
    fprintf(stderr, "compat32: GL dispatch context at 0x%08x (%u entry points, "
            "%u slots CGLMacro.h does not name)\n", context, bound, unmapped);
    pg_gli_context = context;
    return context;
}

/* "_gli_glDisable" -> "_glDisable" without the renderer argument. */
static int gli_dispatch(const char *n, const uint32_t *a, uint64_t *r)
{
    if (strncmp(n, "_gli_gl", 7) != 0) return 0;
    static int trace = -1;
    if (trace < 0) trace = getenv("LP32_TRACE_GLI") != NULL;
    if (trace) {
        fprintf(stderr, "compat32: gli %s(%08x, %08x, %08x, %08x, %08x)\n",
                n + 4, a[1], a[2], a[3], a[4], a[5]);
    }
    return objc_bridge32_dispatch(n + 4, a + 1, r);
}
