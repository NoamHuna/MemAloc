#include "my_malloc.h"

#include <stdint.h>

#define REGION_SIZE       (1u << 20)   /* 1 MiB — set this to your MCU's available RAM */
#define ALIGNMENT         16
#define MIN_SPLIT_PAYLOAD 16

/* ── 1. Static backing array ─────────────────────────────────────────────────
   No OS call. The linker places this in the BSS segment at build time.
   On a bare-metal MCU this just sits in RAM, exactly as a real RTOS heap does. */
static uint8_t g_heap[REGION_SIZE] __attribute__((aligned(16)));

typedef struct block_header {
    size_t               size;    /* payload bytes, excludes header */
    struct block_header *next;    /* next block in region (NULL = last) */
    struct block_header *prev;    /* previous block in region (NULL = first) */
    int                  is_free;
    char                 _pad[4]; /* pad to 32 bytes so every payload is 16-byte aligned */
} block_header_t;

_Static_assert(sizeof(block_header_t) == 32,
    "block_header_t must be 32 bytes for 16-byte payload alignment");

/* ── 3. Statistics ────────────────────────────────────────────────────────── */
static size_t g_bytes_used = 0;   /* current live bytes (payload only) */
static size_t g_hwm        = 0;   /* high-water mark: peak bytes ever in use */
static size_t g_alloc_count = 0;  /* number of live allocations */

static block_header_t *g_head = NULL;

/* ── Init ─────────────────────────────────────────────────────────────────── */
void heap_init(void) {
    g_head          = (block_header_t *)g_heap;
    g_head->size    = REGION_SIZE - sizeof(block_header_t);
    g_head->next    = NULL;
    g_head->prev    = NULL;
    g_head->is_free = 1;
    g_bytes_used    = 0;
    g_hwm           = 0;
    g_alloc_count   = 0;
}

/* ── 2. Heap integrity check ──────────────────────────────────────────────── */
int heap_check(void) {
    if (g_head == NULL) return 0;

    block_header_t *cur  = g_head;
    block_header_t *prev = NULL;
    size_t bytes_walked  = 0;

    while (cur != NULL) {
        /* alignment: every header must be 16-byte aligned */
        if ((uintptr_t)cur % ALIGNMENT != 0) return -1;

        /* prev pointer consistency */
        if (cur->prev != prev) return -2;

        /* size accounting: next must be exactly header+size bytes ahead */
        if (cur->next != NULL) {
            block_header_t *expected_next =
                (block_header_t *)((char *)cur + sizeof(block_header_t) + cur->size);
            if (cur->next != expected_next) return -3;
        }

        bytes_walked += sizeof(block_header_t) + cur->size;
        prev = cur;
        cur  = cur->next;
    }

    /* coverage: walked bytes must equal the entire region */
    if (bytes_walked != REGION_SIZE) return -4;

    return 0;  /* all invariants hold */
}

/* ── my_malloc ────────────────────────────────────────────────────────────── */
void *my_malloc(size_t size) {
    if (g_head == NULL) heap_init();
    if (size == 0) return NULL;

    size = (size + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1);

    block_header_t *cur = g_head;
    while (cur != NULL) {
        if (cur->is_free && cur->size >= size) {
            size_t leftover = cur->size - size;
            if (leftover > sizeof(block_header_t) + MIN_SPLIT_PAYLOAD) {
                block_header_t *new_block =
                    (block_header_t *)((char *)cur + sizeof(block_header_t) + size);
                new_block->size    = leftover - sizeof(block_header_t);
                new_block->next    = cur->next;
                new_block->prev    = cur;
                new_block->is_free = 1;
                if (cur->next != NULL)
                    cur->next->prev = new_block;
                cur->next = new_block;
                cur->size = size;
            }
            cur->is_free = 0;

            /* ── update stats ── */
            g_bytes_used  += sizeof(block_header_t) + cur->size;
            g_alloc_count += 1;
            if (g_bytes_used > g_hwm)
                g_hwm = g_bytes_used;

            return (char *)cur + sizeof(block_header_t);
        }
        cur = cur->next;
    }
    return NULL;
}

/* ── my_free ──────────────────────────────────────────────────────────────── */
void my_free(void *ptr) {
    if (ptr == NULL) return;

    block_header_t *header = (block_header_t *)((char *)ptr - sizeof(block_header_t));

    /* payload is no longer in use; header stays in memory until coalesced */
    g_bytes_used  -= header->size;
    g_alloc_count -= 1;

    header->is_free = 1;
    bool is_coalesced = false;
    /* Coalesce with next block if also free — next's header disappears */
    if (header->next != NULL && header->next->is_free) {
        block_header_t *next = header->next;
        header->size += sizeof(block_header_t) + next->size;
        header->next  = next->next;
        if (next->next != NULL)
            next->next->prev = header;
        g_bytes_used -= sizeof(block_header_t);   /* next's header absorbed */
    }

    /* Coalesce with previous block if also free — our header disappears */
    if (header->prev != NULL && header->prev->is_free) {
        block_header_t *prev = header->prev;
        prev->size += sizeof(block_header_t) + header->size;
        prev->next  = header->next;
        if (header->next != NULL)
            header->next->prev = prev;
        g_bytes_used -= sizeof(block_header_t);   /* our header absorbed */
    }
}

/* ── Stats accessors ──────────────────────────────────────────────────────── */
size_t heap_bytes_used(void)  { return g_bytes_used;   }
size_t heap_bytes_free(void)  { return REGION_SIZE - sizeof(block_header_t) - g_bytes_used; }
size_t heap_hwm(void)         { return g_hwm;          }
size_t heap_alloc_count(void) { return g_alloc_count;  }

/* ── 5. my_alloc_dump — compiled out in release builds ───────────────────── */
#ifndef NDEBUG
#include <stdio.h>
void my_alloc_dump(void) {
    block_header_t *cur = g_head;
    int i = 0;
    printf("--- heap dump | used=%zu free=%zu hwm=%zu allocs=%zu ---\n",
           heap_bytes_used(), heap_bytes_free(), heap_hwm(), heap_alloc_count());
    while (cur != NULL) {
        printf("  [%d] addr=%p  size=%5zu  %s\n",
               i++, (void *)cur, cur->size, cur->is_free ? "FREE" : "USED");
        cur = cur->next;
    }
}
#endif
