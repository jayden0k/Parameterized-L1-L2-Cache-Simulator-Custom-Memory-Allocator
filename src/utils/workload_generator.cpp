#include "utils/workload_generator.hpp"

#include <random>
#include <stdexcept>

namespace cas::utils {

namespace {
[[nodiscard]] cache::AccessType roll_access_type(std::bernoulli_distribution& write_dist, std::mt19937& rng) {
    return write_dist(rng) ? cache::AccessType::Write : cache::AccessType::Read;
}
} // namespace

std::vector<TraceEntry> SyntheticWorkloadGenerator::generate_sequential(const WorkloadConfig& c) {
    std::vector<TraceEntry> out;
    out.reserve(c.num_accesses);

    std::mt19937 rng(c.seed);
    std::bernoulli_distribution write_dist(c.write_ratio);

    std::uint64_t addr = c.base_address;
    for (std::size_t i = 0; i < c.num_accesses; ++i) {
        out.push_back(TraceEntry{
            .type = roll_access_type(write_dist, rng), .address = addr, .size = c.access_size_bytes});
        addr += c.access_size_bytes;
    }
    return out;
}

std::vector<TraceEntry> SyntheticWorkloadGenerator::generate_stride(const WorkloadConfig& c) {
    if (c.stride_bytes == 0) {
        throw std::invalid_argument("SyntheticWorkloadGenerator: stride_bytes must be > 0");
    }

    std::vector<TraceEntry> out;
    out.reserve(c.num_accesses);

    std::mt19937 rng(c.seed);
    std::bernoulli_distribution write_dist(c.write_ratio);

    std::uint64_t addr = c.base_address;
    for (std::size_t i = 0; i < c.num_accesses; ++i) {
        out.push_back(TraceEntry{
            .type = roll_access_type(write_dist, rng), .address = addr, .size = c.access_size_bytes});
        addr += c.stride_bytes;
    }
    return out;
}

std::vector<TraceEntry> SyntheticWorkloadGenerator::generate_random(const WorkloadConfig& c) {
    if (c.address_space_bytes == 0) {
        throw std::invalid_argument("SyntheticWorkloadGenerator: address_space_bytes must be > 0");
    }

    std::vector<TraceEntry> out;
    out.reserve(c.num_accesses);

    std::mt19937 rng(c.seed);
    std::bernoulli_distribution write_dist(c.write_ratio);
    std::uniform_int_distribution<std::uint64_t> addr_dist(
        0, static_cast<std::uint64_t>(c.address_space_bytes - 1));

    for (std::size_t i = 0; i < c.num_accesses; ++i) {
        const std::uint64_t addr = c.base_address + addr_dist(rng);
        out.push_back(TraceEntry{
            .type = roll_access_type(write_dist, rng), .address = addr, .size = c.access_size_bytes});
    }
    return out;
}

std::vector<TraceEntry> SyntheticWorkloadGenerator::generate_spatial_cluster(const WorkloadConfig& c) {
    if (c.address_space_bytes == 0 || c.cluster_size == 0) {
        throw std::invalid_argument(
            "SyntheticWorkloadGenerator: address_space_bytes and cluster_size must be > 0");
    }

    std::vector<TraceEntry> out;
    out.reserve(c.num_accesses);

    std::mt19937 rng(c.seed);
    std::bernoulli_distribution write_dist(c.write_ratio);
    std::uniform_int_distribution<std::uint64_t> cluster_origin_dist(
        0, static_cast<std::uint64_t>(c.address_space_bytes - 1));

    std::size_t emitted = 0;
    while (emitted < c.num_accesses) {
        const std::uint64_t cluster_base = c.base_address + cluster_origin_dist(rng);
        for (std::size_t j = 0; j < c.cluster_size && emitted < c.num_accesses; ++j, ++emitted) {
            const std::uint64_t addr = cluster_base + j * c.cluster_stride_bytes;
            out.push_back(TraceEntry{
                .type = roll_access_type(write_dist, rng), .address = addr, .size = c.access_size_bytes});
        }
    }
    return out;
}

std::vector<TraceEntry> SyntheticWorkloadGenerator::generate(const WorkloadConfig& config) {
    switch (config.pattern) {
        case WorkloadPattern::Sequential:     return generate_sequential(config);
        case WorkloadPattern::Stride:         return generate_stride(config);
        case WorkloadPattern::Random:         return generate_random(config);
        case WorkloadPattern::SpatialCluster: return generate_spatial_cluster(config);
    }
    throw std::invalid_argument("SyntheticWorkloadGenerator: unknown WorkloadPattern");
}

} // namespace cas::utils
