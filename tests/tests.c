#include "my_malloc.h"
#include <stdio.h>
#include <assert.h>

int main(void) {
    int *a = tiny_malloc(sizeof(int));
    int *b = tiny_malloc(sizeof(int));

    assert(a != NULL);
    assert(b != NULL);
    assert(a != b);

    *a = 10;
    *b = 20;

    printf("a is at %p, value %d\n", (void*)a, *a);
    printf("b is at %p, value %d\n", (void*)b, *b);

    return 0;
}