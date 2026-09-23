# Hydrolix C++ Exercise: Design Notes

Matthew Zhan

Both solutions are header-only, C++23, and live in `include/hdx/`. Tests cross-check every fast path against a naive reference, and everything builds warning-free under `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` and runs clean under ASan + UBSan.

---

## Question 1: Find Common Values

### Requirements

The prompt left the output contract open, so I asked on Slack. Confirmed answers:

- **Distinct values** that appear in both inputs. Duplicates are not kept.
- **Output order doesn't matter.** I still return values in order of first appearance in the small vector, because with at most 100 elements it costs nothing, and it makes both scenarios return identical, deterministic results that are easy to test against each other.
- **The small vector has at most 100 elements.** This is a hard guarantee, which is what lets scenario 2 use a fixed-size table on the stack.

On top of that, integers are handled generically via `template <std::integral T>`, so the same code serves Int32, Int64, and UInt64 columns, including their extreme values.

### Scenario 1: long-lived large vector (`CommonValuesIndex`)

The large side is read-mostly, so paying O(N) once to make every query cheap is the right call. The question is which structure.

**Chosen: open-addressing flat hash set with linear probing**, plus an adaptive dense bitmap mode.

- Keys live inline in one contiguous array. No per-node allocation, no pointer chasing, and a probe sequence usually stays within one cache line.
- Power-of-two capacity at load factor ≤ 0.5, so the index is a mask instead of a modulo and expected probe lengths stay short.
- Hash function is Fibonacci (multiplicative) hashing: one multiply and one shift. This matters more than it looks. `std::hash<int>` in libstdc++ is the identity, and real columns (IDs, timestamps) are sequential or clustered, which is the worst case for identity hashing with linear probing.
- Empty slots use `numeric_limits<T>::min()` as a sentinel. Since that value is also legal data, its presence is tracked in a separate flag. No value is silently unsupported.
- Queries prefetch every probe slot first, then probe. With 100 lookups into an ~8 MB table, this overlaps the cache misses instead of paying for them one after another.

**Dense bitmap mode.** If `max - min` is small relative to N, a bitmap is strictly better: exact, one bit per possible value, a single load and bit test per lookup, no hashing at all. The class picks it automatically whenever the bitmap would be no larger than the hash table. For 1M values that means any range up to ~67M, which covers a lot of real ID and time-bucket data.

**Alternatives considered**

| Approach | Why not the default |
|---|---|
| `std::unordered_set` | Node-based: one allocation and ~32+ bytes per element. For 1M ints that's tens of MB, and every lookup is a pointer chase. Build is ~20x slower in my benchmark. |
| Sorted vector + binary search | Compact and simple, but ~20 dependent, mostly cache-missing loads per lookup at N = 1M. Queries measured ~3-4x slower. An Eytzinger layout helps locality but not the log N depth. |
| Bloom filter in front | Only pays off when most probes miss *and* the exact set is too big for cache. Here the exact set is only ~8 MB. |
| Roaring bitmap | Great for mixed sparse/dense data and would generalize the dense mode. More code than this exercise warrants; I'd reach for the library in production. |

**Handling "rarely changes."** The index is immutable after construction, so any number of threads can query it without locks. When the large vector does change, the clean approach is to build a new index off the hot path and swap an `std::shared_ptr<const CommonValuesIndex>` atomically (RCU style). Readers holding the old one finish undisturbed. If changes were small and frequent instead of rare, I'd add an insert method (the table already supports it internally) with a resize at the load-factor limit.

### Scenario 2: one-shot (`find_common_values_once`)

Building anything over the 1M side is wasted work: you touch every element to build it, then use it once. So flip it. **Index the small side, stream the large side once.** A sequential pass is exactly what the hardware prefetcher is best at.

- Small-side table: 256 slots on the stack (a few KB, fits in L1, zero heap allocations). Load factor ≤ 0.4.
- **Range pre-filter:** one unsigned compare against `[min, max]` of the small side rejects out-of-range elements before hashing. Big win when the small set is clustered, nearly free otherwise.
- **512-byte membership filter:** a 4096-bit filter keyed on different hash bits than the table. With ≤ 100 values it rejects ~97.5% of non-matching elements with a single bit test.
- **Early exit** once every distinct small value has been found.

The membership filter came out of benchmarking, not upfront design. My first version went straight from the range check to the hash table, and it was slower than expected: in the clustered case, building a full bitmap over all 1M values was actually *faster*. The cause was branch misprediction. With the table ~40% full, "is this slot empty?" is close to a coin flip for every element. The filter's branch is almost always "not present," so it predicts well. That one change made the path about **5x faster**.

The team confirmed the 100-element limit is a hard guarantee, so the fixed table is always large enough. I kept one cheap safety check anyway: if `small.size()` ever exceeds 100, the function falls back to the dynamically sized index instead of overfilling the table. It's one compare per call, and it turns a future contract change into a slowdown instead of a hang.

**Alternatives considered**

| Approach | Why not |
|---|---|
| Hash set of the large vector | O(N) inserts and allocations for a single use. Measured ~6-9x slower. |
| Sort both + `std::set_intersection` | O(N log N), and it has to copy or mutate the caller's large vector. Measured ~40-50x slower. |
| Sorted small side + binary search | ~7 unpredictable branches per element vs. one well-predicted filter test. |
| SIMD brute force | Compare each element against all small values: ~13 AVX2 compares per element at 100 values. Branch-free and simple, and it likely wins when the small side is tiny (≤ ~16). A natural special case to add, dispatched on `small.size()`. |
| Parallel scan | 1M ints is ~4 MB and memory-bound, done in ~1-3 ms. Thread startup would eat the gain. Worth it for much larger N, e.g. chunked across a thread pool with a shared found-count for early exit. |

### Benchmark

`bench/bench.cpp`, 1M ints, 100 probes (half of them guaranteed hits). Measured on Windows (MSYS2 UCRT64), GCC 16.1, `-O3 -march=native`.

**Uniform 32-bit values** (hash table mode)

| Scenario 1: build once, query many | Build | Query (100 probes) |
|---|---|---|
| `CommonValuesIndex` (flat hash) | **18.4 ms** | 0.96 µs |
| `std::unordered_set` | 596.8 ms | 0.78 µs |
| Sorted vector + binary search | 78.6 ms | 2.88 µs |

| Scenario 2: one-shot | Total per call |
|---|---|
| `find_common_values_once` | **2.22 ms** |
| Build index on large, then query | 18.8 ms |
| Sort both + `set_intersection` | 76.1 ms |

**Clustered values, range 0–10M** (bitmap mode kicks in automatically)

| Scenario 1: build once, query many | Build | Query (100 probes) |
|---|---|---|
| `CommonValuesIndex` (bitmap) | **5.4 ms** | 0.73 µs |
| `std::unordered_set` | 687.4 ms | 0.51 µs |
| Sorted vector + binary search | 83.5 ms | 1.96 µs |

| Scenario 2: one-shot | Total per call |
|---|---|
| `find_common_values_once` | **1.70 ms** |
| Build index on large, then query | 5.4 ms |
| Sort both + `set_intersection` | 77.6 ms |

Summary: the flat table builds ~32x faster than `std::unordered_set` on uniform data, and the bitmap builds ~126x faster on clustered data. The one-shot path is ~8.5x faster than building an index for a single use, and ~34-46x faster than sort + merge.

A note on query latency: `std::unordered_set` looks slightly *faster* on the query row, and I don't want to hide that. Two reasons. First, the benchmark repeats the same 100 lookups, so those specific nodes stay hot in cache, which is exactly the case node-based containers handle least badly. Second, my query also builds a deduplicated output vector while the baseline only counts hits. The real wins for the flat table are the order-of-magnitude faster build, a fraction of the memory (one array vs. a million heap nodes), and much better behavior when the structure isn't already cache-hot, which is the realistic case on a busy server.

## Question 2: `trivially_convertible`

### Rules

Derived from the prompt, then applied recursively through wrappers:

| # | Rule |
|---|---|
| R1 | Leaf types (Int64, String) convert only to the identical leaf. |
| R2 | `X → Nullable(Y)` iff `X → Y`. Gaining NULL-ability is free. |
| R3 | `Nullable(X) → Nullable(Y)` iff `X → Y`. |
| R4 | `Nullable(X) → non-Nullable` is never trivial. NULLs would need a fill value. |
| R5 | `Array(X) → Array(Y)` iff `X → Y`. Array is a container; its elements convert. |
| R6 | Anything else (Array vs. leaf, Int64 vs. String) is not trivial. |

Some consequences worth stating explicitly:

- `Array(Int64) → Array(Nullable(Int64))` is **true**. Each element gains NULL-ability.
- `Array(Nullable(Int64)) → Array(Int64)` is **false**. R4 applies at the element level.
- `Array(Int64) → Nullable(Array(Nullable(Int64)))` is **true** (R2, R5, R2).

Real ClickHouse doesn't allow `Nullable(Array(...))`. I asked on Slack, and the team confirmed it should be allowed for this exercise, so the function handles it generically. If it ever needs to be rejected, it's a one-line check.

### Implementation

An **iterative lockstep walk** over both type trees. Each step either rejects, accepts, or peels one wrapper off one or both sides.

- O(depth), no allocation, and no recursion. A pathologically deep `Array(Array(...))` from untrusted metadata can't overflow the stack. The tests verify a 100,000-deep nesting.
- Malformed input (null pointer, wrapper with no subtype) returns `false` instead of crashing. The signature has no way to report errors, and "not trivially convertible" is the safe answer for a schema migration: worst case, the caller does a real conversion it didn't strictly need.
- Checking Nullable on the `from` side first is what makes R4 fall out naturally, and the back-to-back Nullable guarantee means we never need to peel two Nullable layers in a row.

### Built for change

The Guidance asks for abstractions that simplify future changes. Here's where each likely change lands:

- **Leaf widening** (Int32 → Int64, FixedString(N) → String): one constexpr function, `leaf_trivially_convertible()`, currently identity. Nothing else moves.
- **New single-child wrappers** (LowCardinality): one more case in the walk.
- **Multi-child types** (Tuple, Map): the loop becomes a small explicit stack of `(from, to)` pairs. The rule structure stays the same.

One small observation on the provided helper: `DataType::subtype()` returns the `shared_ptr` by value (`auto`), so every step of any traversal does an atomic ref-count increment and decrement. That's irrelevant for a schema check, but if this ever ran on a hot path I'd suggest `const DataTypePtr& subtype() const`.

---

## Clarifications from Slack

1. Q1: Duplicates are not kept; output order doesn't matter.
2. Q1: The 100-element limit on the small vector is a hard guarantee.
3. Q2: `Nullable(Array(...))` should be allowed.
