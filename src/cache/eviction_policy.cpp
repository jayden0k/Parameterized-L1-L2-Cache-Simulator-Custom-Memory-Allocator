#include "cache/eviction_policy.hpp"

#include <algorithm>
#include <stdexcept>

namespace cas::cache {

// ============================================================================
// LRUPolicy
// ============================================================================
void LRUPolicy::reset(std::uint32_t num_ways) {
    order_.clear();
    index_.clear();
    index_.reserve(num_ways);
}

void LRUPolicy::touch(std::uint32_t way) {
    if (auto it = index_.find(way); it != index_.end()) {
        order_.erase(it->second);
    }
    order_.push_front(way);
    index_[way] = order_.begin();
}

void LRUPolicy::on_insert(std::uint32_t way) { touch(way); }
void LRUPolicy::on_access(std::uint32_t way) { touch(way); }

void LRUPolicy::on_remove(std::uint32_t way) {
    if (auto it = index_.find(way); it != index_.end()) {
        order_.erase(it->second);
        index_.erase(it);
    }
}

std::uint32_t LRUPolicy::select_victim() const {
    if (order_.empty()) {
        throw std::logic_error("LRUPolicy::select_victim called with no tracked ways");
    }
    return order_.back(); // least-recently-used sits at the tail
}

// ============================================================================
// FIFOPolicy
// ============================================================================
void FIFOPolicy::reset(std::uint32_t /*num_ways*/) { queue_.clear(); }

void FIFOPolicy::on_insert(std::uint32_t way) { queue_.push_back(way); }

void FIFOPolicy::on_access(std::uint32_t /*way*/) {
    // FIFO explicitly ignores accesses: only insertion order matters.
}

void FIFOPolicy::on_remove(std::uint32_t way) {
    auto it = std::find(queue_.begin(), queue_.end(), way);
    if (it != queue_.end()) {
        queue_.erase(it);
    }
}

std::uint32_t FIFOPolicy::select_victim() const {
    if (queue_.empty()) {
        throw std::logic_error("FIFOPolicy::select_victim called with no tracked ways");
    }
    return queue_.front(); // oldest insertion
}

// ============================================================================
// LFUPolicy
// ============================================================================
void LFUPolicy::reset(std::uint32_t num_ways) {
    meta_.clear();
    meta_.reserve(num_ways);
    clock_ = 0;
}

void LFUPolicy::on_insert(std::uint32_t way) {
    meta_[way] = Meta{.freq = 1, .insert_seq = clock_++};
}

void LFUPolicy::on_access(std::uint32_t way) {
    if (auto it = meta_.find(way); it != meta_.end()) {
        ++it->second.freq;
    }
    // If `way` was not tracked (defensive), treat like an insert so the
    // policy self-heals rather than silently dropping the way forever.
    else {
        meta_[way] = Meta{.freq = 1, .insert_seq = clock_++};
    }
}

void LFUPolicy::on_remove(std::uint32_t way) { meta_.erase(way); }

std::uint32_t LFUPolicy::select_victim() const {
    if (meta_.empty()) {
        throw std::logic_error("LFUPolicy::select_victim called with no tracked ways");
    }
    // Least frequency wins; ties broken by oldest insertion sequence so the
    // choice is fully deterministic (important for reproducible traces).
    auto best = meta_.begin();
    for (auto it = std::next(meta_.begin()); it != meta_.end(); ++it) {
        const bool lower_freq = it->second.freq < best->second.freq;
        const bool tie_older = it->second.freq == best->second.freq &&
                                it->second.insert_seq < best->second.insert_seq;
        if (lower_freq || tie_older) {
            best = it;
        }
    }
    return best->first;
}

// ============================================================================
// Factory
// ============================================================================
std::unique_ptr<IEvictionPolicy> make_eviction_policy(EvictionPolicyType type) {
    switch (type) {
        case EvictionPolicyType::LRU:  return std::make_unique<LRUPolicy>();
        case EvictionPolicyType::FIFO: return std::make_unique<FIFOPolicy>();
        case EvictionPolicyType::LFU:  return std::make_unique<LFUPolicy>();
    }
    throw std::invalid_argument("make_eviction_policy: unknown EvictionPolicyType");
}

} // namespace cas::cache
