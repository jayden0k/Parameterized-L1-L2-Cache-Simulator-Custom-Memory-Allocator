// ============================================================================
// workload_generator.hpp
//
// SyntheticWorkloadGenerator produces parameterized synthetic access traces
// (as TraceEntry sequences, directly consumable by Cache/CacheHierarchy or
// by the CacheAllocatorBridge) for the four canonical access patterns used
// to stress-test cache behavior:
//
//   Sequential      — monotonically increasing address stream (best case
//                      for spatial locality / prefetch-friendly).
//   Stride          — fixed-stride walk over an address range; strides that
//                      equal or exceed the line size defeat spatial reuse,
//                      while strides that are multiples of (cache_size /
//                      associativity) intentionally induce set conflicts.
//   Random          — uniformly random addresses across the address space
//                      (worst case for temporal/spatial locality; used to
//                      characterize compulsory + capacity miss behavior).
//   SpatialCluster  — repeated bursts of accesses to small contiguous
//                      "clusters" scattered across the address space,
//                      modeling real workloads that combine locality
//                      within an object with poor locality across objects.
// ============================================================================
#pragma once

#include "utils/trace_parser.hpp"

#include <cstdint>
#include <vector>

namespace cas::utils {

enum class WorkloadPattern : std::uint8_t { Sequential, Stride, Random, SpatialCluster };

struct WorkloadConfig {
    WorkloadPattern pattern = WorkloadPattern::Sequential;
    std::uint64_t base_address = 0x10000;
    std::size_t num_accesses = 1000;
    std::size_t access_size_bytes = 8;

    // Stride pattern only.
    std::size_t stride_bytes = 64;

    // Random / SpatialCluster patterns only: the [base_address, base_address
    // + address_space_bytes) window accesses are drawn from.
    std::size_t address_space_bytes = 1u << 20; // 1 MiB

    // SpatialCluster pattern only: number of consecutive accesses issued
    // within a cluster before jumping to a new randomly chosen cluster.
    std::size_t cluster_size = 8;
    std::size_t cluster_stride_bytes = 64;

    unsigned seed = 42;               // PRNG seed; fixes reproducibility
    double write_ratio = 0.3;         // probability an access is a Write vs Read
};

class SyntheticWorkloadGenerator {
public:
    [[nodiscard]] static std::vector<TraceEntry> generate(const WorkloadConfig& config);

private:
    [[nodiscard]] static std::vector<TraceEntry> generate_sequential(const WorkloadConfig& c);
    [[nodiscard]] static std::vector<TraceEntry> generate_stride(const WorkloadConfig& c);
    [[nodiscard]] static std::vector<TraceEntry> generate_random(const WorkloadConfig& c);
    [[nodiscard]] static std::vector<TraceEntry> generate_spatial_cluster(const WorkloadConfig& c);
};

} // namespace cas::utils
