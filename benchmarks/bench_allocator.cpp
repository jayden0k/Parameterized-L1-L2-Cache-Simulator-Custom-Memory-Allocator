// ============================================================================
// bench_allocator.cpp
//
// Micro-benchmarks comparing Memory::PoolAllocator against the system
// allocator (new/delete) across allocation sizes and access patterns, plus
// standalone throughput benchmarks for allocate()/deallocate() and the
// CacheAllocatorBridge integration path.
// ============================================================================
#include "allocator/pool_allocator.hpp"
#include "cache/cache.hpp"
#include "profiler/cache_allocator_bridge.hpp"

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

using namespace cas;

namespace {

cache::CacheHierarchy make_bench_hierarchy() {
    cache::CacheHierarchy::Config cfg;
    cfg.l1_data.cache_size_bytes = 32 * 1024;
    cfg.l1_data.line_size_bytes = 64;
    cfg.l1_data.associativity = 8;
    cfg.l2.cache_size_bytes = 256 * 1024;
    cfg.l2.line_size_bytes = 64;
    cfg.l2.associativity = 16;
    return cache::CacheHierarchy(cfg);
}

} // namespace

// ----------------------------------------------------------------------------
// Fixed-size allocate/deallocate throughput: PoolAllocator vs new/delete.
// ----------------------------------------------------------------------------
static void BM_PoolAllocator_AllocDealloc(benchmark::State& state) {
    const auto size = static_cast<std::size_t>(state.range(0));
    memory::PoolAllocator alloc(64 * 1024 * 1024); // 64 MiB arena, large enough to avoid OOM across iterations

    for (auto _ : state) {
        void* p = alloc.allocate(size);
        benchmark::DoNotOptimize(p);
        alloc.deallocate(p);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
}
BENCHMARK(BM_PoolAllocator_AllocDealloc)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

static void BM_SystemNewDelete_AllocDealloc(benchmark::State& state) {
    const auto size = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        auto* p = new std::byte[size];
        benchmark::DoNotOptimize(p);
        delete[] p;
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
}
BENCHMARK(BM_SystemNewDelete_AllocDealloc)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// ----------------------------------------------------------------------------
// Over-alignment path cost (alignment > 64B triggers the back-pointer slow path).
// ----------------------------------------------------------------------------
static void BM_PoolAllocator_OverAlignedAlloc(benchmark::State& state) {
    const auto alignment = static_cast<std::size_t>(state.range(0));
    memory::PoolAllocator alloc(64 * 1024 * 1024);

    for (auto _ : state) {
        void* p = alloc.allocate(128, alignment);
        benchmark::DoNotOptimize(p);
        alloc.deallocate(p);
    }
}
BENCHMARK(BM_PoolAllocator_OverAlignedAlloc)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(4096);

// ----------------------------------------------------------------------------
// Sustained churn with a live working set (exercises free-list search depth
// and coalescing under realistic fragmentation pressure).
// ----------------------------------------------------------------------------
static void BM_PoolAllocator_ChurnWithWorkingSet(benchmark::State& state) {
    const int working_set = static_cast<int>(state.range(0));
    memory::PoolAllocator alloc(64 * 1024 * 1024);
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> size_dist(16, 512);

    std::vector<void*> live;
    live.reserve(static_cast<std::size_t>(working_set));
    for (int i = 0; i < working_set; ++i) {
        live.push_back(alloc.allocate(size_dist(rng)));
    }

    std::uniform_int_distribution<int> idx_dist(0, working_set - 1);
    for (auto _ : state) {
        int idx = idx_dist(rng);
        alloc.deallocate(live[static_cast<std::size_t>(idx)]);
        live[static_cast<std::size_t>(idx)] = alloc.allocate(size_dist(rng));
    }

    for (void* p : live) {
        alloc.deallocate(p);
    }
}
BENCHMARK(BM_PoolAllocator_ChurnWithWorkingSet)->Arg(64)->Arg(512)->Arg(4096);

// ----------------------------------------------------------------------------
// CacheAllocatorBridge: end-to-end cost of allocation + simulated cache
// probing, to quantify the profiler's overhead relative to raw allocation.
// ----------------------------------------------------------------------------
static void BM_CacheAllocatorBridge_AllocDealloc(benchmark::State& state) {
    const auto size = static_cast<std::size_t>(state.range(0));
    memory::PoolAllocator alloc(64 * 1024 * 1024);
    cache::CacheHierarchy hierarchy = make_bench_hierarchy();
    profiler::CacheAllocatorBridge bridge(alloc, hierarchy);

    for (auto _ : state) {
        void* p = bridge.allocate(size);
        benchmark::DoNotOptimize(p);
        bridge.deallocate(p, size);
    }
}
BENCHMARK(BM_CacheAllocatorBridge_AllocDealloc)->Arg(16)->Arg(64)->Arg(256)->Arg(1024);

// ----------------------------------------------------------------------------
// Cache simulator raw throughput (no allocator involved): how many simulated
// accesses per second the Cache/CacheHierarchy model can sustain.
// ----------------------------------------------------------------------------
static void BM_CacheHierarchy_SequentialAccess(benchmark::State& state) {
    cache::CacheHierarchy hierarchy = make_bench_hierarchy();
    std::uint64_t addr = 0;
    for (auto _ : state) {
        auto r = hierarchy.access(addr, cache::AccessType::Read, 8);
        benchmark::DoNotOptimize(r);
        addr += 64;
    }
}
BENCHMARK(BM_CacheHierarchy_SequentialAccess);

static void BM_CacheHierarchy_RandomAccess(benchmark::State& state) {
    cache::CacheHierarchy hierarchy = make_bench_hierarchy();
    std::mt19937 rng(1);
    std::uniform_int_distribution<std::uint64_t> addr_dist(0, (1u << 24) - 1);
    for (auto _ : state) {
        auto r = hierarchy.access(addr_dist(rng), cache::AccessType::Read, 8);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_CacheHierarchy_RandomAccess);

BENCHMARK_MAIN();
