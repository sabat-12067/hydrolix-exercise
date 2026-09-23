#pragma once
// ============================================================================
// Question 2: trivially_convertible(from_type, to_type)
//
// Rules, derived from the prompt:
//   R1  Leaf types (Int64, String) convert only to the identical leaf.
//   R2  X            -> Nullable(Y)  iff  X -> Y     (gain the ability to hold NULL)
//   R3  Nullable(X)  -> Nullable(Y)  iff  X -> Y
//   R4  Nullable(X)  -> non-Nullable      is never trivial (NULLs need a fill value)
//   R5  Array(X)     -> Array(Y)     iff  X -> Y     (element-wise; Array is a
//                                                     container, not a value)
//   R6  Any other pair of kinds is not trivial.
//
// Consequences worth calling out:
//   * Array(Int64)            -> Array(Nullable(Int64))           true  (R5, R2)
//   * Array(Int64)            -> Nullable(Array(Nullable(Int64))) true  (R2, R5, R2)
//   * Array(Nullable(Int64))  -> Array(Int64)                     false (R5, R4)
//   * Nullable(Array(Int64))  -> Array(Int64)                     false (R4)
//
// Note: real ClickHouse does not allow Nullable(Array(...)). The team
// confirmed on Slack that it should be allowed for this exercise, so the
// function handles it generically. If that ever changes, it's a one-line
// check at the top of walk().
//
// Implementation choice: an ITERATIVE walk over both type trees in lockstep.
//   * Each rule either rejects, accepts, or peels one wrapper off one or both
//     sides. The walk is O(depth), allocation-free, and never recurses, so a
//     pathologically deep Array(Array(...)) from untrusted metadata can't
//     overflow the stack.
//   * Cost note: the provided DataType::subtype() returns the shared_ptr BY
//     VALUE (`auto`), so every step does an atomic ref-count inc/dec. That is
//     negligible for a schema check, but if this ran per-row I'd suggest
//     changing it to `const DataTypePtr& subtype() const` in the helper.
//   * Malformed input (null pointer, wrapper without a subtype) returns false
//     instead of crashing. The public signature can't report errors, and
//     "not trivially convertible" is the safe answer for a schema migration.
//
// Extension points (the Guidance asks for "abstractions to simplify future
// changes"):
//   * Leaf widening (Int32 -> Int64, FixedString(N) -> String) goes in
//     leaf_trivially_convertible(), a single constexpr table.
//   * New single-child wrappers (LowCardinality) are one more case in walk().
//   * Multi-child types (Tuple, Map) would turn the loop into a small explicit
//     stack of (from, to) pairs; the rules stay the same shape.
// ============================================================================

#include <memory>
#include <string>

// ========= Helper Code (as provided) =========
class DataType;
using DataTypePtr = std::shared_ptr<DataType>;

// Please handle these type families ("kinds"):
enum DataTypeKind { nullable, int64, array, string };

// This class holds all necessary information about a column's datatype.
class DataType {
public:
    explicit DataType(DataTypeKind kind, DataTypePtr subtype = {})
        : _kind(kind), _subtype(std::move(subtype)) {}

    [[nodiscard]] auto kind() const { return _kind; }
    [[nodiscard]] auto subtype() const { return _subtype; }

private:
    DataTypeKind _kind;
    DataTypePtr _subtype;
};
// ========= End helper code =========

namespace hdx::detail {

[[nodiscard]] constexpr bool is_wrapper(DataTypeKind k) noexcept {
    return k == DataTypeKind::nullable || k == DataTypeKind::array;
}

// Leaf-to-leaf rule (R1). Today it is identity; adding safe widenings later is
// a one-line change here and nothing else moves.
[[nodiscard]] constexpr bool leaf_trivially_convertible(DataTypeKind from,
                                                        DataTypeKind to) noexcept {
    return from == to;
}

// Walks both trees in lockstep, peeling one wrapper per step.
[[nodiscard]] inline bool walk(DataTypePtr from, DataTypePtr to) noexcept {
    while (true) {
        if (!from || !to) return false;

        const DataTypeKind fk = from->kind();
        const DataTypeKind tk = to->kind();

        // Malformed: a wrapper with no subtype.
        if (is_wrapper(fk) && !from->subtype()) return false;
        if (is_wrapper(tk) && !to->subtype()) return false;

        if (fk == DataTypeKind::nullable) {
            if (tk != DataTypeKind::nullable) return false;  // R4
            from = from->subtype();                          // R3
            to = to->subtype();
            continue;
        }

        if (tk == DataTypeKind::nullable) {  // R2: from is non-Nullable here
            to = to->subtype();
            continue;
        }

        // Neither side is Nullable from here on.
        if (fk == DataTypeKind::array || tk == DataTypeKind::array) {
            if (fk != tk) return false;  // R6: Array vs leaf
            from = from->subtype();      // R5
            to = to->subtype();
            continue;
        }

        return leaf_trivially_convertible(fk, tk);  // R1
    }
}

}  // namespace hdx::detail

// ========= The function we'd like you to write: =========
[[nodiscard]] static bool trivially_convertible(const DataTypePtr& from_type,
                                                const DataTypePtr& to_type) {
    return hdx::detail::walk(from_type, to_type);
}

// ========= Small conveniences for tests / debugging =========
namespace hdx {

[[nodiscard]] inline DataTypePtr Int64() {
    return std::make_shared<DataType>(DataTypeKind::int64);
}
[[nodiscard]] inline DataTypePtr String() {
    return std::make_shared<DataType>(DataTypeKind::string);
}
[[nodiscard]] inline DataTypePtr Nullable(DataTypePtr t) {
    // Mirrors the "back-to-back Nullable is collapsed" guarantee.
    if (t && t->kind() == DataTypeKind::nullable) return t;
    return std::make_shared<DataType>(DataTypeKind::nullable, std::move(t));
}
[[nodiscard]] inline DataTypePtr Array(DataTypePtr t) {
    return std::make_shared<DataType>(DataTypeKind::array, std::move(t));
}

[[nodiscard]] inline std::string to_string(const DataTypePtr& t) {
    if (!t) return "<null>";
    switch (t->kind()) {
        case DataTypeKind::int64:    return "Int64";
        case DataTypeKind::string:   return "String";
        case DataTypeKind::nullable: return "Nullable(" + to_string(t->subtype()) + ")";
        case DataTypeKind::array:    return "Array(" + to_string(t->subtype()) + ")";
    }
    return "<unknown>";
}

}  // namespace hdx
