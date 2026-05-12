#include "cache/cache.hpp"

#include <bit>
#include <format>
#include <stdexcept>

namespace cas::cache {

// ============================================================================
// Free functions / enum-to-string
// ============================================================================
std::string_view to_string(AccessType t) noexcept {
    switch (t) {
        case AccessType::Read:             return "Read";
        case AccessType::Write:            return "Write";
        case AccessType::InstructionFetch: return "IFetch";
    }
    return "Unknown";
}

std::string_view to_string(WritePolicy p) noexcept {
    return p == WritePolicy::WriteBack ? "WriteBack" : "WriteThrough";
}

std::string_view to_string(WriteAllocatePolicy p) noexcept {
    return p == WriteAllocatePolicy::WriteAllocate ? "WriteAllocate" : "NoWriteAllocate";
}

std::string_view to_string(InclusionPolicy p) noexcept {
    switch (p) {
        case InclusionPolicy::Inclusive:               return "Inclusive";
        case InclusionPolicy::Exclusive:                return "Exclusive";
        case InclusionPolicy::NonInclusiveNonExclusive: return "NINE";
    }
    return "Unknown";
}

// Exact base-2 logarithm; throws if `value` is not a power of two. Uses
// std::popcount / std::countr_zero (C++20 <bit>) for a branch-free
// implementation rather than a floating-point log2() call, which would be
// both slower and susceptible to rounding error near powers of two.
std::uint32_t exact_log2(std::uint64_t value) {
    if (value == 0 || (value & (value - 1)) != 0) {
        throw std::invalid_argument(std::format("exact_log2: {} is not a power of two", value));
    }
    return static_cast<std::uint32_t>(std::countr_zero(value));
}

// ============================================================================
// CacheStats
// ============================================================================
double CacheStats::hit_rate() const noexcept {
    const std::uint64_t total = hits + misses;
    return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
}

double CacheStats::miss_rate() const noexcept { return 1.0 - hit_rate(); }

void CacheStats::print(std::ostream& os, std::string_view label) const {
    os << "--- " << label << " stats ---\n"
       << "  reads=" << reads << " writes=" << writes << "\n"
       << "  hits=" << hits << " misses=" << misses
       << std::format(" (hit_rate={:.2f}%)\n", hit_rate() * 100.0)
       << "  evictions=" << evictions << " dirty_evictions=" << dirty_evictions
       << " writebacks=" << writebacks << "\n";
}

// ============================================================================
// Cache
// ============================================================================
Cache::Cache(CacheConfig config) : config_(std::move(config)) {
    validate_and_derive_geometry();

    sets_.resize(num_sets_);
    for (auto& set : sets_) {
        set.lines.resize(ways_);
        set.policy = make_eviction_policy(config_.eviction_policy);
        set.policy->reset(static_cast<std::uint32_t>(ways_));
    }
}

void Cache::validate_and_derive_geometry() {
    if (config_.line_size_bytes < 8 || (config_.line_size_bytes & (config_.line_size_bytes - 1)) != 0) {
        throw std::invalid_argument("Cache: line_size_bytes must be a power of two >= 8");
    }
    if (config_.cache_size_bytes == 0 || config_.cache_size_bytes % config_.line_size_bytes != 0) {
        throw std::invalid_argument("Cache: cache_size_bytes must be a non-zero multiple of line_size_bytes");
    }

    const std::uint64_t total_lines = config_.cache_size_bytes / config_.line_size_bytes;

    // associativity == 0 is our sentinel for "fully associative": every
    // line lives in the single set, i.e. ways == total_lines, num_sets == 1.
    ways_ = (config_.associativity == 0) ? total_lines : config_.associativity;

    if (ways_ == 0 || total_lines % ways_ != 0) {
        throw std::invalid_argument("Cache: associativity must evenly divide (cache_size / line_size)");
    }

    num_sets_ = total_lines / ways_;
    offset_bits_ = exact_log2(config_.line_size_bytes);
    index_bits_ = (num_sets_ == 1) ? 0 : exact_log2(num_sets_);
    index_mask_ = (num_sets_ == 1) ? 0 : (num_sets_ - 1);
}

std::uint64_t Cache::extract_offset(std::uint64_t address) const noexcept {
    const std::uint64_t offset_mask = (std::uint64_t{1} << offset_bits_) - 1;
    return address & offset_mask;
}

std::uint64_t Cache::extract_index(std::uint64_t address) const noexcept {
    return (address >> offset_bits_) & index_mask_;
}

std::uint64_t Cache::extract_tag(std::uint64_t address) const noexcept {
    return address >> (offset_bits_ + index_bits_);
}

std::uint64_t Cache::reconstruct_address(std::uint64_t tag, std::uint64_t index) const noexcept {
    return (tag << (offset_bits_ + index_bits_)) | (index << offset_bits_);
}

std::optional<std::uint32_t> Cache::find_way(const Set& set, std::uint64_t tag) const noexcept {
    for (std::uint32_t way = 0; way < set.lines.size(); ++way) {
        if (set.lines[way].valid && set.lines[way].tag == tag) {
            return way;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Cache::find_free_way(const Set& set) const noexcept {
    for (std::uint32_t way = 0; way < set.lines.size(); ++way) {
        if (!set.lines[way].valid) {
            return way;
        }
    }
    return std::nullopt;
}

AccessResult Cache::access(std::uint64_t address, AccessType type, std::size_t access_size_bytes) {
    (void)access_size_bytes; // tracked only for potential future multi-line-span statistics

    AccessResult result;
    result.set_index = extract_index(address);
    result.tag = extract_tag(address);

    if (type == AccessType::Write) {
        ++stats_.writes;
    } else {
        ++stats_.reads; // Reads and instruction fetches both count as "reads" for occupancy purposes
    }

    Set& set = sets_[result.set_index];

    // --- Hit path -----------------------------------------------------------
    if (auto way = find_way(set, result.tag); way.has_value()) {
        result.hit = true;
        ++stats_.hits;
        set.policy->on_access(*way);

        if (type == AccessType::Write) {
            if (config_.write_policy == WritePolicy::WriteBack) {
                set.lines[*way].dirty = true;
            } else {
                ++stats_.writebacks; // write-through traffic to the next level
            }
        }
        result.latency_cycles = config_.hit_latency_cycles;
        return result;
    }

    // --- Miss path ------------------------------------------------------------
    ++stats_.misses;
    result.hit = false;

    const bool is_write_miss = (type == AccessType::Write);
    const bool should_install = !is_write_miss || config_.write_allocate == WriteAllocatePolicy::WriteAllocate;

    if (!should_install) {
        // No-write-allocate write miss: the write goes straight to the next
        // level ("write-around"); the array is left untouched.
        ++stats_.writebacks;
        result.latency_cycles = config_.miss_penalty_cycles;
        result.line_installed = false;
        return result;
    }

    // Choose a destination way: prefer an invalid (never-used or previously
    // invalidated) line; otherwise consult the eviction policy.
    std::uint32_t victim_way;
    if (auto free_way = find_free_way(set); free_way.has_value()) {
        victim_way = *free_way;
    } else {
        victim_way = set.policy->select_victim();
        CacheLine& victim_line = set.lines[victim_way];

        result.evicted = true;
        ++stats_.evictions;
        result.evicted_address = reconstruct_address(victim_line.tag, result.set_index);

        if (victim_line.dirty) {
            result.writeback_needed = true;
            ++stats_.dirty_evictions;
            ++stats_.writebacks;
        }
        set.policy->on_remove(victim_way);
    }

    CacheLine& line = set.lines[victim_way];
    line.valid = true;
    line.tag = result.tag;
    // Under write-back, a write-miss fill is immediately dirty (the data
    // differs from the next level until it is later evicted/flushed).
    // Under write-through, lines are never left dirty in this cache.
    line.dirty = (type == AccessType::Write) && (config_.write_policy == WritePolicy::WriteBack);
    if (type == AccessType::Write && config_.write_policy == WritePolicy::WriteThrough) {
        ++stats_.writebacks;
    }

    set.policy->on_insert(victim_way);
    result.line_installed = true;
    result.latency_cycles = config_.miss_penalty_cycles;
    return result;
}

bool Cache::invalidate(std::uint64_t address, bool* was_dirty) {
    const std::uint64_t index = extract_index(address);
    const std::uint64_t tag = extract_tag(address);
    Set& set = sets_[index];

    auto way = find_way(set, tag);
    if (!way.has_value()) {
        if (was_dirty) *was_dirty = false;
        return false;
    }

    CacheLine& line = set.lines[*way];
    if (was_dirty) *was_dirty = line.dirty;

    line.valid = false;
    line.dirty = false;
    set.policy->on_remove(*way);
    return true;
}

bool Cache::probe(std::uint64_t address) const {
    const std::uint64_t index = extract_index(address);
    const std::uint64_t tag = extract_tag(address);
    return find_way(sets_[index], tag).has_value();
}

// ============================================================================
// CacheHierarchy
// ============================================================================
CacheHierarchy::CacheHierarchy(Config cfg)
    : cfg_(cfg),
      split_(cfg.split_l1),
      l1_data_(cfg.l1_data),
      l1_instr_(cfg.split_l1 ? std::make_optional<Cache>(cfg.l1_instruction) : std::nullopt),
      l2_(cfg.l2) {}

Cache& CacheHierarchy::select_l1(AccessType type) noexcept {
    if (split_ && type == AccessType::InstructionFetch) {
        return *l1_instr_;
    }
    return l1_data_;
}

CacheHierarchy::HierarchyAccessResult CacheHierarchy::access(std::uint64_t address, AccessType type,
                                                               std::size_t size) {
    HierarchyAccessResult hr;
    Cache& l1 = select_l1(type);

    const AccessResult l1_result = l1.access(address, type, size);
    hr.l1_hit = l1_result.hit;

    if (l1_result.hit) {
        hr.total_latency_cycles = l1_result.latency_cycles;
        return hr;
    }

    // L1 miss: charge the local L1 miss-detection penalty, then propagate
    // any dirty writeback from the evicted L1 line down to L2 first so L2
    // sees an up-to-date value before (potentially) being probed for it.
    std::uint32_t latency = l1_result.latency_cycles;

    if (l1_result.evicted && l1_result.writeback_needed) {
        l2_.access(l1_result.evicted_address, AccessType::Write, 1);
    }

    // L2 is logically unified; instruction fetches are treated as reads.
    const AccessType l2_access_type = (type == AccessType::InstructionFetch) ? AccessType::Read : type;
    const AccessResult l2_result = l2_.access(address, l2_access_type, size);
    hr.l2_hit = l2_result.hit;

    latency += l2_result.hit ? l2_result.latency_cycles
                              : (l2_result.latency_cycles + cfg_.main_memory_latency_cycles);
    hr.total_latency_cycles = latency;

    // --- Inclusion-policy enforcement ---------------------------------------
    if (cfg_.inclusion == InclusionPolicy::Inclusive && l2_result.evicted) {
        // L2 evicted a line to make room; an inclusive hierarchy requires
        // that line no longer be cached at L1 either (back-invalidation).
        // If it was dirty in L1, that data is already lost unless L1 wrote
        // it back on its own eviction earlier — a full implementation would
        // snoop L1 before evicting from L2; we document this as a known
        // simplification of the reference model.
        l1_data_.invalidate(l2_result.evicted_address);
        if (split_) {
            l1_instr_->invalidate(l2_result.evicted_address);
        }
    } else if (cfg_.inclusion == InclusionPolicy::Exclusive && l2_result.hit) {
        // Exclusive hierarchies never let the same line reside at both
        // levels simultaneously: once a line is pulled up into L1 on an L2
        // hit, remove it from L2.
        l2_.invalidate(address);
    }

    return hr;
}

void CacheHierarchy::print_summary(std::ostream& os) const {
    os << "==== Cache Hierarchy Summary ====\n"
       << "Inclusion policy: " << to_string(cfg_.inclusion) << "\n";
    l1_data_.stats().print(os, cfg_.l1_data.name);
    if (split_) {
        l1_instr_->stats().print(os, cfg_.l1_instruction.name);
    }
    l2_.stats().print(os, cfg_.l2.name);
}

} // namespace cas::cache
