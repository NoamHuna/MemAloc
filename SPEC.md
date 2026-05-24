# Custom Memory Allocator — Full Specification

This document specifies what the program does, how it does it, and what "done" means
for each stage. It's both the project plan and the reference you keep open while coding.

---

## 1. What this program is

A user-space memory allocator written in C, exposing three functions:

```c
void *my_malloc(size_t size);
void  my_free(void *ptr);
void  my_alloc_dump(void);  /* debug helper */
```

`my_malloc(n)` returns a pointer to a block of at least `n` bytes of usable memory.
`my_free(p)` returns a block previously returned by `my_malloc` to the allocator so
it can be reused by a future `my_malloc` call.

The program does **not** replace the system `malloc`. It coexists with it. The test
binary uses both: the OS `malloc` for whatever stdlib needs internally, and `my_malloc`
explicitly when the test code calls it.

The allocator gets its raw memory from the OS once, via a single `mmap` call, then
manages that region itself with no further OS calls.

---

## 2. What the program does NOT do (in v0.1)

Listing this explicitly because half the work in a project like this is being clear
about scope.

- Does not implement `realloc`, `calloc`, `free` (the real names) — only `my_*` variants
- Does not coalesce adjacent free blocks (planned for v0.2)
- Does not grow the heap dynamically — one fixed 1 MiB region, total
- Is not thread-safe — single-threaded use only
- Does not handle out-of-memory by erroring loudly; just returns `NULL`
- Does not detect double-free or use-after-free (those are debugging-allocator features,
  out of scope)

---

## 3. Architecture overview

```
┌─────────────────────────────────────────────────────────────┐
│                      Test program                            │
│   (tests/tests.c — calls my_malloc / my_free)               │
└────────────────────────────┬────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────┐
│                  my_malloc / my_free                         │
│              (src/my_malloc.c — this project)                │
│                                                              │
│   Manages a free list inside a 1 MiB mmap'd region:          │
│                                                              │
│   region start                                  region end   │
│        │                                              │      │
│        ▼                                              ▼      │
│   ┌────────┬──────────┬────────┬────────────────────────┐   │
│   │ hdr  U │ payload  │ hdr  F │ payload (free)         │   │
│   └────────┴──────────┴────────┴────────────────────────┘   │
│      ↑                    ↑                                  │
│     used                 free                                │
└────────────────────────────┬────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────┐
│                    Operating system                          │
│           (one mmap call at first use, 1 MiB region)         │
└─────────────────────────────────────────────────────────────┘
```

Every block in the region begins with a small `block_header_t` followed by the
payload (user-visible) bytes. Headers form a singly-linked list via the `next`
pointer, so you can walk every block from `g_head` to NULL.

---

## 4. Data structures

### 4.1 Block header

```c
typedef struct block_header {
    size_t               size;    /* payload size in bytes, excludes header */
    struct block_header *next;    /* next block in the region (NULL = last) */
    int                  is_free; /* 1 = free, 0 = used */
} block_header_t;
```

**Important:**
- `size` is the **payload** size — what the user asked for (rounded up for alignment).
  The total bytes a block consumes in the region is `sizeof(block_header_t) + size`.
- `next` always points to the **physically next** block in memory, not the "next
  free block." This means walking `g_head` walks the entire region in address order,
  whether blocks are free or used.
- `is_free` is what determines whether `my_malloc` can use a block.

### 4.2 Global state

```c
static block_header_t *g_head = NULL;  /* first block in the region; NULL until init */
```

That's the entire global state. No free-list separate from the block list, no size
classes, no bookkeeping arrays. One pointer to one linked list.

### 4.3 Memory layout invariants

These must always hold. If you ever observe them broken, you have a bug:

1. **Coverage.** Walking `g_head` covers the entire 1 MiB region exactly — no gaps,
   no overlaps.
2. **Header alignment.** Every block header starts at an address aligned to 16 bytes.
3. **Payload alignment.** Every payload pointer returned to the user is aligned to
   16 bytes. This holds automatically if `sizeof(block_header_t)` is a multiple of 16.
4. **Size accounting.** For any block, `(size_t)next - (size_t)current ==
   sizeof(block_header_t) + current->size`, except when `next == NULL`, in which case
   `current` is the last block and extends to the end of the region.

---

## 5. Algorithms

### 5.1 Initialization (`heap_init`)

Called lazily from `my_malloc` the first time it's invoked. Steps:

1. Call `mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)`.
2. If `mmap` returns `MAP_FAILED`, return -1 (caller will return NULL to user).
3. Cast the returned pointer to `block_header_t *` and assign to `g_head`.
4. Initialize this one block:
   - `size = REGION_SIZE - sizeof(block_header_t)` (one big free payload)
   - `next = NULL` (only block)
   - `is_free = 1`
5. Return 0.

After init, the region contains exactly one free block covering everything.

### 5.2 Allocation (`my_malloc`)

```
INPUT:  size (bytes requested by user)
OUTPUT: pointer to a payload region of at least `size` bytes, or NULL

1. If size == 0, return NULL.
2. If g_head == NULL, call heap_init(). If init fails, return NULL.
3. Align size up to 16:   size = (size + 15) & ~15
4. Walk the block list from g_head:
     for cur = g_head; cur != NULL; cur = cur->next:
         if cur->is_free AND cur->size >= size:
             FOUND. Go to step 5.
   If no block found, return NULL (out of memory).
5. (Stage 3) Maybe split the block:
     leftover = cur->size - size
     if leftover > sizeof(block_header_t) + MIN_SPLIT_PAYLOAD:
         new_block_addr = (char*)cur + sizeof(block_header_t) + size
         new_block = (block_header_t*) new_block_addr
         new_block->size = leftover - sizeof(block_header_t)
         new_block->next = cur->next
         new_block->is_free = 1
         cur->next = new_block
         cur->size = size
6. Mark cur as used:
     cur->is_free = 0
7. Return pointer just past the header:
     return (char*)cur + sizeof(block_header_t)
```

`MIN_SPLIT_PAYLOAD` is a small constant (e.g., 16). Don't split if the leftover
block would have a payload of less than this — too small to be useful, just wastes
header space.

### 5.3 Free (`my_free`)

```
INPUT:  ptr (a payload pointer previously returned by my_malloc)
OUTPUT: nothing

1. If ptr == NULL, return immediately (this is allowed and is a no-op).
2. Recover the header:
     header = (block_header_t*)((char*)ptr - sizeof(block_header_t))
3. Mark it free:
     header->is_free = 1
4. (Future / v0.2) Coalesce with header->next if next is also free:
     if header->next != NULL AND header->next->is_free:
         header->size += sizeof(block_header_t) + header->next->size
         header->next = header->next->next
```

Step 4 is **out of scope for tomorrow.** Do not implement it on day 1. It's listed
here so the spec is complete, but ship without it.

### 5.4 Dump (`my_alloc_dump`) — debug helper

Walks the entire block list and prints each block's address, size, and free/used
status. Use this constantly while debugging. There is no requirement on the exact
output format; it's purely for you.

---

## 6. Concrete example trace

To make the algorithm tangible, here's what the region looks like over a sequence
of operations. Sizes are approximate; the real numbers depend on `sizeof(block_header_t)`,
which is typically 24-32 bytes on a 64-bit system.

**After init:**
```
[ hdr (1MiB-24 free) ]
                       (one giant free block)
```

**After `p1 = my_malloc(100)`** (rounded up to 112 with alignment):
```
[ hdr (112 used) ][ payload p1 ][ hdr (1MiB-160 free) ]
                                  (rest, still one big free block)
```

**After `p2 = my_malloc(200)`** (rounded to 208):
```
[ hdr (112 used) ][ p1 ][ hdr (208 used) ][ p2 ][ hdr (rest free) ]
```

**After `my_free(p1)`:**
```
[ hdr (112 FREE) ][ p1 ][ hdr (208 used) ][ p2 ][ hdr (rest free) ]
                  ^ memory technically dirty, but block is free
```

**After `p3 = my_malloc(50)`** (rounded to 64) — first-fit reuses p1's block:
With splitting: the 112-byte block splits into a 64-byte used block + a leftover
free block.
```
[ hdr (64 used) ][ p3 ][ hdr (24-ish FREE) ][ hdr (208 used) ][ p2 ][ hdr (rest free) ]
```
(Whether the leftover is large enough to split depends on your `MIN_SPLIT_PAYLOAD`
threshold and `sizeof(block_header_t)`.)

This is the canonical picture. When something goes wrong, draw this same picture
on paper for your current state and compare to what `my_alloc_dump` prints.

---

## 7. Edge cases and how to handle each

| Case                                  | Required behavior                              |
|---------------------------------------|------------------------------------------------|
| `my_malloc(0)`                        | Return `NULL`                                  |
| `my_malloc(huge_number)`              | Return `NULL` (out of memory)                  |
| First-ever call (heap not init'd)     | Call `heap_init`; if it fails, return `NULL`   |
| No free block large enough            | Return `NULL`                                  |
| Free block exactly matches size       | Use it whole, no split                         |
| Free block much larger than size      | Split (stage 3) so leftover stays usable       |
| `my_free(NULL)`                       | No-op, return immediately                      |
| `my_free` on already-freed pointer    | Undefined behavior in v0.1 (do not handle)     |
| `my_free` on non-allocator pointer    | Undefined behavior (do not handle)             |

The last two are real problems for production allocators but explicitly out of scope
here. Document this in the README.

---

## 8. Constants

```c
#define REGION_SIZE        (1u << 20)   /* 1 MiB total */
#define ALIGNMENT          16
#define MIN_SPLIT_PAYLOAD  16           /* don't split if leftover < this */
```

Why these values:
- `REGION_SIZE = 1 MiB`: small enough that bugs show up fast (you'll exhaust it in
  tests), large enough that you can run hundreds of allocations.
- `ALIGNMENT = 16`: matches the de-facto standard on both x86-64 and ARM64. It's
  what `malloc` actually returns on macOS and Linux.
- `MIN_SPLIT_PAYLOAD = 16`: a leftover block smaller than the alignment is useless;
  don't bother creating it.

---

## 9. Stages and what each delivers

### Stage 1 — Bump allocator (warm-up, ~30 min)

A simplified version: ignore the block list entirely, just maintain a `next_free`
pointer that advances on each allocation. `my_free` does nothing.

**Deliverable:** `my_malloc` returns distinct, aligned pointers that you can write
to. The bump variant gets thrown away in stage 2, but it forces you to get the
`mmap` call, alignment, and pointer arithmetic right before adding any data
structure complexity.

**Tests passing:** `test_basic_alloc`, `test_multiple_allocs`.

### Stage 2 — Free list with first-fit (~2 hr)

Replace the bump cursor with the block-list design in section 4. Implement the
allocation algorithm from section 5.2, but **skip step 5 (splitting)** for now —
just use the entire chosen block.

This means after `my_malloc(8)`, the whole 1 MiB-ish initial block is now "used"
with only 8 useful bytes — terrible, but valid. The next `my_malloc` will return
NULL until something is freed.

**Deliverable:** allocations work, frees mark blocks reusable, `my_alloc_dump`
shows what's going on. Reuse works: free a block, alloc again, get the same
address back.

**Tests passing:** all stage 1 tests plus `test_free_and_reuse`.

### Stage 3 — Block splitting (~1 hr)

Add step 5 from section 5.2. Now a small allocation only consumes a small block,
leaving the rest free.

**Deliverable:** can do many small allocations within the region, see them fit
side-by-side via `my_alloc_dump`.

**Tests passing:** all previous tests plus `test_varied_sizes`.

### Stage 4 — Polish and push (~1 hr)

- Run all tests under AddressSanitizer:
  `cmake -B build-asan -DUSE_ASAN=ON && cmake --build build-asan && ./build-asan/tests`
  Output must be clean (no ASan reports).
- Update README with anything you learned, any decisions you made differently
  from the spec, anything you debugged that's worth mentioning.
- Final commit, push to GitHub.

**Deliverable:** public, working repo.

---

## 10. What "done" looks like at the end of tomorrow

A GitHub repo with:

- All four stages complete and committed (with reasonable commit messages,
  one per stage at minimum — more is fine)
- Test binary that runs all four enabled tests and prints "All tests passed"
- Clean output under AddressSanitizer
- README that matches the actual state of the code (no lies about features
  that aren't implemented)
- A "Roadmap" section that lists coalescing, realloc/calloc, benchmarks as
  next steps

What it does NOT need to have at end of day 1:

- Coalescing
- realloc / calloc
- Benchmarks
- Thread safety
- CI

Those are the day-2 deliverables that turn a "real student project" into a
"real piece of work." They're easy to add later because the foundation is solid.

---

## 11. Common bugs and how to recognize them

While coding, these are the bugs you'll most likely hit. Write this list down on
paper:

| Symptom                                  | Likely cause                                   |
|------------------------------------------|------------------------------------------------|
| Segfault on first write to allocated mem | Pointer math wrong; returning header, not payload |
| Two `my_malloc` calls return same addr   | Forgot to mark first block used, or list walk broken |
| `my_alloc_dump` shows ever-growing sizes | Split math wrong; leftover bigger than original |
| Sizes drift away from multiples of 16    | Missing `align_up` somewhere                   |
| Crash after several alloc/free cycles    | Header overwritten by payload — alignment / size off-by-one |
| ASan reports "heap-buffer-overflow"      | Writing past end of payload (test bug, not allocator bug) |
| ASan reports "stack-use-after-scope"     | Test bug, not allocator bug                    |

When you hit one, print everything with `my_alloc_dump` before and after the
suspect call, and compare to what the spec says should happen (sections 5 and 6).
Most bugs become obvious in one diff.

---

## 12. Why this project is worth doing

A custom allocator forces you to think carefully about:

- Memory layout in bytes (not abstract objects)
- Alignment requirements
- Pointer arithmetic with explicit casts to `char *`
- The boundary between user data and bookkeeping metadata
- The trade-off space: speed vs fragmentation, simplicity vs features
- Why real `malloc` is more complex than this (size classes, arenas, thread locality)

These are exactly the skills C/C++ systems hiring managers test for. Even a v0.1
allocator gives you something concrete and personal to talk about in an interview
that nobody else will have.
