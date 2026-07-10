#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

namespace lci {

/// Statistics tracked by the slab allocator.
struct AllocatorStats {
    int64_t allocations{};
    int64_t reuses{};
    int64_t pool_hits{};
    int64_t pool_misses{};
    /// Cumulative element capacity handed out by get().
    int64_t total_capacity{};
};

/// Configuration for a single slab tier.
struct SlabTierConfig {
    int capacity{};
};

/// Default tier configurations for general workloads.
inline constexpr std::array<SlabTierConfig, 7> kDefaultTierConfigs{{
    {8}, {16}, {32}, {64}, {128}, {256}, {512},
}};

/// Tier configurations optimized for trigram location arrays.
inline constexpr std::array<SlabTierConfig, 5> kTrigramTierConfigs{{
    {8}, {16}, {32}, {64}, {128},
}};

/// A single free-list node stored in-place within a freed slab block.
struct FreeNode {
    FreeNode* next{};
};

/// A thread-safe, tier-based slab allocator for fixed-size blocks.
///
/// Each tier manages blocks of a single capacity (element count * element size).
/// Freed blocks are returned to a shared free list for reuse. Allocation and
/// return operations are serialized because the arenas and lists are shared.
///
/// Template parameter T is the element type stored in allocated slabs.
template <typename T>
class SlabAllocator {
  public:
    /// Constructs a slab allocator from the given tier configurations.
    explicit SlabAllocator(std::span<const SlabTierConfig> configs)
        : tier_count_(static_cast<int>(configs.size())) {
        if (configs.empty() ||
            configs.size() > static_cast<size_t>(kMaxTiers)) {
            throw std::invalid_argument("slab allocator requires 1 to 8 tiers");
        }
        int previous_capacity = 0;
        for (int i = 0; i < tier_count_; ++i) {
            if (configs[static_cast<size_t>(i)].capacity <= previous_capacity) {
                throw std::invalid_argument(
                    "slab tier capacities must be positive and increasing");
            }
            tiers_[i].capacity = configs[static_cast<size_t>(i)].capacity;
            tiers_[i].byte_size = block_bytes(tiers_[i].capacity);
            previous_capacity = tiers_[i].capacity;
        }
    }

    /// Constructs a slab allocator with default tier configurations.
    SlabAllocator()
        : SlabAllocator(kDefaultTierConfigs) {}

    ~SlabAllocator() {
        for (auto& arena : arenas_) {
            ::operator delete(arena.data);
        }
    }

    SlabAllocator(const SlabAllocator&) = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;
    SlabAllocator(SlabAllocator&&) = delete;
    SlabAllocator& operator=(SlabAllocator&&) = delete;

    /// Returns a pointer to an array of T with at least the requested capacity.
    /// The caller must track the returned capacity tier for correct Put() calls.
    /// Returns {pointer, actual_capacity}.
    std::pair<T*, int> get(int capacity) {
        if (capacity <= 0) {
            return {nullptr, 0};
        }

        std::lock_guard lock(storage_mu_);
        for (int i = 0; i < tier_count_; ++i) {
            if (tiers_[i].capacity >= capacity) {
                return get_from_tier(i);
            }
        }

        // No tier large enough: allocate directly.
        stats_.allocations.fetch_add(1, std::memory_order_relaxed);
        stats_.pool_misses.fetch_add(1, std::memory_order_relaxed);
        stats_.total_capacity.fetch_add(
            static_cast<int64_t>(capacity), std::memory_order_relaxed);

        auto* p = static_cast<T*>(::operator new(
            static_cast<size_t>(capacity) * sizeof(T)));
        return {p, capacity};
    }

    /// Returns a block to the appropriate tier's free list for reuse.
    /// The actual_capacity must match a tier capacity exactly.
    void put(T* ptr, int actual_capacity) {
        if (ptr == nullptr || actual_capacity <= 0) {
            return;
        }

        std::lock_guard lock(storage_mu_);
        for (int i = 0; i < tier_count_; ++i) {
            if (tiers_[i].capacity == actual_capacity) {
                put_to_tier(i, ptr);
                return;
            }
        }

        // No matching tier: was a direct allocation, free it.
        ::operator delete(ptr);
    }

    /// Returns a snapshot of current allocation statistics.
    AllocatorStats get_stats() const {
        return {
            stats_.allocations.load(std::memory_order_relaxed),
            stats_.reuses.load(std::memory_order_relaxed),
            stats_.pool_hits.load(std::memory_order_relaxed),
            stats_.pool_misses.load(std::memory_order_relaxed),
            stats_.total_capacity.load(std::memory_order_relaxed),
        };
    }

    /// Resets all statistics to zero.
    void reset_stats() {
        stats_.allocations.store(0, std::memory_order_relaxed);
        stats_.reuses.store(0, std::memory_order_relaxed);
        stats_.pool_hits.store(0, std::memory_order_relaxed);
        stats_.pool_misses.store(0, std::memory_order_relaxed);
        stats_.total_capacity.store(0, std::memory_order_relaxed);
    }

  private:
    static constexpr int kMaxTiers = 8;
    static constexpr size_t kArenaSize = 64 * 1024;  // 64 KiB per arena

    struct Tier {
        int capacity{};
        size_t byte_size{};
        FreeNode* free_list{};
    };

    struct AtomicStats {
        std::atomic<int64_t> allocations{};
        std::atomic<int64_t> reuses{};
        std::atomic<int64_t> pool_hits{};
        std::atomic<int64_t> pool_misses{};
        std::atomic<int64_t> total_capacity{};
    };

    struct Arena {
        void* data{};
        size_t used{};
        size_t size{};
    };

    static constexpr size_t block_bytes(int capacity) {
        size_t raw = static_cast<size_t>(capacity) * sizeof(T);
        // Ensure blocks are large enough to hold a FreeNode pointer.
        return raw < sizeof(FreeNode) ? sizeof(FreeNode) : raw;
    }

    std::pair<T*, int> get_from_tier(int tier_idx) {
        auto& tier = tiers_[tier_idx];

        // Fast path: pop from free list.
        if (tier.free_list != nullptr) {
            auto* node = tier.free_list;
            tier.free_list = node->next;
            auto* ptr = reinterpret_cast<T*>(node);

            stats_.reuses.fetch_add(1, std::memory_order_relaxed);
            stats_.pool_hits.fetch_add(1, std::memory_order_relaxed);
            stats_.total_capacity.fetch_add(
                static_cast<int64_t>(tier.capacity), std::memory_order_relaxed);
            return {ptr, tier.capacity};
        }

        // Slow path: allocate from arena.
        auto* ptr = allocate_from_arena(tier.byte_size);

        stats_.allocations.fetch_add(1, std::memory_order_relaxed);
        stats_.pool_misses.fetch_add(1, std::memory_order_relaxed);
        stats_.total_capacity.fetch_add(
            static_cast<int64_t>(tier.capacity), std::memory_order_relaxed);
        return {reinterpret_cast<T*>(ptr), tier.capacity};
    }

    void put_to_tier(int tier_idx, T* ptr) {
        auto& tier = tiers_[tier_idx];
        auto* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = tier.free_list;
        tier.free_list = node;
    }

    void* allocate_from_arena(size_t bytes) {
        // Try current arena.
        if (!arenas_.empty()) {
            auto& current = arenas_.back();
            // Align to pointer size.
            size_t aligned = (current.used + alignof(std::max_align_t) - 1)
                             & ~(alignof(std::max_align_t) - 1);
            if (aligned + bytes <= current.size) {
                void* ptr = static_cast<char*>(current.data) + aligned;
                current.used = aligned + bytes;
                return ptr;
            }
        }

        // Allocate new arena.
        size_t arena_size = bytes > kArenaSize ? bytes : kArenaSize;
        void* data = ::operator new(arena_size);
        arenas_.push_back({data, bytes, arena_size});
        return data;
    }

    std::array<Tier, kMaxTiers> tiers_{};
    int tier_count_{};
    std::mutex storage_mu_;
    AtomicStats stats_{};
    std::vector<Arena> arenas_;
};

}  // namespace lci
