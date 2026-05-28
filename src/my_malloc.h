#ifndef MY_MALLOC_H
#define MY_MALLOC_H

#include <stddef.h>

/* Core allocator */
void  heap_init(void);
void *my_malloc(size_t size);
void  my_free(void *ptr);

/* Integrity check — returns 0 if all invariants hold, negative error code otherwise */
int   heap_check(void);

/* Statistics */
size_t heap_bytes_used(void);
size_t heap_bytes_free(void);
size_t heap_hwm(void);
size_t heap_alloc_count(void);

/* Debug dump — compiled out when NDEBUG is defined */
#ifndef NDEBUG
void my_alloc_dump(void);
#endif

#endif
