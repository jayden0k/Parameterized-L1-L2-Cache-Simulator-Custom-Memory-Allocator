# Cache & Allocator Simulator

A parameterized L1/L2 cache simulator coupled with a custom user-space
memory allocator, written in modern C++20.

## Resume bullet → implementation map

> Developed a configurable L1/L2 cache simulator supporting variable cache
> geometry, LRU/FIFO/LFU replacement policies, write-back/write-through
> behavior, and configurable allocation policies.

- **Variable cache geometry**: `CacheConfig{cache_size_bytes, line_size_bytes,
  associativity}` in `include/cache/cache.hpp`, validated and decomposed into
  offset/index/tag bit widths in `Cache::validate_and_derive_geometry()`
  (`src/cache/cache.cpp`). `associativity == 1` is direct-mapped,
  `associativity == 0` is a fully-associative sentinel, anything else is
  N-way set-associative.
- **LRU/FIFO/LFU**: `IEvictionPolicy` strategy interface + `LRUPolicy`,
  `FIFOPolicy`, `LFUPolicy` in `include/cache/eviction_policy.hpp` /
  `src/cache/eviction_policy.cpp`, selected per-cache via
  `CacheConfig::eviction_policy` and constrained at compile time by the
  `EvictionPolicyConcept` C++20 concept.
- **Write-back/write-through**: `WritePolicy` enum, enforced in
  `Cache::access()`'s hit and miss-fill paths.
- **Configurable allocation policies**: `WriteAllocatePolicy`
  (write-allocate / no-write-allocate) on cache misses, plus
  `InclusionPolicy` (inclusive / exclusive / non-inclusive-non-exclusive)
  governing how `CacheHierarchy` propagates evictions between L1 and L2.

> Implemented a custom 64-byte-aligned memory allocator with boundary-tag
> coalescing, allocation tracking, fragmentation metrics, and explicit
> alignment guarantees using modern C++20 memory primitives.

- **64-byte-aligned by default**: `PoolAllocator::kDefaultAlignment = 64`;
  every `BlockHeader` is `alignas(64)`, so its size is itself a multiple of
  64, which guarantees every payload pointer on the standard path is
  cache-line-aligned "for free" — see the invariant documented on
  `split_block_if_worthwhile()` in `src/allocator/pool_allocator.cpp`.
- **Boundary-tag coalescing**: each block carries `prev_phys`/`next_phys`
  (the physical, address-ordered block list) so `coalesce_with_neighbors()`
  merges adjacent free blocks in O(1) with no search.
- **Allocation tracking**: `AllocatorStats` tracks lifetime/active
  allocation counts, used/free bytes, and free-block counts, read back via
  `PoolAllocator::stats()`.
- **Fragmentation metrics**: `AllocatorStats::external_fragmentation_pct()`
  (`1 - largest_free_block / total_free`) and
  `AllocatorStats::metadata_overhead_pct()` (`header_bytes / pool_bytes`).
- **Explicit alignment guarantees via modern C++20 memory primitives**:
  the over-64-byte-alignment path in `PoolAllocator::allocate()` uses
  `std::align` (`<memory>`) directly against a `(pointer, remaining-space)`
  pair rather than hand-rolled bit masking, alongside `std::byte`,
  `std::span`, `std::optional`, and `alignas`/`std::max_align_t` elsewhere
  in the allocator and cache code.

> Built trace parsing and synthetic workload generation for sequential,
> stride, random, and spatial-locality access patterns, with automated
> unit tests and performance benchmarks.

- **Trace parsing**: `TraceParser` (`include/utils/trace_parser.hpp`)
  reads Dinero/Valgrind-`lackey`-style `L`/`S`/`M`/`I` trace lines.
- **Synthetic workloads**: `SyntheticWorkloadGenerator` (
  `include/utils/workload_generator.hpp`) generates
  `Sequential` / `Stride` / `Random` / `SpatialCluster` access patterns.
- **Automated unit tests**: `tests/test_cache.cpp` (geometry, hit/miss,
  eviction correctness under capacity pressure, hierarchy inclusion
  behavior) and `tests/test_allocator.cpp` (alignment, coalescing,
  fragmentation, corruption/double-free detection, randomized churn), 35
  GoogleTest cases total, all passing.
- **Performance benchmarks**: `benchmarks/bench_allocator.cpp` compares
  `PoolAllocator` against `new`/`delete`, measures the over-alignment
  slow-path cost, sustained churn under a live working set, the
  `CacheAllocatorBridge` integration overhead, and raw `CacheHierarchy`
  throughput, via Google Benchmark.

## Components

| Component | Namespace | Path |
|---|---|---|
| Cache simulator | `cas::cache` | `include/cache`, `src/cache` |
| Eviction policies (LRU/FIFO/LFU) | `cas::cache` | `include/cache/eviction_policy.hpp` |
| Pool allocator | `cas::memory` | `include/allocator`, `src/allocator` |
| Trace parser (Dinero/lackey format) | `cas::utils` | `include/utils/trace_parser.hpp` |
| Synthetic workload generator | `cas::utils` | `include/utils/workload_generator.hpp` |
| Cache/Allocator profiler bridge | `cas::profiler` | `include/profiler`, `src/profiler` |

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # run unit tests
./build/cas_benchmarks                       # run micro-benchmarks
./build/cas_demo                             # run the built-in demo
./build/cas_demo path/to/trace.txt           # replay a Dinero/lackey trace
```

CMake options (pass as `-DOPTION=ON/OFF`):

- `CAS_BUILD_TESTS` (default ON) — build the GoogleTest suite
- `CAS_BUILD_BENCHMARKS` (default ON) — build Google Benchmark suite
- `CAS_BUILD_DEMO` (default ON) — build `cas_demo`
- `CAS_ENABLE_SANITIZERS` (default OFF) — ASan/UBSan in Debug builds
- `CAS_WARNINGS_AS_ERRORS` (default OFF)

GoogleTest and Google Benchmark are fetched automatically via
`FetchContent` on first configure (requires network access to GitHub).

## Cache simulator design

- Geometry (`cache_size`, `line_size`, `associativity`) is validated and
  decomposed into `offset_bits` / `index_bits` / `tag` at construction time.
  `associativity == 0` is a sentinel for fully-associative (single set).
- Eviction policy is a run-time **Strategy** behind `IEvictionPolicy`
  (`LRUPolicy`, `FIFOPolicy`, `LFUPolicy`), constrained at compile time by
  the `EvictionPolicyConcept` C++20 concept. Each cache **set** owns its own
  policy instance so recency/frequency state never leaks across sets.
- `WritePolicy` (write-back/write-through) and `WriteAllocatePolicy`
  (write-allocate/no-write-allocate) are independently configurable.
- `CacheHierarchy` composes an L1 (unified, or split I/D) with an L2,
  forwards misses, propagates dirty L1 writebacks into L2 before probing it,
  and enforces `InclusionPolicy` (inclusive back-invalidation / exclusive
  single-residency / non-inclusive-non-exclusive).

## Allocator design

`Memory::PoolAllocator` (namespace `cas::memory`) reserves one fixed-size
arena directly from the OS (`mmap`/`VirtualAlloc` — **no libc `malloc`/`new`
is used inside the allocator's own implementation**) and sub-allocates from
it using:

- An embedded, cache-line-sized (`alignas(64)`) `BlockHeader` placed
  immediately before every block's payload, containing size, free/used
  status, an **intrusive physical block list** (`prev_phys`/`next_phys`) for
  O(1) neighbor coalescing, an **intrusive free list**
  (`prev_free`/`next_free`), and a magic-number guard used to detect
  corruption, double-frees, and foreign pointers.
- Default 64-byte alignment falls out "for free" from the header size being
  a multiple of 64; alignments above 64 bytes use an over-allocation +
  back-pointer scheme (the true header address is stashed 8 bytes before the
  aligned pointer handed back to the caller).
- `AllocatorStats` reports external fragmentation
  (`1 - largest_free_block / total_free`) and metadata overhead
  (`header_bytes / total_pool_bytes`).

## AI-assisted utilities

- `TraceParser` reads Dinero/Valgrind-`lackey`-style traces (`L`/`S`/`M`/`I`
  ops), tolerating and reporting malformed lines without aborting the whole
  parse.
- `SyntheticWorkloadGenerator` produces Sequential, Stride, Random, and
  SpatialCluster access patterns with reproducible seeding.
- GoogleTest suites cover geometry/bit-arithmetic, hit/miss semantics under
  every write/allocate policy combination, eviction correctness under
  capacity pressure for all three policies (including LFU tie-breaking),
  hierarchy inclusion/exclusion behavior, allocator alignment guarantees,
  O(1) coalescing, fragmentation metrics, corruption/double-free detection,
  and randomized churn stress tests validated via `validate_heap()`.
