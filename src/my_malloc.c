#include "my_malloc.h"

#include <stdio.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/* From step 1: prove mmap works */
void hello(void) {
    void *region = mmap(NULL, 4096,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        printf("mmap failed\n");
        return;
    }
    printf("got region at %p\n", region);

    int *as_int = (int *)region;
    *as_int = 42;
    printf("read back: %d\n", *as_int);
}

/* Step 2: the actual tiny allocator */
static char *g_cursor = NULL;
static char *g_end    = NULL;

void *tiny_malloc(size_t size) {
    /* First call: get the region from the OS */
    if (g_cursor == NULL) {
        void *region = mmap(NULL, 4096,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (region == MAP_FAILED) return NULL;
        g_cursor = (char *)region;
        g_end    = g_cursor + 4096;
    }

    /* Out of memory? */
    if (g_cursor + size > g_end) return NULL;

    /* Hand out the current position, advance the cursor */
    void *result = g_cursor;
    g_cursor += size;
    return result;
}
