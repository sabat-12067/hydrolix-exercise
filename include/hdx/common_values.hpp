#pragma once
// ============================================================================
// Question 1: Find Common Values
//
// Semantics (confirmed with the team on Slack):
//   * "Common values" = the DISTINCT values that appear in both inputs.
//     Duplicates in either input are collapsed.
//   * Output order is not required. We still return values in order of first
//     occurrence in the SMALL vector, because it costs nothing (<= 100
//     elements) and makes both scenarios return identical, deterministic
//     results that are easy to test against each other.
//   * The small vector has at most 100 elements (hard guarantee).
//
// Both implementations are templated on std::integral, so the same code works
// for Int32 / Int64 / UInt64 columns. The prompt's case is `int`.
// ============================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace hdx {

namespace detail {

// Fibonacci (multiplicative) hashing: one multiply + one shift. It spreads
// sequential / clustered integers well, which matters because real columns
// (IDs, timestamps) are rarely uniform. std::hash<int> in libstdc++ is the
// identity function, which clusters badly under linear probing.
template <std::integral T>
[[nodiscard]] constexpr std::size_t fib_hash(T value, unsigned shift) noexcept {
    const auto bits = static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
    return static_cast<std::size_t>((bits * 0x9E3779B97F4A7C15ULL) >> shift);
}

// Offset of v from lo, computed in unsigned arithmetic so extreme ranges like
// [INT64_MIN, INT64_MAX] never hit signed overflow. Values below lo wrap to a
// huge number, so a single `offset > width` check covers both sides.
template <std::integral T>
[[nodiscard]] constexpr std::uint64_t unsigned_offset(T v, T lo) noexcept {
    using U = std::make_unsigned_t<T>;
    return static_cast<std::uint64_t>(static_cast<U>(static_cast<U>(v) - static_cast<U>(lo)));
}

inline void prefetch(const void* p) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, /*rw=*/0, /*locality=*/1);
#else
    (void)p;
#endif
}

}  // namespace detail

// ----------------------------------------------------------------------------
// Scenario 1: the large vector is long-lived and rarely changes.
//
// Pay a one-time O(N) build so every later lookup is ~O(1) with roughly one
// cache miss per probe value.
//
// Structure: open-addressing flat hash set with linear probing.
//   * Keys stored inline in one contiguous array: no per-node allocation, no
//     pointer chasing, and a probe sequence usually stays in one cache line.
//   * Power-of-two capacity, load factor <= 0.5: mask instead of modulo, and
//     short expected probe lengths (~1.5 for hits, ~2.5 for misses).
//   * Empty slots use a sentinel (numeric_limits<T>::min()). That value is
//     also legal data, so its presence is tracked in a separate bool rather
//     than silently reserving it.
//
// Adaptive dense mode: if the value range (max - min) is small relative to N,
// a bitmap wins outright: exact, 1 bit per possible value, one load + bit test
// per lookup, no hashing. It is chosen automatically whenever the bitmap is no
// larger than the hash table would be.
//
// Alternatives considered (details in DESIGN.md):
//   * std::unordered_set: node-based, an allocation and ~32+ bytes per element;
//     for 1M ints that is tens of MB and a pointer chase per lookup.
//   * Sorted vector + binary search: compact and simple, but ~20 dependent,
//     mostly cache-missing loads per lookup at N = 1M. Eytzinger layout helps
//     locality but not the log N depth.
//   * Bloom filter in front: only pays off when most probes miss AND the exact
//     set doesn't fit in cache. Here the exact set is only ~8 MB.
//
// Thread safety: immutable after construction, so concurrent common_values()
// calls are safe without locks. "Rarely changes" is handled by rebuilding a new
// index and swapping a shared_ptr (RCU style); see DESIGN.md.
// ----------------------------------------------------------------------------
template <std::integral T = int>
class CommonValuesIndex {
public:
    explicit CommonValuesIndex(std::span<const T> large) { build(large); }

    explicit CommonValuesIndex(const std::vector<T>& large)
        : CommonValuesIndex(std::span<const T>{large}) {}

    [[nodiscard]] bool contains(T value) const noexcept {
        return dense_ ? dense_contains(value) : hash_contains(value);
    }

    // Distinct values of `small` that exist in the index, in order of first
    // appearance in `small`.
    [[nodiscard]] std::vector<T> common_values(std::span<const T> small) const {
        std::vector<T> out;
        out.reserve(small.size());

        // Pass 1: prefetch every probe slot up front. With <= 100 probes into
        // an ~8 MB table, this overlaps the cache misses (memory-level
        // parallelism) instead of paying them back to back.
        if (!dense_ && !slots_.empty()) {
            for (const T v : small) {
                detail::prefetch(&slots_[detail::fib_hash(v, shift_)]);
            }
        }

        // Pass 2: probe. Dedup against `out` with a linear scan: for <= 100
        // elements that beats any set (no allocation, stays in L1).
        for (const T v : small) {
            if (contains(v) && std::ranges::find(out, v) == out.end()) {
                out.push_back(v);
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<T> common_values(const std::vector<T>& small) const {
        return common_values(std::span<const T>{small});
    }

    [[nodiscard]] bool uses_dense_bitmap() const noexcept { return dense_; }
    [[nodiscard]] std::size_t distinct_count() const noexcept { return distinct_; }

private:
    static constexpr T kEmpty = std::numeric_limits<T>::min();

    // ---- build -------------------------------------------------------------
    void build(std::span<const T> large) {
        if (large.empty()) return;

        const auto [lo, hi] = std::ranges::minmax(large);
        const std::uint64_t width = detail::unsigned_offset(hi, lo);

        const std::size_t hash_capacity =
            std::bit_ceil(std::max<std::size_t>(large.size() * 2, 16));
        const std::uint64_t hash_bits =
            static_cast<std::uint64_t>(hash_capacity) * sizeof(T) * 8;

        // Dense when the bitmap (width + 1 bits) is no bigger than the table.
        // `width < hash_bits` also avoids overflow of width + 1.
        if (width < hash_bits) {
            build_dense(large, lo, width + 1);
        } else {
            build_hash(large, hash_capacity);
        }
    }

    void build_dense(std::span<const T> large, T lo, std::uint64_t range_size) {
        dense_ = true;
        base_ = lo;
        range_size_ = range_size;
        bits_.assign(static_cast<std::size_t>((range_size + 63) / 64), 0);
        for (const T v : large) {
            const std::uint64_t off = detail::unsigned_offset(v, lo);
            auto& word = bits_[off >> 6];
            const std::uint64_t bit = std::uint64_t{1} << (off & 63);
            distinct_ += (word & bit) == 0;
            word |= bit;
        }
    }

    void build_hash(std::span<const T> large, std::size_t capacity) {
        slots_.assign(capacity, kEmpty);
        mask_ = capacity - 1;
        shift_ = 64u - static_cast<unsigned>(std::countr_zero(capacity));
        for (const T v : large) insert(v);
    }

    void insert(T v) noexcept {
        if (v == kEmpty) [[unlikely]] {
            distinct_ += !has_sentinel_value_;
            has_sentinel_value_ = true;
            return;
        }
        for (std::size_t i = detail::fib_hash(v, shift_);; i = (i + 1) & mask_) {
            if (slots_[i] == v) return;
            if (slots_[i] == kEmpty) {
                slots_[i] = v;
                ++distinct_;
                return;
            }
        }
    }

    // ---- lookup ------------------------------------------------------------
    [[nodiscard]] bool hash_contains(T v) const noexcept {
        if (slots_.empty()) return false;
        if (v == kEmpty) [[unlikely]] return has_sentinel_value_;
        for (std::size_t i = detail::fib_hash(v, shift_);; i = (i + 1) & mask_) {
            const T s = slots_[i];
            if (s == v) return true;
            if (s == kEmpty) return false;
        }
    }

    [[nodiscard]] bool dense_contains(T v) const noexcept {
        const std::uint64_t off = detail::unsigned_offset(v, base_);
        if (off >= range_size_) return false;
        return (bits_[off >> 6] >> (off & 63)) & 1u;
    }

    // Hash mode
    std::vector<T> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    bool has_sentinel_value_ = false;

    // Dense mode
    bool dense_ = false;
    T base_{};
    std::uint64_t range_size_ = 0;
    std::vector<std::uint64_t> bits_;

    std::size_t distinct_ = 0;
};

template <std::integral T>
CommonValuesIndex(const std::vector<T>&) -> CommonValuesIndex<T>;
template <std::integral T>
CommonValuesIndex(std::span<const T>) -> CommonValuesIndex<T>;

// ----------------------------------------------------------------------------
// Scenario 2: both vectors are short-lived (used once, then destroyed).
//
// Building ANY structure over the 1M side is wasted work: we'd touch every
// element to build it and then use it once. Instead, index the SMALL side
// (<= 100 values) and stream over the large side exactly once: a sequential
// pass that the hardware prefetcher handles perfectly.
//
// Small-side structure: fixed 256-slot open-addressing table on the stack.
//   * A few KB, lives in L1, zero heap allocations.
//   * Load factor <= 100/256 ~ 0.4, so ~1 probe on average.
//
// Three cheap wins in the hot loop:
//   1. Range pre-filter on [min, max] of the small side: one unsigned compare
//      rejects out-of-range elements before hashing. Big win when the small
//      set is clustered (common for IDs / time buckets), ~free otherwise.
//   2. 4096-bit (512 B) membership filter keyed on different hash bits than
//      the table. With <= 100 values, ~97.5% of non-matching elements are
//      rejected by one bit test on a branch that is almost always "not
//      taken", so it predicts well. Without it, the table's "slot empty?"
//      branch is a ~40/60 coin flip per element and mispredicts constantly;
//      benchmarking showed this was the main cost of the loop.
//   3. Early exit: once every distinct small value is found, stop scanning.
//
// Alternatives considered:
//   * Hash set of the large vector: O(N) allocations/inserts for one use.
//   * Sort both + std::set_intersection: O(N log N), and copies or mutates
//     the caller's large vector.
//   * Sorted small side + binary search per element: ~7 unpredictable branches
//     per element vs ~1 probe here.
//   * SIMD brute force (compare each element against all small values): ~13
//     AVX2 compares per element at 100 values; branch-free and simple, and it
//     wins when the small side is tiny (<= ~16). A natural future special case.
//   * Parallel scan: 1M ints is ~4 MB and memory-bound, done in ~1 ms; thread
//     startup would dominate. Worth it for much larger N.
// ----------------------------------------------------------------------------
template <std::integral T>
[[nodiscard]] std::vector<T> find_common_values_once(std::span<const T> large,
                                                     std::span<const T> small) {
    constexpr std::size_t kMaxSmall = 100;  // from the problem statement
    constexpr std::size_t kCapacity = 256;  // power of two >= 2 * kMaxSmall
    constexpr unsigned kShift = 64u - static_cast<unsigned>(std::countr_zero(kCapacity));
    constexpr std::size_t kMask = kCapacity - 1;
    static_assert(std::has_single_bit(kCapacity) && kCapacity >= 2 * kMaxSmall);

    if (large.empty() || small.empty()) return {};

    // The <= 100 bound is a hard guarantee (confirmed with the team), so this
    // branch never runs for valid input. It is kept as a one-compare safety
    // net: if the contract is ever broken, we fall back to the dynamically
    // sized path instead of overfilling the fixed table.
    if (small.size() > kMaxSmall) [[unlikely]] {
        return CommonValuesIndex<T>{large}.common_values(small);
    }

    struct Slot {
        T value{};
        bool used = false;
        bool found = false;
    };
    std::array<Slot, kCapacity> table{};

    auto find_slot = [&table](T v) -> Slot* {
        for (std::size_t i = detail::fib_hash(v, kShift);; i = (i + 1) & kMask) {
            if (!table[i].used) return nullptr;
            if (table[i].value == v) return &table[i];
        }
    };

    std::size_t distinct = 0;
    for (const T v : small) {
        for (std::size_t i = detail::fib_hash(v, kShift);; i = (i + 1) & kMask) {
            if (!table[i].used) {
                table[i] = Slot{v, true, false};
                ++distinct;
                break;
            }
            if (table[i].value == v) break;
        }
    }

    // Membership pre-filter: top 12 bits of the same multiplicative hash.
    constexpr unsigned kFilterShift = 64u - 12u;
    std::array<std::uint64_t, 4096 / 64> filter{};
    for (const T v : small) {
        const std::size_t h = detail::fib_hash(v, kFilterShift);
        filter[h >> 6] |= std::uint64_t{1} << (h & 63);
    }
    const auto maybe_present = [&filter](T x) noexcept {
        const std::size_t h = detail::fib_hash(x, kFilterShift);
        return ((filter[h >> 6] >> (h & 63)) & 1u) != 0;
    };

    const auto [lo, hi] = std::ranges::minmax(small);
    const std::uint64_t width = detail::unsigned_offset(hi, lo);

    // Single pass over the large side, with early exit once all are found.
    std::size_t found = 0;
    for (const T x : large) {
        if (detail::unsigned_offset(x, lo) > width) continue;
        if (!maybe_present(x)) [[likely]] continue;
        if (Slot* s = find_slot(x); s && !s->found) {
            s->found = true;
            if (++found == distinct) break;
        }
    }

    // Emit in small-vector order (same contract as CommonValuesIndex).
    std::vector<T> out;
    out.reserve(found);
    for (const T v : small) {
        if (Slot* s = find_slot(v); s->found) {
            out.push_back(v);
            s->found = false;  // emit each distinct value once
        }
    }
    return out;
}

template <std::integral T>
[[nodiscard]] std::vector<T> find_common_values_once(const std::vector<T>& large,
                                                     const std::vector<T>& small) {
    return find_common_values_once(std::span<const T>{large}, std::span<const T>{small});
}

}  // namespace hdx
