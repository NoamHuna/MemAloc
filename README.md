# Custom Memory Allocator

A free-list memory allocator in C, built from scratch to understand how `malloc` works under the hood.

## Design

- Single `mmap`-backed region of 1 MiB
- Singly-linked list of block headers (size, next, is_free)
- First-fit allocation
- Block splitting on allocate (leftover stays free and reusable)
- 16-byte alignment

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

## Tested on

- macOS (Apple Clang)
- _(also runs on Linux with GCC — TODO: set up CI)_

## Trade-offs and limitations

- First-fit, not best-fit — faster but more fragmentation
- No coalescing yet — adjacent freed blocks don't merge (coming in v0.2)
- Single fixed region — capped at `REGION_SIZE`, no growth
- Not thread-safe

## Roadmap

- [ ] Coalescing on `my_free`
- [ ] `my_realloc`, `my_calloc`
- [ ] Benchmarks against `glibc` malloc
- [ ] Optional thread safety with a mutex
