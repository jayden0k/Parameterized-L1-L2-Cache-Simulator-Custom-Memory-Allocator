// ============================================================================
// main.cpp — demo driver.
//
// Usage:
//   cas_demo                       runs a built-in synthetic-workload demo
//   cas_demo <trace-file-path>     replays a Dinero/lackey-style trace file
// ============================================================================
#include "allocator/pool_allocator.hpp"
#include "cache/cache.hpp"
#include "profiler/cache_allocator_bridge.hpp"
#include "utils/trace_parser.hpp"
#include "utils/workload_generator.hpp"

#include <iostream>

using namespace cas;

namespace {

cache::CacheHierarchy make_default_hierarchy() {
    cache::CacheHierarchy::Config cfg;

    cfg.l1_data.name = "L1D";
    cfg.l1_data.cache_size_bytes = 32 * 1024;
    cfg.l1_data.line_size_bytes = 64;
    cfg.l1_data.associativity = 8;
    cfg.l1_data.eviction_policy = cache::EvictionPolicyType::LRU;
    cfg.l1_data.write_policy = cache::WritePolicy::WriteBack;
    cfg.l1_data.write_allocate = cache::WriteAllocatePolicy::WriteAllocate;
    cfg.l1_data.hit_latency_cycles = 4;
    cfg.l1_data.miss_penalty_cycles = 1; // local detection overhead only; L2/mem penalty added separately

    cfg.l2.name = "L2";
    cfg.l2.cache_size_bytes = 256 * 1024;
    cfg.l2.line_size_bytes = 64;
    cfg.l2.associativity = 16;
    cfg.l2.eviction_policy = cache::EvictionPolicyType::LRU;
    cfg.l2.write_policy = cache::WritePolicy::WriteBack;
    cfg.l2.write_allocate = cache::WriteAllocatePolicy::WriteAllocate;
    cfg.l2.hit_latency_cycles = 12;
    cfg.l2.miss_penalty_cycles = 1;

    cfg.inclusion = cache::InclusionPolicy::NonInclusiveNonExclusive;
    cfg.main_memory_latency_cycles = 200;
    cfg.split_l1 = false;

    return cache::CacheHierarchy(cfg);
}

void run_trace(cache::CacheHierarchy& hierarchy, const std::vector<utils::TraceEntry>& entries) {
    std::uint64_t total_latency = 0;
    for (const auto& e : entries) {
        auto r = hierarchy.access(e.address, e.type, e.size);
        total_latency += r.total_latency_cycles;
    }
    std::cout << "Replayed " << entries.size() << " accesses, total simulated latency = " << total_latency
              << " cycles\n\n";
    hierarchy.print_summary(std::cout);
}

void run_synthetic_demo() {
    std::cout << "=== Synthetic workload demo (Sequential / Stride / Random / SpatialCluster) ===\n\n";

    for (auto pattern : {utils::WorkloadPattern::Sequential, utils::WorkloadPattern::Stride,
                          utils::WorkloadPattern::Random, utils::WorkloadPattern::SpatialCluster}) {
        utils::WorkloadConfig wc;
        wc.pattern = pattern;
        wc.num_accesses = 20000;
        wc.access_size_bytes = 8;
        wc.stride_bytes = 128;
        wc.address_space_bytes = 4 * 1024 * 1024;
        wc.cluster_size = 16;
        wc.cluster_stride_bytes = 64;
        wc.seed = 1234;
        wc.write_ratio = 0.3;

        auto entries = utils::SyntheticWorkloadGenerator::generate(wc);

        cache::CacheHierarchy hierarchy = make_default_hierarchy();
        std::cout << "--- pattern: " << static_cast<int>(pattern) << " ---\n";
        run_trace(hierarchy, entries);
        std::cout << "\n";
    }
}

void run_allocator_bridge_demo() {
    std::cout << "=== CacheAllocatorBridge demo ===\n\n";

    memory::PoolAllocator allocator(4 * 1024 * 1024); // 4 MiB arena
    cache::CacheHierarchy hierarchy = make_default_hierarchy();
    profiler::CacheAllocatorBridge bridge(allocator, hierarchy);

    std::vector<void*> live;
    for (int i = 0; i < 500; ++i) {
        std::size_t size = 16 + static_cast<std::size_t>(i % 64) * 8;
        void* p = bridge.allocate(size, memory::PoolAllocator::kDefaultAlignment);
        live.push_back(p);
        if (i % 3 == 0 && !live.empty()) {
            bridge.deallocate(live.front(), 16);
            live.erase(live.begin());
        }
    }
    for (void* p : live) {
        bridge.deallocate(p, 16);
    }

    bridge.summary().print(std::cout);
    std::cout << "\n";
    allocator.stats().print(std::cout);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        try {
            auto parsed = utils::TraceParser::parse_file(argv[1]);
            std::cout << "Parsed " << parsed.entries.size() << " entries, " << parsed.errors.size()
                      << " malformed lines skipped.\n\n";
            cache::CacheHierarchy hierarchy = make_default_hierarchy();
            run_trace(hierarchy, parsed.entries);
        } catch (const std::exception& ex) {
            std::cerr << "Error: " << ex.what() << "\n";
            return 1;
        }
        return 0;
    }

    run_synthetic_demo();
    run_allocator_bridge_demo();
    return 0;
}
