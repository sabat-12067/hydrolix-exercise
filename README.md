# Hydrolix C++ Candidate Exercise

Header-only C++23 solutions to both questions. Design reasoning, tradeoffs, and benchmark results are in **[DESIGN.md](DESIGN.md)**.

| File | Contents |
|---|---|
| `include/hdx/common_values.hpp` | Q1: `CommonValuesIndex` (long-lived) and `find_common_values_once` (one-shot) |
| `include/hdx/type_conversion.hpp` | Q2: provided `DataType` helper + `trivially_convertible` |
| `tests/tests.cpp` | Unit tests, edge cases, and randomized cross-checks against a naive reference |
| `bench/bench.cpp` | Benchmark vs `std::unordered_set`, sorted vector, and sort + merge |

## Build and run

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/tests
./build/bench

# Tests under AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DHDX_SANITIZE=ON
cmake --build build-san --target tests && ./build-san/tests
```

Requires CMake 3.20+ and GCC 13+ or Clang 17+. Builds cleanly with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.

## Quick usage

```cpp
#include <hdx/common_values.hpp>
#include <hdx/type_conversion.hpp>

// Q1, scenario 1: build once, query many times (thread-safe queries)
hdx::CommonValuesIndex index{large_vector};
auto common = index.common_values(small_vector);

// Q1, scenario 2: one-shot
auto common_once = hdx::find_common_values_once(large_vector, small_vector);

// Q2
using namespace hdx;
trivially_convertible(Int64(), Nullable(Int64()));                // true
trivially_convertible(Array(Int64()), Array(Nullable(Int64())));  // true
trivially_convertible(Nullable(Int64()), Int64());                // false
```
