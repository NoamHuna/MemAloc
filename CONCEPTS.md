# Memory Allocator — Concepts and System Calls

A deeper look at the *ideas* behind each function and the system calls we'll use
to implement them. SPEC.md tells you *what* to build. This document tells you
*why* it works.

---

## Part 1 — The big picture: what is an allocator, really?

Programs need memory of varying sizes at runtime. They could ask the OS every time
they need a few bytes, but that would be slow: each request crosses the user-kernel
boundary, the kernel updates its page tables, and you're talking thousands of
nanoseconds for what should be tens.

So instead, every program uses an **allocator**: a userspace library that asks the
OS for big chunks of memory once in a while, then hands out small pieces of those
chunks to the program quickly, without involving the kernel again.

**The deal an allocator makes with the program:**

> "Tell me how many bytes you need with `malloc(n)`. I'll give you a pointer to at
> least `n` writable bytes. They're yours until you call `free(ptr)` with that
> exact pointer. Don't write past the end; don't free the same pointer twice;
> don't use a pointer after freeing it."

That's it. The allocator's whole job is to make `malloc(n)` and `free(ptr)` fast
and correct, given that contract.

**The deal the allocator makes with the OS:**

> "Give me a big region of address space. I'll subdivide and recycle it myself.
> I'll tell you when I want more, and you can ignore me until then."

Our allocator makes a 1 MiB region from the OS once (via `mmap`) and never asks
again. Everything afterwards is pointer arithmetic and bookkeeping in our own
data structures.

---

## Part 2 — The system call: `mmap`

`mmap` is the syscall that asks the kernel for a region of virtual memory. Its
signature looks intimidating but most of the parameters are obvious once you
understand what each is for:

```c
void *mmap(void *addr,      /* where in address space (NULL = "kernel picks") */
           size_t length,   /* how many bytes */
           int prot,        /* what operations are allowed (read/write/exec) */
           int flags,       /* how the memory behaves */
           int fd,          /* file descriptor (we don't use a file) */
           off_t offset);   /* offset into the file (we don't use a file) */
```

Our actual call:

```c
void *region = mmap(NULL,                          /* let kernel choose address */
                    REGION_SIZE,                   /* 1 MiB */
                    PROT_READ | PROT_WRITE,        /* we can read and write it */
                    MAP_PRIVATE | MAP_ANONYMOUS,   /* private to us, not file-backed */
                    -1,                            /* no fd (anonymous) */
                    0);                            /* no offset (anonymous) */
```

### What each part means

- **`addr = NULL`**: We don't care where in our virtual address space the region
  lives. The kernel picks somewhere reasonable. (Asking for a specific address is
  for very advanced cases like JIT compilers or shared memory.)

- **`length = REGION_SIZE`**: We get 1 MiB. The kernel will round this up to a
  multiple of the page size (4 KiB on most systems, 16 KiB on Apple Silicon), but
  for 1 MiB that's already aligned.

- **`prot = PROT_READ | PROT_WRITE`**: The region is readable and writable. Not
  executable — we're not writing JIT code. If we try to execute bytes in this
  region, the CPU traps to the kernel and the kernel kills our process.

- **`flags = MAP_PRIVATE | MAP_ANONYMOUS`**: This is the magic combination for
  "give me fresh memory, not connected to any file." `MAP_ANONYMOUS` means "no
  file backing." `MAP_PRIVATE` means "changes don't propagate anywhere; this is
  just mine." Together they say "fresh, zeroed memory, all for me."

- **`fd = -1, offset = 0`**: Required to be these specific values when
  `MAP_ANONYMOUS` is set. They're meaningful when you're mapping a file, irrelevant
  when you're not.

### What `mmap` returns

A pointer to the start of the region — or `MAP_FAILED` (which is `(void*)-1`,
not `NULL`!) on failure. Always check `if (region == MAP_FAILED)`, not
`if (region == NULL)`.

### Why `mmap` and not other options

There are three ways a userspace program can get memory from the OS:

1. **`brk`/`sbrk`** — The classic Unix way. There's a "program break" pointer,
   and you can move it up to extend your data segment. Old, simple, but
   one-dimensional (only one heap, can only grow) and deprecated on macOS.

2. **`mmap`** — The modern way. You can request as many independent regions as
   you want, anywhere in your address space. This is what `malloc` actually uses
   for large allocations on every modern system.

3. **The stack** — Grows automatically when you call functions. Out of scope for
   an allocator.

We use `mmap` because: it's modern, it works the same on macOS and Linux (with
the `MAP_ANON` vs `MAP_ANONYMOUS` naming quirk), it gives us pre-zeroed memory,
and it's what every real allocator uses for non-trivial sizes.

### The corresponding "undo"

When the process exits, the OS reclaims everything automatically. If we ever
wanted to give the region back to the OS explicitly, the syscall is `munmap`:

```c
munmap(region, REGION_SIZE);
```

We don't call this in our v0.1 — the region lives until the process exits.

---

## Part 3 — The ideas behind each function

Now: what is each function actually doing, conceptually?

### `heap_init` — turning raw bytes into a managed region

The OS gave us a 1 MiB blank slate. Just bytes. We need to start *managing* them,
which means imposing structure: deciding where blocks begin and end, tracking
which are in use, knowing which are available.

The cleanest possible starting state: **one giant free block covering everything.**

Visually:
```
[ header ][ ............ payload (1 MiB - sizeof(header)) ............ ]
  size = REGION_SIZE - sizeof(header)
  next = NULL
  is_free = 1
```

That's the entire `heap_init`. Write one header at the start of the region,
describing the whole region as one free block. Future `my_malloc` calls will
carve pieces out of this; future `my_free` calls will mark pieces as available
again.

**The conceptual move:** raw memory → memory with metadata describing what's
where. This is the core idea of every allocator: you store the bookkeeping
*inside* the same memory you're managing. It's like a parking garage where
the signs are painted on the parking spots themselves.

**Why lazy init (only on first `my_malloc`)?** Because if a program never calls
`my_malloc`, it shouldn't pay the cost (1 MiB of virtual address space, the
syscall). Doing it on first use means "you pay for it when you need it." This
is a common pattern in C libraries.

### `my_malloc` — finding a home for `n` bytes

Conceptually, allocation has three sub-problems:

**Problem 1: Where do I look?**

We need somewhere with at least `n` bytes of free, usable space. We have a list
of all blocks — used and free — and we walk it looking at the free ones until
we find one big enough.

This is called "first-fit." Alternatives include:
- **Best-fit:** find the *smallest* free block that's big enough (less waste,
  more search time)
- **Worst-fit:** find the *largest* free block (counterintuitively keeps remaining
  blocks usable)
- **Next-fit:** like first-fit but start where you left off last time (avoids
  always burning through the first blocks)

First-fit is the simplest and good enough for our purposes. We mention this
trade-off in the README — it shows we know there *are* trade-offs.

**Problem 2: What if the block is too big?**

If the user asks for 100 bytes and we find a 10,000-byte free block, giving
them the whole block wastes 9,900 bytes — they'll never be available for
anything else until that 100-byte allocation is freed.

So we **split**: carve off the first 100 bytes (plus header) as the used block,
leave the remaining 9,900-ish bytes as a smaller free block. Now both pieces
are useful: the user got their 100 bytes, and the leftover is still available
for future small allocations.

The threshold (`MIN_SPLIT_PAYLOAD`) exists because there's a minimum useful size
for a block — if splitting would leave a 4-byte free block, that's barely worth
the header overhead. Just give the user the whole block.

**Problem 3: How do I tell the user where their bytes are?**

The user wants a payload pointer, not a header pointer. The address arithmetic:

```c
return (char *)header + sizeof(*header);
```

Notice the cast to `char *`. This is critical. C pointer arithmetic scales by
the pointed-to type. `header + 1` advances by `sizeof(*header)` bytes (so to the
*next header*, not to the byte after this header). `(char *)header + sizeof(*header)`
advances by exactly that many bytes, because `sizeof(char) == 1`. Whenever you're
doing byte-level math in C, cast to `char *` first. This is one of the most common
sources of pointer bugs in allocator code.

### `my_free` — giving bytes back

Conceptually, the inverse of `my_malloc`: take the payload pointer the user has,
find the block it belongs to, mark that block free.

**The reverse arithmetic:**

```c
block_header_t *header = (block_header_t *)((char *)ptr - sizeof(block_header_t));
```

We subtract the header size in bytes (note the `char *` cast again) to get back
to the start of the header. We trust the user that `ptr` is genuinely a pointer
we returned earlier — if they pass garbage, behavior is undefined.

**Why we don't validate the pointer:**

A production allocator might check that `ptr` is within the region, that the
header looks plausible, that the block isn't already free. We don't, because:
- It's slow (extra checks on every `free`)
- It's surprisingly hard to do well (you can't perfectly detect a forged header)
- That's what AddressSanitizer is for during testing

**The actual work:**

```c
header->is_free = 1;
```

One line. That's all "freeing" actually means: flip a flag. The memory is still
physically there. Its contents are still whatever the user wrote into it
(probably; the OS won't zero it for us). It's just available for future
allocations now.

**Why coalescing is a v0.2 feature:**

Without coalescing, this scenario is bad:
```
[ 50-byte FREE ][ 50-byte FREE ][ 50-byte FREE ]
```
Three adjacent free blocks, total 150 bytes free. But if the user asks for 120
bytes, *none* of these individual blocks is big enough. Allocation fails despite
plenty of free memory.

Coalescing fixes this: when freeing a block, check if the *next* block is also
free, and merge them. After coalescing, the three blocks above become:
```
[ 150-ish-byte FREE ]
```
Now a 120-byte request succeeds.

We're skipping this for v0.1 because (a) it requires careful pointer juggling and
(b) it doubles the trickiness of `my_free`. Better to ship a smaller correct thing
and add coalescing tomorrow.

### `my_alloc_dump` — your debugging eyes

Not exciting conceptually, but practically essential. You will spend hours
debugging this allocator. The only way to see what's happening inside the region
is to walk the list and print every block.

A good dump tells you, at a glance:
- How many blocks exist
- Which are free, which are used
- Their sizes
- Their addresses (so you can verify they're contiguous and aligned)

Call this liberally during development. Then before you push, leave the function
in the code — it's useful, and it shows reviewers you wrote with debuggability in
mind.

---

## Part 4 — The toolbox: every system call and library function we use

Here's the complete inventory. For a project this small it's a short list, which
is part of why memory allocators are a beloved teaching project — you learn one
syscall deeply rather than ten shallowly.

### `mmap` — `<sys/mman.h>`

Get a region of memory from the OS. Covered in detail in Part 2.

### `munmap` — `<sys/mman.h>`

Give a region back. *Not used in v0.1*, listed for completeness. The OS reclaims
everything on process exit anyway.

### `printf` — `<stdio.h>`

Standard C output. Used only in `my_alloc_dump` for debug output. Not in the
allocator's hot path.

### `assert` — `<assert.h>`

Used in the test file to verify expectations. Not in the allocator itself.

### `memset` — `<string.h>`

Used in tests to fill allocated memory with a known pattern, then verify that
subsequent operations don't trample it. Not in the allocator.

### Integer types from `<stdint.h>` and `<stddef.h>`

- `size_t` (from `<stddef.h>`): the type for "a count of bytes" or "an index."
  Used throughout. It's unsigned and big enough to hold any object size on the
  platform.
- `uintptr_t` (from `<stdint.h>`): an integer big enough to hold a pointer.
  Useful if you ever need to do integer math on addresses (we mostly don't —
  `char *` arithmetic is cleaner).

### Compile-time only: `sizeof`, `offsetof`

Not functions, but worth mentioning:
- `sizeof(block_header_t)` — the size of our header in bytes. Compiler computes
  this at compile time. Used everywhere we convert between header pointers and
  payload pointers.
- `offsetof(struct, field)` — the byte offset of a field within a struct. We
  probably won't need it for v0.1, but it's the right tool if you ever want
  "given a pointer to a field, get back to the containing struct."

### That's it

No `malloc`, no `free`, no `realloc`, no `brk`, no `sbrk` — those are exactly
the things we're *replacing*. No threading primitives, no file I/O, no signals.
The complete syscall surface is one `mmap` call.

This minimal toolbox is what makes the project tractable in 4-5 hours. You're
not learning ten APIs; you're learning one syscall and a lot of pointer
arithmetic.

---

## Part 5 — Mental models worth carrying with you

Some shorthand ways of thinking about the system that may help while you code.

### "The header is the block."

Don't think of blocks as having a header and a payload as two separate things.
The header *is* the block from the allocator's point of view. The payload is
just "the bytes after the header that the user gets to write into." When you
walk the list, you're walking headers. When you split, you're creating a new
header. When you mark free, you're flipping a flag in a header.

### "Pointer arithmetic is byte arithmetic, but only on `char *`."

Every time you're tempted to do address math, ask: "Am I in units of bytes, or
units of whatever-type-this-pointer-is?" If bytes, cast to `char *` first. This
single discipline prevents the most common class of allocator bugs.

### "Alignment is a downstream property of layout."

If `sizeof(block_header_t)` is a multiple of 16, and the region starts at a
16-byte-aligned address (it does, `mmap` returns page-aligned addresses), and
every block's payload size is rounded up to a multiple of 16, then every header
and every payload pointer is automatically 16-byte aligned. You get alignment
for free as long as you maintain the invariants. You don't need to "align"
anywhere except in `align_up`.

### "Free doesn't return memory to the OS."

`my_free` doesn't shrink the region. It doesn't tell the kernel anything. It
just flips a flag in our bookkeeping. The memory page is still mapped, still
counts against our process's RSS until the process exits. This is how real
`malloc` works too — that's why programs with leaks slowly grow without ever
shrinking.

### "The allocator is a state machine over a fixed region."

There are only a few legal operations on our region:
- Carve a block out of a free one (allocate)
- Mark a used block free (free)
- (v0.2) Merge two adjacent free blocks (coalesce)
- (v0.3) Split a free block on demand (we do this in allocate)

The state of the region after any sequence of operations is fully determined by
the operations. If you ever lose track of what state you should be in, replay
the operations on paper from `heap_init` and compare.

---

## Part 6 — Why this design is "simple but not toy"

We're building the simplest allocator that's still recognizable as a real
allocator. Each design choice is deliberate:

| Choice                           | Why this and not something fancier         |
|----------------------------------|--------------------------------------------|
| Single mmap'd region             | One syscall, predictable layout            |
| In-band headers                  | Standard technique; alternative is much harder |
| Singly-linked list               | Simplest data structure that works         |
| First-fit                        | Simplest search policy                     |
| Split on allocate                | Required to be more than a toy             |
| No coalescing (v0.1)             | Out of scope; documented as next step      |
| No thread safety                 | Out of scope; documented as next step      |
| 16-byte alignment                | Matches real-world ABI                     |
| Lazy init                        | Pay only when you use it                   |

Every "no" here is a feature we *understand but chose not to implement yet*,
not a feature we don't know about. That's the difference between scoping and
ignorance, and it's what a recruiter wants to see in your README.

---

## Recommended reading order tomorrow

1. Read this document once tonight, slowly. Don't try to memorize — just absorb
   the mental models.
2. In the morning, open SPEC.md §5 (algorithms) and §6 (worked example).
3. Code stage 1.
4. When stuck, come back to this document's Part 3 for the relevant function,
   and SPEC.md §11 for the bug table.
5. Repeat for stages 2 and 3.

Have fun. The first time `my_alloc_dump` prints what you expected after a
non-trivial sequence of operations, it'll click.
