# Custom Memory Allocator

A free-list memory allocator written in C from scratch, designed with embedded systems in mind. No OS dependencies, no stdlib in the hot path — just pointer arithmetic and a static backing array.

## What it does

Manages a fixed 1 MiB region of memory entirely in user space. The allocator asks for nothing from the OS at runtime — the region is a static array placed in BSS by the linker at compile time, making it suitable for bare-metal targets where `mmap` doesn't exist.

## Design

### Memory layout

Every block in the region starts with a 32-byte header followed by the user payload:

```
[ size | next | prev | is_free | pad ][ payload ][ size | next | prev | is_free | pad ][ payload ] ...
|------------ 32 bytes --------------|           |------------ 32 bytes --------------|
```

At startup the entire region is one giant free block. Allocations carve pieces out of free blocks; frees mark blocks available and immediately coalesce with neighbours.

### Block header

```c
typedef struct block_header {
    size_t               size;     // payload bytes only, excludes header
    struct block_header *next;     // next block in region (NULL = last)
    struct block_header *prev;     // previous block in region (NULL = first)
    int                  is_free;
    char                 _pad[4];  // pad to 32 bytes for 16-byte payload alignment
} block_header_t;
```

The header is exactly 32 bytes (`_Static_assert` enforces this at compile time), so every payload pointer is automatically 16-byte aligned — no per-allocation alignment math needed.

### Allocation — first-fit with splitting

1. Round requested size up to the nearest multiple of 16
2. Walk the block list from the head, find the first free block big enough
3. If the leftover after carving exceeds `sizeof(header) + MIN_SPLIT_PAYLOAD`, split it — write a new header into the region and wire it into the doubly-linked list
4. Mark the block used, return the payload pointer

### Free with bidirectional coalescing

`my_free` immediately merges the freed block with adjacent free neighbours:

- **Next coalesce**: if `header->next` is free, absorb it — one header disappears
- **Prev coalesce**: if `header->prev` is free, absorb into it — our header disappears

This prevents fragmentation: three adjacent 64-byte free blocks collapse into one 192-byte block, making a `my_malloc(150)` succeed where it would otherwise fail.

### Statistics and diagnostics

```c
size_t heap_bytes_used(void);   // current live bytes (header + payload per block)
size_t heap_bytes_free(void);   // remaining usable bytes
size_t heap_hwm(void);          // peak bytes ever in use (high-water mark)
size_t heap_alloc_count(void);  // number of live allocations
```

The high-water mark never decreases — it captures the worst-case RAM usage across the full lifetime of the program, which is critical for sizing RAM at design time on embedded targets.

Header bytes are counted honestly: they are added on `my_malloc` and only subtracted when a coalesce absorbs a header back into a merged block. A freed-but-not-coalesced header still occupies RAM and is still counted.

### Heap integrity checker

```c
int heap_check(void);  // returns 0 if valid, negative error code otherwise
```

Walks the entire block list and verifies four invariants:

| Return | Invariant checked |
|--------|-------------------|
| `-1`   | Every header is 16-byte aligned |
| `-2`   | `prev` pointer is consistent with forward walk |
| `-3`   | `next` is exactly `header + size` bytes ahead |
| `-4`   | Total bytes walked equals `REGION_SIZE` exactly |

Useful on embedded targets where AddressSanitizer is unavailable — call it after critical operations to catch corruption early.

### Debug dump

```c
void my_alloc_dump(void);  // compiled out when NDEBUG is defined
```

Prints every block's address, size, and status alongside current stats. Zero overhead in release builds.

## Build

```bash
cmake -B build
cmake --build build
./build/tests
```

For an AddressSanitizer build:

```bash
cmake -B build-asan -DUSE_ASAN=ON
cmake --build build-asan
./build-asan/tests
```

Release build (disables `my_alloc_dump` and its `printf` dependency):

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

## Tests

15 tests covering:

- Basic allocation and distinct pointer guarantees
- 16-byte alignment on all pointer sizes
- Free and reuse of the same address
- Write pattern integrity (no header/payload overlap)
- Block splitting visibility
- Edge cases: `my_malloc(0)`, `my_free(NULL)`
- 1024 interleaved alloc/free cycles
- Bidirectional coalescing (next, prev, both neighbours)
- Heap integrity check
- Statistics and high-water mark correctness
- Heap exhaustion behaviour
- Oversized allocation rejection

## Trade-offs and limitations

- **First-fit, not best-fit** — O(n) search from the head on every allocation; best-fit would reduce fragmentation at the cost of longer searches
- **Single fixed region** — capped at `REGION_SIZE` (1 MiB by default), no dynamic growth
- **Not thread-safe** — no locking; single-threaded use only
- **No `realloc` / `calloc`** — only `my_malloc` and `my_free`
- **Double-free and use-after-free are undefined** — no detection; use AddressSanitizer during development

## Roadmap

- [ ] `my_realloc`, `my_calloc`
- [ ] Benchmarks against system `malloc`
- [ ] Optional mutex for thread safety
- [ ] Multiple arenas / size classes for reduced fragmentation
