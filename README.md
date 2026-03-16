# halloc

A custom heap memory allocator written in C, implementing `malloc`, `free`, `calloc`, and `realloc` from scratch. Uses boundary tags for O(1) coalescing, an explicit doubly-linked free list, and communicates directly with the OS via `VirtualAlloc` (Windows) / `mmap` (Linux).

Outperforms system `malloc` by ~30% on bulk allocation workloads.

---

## Why build a memory allocator?

Every C and C++ program ultimately depends on a memory allocator. Understanding how one works — really works, at the level of raw pointers, bit manipulation, and OS memory APIs — is the difference between using a tool and understanding it.

This project implements the same fundamental techniques used in production allocators like **dlmalloc**, **jemalloc**, and **tcmalloc**: boundary tags for constant-time coalescing, free lists for fast block lookup, and splitting to minimise internal fragmentation.

---

## Features

- `halloc` / `hfree` / `hcalloc` / `hrealloc` — complete standard allocator interface
- **Boundary tags** — every block has a header and footer encoding size and free status, enabling O(1) coalescing in both directions without traversing the heap
- **Explicit doubly-linked free list** — stored inside free blocks themselves, O(1) insert and remove
- **Coalescing** — adjacent free blocks are merged immediately on `hfree`, preventing external fragmentation
- **Splitting** — oversized free blocks are split on allocation, minimising internal fragmentation
- **16-byte alignment** — all returned pointers satisfy x64 SIMD alignment requirements
- **Sentinel blocks** — prologue and epilogue blocks prevent coalescing from walking off heap boundaries
- **Cross-platform** — `VirtualAlloc` on Windows, `mmap` on Linux/macOS
- **28 tests** covering correctness, alignment, coalescing, and stress scenarios
- **Benchmark suite** comparing throughput against system `malloc`

---

## Architecture

### Block layout

Every allocation is represented as a block in memory:

```
┌──────────────────────────────────────┐  ← block_header (16 bytes)
│  size_and_flags                      │    bits 63..1 = block size
│  _pad                                │    bit 0 = free flag (1=free, 0=used)
├──────────────────────────────────────┤  ← user pointer (returned to caller)
│                                      │
│  user data (size bytes)              │
│  [if free: free_node prev/next here] │
│                                      │
├──────────────────────────────────────┤  ← block_footer (16 bytes)
│  size_and_flags (mirrors header)     │
│  _pad                                │
└──────────────────────────────────────┘
```

The footer is the key insight — it makes the previous block's size readable in O(1) without traversing the heap forward, enabling **bidirectional coalescing**.

### Free list

Free blocks repurpose their user data region to store a doubly-linked list node:

```c
typedef struct free_node {
    struct free_node* prev;
    struct free_node* next;
} free_node;
```

This means the free list has zero memory overhead beyond the block overhead already paid — we use memory that would otherwise be wasted.

### Heap layout

```
[prologue][main free block                              ][epilogue]
    ↑                                                        ↑
always allocated,                                    always allocated,
size=0, prevents                                     size=0, prevents
backward coalesc                                     forward coalesc
```

### Coalescing — four cases

On every `hfree`, the allocator checks both neighbouring blocks:

```
Case 1: [USED][this][USED]  →  no merge
Case 2: [USED][this][FREE]  →  merge with next
Case 3: [FREE][this][USED]  →  merge with prev
Case 4: [FREE][this][FREE]  →  merge with both
```

All four cases run in O(1) because boundary tags give us immediate access to neighbour sizes.

---

## Performance

Benchmarked on Windows x64 (Release build, 50,000 iterations):

```
[ Small allocations (64 bytes) ]
  halloc(64)                            30,554,876 ops/sec    32.7 ns/op
  hfree(64)                             29,664,787 ops/sec    33.7 ns/op
  malloc(64)                            23,350,301 ops/sec    42.8 ns/op   ← system
  free(64)                              25,425,883 ops/sec    39.3 ns/op   ← system

[ Mixed sizes (8 - 1024 bytes) ]
  halloc+hfree mixed sizes              14,713,672 ops/sec    68.0 ns/op
  malloc+free mixed sizes               12,909,556 ops/sec    77.5 ns/op   ← system

[ Interleaved alloc/free ]
  halloc+hfree interleaved              17,917,936 ops/sec    55.8 ns/op
  malloc+free interleaved               20,237,999 ops/sec    49.4 ns/op   ← system
```

**halloc is ~30% faster than system malloc on bulk allocation workloads** because it operates on a single pre-allocated heap region with no OS calls per allocation and no thread-safety overhead.

**System malloc wins on interleaved workloads** because production allocators use segregated size classes — separate free lists per size range — which avoid the linear free list traversal that halloc's first-fit strategy performs. This is the key limitation of the current design and the primary motivation for size-class allocators like jemalloc.

---

## Key design decisions

**Boundary tags over header-only blocks** — storing the block size in both the header and footer doubles the per-block overhead (32 bytes vs 16 bytes) but enables O(1) backward coalescing. Without a footer, merging with the previous block requires a full forward scan from the heap start. For workloads with many short-lived allocations, the coalescing benefit far outweighs the overhead cost.

**Explicit free list over implicit** — an implicit free list walks every block (allocated and free) on each allocation. An explicit free list only walks free blocks, making allocation O(free blocks) rather than O(total blocks). Under high allocation pressure this is a significant difference.

**First-fit over best-fit** — first-fit allocates the first free block large enough to satisfy the request. Best-fit searches the entire free list for the smallest sufficient block, minimising wasted space but at O(n) cost per allocation. First-fit is faster and performs well in practice because coalescing keeps large blocks available.

**Size and free flag packed into one word** — block size is always a multiple of 16 (due to alignment), so the lowest 4 bits of the size field are always zero. We steal bit 0 to store the free/allocated flag. This avoids a separate flag field and keeps the header compact.

**Sentinel blocks** — rather than special-casing the first and last blocks in every coalesce operation, we place zero-size allocated prologue and epilogue blocks at the heap boundaries. Coalescing naturally stops when it reads a zero-size allocated neighbour. This simplifies the four coalesce cases to identical code paths.

---

## Building

**Prerequisites:** MSVC (Windows) or GCC/Clang (Linux/macOS), GNU Make.

```cmd
# Windows (x64 Native Tools Command Prompt)
nmake all

# Linux / macOS
make all
```

Produces `halloc_test` and `bench` executables.

---

## Running tests

```cmd
.\halloc_test.exe
```

```
--- Basic allocation ---
  [PASS] halloc(8) returns non-null
  [PASS] halloc(8) is 16-byte aligned
  ...
--- Stress — 10000 random allocations and frees ---
  [PASS] 10000 allocations all succeed and are aligned
  [PASS] No memory corruption detected across 10000 blocks
  [PASS] 1MB allocation succeeds after full stress cycle

========================================
  28 tests run | 28 passed | 0 failed
========================================
```

## Running benchmarks

```cmd
.\bench.exe
```

---

## Limitations and future work

- **Single-threaded** — no mutex protection. A production allocator would use per-thread caches (like tcmalloc's thread-local storage) to avoid lock contention entirely.
- **Fixed heap size** — the heap is pre-allocated at initialisation. A production allocator grows the heap on demand via additional `VirtualAlloc`/`mmap` calls.
- **First-fit only** — segregated size classes (separate free lists per size range) would dramatically improve interleaved workload performance by eliminating free list traversal for common sizes.
- **No large allocation path** — allocations above a threshold (e.g. 512KB) should bypass the free list entirely and go directly to `VirtualAlloc`/`mmap`, as jemalloc and mimalloc do.
