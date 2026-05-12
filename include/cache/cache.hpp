// ============================================================================
// cache.hpp
//
// Parameterized set-associative cache model (Core::Cache equivalent, in the
// cas::cache namespace) plus a two-level CacheHierarchy that wires an L1
// (unified, or split I/D) in front of an L2, with configurable write policy,
// write-allocate policy, and inclusion policy.
//
// Address decomposition follows the classic scheme:
//
//     [ ... tag ... | index | offset ]
//              MSB                 LSB
//
//   offset_bits = log2(line_size_bytes)
//   index_bits  = log2(num_sets)
//   tag         = remaining high-order bits
//
// Direct-mapped  == associativity 1, num_sets == num_lines
// Set-associative == associativity N, num_sets == num_lines / N
// Fully-associative == associativity == num_lines, num_sets == 1
// ============================================================================
#pragma once

#include "cache/eviction_policy.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace cas::cache {

enum class AccessType : std::uint8_t { Read, Write, InstructionFetch };
enum class WritePolicy : std::uint8_t { WriteBack, WriteThrough };
enum class WriteAllocatePolicy : std::uint8_t { WriteAllocate, NoWriteAllocate };
enum class InclusionPolicy : std::uint8_t { Inclusive, Exclusive, NonInclusiveNonExclusive };

[[nodiscard]] std::string_view to_string(AccessType t) noexcept;
[[nodiscard]] std::string_view to_string(WritePolicy p) noexcept;
[[nodiscard]] std::string_view to_string(WriteAllocatePolicy p) noexcept;
[[nodiscard]] std::string_view to_string(InclusionPolicy p) noexcept;

// ----------------------------------------------------------------------------
// CacheConfig — full geometry + behavioral configuration for a single level.
// ----------------------------------------------------------------------------
struct CacheConfig {
    std::string name = "L1";
    std::size_t cache_size_bytes = 32 * 1024;   // total data capacity
    std::size_t line_size_bytes = 64;           // must be a power of two, >= 8
    std::size_t associativity = 8;              // ways per set; 0 => fully associative
    EvictionPolicyType eviction_policy = EvictionPolicyType::LRU;
    WritePolicy write_policy = WritePolicy::WriteBack;
    WriteAllocatePolicy write_allocate = WriteAllocatePolicy::WriteAllocate;
    std::uint32_t hit_latency_cycles = 4;   // cycles charged on a hit at this level
    std::uint32_t miss_penalty_cycles = 12; // cycles charged locally before consulting next level
};

struct CacheLine {
    bool valid = false;
    bool dirty = false;
    std::uint64_t tag = 0;
};

// Result of a single Cache::access() call at ONE level.
struct AccessResult {
    bool hit = false;
    bool evicted = false;            // a valid line was replaced to service this access
    bool writeback_needed = false;   // the evicted line was dirty and must be written to the next level
    std::uint64_t evicted_address = 0;
    std::uint32_t latency_cycles = 0;
    std::uint64_t set_index = 0;
    std::uint64_t tag = 0;
    bool line_installed = false;     // false when a no-write-allocate write-miss bypasses the array
};

struct CacheStats {
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t dirty_evictions = 0;
    std::uint64_t writebacks = 0; // includes both eviction writebacks and write-through traffic

    [[nodiscard]] double hit_rate() const noexcept;
    [[nodiscard]] double miss_rate() const noexcept;
    void print(std::ostream& os, std::string_view label) const;
};

// ----------------------------------------------------------------------------
// Cache — a single parameterized set-associative cache level.
// ----------------------------------------------------------------------------
class Cache {
public:
    explicit Cache(CacheConfig config);

    // Services one memory access. `access_size_bytes` is informational only
    // at this level (used for statistics); the simulator treats every access
    // as touching exactly the cache line containing `address`.
    AccessResult access(std::uint64_t address, AccessType type, std::size_t access_size_bytes = 1);

    // Explicit invalidation, used for coherence / inclusive-hierarchy
    // back-invalidation. Returns true iff a valid line matching `address`
    // was found and invalidated (writing back first is the caller's concern;
    // this function reports whether the invalidated line was dirty via the
    // optional out-parameter).
    bool invalidate(std::uint64_t address, bool* was_dirty = nullptr);

    // Returns true iff `address` currently hits (without side effects such
    // as recency updates) — useful for exclusive-hierarchy bookkeeping.
    [[nodiscard]] bool probe(std::uint64_t address) const;

    // --- Bit-manipulation helpers (pure, explicit, unit-testable) ---------
    [[nodiscard]] std::uint32_t offset_bits() const noexcept { return offset_bits_; }
    [[nodiscard]] std::uint32_t index_bits() const noexcept { return index_bits_; }
    [[nodiscard]] std::uint64_t num_sets() const noexcept { return num_sets_; }
    [[nodiscard]] std::uint64_t associativity() const noexcept { return ways_; }
    [[nodiscard]] std::uint64_t extract_offset(std::uint64_t address) const noexcept;
    [[nodiscard]] std::uint64_t extract_index(std::uint64_t address) const noexcept;
    [[nodiscard]] std::uint64_t extract_tag(std::uint64_t address) const noexcept;
    [[nodiscard]] std::uint64_t reconstruct_address(std::uint64_t tag, std::uint64_t index) const noexcept;

    [[nodiscard]] const CacheConfig& config() const noexcept { return config_; }
    [[nodiscard]] const CacheStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = CacheStats{}; }

private:
    struct Set {
        std::vector<CacheLine> lines;
        std::unique_ptr<IEvictionPolicy> policy;
    };

    CacheConfig config_;
    std::vector<Set> sets_;
    std::uint32_t offset_bits_ = 0;
    std::uint32_t index_bits_ = 0;
    std::uint64_t num_sets_ = 1;
    std::uint64_t ways_ = 1;
    std::uint64_t index_mask_ = 0;
    CacheStats stats_;

    void validate_and_derive_geometry();
    [[nodiscard]] std::optional<std::uint32_t> find_way(const Set& set, std::uint64_t tag) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> find_free_way(const Set& set) const noexcept;
};

// Computes ceil(log2) style exact log base 2; throws std::invalid_argument
// if `value` is not an exact power of two. Exposed for reuse/testing.
[[nodiscard]] std::uint32_t exact_log2(std::uint64_t value);

// ----------------------------------------------------------------------------
// CacheHierarchy — composes an L1 (unified or split I/D) with an L2, and
// implements miss-forwarding, dirty writeback propagation on L1 eviction,
// and inclusion-policy back-invalidation / forced-exclusivity.
// ----------------------------------------------------------------------------
class CacheHierarchy {
public:
    struct Config {
        CacheConfig l1_data{.name = "L1D"};
        CacheConfig l2{.name = "L2", .cache_size_bytes = 256 * 1024, .associativity = 16,
                       .hit_latency_cycles = 12, .miss_penalty_cycles = 0};
        InclusionPolicy inclusion = InclusionPolicy::NonInclusiveNonExclusive;
        std::uint32_t main_memory_latency_cycles = 200;

        bool split_l1 = false;                 // true => separate I$/D$ at L1
        CacheConfig l1_instruction{.name = "L1I"};
    };

    struct HierarchyAccessResult {
        bool l1_hit = false;
        bool l2_hit = false;
        std::uint32_t total_latency_cycles = 0;
    };

    explicit CacheHierarchy(Config cfg);

    HierarchyAccessResult access(std::uint64_t address, AccessType type, std::size_t size = 1);

    [[nodiscard]] const Cache& l1_data() const noexcept { return l1_data_; }
    [[nodiscard]] const Cache* l1_instruction() const noexcept { return split_ ? &(*l1_instr_) : nullptr; }
    [[nodiscard]] const Cache& l2() const noexcept { return l2_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

    void print_summary(std::ostream& os) const;

private:
    Config cfg_;
    bool split_;
    Cache l1_data_;
    std::optional<Cache> l1_instr_;
    Cache l2_;

    [[nodiscard]] Cache& select_l1(AccessType type) noexcept;
};

} // namespace cas::cache
