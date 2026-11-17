// Test-only dyld interposition. A separate image is required because dyld does
// not interpose an image's own references to its replacement functions.
#include <stdlib.h>
#include <stddef.h>

static _Thread_local int tracking;
static _Thread_local size_t allocations;
void mv_test_track_allocations(int enabled) { tracking = enabled; }
size_t mv_test_allocation_count(void) { return allocations; }

#define MV_INTERPOSE(replacement, original) \
    __attribute__((used, section("__DATA,__interpose"))) \
    static const struct { const void *replacementFunction; const void *originalFunction; } interpose_##original = \
        {(const void *)&replacement, (const void *)&original};
static void *tracked_malloc(size_t size) { if (tracking) ++allocations; return malloc(size); }
static void *tracked_calloc(size_t count, size_t size) { if (tracking) ++allocations; return calloc(count, size); }
static void *tracked_realloc(void *p, size_t size) { if (tracking) ++allocations; return realloc(p, size); }
static void *tracked_aligned_alloc(size_t align, size_t size) { if (tracking) ++allocations; return aligned_alloc(align, size); }
static int tracked_posix_memalign(void **p, size_t align, size_t size) { if (tracking) ++allocations; return posix_memalign(p, align, size); }
MV_INTERPOSE(tracked_malloc, malloc)
MV_INTERPOSE(tracked_calloc, calloc)
MV_INTERPOSE(tracked_realloc, realloc)
MV_INTERPOSE(tracked_aligned_alloc, aligned_alloc)
MV_INTERPOSE(tracked_posix_memalign, posix_memalign)
