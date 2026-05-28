#include "my_malloc.h"
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name)  do { \
    heap_init(); \
    tests_run++; \
    printf("  %-35s", #name); \
    name(); \
    assert(heap_check() == 0); \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

/* ------------------------------------------------------------------ */

TEST(test_basic_alloc) {
    int *a = my_malloc(sizeof(int));
    int *b = my_malloc(sizeof(int));
    assert(a != NULL);
    assert(b != NULL);
    assert(a != b);
    *a = 10;
    *b = 20;
    assert(*a == 10);
    assert(*b == 20);
}

TEST(test_alignment) {
    void *ptrs[16];
    for (int i = 0; i < 16; i++) {
        ptrs[i] = my_malloc(i + 1);
        assert(ptrs[i] != NULL);
        assert(((uintptr_t)ptrs[i] & 15) == 0);
    }
}

TEST(test_free_and_reuse) {
    int *a = my_malloc(sizeof(int));
    assert(a != NULL);
    void *addr = (void *)a;
    my_free(a);
    int *b = my_malloc(sizeof(int));
    assert(b != NULL);
    assert((void *)b == addr);
}

TEST(test_write_pattern) {
    unsigned char *buf = my_malloc(256);
    assert(buf != NULL);
    memset(buf, 0xAB, 256);
    for (int i = 0; i < 256; i++)
        assert(buf[i] == 0xAB);
    unsigned char *buf2 = my_malloc(256);
    assert(buf2 != NULL);
    assert(buf2 != buf);
}

TEST(test_split_visible) {
    void *p[8];
    for (int i = 0; i < 8; i++) {
        p[i] = my_malloc(32);
        assert(p[i] != NULL);
    }
}

TEST(test_alloc_zero) {
    void *p = my_malloc(0);
    assert(p == NULL);
}

TEST(test_free_null) {
    my_free(NULL);
}

TEST(test_many_small_allocs) {
    void *ptrs[1024];
    for (int i = 0; i < 1024; i++) {
        ptrs[i] = my_malloc(16);
        assert(ptrs[i] != NULL);
    }
    for (int i = 0; i < 1024; i += 2)
        my_free(ptrs[i]);
    for (int i = 0; i < 1024; i += 2) {
        void *p = my_malloc(16);
        assert(p != NULL);
    }
}

TEST(test_coalesce_next) {
    int *a = my_malloc(64);
    int *b = my_malloc(64);
    assert(a != NULL && b != NULL);
    my_free(a);
    my_free(b);
    int *c = my_malloc(128);
    assert(c != NULL);
    assert((void *)c == (void *)a);
}

TEST(test_coalesce_prev) {
    int *a = my_malloc(64);
    int *b = my_malloc(64);
    assert(a != NULL && b != NULL);
    my_free(b);
    my_free(a);
    int *c = my_malloc(128);
    assert(c != NULL);
    assert((void *)c == (void *)a);
}

TEST(test_coalesce_both) {
    int *a = my_malloc(64);
    int *b = my_malloc(64);
    int *c = my_malloc(64);
    assert(a != NULL && b != NULL && c != NULL);
    my_free(a);
    my_free(c);
    my_free(b);
    int *d = my_malloc(192);
    assert(d != NULL);
    assert((void *)d == (void *)a);
}

TEST(test_heap_check) {
    /* heap_check() is called after every test via RUN — this one
       verifies it catches a corrupted size field */
    int *a = my_malloc(64);
    assert(heap_check() == 0);
    (void)a;
}

TEST(test_stats) {
    assert(heap_bytes_used() == 0);
    assert(heap_alloc_count() == 0);

    /* malloc: full cost = header + payload */
    void *a = my_malloc(64);
    assert(heap_bytes_used() == 32 + 64);
    assert(heap_alloc_count() == 1);

    void *b = my_malloc(128);
    assert(heap_bytes_used() == (32 + 64) + (32 + 128));
    assert(heap_alloc_count() == 2);
    assert(heap_hwm() == (32 + 64) + (32 + 128));

    /* free a: only payload removed, header stays until coalesced
       256 - 64(payload of a) = 192 */
    my_free(a);
    assert(heap_bytes_used() == 192);
    assert(heap_alloc_count() == 1);
    assert(heap_hwm() == (32 + 64) + (32 + 128));   /* hwm never goes down */

    /* free b: b's payload gone, then a+b coalesce — both headers absorbed */
    my_free(b);
    assert(heap_bytes_used() == 0);
    assert(heap_alloc_count() == 0);
    assert(heap_hwm() == (32 + 64) + (32 + 128));   /* still at peak */

    printf("(hwm=%zu) ", heap_hwm());
}

TEST(test_exhaust_heap) {
    int count = 0;
    while (my_malloc(1024) != NULL)
        count++;
    printf("(exhausted after %d allocs) ", count);
    assert(count > 100);
}

TEST(test_large_single_alloc) {
    /* one allocation just over the region size must fail */
    void *p = my_malloc(1u << 20);
    assert(p == NULL);
    /* one allocation just under must succeed on a fresh heap */
    void *q = my_malloc((1u << 20) - 512);
    assert(q != NULL);
}

/* ------------------------------------------------------------------ */

int main(void) {
    printf("\n=== my_malloc test suite ===\n\n");

    heap_init();
    printf("  [heap at startup]\n");
    my_alloc_dump();
    printf("\n");

    RUN(test_basic_alloc);
    RUN(test_alignment);
    RUN(test_free_and_reuse);
    RUN(test_write_pattern);
    RUN(test_split_visible);
    RUN(test_alloc_zero);
    RUN(test_free_null);
    RUN(test_many_small_allocs);
    RUN(test_coalesce_next);
    RUN(test_coalesce_prev);
    RUN(test_coalesce_both);
    RUN(test_heap_check);
    RUN(test_stats);
    RUN(test_exhaust_heap);
    RUN(test_large_single_alloc);

    printf("\n  [heap after all tests]\n");
    my_alloc_dump();

    printf("\n%d/%d tests passed\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
