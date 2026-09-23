// Self-contained test runner (no external framework, so it builds anywhere).
#include <hdx/common_values.hpp>
#include <hdx/type_conversion.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, std::string_view what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << "  FAIL: " << what << '\n';
    }
}

// Naive reference: obviously-correct, used to cross-check the fast paths.
template <typename T>
std::vector<T> reference_common(const std::vector<T>& large, const std::vector<T>& small) {
    const std::unordered_set<T> big(large.begin(), large.end());
    std::unordered_set<T> seen;
    std::vector<T> out;
    for (T v : small) {
        if (big.contains(v) && seen.insert(v).second) out.push_back(v);
    }
    return out;
}

template <typename T>
void check_both(const std::vector<T>& large, const std::vector<T>& small, std::string_view what) {
    const auto expected = reference_common(large, small);
    const hdx::CommonValuesIndex idx{large};
    check(idx.common_values(small) == expected, what);
    check(hdx::find_common_values_once(large, small) == expected, what);
}

// ---------------------------------------------------------------------------
void test_common_values_basic() {
    std::cout << "[Q1] basic cases\n";
    check_both<int>({}, {}, "both empty");
    check_both<int>({1, 2, 3}, {}, "small empty");
    check_both<int>({}, {1, 2, 3}, "large empty");
    check_both<int>({5, 1, 9, 3}, {3, 7, 5}, "simple overlap, small-order output");
    check_both<int>({1, 1, 1, 2}, {1, 1, 2, 2}, "duplicates collapse");
    check_both<int>({10, 20, 30}, {40, 50}, "no overlap");
    check_both<int>({-5, -1, 0, 7}, {0, -5, 8}, "negatives");

    // Sentinel value must behave like any other value.
    constexpr int kMin = std::numeric_limits<int>::min();
    constexpr int kMax = std::numeric_limits<int>::max();
    check_both<int>({kMin, kMax, 0}, {kMin, 1, kMax}, "INT_MIN / INT_MAX (hash mode)");
    check_both<int>({kMax, 0}, {kMin}, "INT_MIN absent");

    constexpr auto kMin64 = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax64 = std::numeric_limits<std::int64_t>::max();
    check_both<std::int64_t>({kMin64, kMax64}, {kMax64, kMin64, 0}, "int64 full range");
    check_both<std::uint64_t>({0, ~0ULL}, {~0ULL, 1}, "uint64 extremes");
}

void test_common_values_modes() {
    std::cout << "[Q1] dense vs hash mode selection\n";
    std::vector<int> dense(1'000'000);
    for (int i = 0; i < 1'000'000; ++i) dense[i] = i * 3;  // range 3M, fits bitmap
    const hdx::CommonValuesIndex dense_idx{dense};
    check(dense_idx.uses_dense_bitmap(), "clustered data picks bitmap");
    check(dense_idx.distinct_count() == 1'000'000, "bitmap distinct count");
    check(dense_idx.common_values(std::vector<int>{0, 1, 3, 2'999'997, 3'000'000, -3}) ==
              std::vector<int>{0, 3, 2'999'997},
          "bitmap lookups incl. out-of-range");

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> any(std::numeric_limits<int>::min(),
                                           std::numeric_limits<int>::max());
    std::vector<int> sparse(1'000'000);
    for (auto& v : sparse) v = any(rng);
    const hdx::CommonValuesIndex sparse_idx{sparse};
    check(!sparse_idx.uses_dense_bitmap(), "random 32-bit data picks hash table");
}

void test_common_values_randomized() {
    std::cout << "[Q1] randomized cross-check vs reference (both scenarios)\n";
    std::mt19937_64 rng(12345);
    for (int round = 0; round < 200; ++round) {
        // Alternate between narrow ranges (dense mode, many hits) and wide
        // ranges (hash mode, mostly misses).
        const std::int64_t span = (round % 2 == 0) ? 5'000 : 4'000'000'000LL;
        std::uniform_int_distribution<std::int64_t> dist(-span / 2, span / 2);
        std::uniform_int_distribution<std::size_t> nlarge(0, 20'000);
        std::uniform_int_distribution<std::size_t> nsmall(0, 100);

        std::vector<std::int64_t> large(nlarge(rng));
        for (auto& v : large) v = dist(rng);
        std::vector<std::int64_t> small(nsmall(rng));
        for (auto& v : small) {
            // Bias toward guaranteed hits so both paths get exercised.
            v = (!large.empty() && rng() % 2) ? large[rng() % large.size()] : dist(rng);
        }
        check_both(large, small, "randomized round");
    }
}

void test_common_values_contract_edges() {
    std::cout << "[Q1] out-of-contract small vector (> 100) still correct\n";
    std::vector<int> large(10'000);
    for (int i = 0; i < 10'000; ++i) large[i] = i;
    std::vector<int> small(500);
    for (int i = 0; i < 500; ++i) small[i] = i * 37;
    check_both(large, small, "small.size() == 500");
}

// ---------------------------------------------------------------------------
void test_trivially_convertible() {
    using namespace hdx;
    std::cout << "[Q2] trivially_convertible\n";

    auto expect = [](const DataTypePtr& a, const DataTypePtr& b, bool want) {
        const bool got = trivially_convertible(a, b);
        check(got == want, to_string(a) + " -> " + to_string(b) +
                               (want ? " should be true" : " should be false"));
    };

    // Provided example tests.
    auto int64 = std::make_shared<DataType>(DataTypeKind::int64);
    auto null_int64 = std::make_shared<DataType>(DataTypeKind::nullable, int64);
    auto str = std::make_shared<DataType>(DataTypeKind::string);
    assert(trivially_convertible(int64, null_int64));
    assert(!trivially_convertible(null_int64, int64));
    assert(!trivially_convertible(int64, str));

    // Identity.
    expect(Int64(), Int64(), true);
    expect(String(), String(), true);
    expect(Nullable(Int64()), Nullable(Int64()), true);
    expect(Array(String()), Array(String()), true);

    // Leaves.
    expect(Int64(), String(), false);
    expect(String(), Int64(), false);
    expect(String(), Nullable(String()), true);
    expect(Int64(), Nullable(String()), false);
    expect(Nullable(Int64()), Nullable(String()), false);

    // Arrays.
    expect(Array(Int64()), Array(Nullable(Int64())), true);
    expect(Array(Nullable(Int64())), Array(Int64()), false);
    expect(Array(Int64()), Int64(), false);
    expect(Int64(), Array(Int64()), false);
    expect(Array(Int64()), Array(String()), false);
    expect(Array(Array(Int64())), Array(Array(Nullable(Int64()))), true);
    expect(Array(Array(Int64())), Array(Int64()), false);

    // Nullable wrapping arrays (generic handling; see header note).
    expect(Array(Int64()), Nullable(Array(Int64())), true);
    expect(Array(Int64()), Nullable(Array(Nullable(Int64()))), true);
    expect(Nullable(Array(Int64())), Array(Int64()), false);
    expect(Nullable(Array(Int64())), Nullable(Array(Nullable(Int64()))), true);
    expect(Array(Nullable(Array(Int64()))), Array(Nullable(Array(Nullable(Int64())))), true);
    expect(Array(Nullable(Array(Nullable(Int64())))), Array(Nullable(Array(Int64()))), false);

    // Malformed input is rejected, never crashes.
    expect(nullptr, Int64(), false);
    expect(Int64(), nullptr, false);
    expect(std::make_shared<DataType>(DataTypeKind::array), Array(Int64()), false);
    expect(Int64(), std::make_shared<DataType>(DataTypeKind::nullable), false);

    // Deep nesting: iterative walk, no recursion depth issue.
    DataTypePtr deep_from = Int64();
    DataTypePtr deep_to = Nullable(Int64());
    for (int i = 0; i < 100'000; ++i) {
        deep_from = Array(deep_from);
        deep_to = Array(deep_to);
    }
    check(trivially_convertible(deep_from, deep_to), "100k-deep Array nesting");
    // Tear down deep chains iteratively (shared_ptr's recursive destructor
    // would otherwise overflow the stack; a test-only concern).
    for (auto* p : {&deep_from, &deep_to}) {
        while (*p) *p = (*p)->subtype();
    }
}

}  // namespace

int main() {
    test_common_values_basic();
    test_common_values_modes();
    test_common_values_randomized();
    test_common_values_contract_edges();
    test_trivially_convertible();

    std::cout << '\n' << (g_checks - g_failures) << '/' << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
