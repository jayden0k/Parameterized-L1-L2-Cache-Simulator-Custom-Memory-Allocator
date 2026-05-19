// ============================================================================
// eviction_policy.hpp
//
// Strategy-pattern abstraction for cache-line replacement policies, plus a
// C++20 concept that constrains any policy usable by Cache::Cache at
// compile time (even though we dispatch through the virtual interface at
// run time, so that policies can be chosen from a config file / CLI flag).
//
// Every policy operates purely on "way" indices (0..associativity-1) within
// a single cache set. The Cache owns one policy instance per set so that
// recency/frequency/insertion-order state never leaks across sets.
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace cas::cache {

// Supported eviction policy kinds, selectable via CacheConfig.
enum class EvictionPolicyType : std::uint8_t { LRU, FIFO, LFU };

[[nodiscard]] constexpr std::string_view to_string(EvictionPolicyType type) noexcept {
    switch (type) {
        case EvictionPolicyType::LRU:  return "LRU";
        case EvictionPolicyType::FIFO: return "FIFO";
        case EvictionPolicyType::LFU:  return "LFU";
    }
    return "UNKNOWN";
}

// ----------------------------------------------------------------------------
// IEvictionPolicy — abstract strategy interface (Strategy Pattern).
//
// Lifecycle contract used by Cache::Cache for a given set with W ways:
//   1. reset(W)              — called once when the set is constructed.
//   2. on_insert(way)        — called when `way` transitions invalid -> valid
//                               (a miss was serviced and a line was filled).
//   3. on_access(way)        — called on every hit to an already-valid way.
//   4. on_remove(way)        — called when `way` transitions valid -> invalid
//                               (explicit invalidation or eviction victim
//                               chosen and about to be overwritten).
//   5. select_victim()       — called on a miss with no invalid way available;
//                               must return a way index currently considered
//                               "tracked" (i.e. inserted, not yet removed).
// ----------------------------------------------------------------------------
class IEvictionPolicy {
public:
    virtual ~IEvictionPolicy() = default;

    virtual void reset(std::uint32_t num_ways) = 0;
    virtual void on_insert(std::uint32_t way) = 0;
    virtual void on_access(std::uint32_t way) = 0;
    virtual void on_remove(std::uint32_t way) = 0;
    [[nodiscard]] virtual std::uint32_t select_victim() const = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// C++20 concept: anything that behaves like a policy (used for compile-time
// constrained factory helpers / templated test harnesses). A type need not
// literally derive from IEvictionPolicy to satisfy this concept, but every
// concrete policy shipped here does, which lets Cache::Cache store them
// polymorphically via std::unique_ptr<IEvictionPolicy>.
template <typename T>
concept EvictionPolicyConcept = requires(T policy, std::uint32_t way, std::uint32_t n) {
    { policy.reset(n) } -> std::same_as<void>;
    { policy.on_insert(way) } -> std::same_as<void>;
    { policy.on_access(way) } -> std::same_as<void>;
    { policy.on_remove(way) } -> std::same_as<void>;
    { policy.select_victim() } -> std::convertible_to<std::uint32_t>;
};

// ----------------------------------------------------------------------------
// LRUPolicy — O(1) least-recently-used via doubly linked list + hash index.
//
// `order_` holds way indices from most-recently-used (front) to
// least-recently-used (back). `index_` maps way -> iterator into `order_`
// so both access-promotion and removal are O(1).
// ----------------------------------------------------------------------------
class LRUPolicy final : public IEvictionPolicy {
public:
    void reset(std::uint32_t num_ways) override;
    void on_insert(std::uint32_t way) override;
    void on_access(std::uint32_t way) override;
    void on_remove(std::uint32_t way) override;
    [[nodiscard]] std::uint32_t select_victim() const override;
    [[nodiscard]] std::string_view name() const noexcept override { return "LRU"; }

private:
    std::list<std::uint32_t> order_;                                       // MRU..LRU
    std::unordered_map<std::uint32_t, std::list<std::uint32_t>::iterator> index_;

    void touch(std::uint32_t way); // move-to-front helper shared by insert/access
};

// ----------------------------------------------------------------------------
// FIFOPolicy — first-in-first-out. Unlike LRU, accesses do NOT reorder the
// queue; only the original insertion order determines the victim.
// ----------------------------------------------------------------------------
class FIFOPolicy final : public IEvictionPolicy {
public:
    void reset(std::uint32_t num_ways) override;
    void on_insert(std::uint32_t way) override;
    void on_access(std::uint32_t way) override; // intentionally a no-op for FIFO
    void on_remove(std::uint32_t way) override;
    [[nodiscard]] std::uint32_t select_victim() const override;
    [[nodiscard]] std::string_view name() const noexcept override { return "FIFO"; }

private:
    std::deque<std::uint32_t> queue_; // front = oldest (next victim)
};

// ----------------------------------------------------------------------------
// LFUPolicy — least-frequently-used with an explicit tie-breaker.
//
// Each tracked way carries a reference-count `freq_` (incremented on both
// insert and every subsequent access) and an `insert_seq_` monotonically
// increasing sequence number. The victim is the way with the smallest
// frequency; ties are broken by the OLDEST insertion sequence (i.e. FIFO
// among equally-frequent ways), which keeps the policy deterministic and
// prevents newly-inserted lines from being unfairly favored/penalized.
// ----------------------------------------------------------------------------
class LFUPolicy final : public IEvictionPolicy {
public:
    void reset(std::uint32_t num_ways) override;
    void on_insert(std::uint32_t way) override;
    void on_access(std::uint32_t way) override;
    void on_remove(std::uint32_t way) override;
    [[nodiscard]] std::uint32_t select_victim() const override;
    [[nodiscard]] std::string_view name() const noexcept override { return "LFU"; }

private:
    struct Meta {
        std::uint64_t freq = 0;
        std::uint64_t insert_seq = 0;
    };
    std::unordered_map<std::uint32_t, Meta> meta_;
    std::uint64_t clock_ = 0; // monotonically increasing insertion sequence generator
};

// Factory: builds a fresh, un-reset policy instance for the given type.
// The caller (Cache::Cache) must invoke reset(num_ways) before first use.
[[nodiscard]] std::unique_ptr<IEvictionPolicy> make_eviction_policy(EvictionPolicyType type);

static_assert(EvictionPolicyConcept<LRUPolicy>);
static_assert(EvictionPolicyConcept<FIFOPolicy>);
static_assert(EvictionPolicyConcept<LFUPolicy>);

} // namespace cas::cache
