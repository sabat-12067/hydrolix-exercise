// Micro-benchmark: chosen approaches vs the obvious alternatives.
// Numbers are indicative only; run on the target hardware before trusting them.
#include <hdx/common_values.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

template <typename F>
double time_us(F&& f, int iters) {
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) f();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
}

volatile std::size_t g_sink;  // defeat dead-code elimination

std::vector<int> make_large(std::mt19937& rng, bool dense) {
    std::vector<int> v(1'000'000);
    if (dense) {
        std::uniform_int_distribution<int> d(0, 10'000'000);
        for (auto& x : v) x = d(rng);
    } else {
        std::uniform_int_distribution<int> d(std::numeric_limits<int>::min(),
                                             std::numeric_limits<int>::max());
        for (auto& x : v) x = d(rng);
    }
    return v;
}

std::vector<int> make_small(std::mt19937& rng, const std::vector<int>& large) {
    std::vector<int> s(100);
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = (i % 2) ? large[rng() % large.size()] : static_cast<int>(rng());
    }
    return s;
}

void run(bool dense) {
    std::mt19937 rng(7);
    const auto large = make_large(rng, dense);
    const auto small = make_small(rng, large);
    std::printf("\n=== large: 1M ints, %s ===\n",
                dense ? "clustered range [0, 10M]" : "uniform 32-bit");

    // ---- Scenario 1: build once, query many times --------------------------
    std::printf("Scenario 1 (long-lived large vector): build ms | query us (100 probes)\n");
    {
        std::unique_ptr<hdx::CommonValuesIndex<int>> idx;
        const double build = time_us([&] { idx = std::make_unique<hdx::CommonValuesIndex<int>>(large); }, 3);
        const double q = time_us([&] { g_sink = idx->common_values(small).size(); }, 20'000);
        std::printf("  CommonValuesIndex (%s)  %8.2f | %7.3f\n",
                    idx->uses_dense_bitmap() ? "bitmap" : "flat hash", build / 1000, q);
    }
    {
        std::unordered_set<int> set;
        const double build = time_us([&] { set = std::unordered_set<int>(large.begin(), large.end()); }, 3);
        const double q = time_us([&] {
            std::size_t n = 0;
            for (int v : small) n += set.contains(v);
            g_sink = n;
        }, 20'000);
        std::printf("  std::unordered_set            %8.2f | %7.3f\n", build / 1000, q);
    }
    {
        std::vector<int> sorted;
        const double build = time_us([&] {
            sorted = large;
            std::ranges::sort(sorted);
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        }, 3);
        const double q = time_us([&] {
            std::size_t n = 0;
            for (int v : small) n += std::ranges::binary_search(sorted, v);
            g_sink = n;
        }, 20'000);
        std::printf("  sorted vector + binary search %8.2f | %7.3f\n", build / 1000, q);
    }

    // ---- Scenario 2: one-shot ----------------------------------------------
    std::printf("Scenario 2 (one-shot, total us per call):\n");
    const double once = time_us([&] { g_sink = hdx::find_common_values_once(large, small).size(); }, 50);
    std::printf("  find_common_values_once       %10.1f\n", once);
    const double build_large = time_us([&] {
        g_sink = hdx::CommonValuesIndex<int>{large}.common_values(small).size();
    }, 5);
    std::printf("  build index on large, query   %10.1f\n", build_large);
    const double sort_merge = time_us([&] {
        auto a = large;
        auto b = small;
        std::ranges::sort(a);
        std::ranges::sort(b);
        std::vector<int> out;
        std::ranges::set_intersection(a, b, std::back_inserter(out));
        g_sink = out.size();
    }, 5);
    std::printf("  sort both + set_intersection  %10.1f\n", sort_merge);
}

}  // namespace

int main() {
    run(/*dense=*/false);
    run(/*dense=*/true);
}
