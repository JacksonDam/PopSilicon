#ifndef LP32_NAME_MATCH_H
#define LP32_NAME_MATCH_H

#include <string.h>

/* Import names are matched against string literals in long if-chains.  The
   game issues more than ten thousand imports per frame, so the chains are a
   measurable share of frame time.  Comparing the cached length first rejects
   almost every candidate with one integer compare; only same-length names
   reach memcmp, which the compiler expands inline for literal sizes. */
#define LP32_NAME_IS(name, length, literal) \
    ((length) == sizeof(literal) - 1 && \
     memcmp((name), (literal), sizeof(literal) - 1) == 0)

#endif
