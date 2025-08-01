#include <Columns/ColumnVector.h>

#include <base/bit_cast.h>
#include <base/scope_guard.h>
#include <base/sort.h>
#include <base/unaligned.h>
#include <Columns/ColumnCompressed.h>
#include <Columns/ColumnsCommon.h>
#include <Columns/ColumnConst.h>
#include <Columns/MaskOperations.h>
#include <Columns/RadixSortHelper.h>
#include <IO/WriteHelpers.h>
#include <Common/Arena.h>
#include <Common/Exception.h>
#include <Common/FieldVisitorToString.h>
#include <Common/HashTable/Hash.h>
#include <Common/HashTable/StringHashSet.h>
#include <Common/NaNUtils.h>
#include <Common/RadixSort.h>
#include <Common/SipHash.h>
#include <Common/TargetSpecific.h>
#include <Common/WeakHash.h>
#include <Common/assert_cast.h>
#include <Common/findExtreme.h>
#include <Common/iota.h>
#include <DataTypes/FieldToDataType.h>

#include <bit>
#include <cstring>

#if defined(__SSE2__)
#    include <emmintrin.h>
#endif

#include "config.h"

#if USE_MULTITARGET_CODE
#    include <immintrin.h>
#endif

#if USE_EMBEDDED_COMPILER
#include <DataTypes/Native.h>
#include <llvm/IR/IRBuilder.h>
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int PARAMETER_OUT_OF_BOUND;
    extern const int SIZES_OF_COLUMNS_DOESNT_MATCH;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

template <typename T>
const char * ColumnVector<T>::deserializeAndInsertFromArena(const char * pos)
{
    data.emplace_back(unalignedLoad<T>(pos));
    return pos + sizeof(T);
}

template <typename T>
const char * ColumnVector<T>::skipSerializedInArena(const char * pos) const
{
    return pos + sizeof(T);
}

template <typename T>
void ColumnVector<T>::updateHashWithValue(size_t n, SipHash & hash) const
{
    hash.update(data[n]);
}

template <typename T>
WeakHash32 ColumnVector<T>::getWeakHash32() const
{
    auto s = data.size();
    WeakHash32 hash(s);

    const T * begin = data.data();
    const T * end = begin + s;
    UInt32 * hash_data = hash.getData().data();

    while (begin < end)
    {
        *hash_data = static_cast<UInt32>(hashCRC32(*begin, *hash_data));
        ++begin;
        ++hash_data;
    }

    return hash;
}

template <typename T>
void ColumnVector<T>::updateHashFast(SipHash & hash) const
{
    hash.update(reinterpret_cast<const char *>(data.data()), size() * sizeof(data[0]));
}

template <typename T>
struct ColumnVector<T>::less
{
    const Self & parent;
    int nan_direction_hint;
    less(const Self & parent_, int nan_direction_hint_) : parent(parent_), nan_direction_hint(nan_direction_hint_) {}
    bool operator()(size_t lhs, size_t rhs) const { return CompareHelper<T>::less(parent.data[lhs], parent.data[rhs], nan_direction_hint); }
};

template <typename T>
struct ColumnVector<T>::less_stable
{
    const Self & parent;
    int nan_direction_hint;
    less_stable(const Self & parent_, int nan_direction_hint_) : parent(parent_), nan_direction_hint(nan_direction_hint_) {}
    bool operator()(size_t lhs, size_t rhs) const
    {
        if (unlikely(parent.data[lhs] == parent.data[rhs]))
            return lhs < rhs;

        if constexpr (is_floating_point<T>)
        {
            if (unlikely(isNaN(parent.data[lhs]) && isNaN(parent.data[rhs])))
            {
                return lhs < rhs;
            }
        }

        return CompareHelper<T>::less(parent.data[lhs], parent.data[rhs], nan_direction_hint);
    }
};

template <typename T>
struct ColumnVector<T>::greater
{
    const Self & parent;
    int nan_direction_hint;
    greater(const Self & parent_, int nan_direction_hint_) : parent(parent_), nan_direction_hint(nan_direction_hint_) {}
    bool operator()(size_t lhs, size_t rhs) const { return CompareHelper<T>::greater(parent.data[lhs], parent.data[rhs], nan_direction_hint); }
};

template <typename T>
struct ColumnVector<T>::greater_stable
{
    const Self & parent;
    int nan_direction_hint;
    greater_stable(const Self & parent_, int nan_direction_hint_) : parent(parent_), nan_direction_hint(nan_direction_hint_) {}
    bool operator()(size_t lhs, size_t rhs) const
    {
        if (unlikely(parent.data[lhs] == parent.data[rhs]))
            return lhs < rhs;

        if constexpr (is_floating_point<T>)
        {
            if (unlikely(isNaN(parent.data[lhs]) && isNaN(parent.data[rhs])))
            {
                return lhs < rhs;
            }
        }

        return CompareHelper<T>::greater(parent.data[lhs], parent.data[rhs], nan_direction_hint);
    }
};

template <typename T>
struct ColumnVector<T>::equals
{
    const Self & parent;
    int nan_direction_hint;
    equals(const Self & parent_, int nan_direction_hint_) : parent(parent_), nan_direction_hint(nan_direction_hint_) {}
    bool operator()(size_t lhs, size_t rhs) const { return CompareHelper<T>::equals(parent.data[lhs], parent.data[rhs], nan_direction_hint); }
};

/// Radix sort treats all positive NaNs to be greater than all numbers.
/// And all negative NaNs to be lower that all numbers.
/// Standard is not specifying which sign NaN should have, so after sort NaNs can
/// be grouped on any side of the array or on both.
/// Move all NaNs to the requested part of result.
template <typename T>
void moveNanToRequestedSide(
    typename ColumnVector<T>::Permutation::iterator begin,
    typename ColumnVector<T>::Permutation::iterator end,
    const typename ColumnVector<T>::Container & data,
    size_t data_size,
    bool reverse,
    int nan_direction_hint
)
{
    const auto is_nulls_last = ((nan_direction_hint > 0) != reverse);
    size_t nans_to_move = 0;

    for (size_t i = 0; i < data_size; ++i)
    {
        if (isNaN(data[begin[is_nulls_last ? i : data_size - 1 - i]]))
            ++nans_to_move;
        else
            break;
    }

    if (nans_to_move)
        std::rotate(begin, begin + (is_nulls_last ? nans_to_move : data_size - nans_to_move), end);
}

#if USE_EMBEDDED_COMPILER

template <typename T>
bool ColumnVector<T>::isComparatorCompilable() const
{
    /// TODO: for std::is_floating_point_v<T> we need implement is_nan in LLVM IR.
    return std::is_integral_v<T>;
}

template <typename T>
llvm::Value * ColumnVector<T>::compileComparator(llvm::IRBuilderBase & builder, llvm::Value * lhs, llvm::Value * rhs, llvm::Value *) const
{
    llvm::IRBuilder<> & b = static_cast<llvm::IRBuilder<> &>(builder);

    if constexpr (std::is_integral_v<T>)
    {
        // a > b ? 1 : (a < b ? -1 : 0);

        bool is_signed = std::is_signed_v<T>;

        auto * lhs_greater_than_rhs_result = llvm::ConstantInt::getSigned(b.getInt8Ty(), 1);
        auto * lhs_less_than_rhs_result = llvm::ConstantInt::getSigned(b.getInt8Ty(), -1);
        auto * lhs_equals_rhs_result = llvm::ConstantInt::getSigned(b.getInt8Ty(), 0);

        auto * lhs_greater_than_rhs = is_signed ? b.CreateICmpSGT(lhs, rhs) : b.CreateICmpUGT(lhs, rhs);
        auto * lhs_less_than_rhs = is_signed ? b.CreateICmpSLT(lhs, rhs) : b.CreateICmpULT(lhs, rhs);
        auto * if_lhs_less_than_rhs_result = b.CreateSelect(lhs_less_than_rhs, lhs_less_than_rhs_result, lhs_equals_rhs_result);

        return b.CreateSelect(lhs_greater_than_rhs, lhs_greater_than_rhs_result, if_lhs_less_than_rhs_result);
    }
    else
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Method compileComparator is not supported for type {}", TypeName<T>);
    }
}

#endif

template <typename T>
void ColumnVector<T>::getPermutation(IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                                    size_t limit, int nan_direction_hint, IColumn::Permutation & res) const
{
    size_t data_size = data.size();
    res.resize_exact(data_size);

    if (data_size == 0)
        return;

    if (limit >= data_size)
        limit = 0;

    iota(res.data(), data_size, IColumn::Permutation::value_type(0));

    if constexpr (has_find_extreme_implementation<T> && !is_floating_point<T>)
    {
        /// Disabled for floating point:
        /// * floating point: We don't deal with nan_direction_hint
        /// * stability::Stable: We might return any value, not the first
        if ((limit == 1) && (stability == IColumn::PermutationSortStability::Unstable))
        {
            std::optional<size_t> index;
            if (direction == IColumn::PermutationSortDirection::Ascending)
                index = findExtremeMinIndex(data.data(), 0, data.size());
            else
                index = findExtremeMaxIndex(data.data(), 0, data.size());
            if (index)
            {
                res.data()[0] = *index;
                return;
            }
        }
    }

    if constexpr (is_arithmetic_v<T> && !is_big_int_v<T>)
    {
        if (!limit)
        {
            /// A case for radix sort
            /// LSD RadixSort is stable

            bool reverse = direction == IColumn::PermutationSortDirection::Descending;
            bool ascending = direction == IColumn::PermutationSortDirection::Ascending;
            bool sort_is_stable = stability == IColumn::PermutationSortStability::Stable;

            /// TODO: LSD RadixSort is currently not stable if direction is descending, or value is floating point
            bool use_radix_sort = (sort_is_stable && ascending && !is_floating_point<T>) || !sort_is_stable;

            /// Thresholds on size. Lower threshold is arbitrary. Upper threshold is chosen by the type for histogram counters.
            if (data_size >= 256 && data_size <= std::numeric_limits<UInt32>::max() && use_radix_sort)
            {
                bool try_sort = false;

                if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Unstable)
                    try_sort = trySort(res.begin(), res.end(), less(*this, nan_direction_hint));
                else if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Stable)
                    try_sort = trySort(res.begin(), res.end(), less_stable(*this, nan_direction_hint));
                else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Unstable)
                    try_sort = trySort(res.begin(), res.end(), greater(*this, nan_direction_hint));
                else
                    try_sort = trySort(res.begin(), res.end(), greater_stable(*this, nan_direction_hint));

                if (try_sort)
                    return;

                PaddedPODArray<ValueWithIndex<T>> pairs(data_size);
                for (UInt32 i = 0; i < static_cast<UInt32>(data_size); ++i)
                    pairs[i] = {data[i], i};

                RadixSort<RadixSortTraits<T>>::executeLSD(pairs.data(), data_size, reverse, res.data());
                if constexpr (is_floating_point<T>)
                    moveNanToRequestedSide<T>(res.begin(), res.end(), data, data_size, reverse, nan_direction_hint);

                return;
            }
        }
    }

    if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Unstable)
        this->getPermutationImpl(limit, res, less(*this, nan_direction_hint), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Stable)
        this->getPermutationImpl(limit, res, less_stable(*this, nan_direction_hint), DefaultSort(), DefaultPartialSort());
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Unstable)
        this->getPermutationImpl(limit, res, greater(*this, nan_direction_hint), DefaultSort(), DefaultPartialSort());
    else
        this->getPermutationImpl(limit, res, greater_stable(*this, nan_direction_hint), DefaultSort(), DefaultPartialSort());
}

template <typename T>
void ColumnVector<T>::updatePermutation(IColumn::PermutationSortDirection direction, IColumn::PermutationSortStability stability,
                                    size_t limit, int nan_direction_hint, IColumn::Permutation & res, EqualRanges & equal_ranges) const
{
    auto sort = [&](auto begin, auto end, auto pred)
    {
        bool reverse = direction == IColumn::PermutationSortDirection::Descending;
        bool ascending = direction == IColumn::PermutationSortDirection::Ascending;
        bool sort_is_stable = stability == IColumn::PermutationSortStability::Stable;

        /// A case for radix sort
        if constexpr (is_arithmetic_v<T> && !is_big_int_v<T>)
        {
            /// TODO: LSD RadixSort is currently not stable if direction is descending, or value is floating point
            bool use_radix_sort = (sort_is_stable && ascending && !is_floating_point<T>) || !sort_is_stable;
            size_t range_size = end - begin;

            /// Thresholds on size. Lower threshold is arbitrary. Upper threshold is chosen by the type for histogram counters.
            if (range_size >= 256 && range_size <= std::numeric_limits<UInt32>::max() && use_radix_sort)
            {
                bool try_sort = trySort(begin, end, pred);
                if (try_sort)
                    return;

                PaddedPODArray<ValueWithIndex<T>> pairs(range_size);
                size_t index = 0;

                for (auto * it = begin; it != end; ++it)
                {
                    pairs[index] = {data[*it], static_cast<UInt32>(*it)};
                    ++index;
                }

                RadixSort<RadixSortTraits<T>>::executeLSD(pairs.data(), range_size, reverse, begin);
                if constexpr (is_floating_point<T>)
                    moveNanToRequestedSide<T>(begin, end, data, range_size, reverse, nan_direction_hint);

                return;
            }
        }

        ::sort(begin, end, pred);
    };
    auto partial_sort = [](auto begin, auto mid, auto end, auto pred) { ::partial_sort(begin, mid, end, pred); };

    if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Unstable)
    {
        this->updatePermutationImpl(
            limit, res, equal_ranges,
            less(*this, nan_direction_hint),
            equals(*this, nan_direction_hint),
            sort, partial_sort);
    }
    else if (direction == IColumn::PermutationSortDirection::Ascending && stability == IColumn::PermutationSortStability::Stable)
    {
        this->updatePermutationImpl(
            limit, res, equal_ranges,
            less_stable(*this, nan_direction_hint),
            equals(*this, nan_direction_hint),
            sort, partial_sort);
    }
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Unstable)
    {
        this->updatePermutationImpl(
            limit, res, equal_ranges,
            greater(*this, nan_direction_hint),
            equals(*this, nan_direction_hint),
            sort, partial_sort);
    }
    else if (direction == IColumn::PermutationSortDirection::Descending && stability == IColumn::PermutationSortStability::Stable)
    {
        this->updatePermutationImpl(
            limit, res, equal_ranges,
            greater_stable(*this, nan_direction_hint),
            equals(*this, nan_direction_hint),
            sort, partial_sort);
    }
}

template<typename T>
size_t ColumnVector<T>::estimateCardinalityInPermutedRange(const IColumn::Permutation & permutation, const EqualRange & equal_range) const
{
    const size_t range_size = equal_range.size();
    if (range_size <= 1)
        return range_size;

    /// TODO use sampling if the range is too large (e.g. 16k elements, but configurable)
    StringHashSet elements;
    bool inserted = false;
    for (size_t i = equal_range.from; i < equal_range.to; ++i)
    {
        size_t permuted_i = permutation[i];
        StringRef value = getDataAt(permuted_i);
        elements.emplace(value, inserted);
    }
    return elements.size();
}

template <typename T>
MutableColumnPtr ColumnVector<T>::cloneResized(size_t size) const
{
    auto res = this->create(size);

    if (size > 0)
    {
        auto & new_col = static_cast<Self &>(*res);
        new_col.data.resize_exact(size);

        size_t count = std::min(this->size(), size);
        memcpy(new_col.data.data(), data.data(), count * sizeof(data[0]));

        if (size > count)
            memset(static_cast<void *>(&new_col.data[count]), 0, (size - count) * sizeof(ValueType));
    }

    return res;
}

template <typename T>
std::pair<String, DataTypePtr> ColumnVector<T>::getValueNameAndType(size_t n) const
{
    chassert(n < data.size()); /// This assert is more strict than the corresponding assert inside PODArray.
    const auto & val = castToNearestFieldType(data[n]);
    return {FieldVisitorToString()(val), FieldToDataType()(val)};
}

template <typename T>
UInt64 ColumnVector<T>::get64(size_t n [[maybe_unused]]) const
{
    if constexpr (is_arithmetic_v<T>)
        return bit_cast<UInt64>(data[n]);
    else
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot get the value of {} as UInt64", TypeName<T>);
}

template <typename T>
inline Float64 ColumnVector<T>::getFloat64(size_t n [[maybe_unused]]) const
{
    if constexpr (is_arithmetic_v<T>)
        return static_cast<Float64>(data[n]);
    else
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot get the value of {} as Float64", TypeName<T>);
}

template <typename T>
Float32 ColumnVector<T>::getFloat32(size_t n [[maybe_unused]]) const
{
    if constexpr (is_arithmetic_v<T>)
        return static_cast<Float32>(data[n]);
    else
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Cannot get the value of {} as Float32", TypeName<T>);
}

template <typename T>
bool ColumnVector<T>::tryInsert(const DB::Field & x)
{
    NearestFieldType<T> value;
    if (!x.tryGet<NearestFieldType<T>>(value))
    {
        if constexpr (std::is_same_v<T, UInt8>)
        {
            /// It's also possible to insert boolean values into UInt8 column.
            bool boolean_value;
            if (x.tryGet<bool>(boolean_value))
            {
                data.push_back(static_cast<T>(boolean_value));
                return true;
            }
        }
        return false;
    }
    data.push_back(static_cast<T>(value));
    return true;
}

template <typename T>
#if !defined(DEBUG_OR_SANITIZER_BUILD)
void ColumnVector<T>::insertRangeFrom(const IColumn & src, size_t start, size_t length)
#else
void ColumnVector<T>::doInsertRangeFrom(const IColumn & src, size_t start, size_t length)
#endif
{
    const ColumnVector & src_vec = assert_cast<const ColumnVector &>(src);

    if (start + length > src_vec.data.size())
        throw Exception(ErrorCodes::PARAMETER_OUT_OF_BOUND,
                        "Parameters start = {}, length = {} are out of bound "
                        "in ColumnVector<T>::insertRangeFrom method (data.size() = {}).",
                        toString(start), toString(length), toString(src_vec.data.size()));

    size_t old_size = data.size();
    data.resize(old_size + length);
    memcpy(data.data() + old_size, &src_vec.data[start], length * sizeof(data[0]));
}

static inline UInt64 blsr(UInt64 mask)
{
#ifdef __BMI__
    return _blsr_u64(mask);
#else
    return mask & (mask-1);
#endif
}

/// If mask is a number of this kind: [0]*[1]* function returns the length of the cluster of 1s.
/// Otherwise it returns the special value: 0xFF.
uint8_t prefixToCopy(UInt64 mask)
{
    if (mask == 0)
        return 0;
    if (mask == static_cast<UInt64>(-1))
        return 64;
    /// Row with index 0 correspond to the least significant bit.
    /// So the length of the prefix to copy is 64 - #(leading zeroes).
    const UInt64 leading_zeroes = __builtin_clzll(mask);
    if (mask == ((static_cast<UInt64>(-1) << leading_zeroes) >> leading_zeroes))
        return 64 - leading_zeroes;
    return 0xFF;
}

uint8_t suffixToCopy(UInt64 mask)
{
    const auto prefix_to_copy = prefixToCopy(~mask);
    return prefix_to_copy >= 64 ? prefix_to_copy : 64 - prefix_to_copy;
}

DECLARE_DEFAULT_CODE(
template <typename T, typename Container, size_t SIMD_ELEMENTS>
inline void doFilterAligned(const UInt8 *& filt_pos, const UInt8 *& filt_end_aligned, const T *& data_pos, Container & res_data)
{
    while (filt_pos < filt_end_aligned)
    {
        UInt64 mask = bytes64MaskToBits64Mask(filt_pos);
        const uint8_t prefix_to_copy = prefixToCopy(mask);

        if (0xFF != prefix_to_copy)
        {
            res_data.insert(data_pos, data_pos + prefix_to_copy);
        }
        else
        {
            const uint8_t suffix_to_copy = suffixToCopy(mask);
            if (0xFF != suffix_to_copy)
            {
                res_data.insert(data_pos + SIMD_ELEMENTS - suffix_to_copy, data_pos + SIMD_ELEMENTS);
            }
            else
            {
                while (mask)
                {
                    size_t index = std::countr_zero(mask);
                    res_data.push_back(data_pos[index]);
                    mask = blsr(mask);
                }
            }
        }

        filt_pos += SIMD_ELEMENTS;
        data_pos += SIMD_ELEMENTS;
    }
}
)

namespace
{
template <typename T, typename Container>
void resize(Container & res_data, size_t reserve_size)
{
#if defined(MEMORY_SANITIZER)
    res_data.resize_fill(reserve_size, static_cast<T>(0)); // MSan doesn't recognize that all allocated memory is written by AVX-512 intrinsics.
#else
    res_data.resize(reserve_size);
#endif
}
}

DECLARE_AVX512VBMI2_SPECIFIC_CODE(
template <size_t ELEMENT_WIDTH>
inline void compressStoreAVX512(const void *src, void *dst, const UInt64 mask)
{
    __m512i vsrc = _mm512_loadu_si512(src);
    if constexpr (ELEMENT_WIDTH == 1)
        _mm512_mask_compressstoreu_epi8(dst, static_cast<__mmask64>(mask), vsrc);
    else if constexpr (ELEMENT_WIDTH == 2)
        _mm512_mask_compressstoreu_epi16(dst, static_cast<__mmask32>(mask), vsrc);
    else if constexpr (ELEMENT_WIDTH == 4)
        _mm512_mask_compressstoreu_epi32(dst, static_cast<__mmask16>(mask), vsrc);
    else if constexpr (ELEMENT_WIDTH == 8)
        _mm512_mask_compressstoreu_epi64(dst, static_cast<__mmask8>(mask), vsrc);
}

template <typename T, typename Container, size_t SIMD_ELEMENTS>
inline void doFilterAligned(const UInt8 *& filt_pos, const UInt8 *& filt_end_aligned, const T *& data_pos, Container & res_data)
{
    static constexpr size_t VEC_LEN = 64;   /// AVX512 vector length - 64 bytes
    static constexpr size_t ELEMENT_WIDTH = sizeof(T);
    static constexpr size_t ELEMENTS_PER_VEC = VEC_LEN / ELEMENT_WIDTH;
    static constexpr UInt64 KMASK = 0xffffffffffffffff >> (64 - ELEMENTS_PER_VEC);

    size_t current_offset = res_data.size();
    size_t reserve_size = res_data.size();
    size_t alloc_size = SIMD_ELEMENTS * 2;

    while (filt_pos < filt_end_aligned)
    {
        /// to avoid calling resize too frequently, resize to reserve buffer.
        if (reserve_size - current_offset < SIMD_ELEMENTS)
        {
            reserve_size += alloc_size;
            resize<T>(res_data, reserve_size);
            alloc_size *= 2;
        }

        UInt64 mask = bytes64MaskToBits64Mask(filt_pos);

        if (0xffffffffffffffff == mask)
        {
            for (size_t i = 0; i < SIMD_ELEMENTS; i += ELEMENTS_PER_VEC)
                _mm512_storeu_si512(reinterpret_cast<void *>(&res_data[current_offset + i]),
                        _mm512_loadu_si512(reinterpret_cast<const void *>(data_pos + i)));
            current_offset += SIMD_ELEMENTS;
        }
        else
        {
            if (mask)
            {
                for (size_t i = 0; i < SIMD_ELEMENTS; i += ELEMENTS_PER_VEC)
                {
                    compressStoreAVX512<ELEMENT_WIDTH>(reinterpret_cast<const void *>(data_pos + i),
                            reinterpret_cast<void *>(&res_data[current_offset]), mask & KMASK);
                    current_offset += std::popcount(mask & KMASK);
                    /// prepare mask for next iter, if ELEMENTS_PER_VEC = 64, no next iter
                    if constexpr (ELEMENTS_PER_VEC < 64)
                    {
                        mask >>= ELEMENTS_PER_VEC;
                    }
                }
            }
        }

        filt_pos += SIMD_ELEMENTS;
        data_pos += SIMD_ELEMENTS;
    }
    /// Resize to the real size.
    res_data.resize_exact(current_offset);
}
)

template <typename T>
ColumnPtr ColumnVector<T>::filter(const IColumn::Filter & filt, ssize_t result_size_hint) const
{
    size_t size = data.size();
    if (size != filt.size())
        throw Exception(ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH, "Size of filter ({}) doesn't match size of column ({})", filt.size(), size);

    auto res = this->create();
    Container & res_data = res->getData();

    if (result_size_hint)
        res_data.reserve_exact(result_size_hint > 0 ? result_size_hint : size);

    const UInt8 * filt_pos = filt.data();
    const UInt8 * filt_end = filt_pos + size;
    const T * data_pos = data.data();

    /** A slightly more optimized version.
      * Based on the assumption that often pieces of consecutive values
      *  completely pass or do not pass the filter.
      * Therefore, we will optimistically check the parts of `SIMD_ELEMENTS` values.
      */
    static constexpr size_t SIMD_ELEMENTS = 64;
    const UInt8 * filt_end_aligned = filt_pos + size / SIMD_ELEMENTS * SIMD_ELEMENTS;

#if USE_MULTITARGET_CODE
    static constexpr bool VBMI2_CAPABLE = sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8;
    if (VBMI2_CAPABLE && isArchSupported(TargetArch::AVX512VBMI2))
        TargetSpecific::AVX512VBMI2::doFilterAligned<T, Container, SIMD_ELEMENTS>(filt_pos, filt_end_aligned, data_pos, res_data);
    else
#endif
        TargetSpecific::Default::doFilterAligned<T, Container, SIMD_ELEMENTS>(filt_pos, filt_end_aligned, data_pos, res_data);

    while (filt_pos < filt_end)
    {
        if (*filt_pos)
            res_data.push_back(*data_pos);

        ++filt_pos;
        ++data_pos;
    }

    return res;
}

template <typename T>
void ColumnVector<T>::expand(const IColumn::Filter & mask, bool inverted)
{
    expandDataByMask<T>(data, mask, inverted);
}

template <typename T>
void ColumnVector<T>::applyZeroMap(const IColumn::Filter & filt, bool inverted)
{
    size_t size = data.size();
    if (size != filt.size())
        throw Exception(ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH, "Size of filter ({}) doesn't match size of column ({})", filt.size(), size);

    const UInt8 * filt_pos = filt.data();
    const UInt8 * filt_end = filt_pos + size;
    T * data_pos = data.data();

    if (inverted)
    {
        for (; filt_pos < filt_end; ++filt_pos, ++data_pos)
            if (!*filt_pos)
                *data_pos = 0;
    }
    else
    {
        for (; filt_pos < filt_end; ++filt_pos, ++data_pos)
            if (*filt_pos)
                *data_pos = 0;
    }
}

template <typename T>
ColumnPtr ColumnVector<T>::permute(const IColumn::Permutation & perm, size_t limit) const
{
    return permuteImpl(*this, perm, limit);
}

template <typename T>
ColumnPtr ColumnVector<T>::index(const IColumn & indexes, size_t limit) const
{
    return selectIndexImpl(*this, indexes, limit);
}

#ifdef __SSE2__

namespace
{
    /** Optimization for ColumnVector replicate using SIMD instructions.
      * For such optimization it is important that data is right padded with 15 bytes.
      *
      * Replicate span size is offsets[i] - offsets[i - 1].
      *
      * Split spans into 3 categories.
      * 1. Span with 0 size. Continue iteration.
      *
      * 2. Span with 1 size. Update pointer from which data must be copied into result.
      * Then if we see span with size 1 or greater than 1 copy data directly into result data and reset pointer.
      * Example:
      * Data: 1 2 3 4
      * Offsets: 1 2 3 4
      * Result data: 1 2 3 4
      *
      * 3. Span with size greater than 1. Save single data element into register and copy it into result data.
      * Example:
      * Data: 1 2 3 4
      * Offsets: 4 4 4 4
      * Result data: 1 1 1 1
      *
      * Additional handling for tail is needed if pointer from which data must be copied from span with size 1 is not null.
      */
    template<typename IntType>
    requires (std::is_same_v<IntType, Int32> || std::is_same_v<IntType, UInt32>)
    void replicateSSE2Int32(const IntType * __restrict data, IntType * __restrict result_data, const IColumn::Offsets & offsets)
    {
        const IntType * data_copy_begin_ptr = nullptr;
        size_t offsets_size = offsets.size();

        for (size_t offset_index = 0; offset_index < offsets_size; ++offset_index)
        {
            size_t span = offsets[offset_index] - offsets[offset_index - 1];
            if (span == 1)
            {
                if (!data_copy_begin_ptr)
                    data_copy_begin_ptr = data + offset_index;

                continue;
            }

            /// Copy data

            if (data_copy_begin_ptr)
            {
                size_t copy_size = (data + offset_index) - data_copy_begin_ptr;
                bool remainder = copy_size % 4;
                size_t sse_copy_counter = (copy_size / 4) + remainder;
                auto * result_data_copy = result_data;

                while (sse_copy_counter)
                {
                    __m128i copy_batch = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data_copy_begin_ptr));
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(result_data_copy), copy_batch);
                    result_data_copy += 4;
                    data_copy_begin_ptr += 4;
                    --sse_copy_counter;
                }

                result_data += copy_size;
                data_copy_begin_ptr = nullptr;
            }

            if (span == 0)
                continue;

            /// Copy single data element into result data

            bool span_remainder = span % 4;
            size_t copy_counter = (span / 4) + span_remainder;
            auto * result_data_tmp = result_data;
            __m128i copy_element_data = _mm_set1_epi32(data[offset_index]);

            while (copy_counter)
            {
                _mm_storeu_si128(reinterpret_cast<__m128i *>(result_data_tmp), copy_element_data);
                result_data_tmp += 4;
                --copy_counter;
            }

            result_data += span;
        }

        /// Copy tail if needed

        if (data_copy_begin_ptr)
        {
            size_t copy_size = (data + offsets_size) - data_copy_begin_ptr;
            bool remainder = copy_size % 4;
            size_t sse_copy_counter = (copy_size / 4) + remainder;

            while (sse_copy_counter)
            {
                __m128i copy_batch = _mm_loadu_si128(reinterpret_cast<const __m128i *>(data_copy_begin_ptr));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(result_data), copy_batch);
                result_data += 4;
                data_copy_begin_ptr += 4;
                --sse_copy_counter;
            }
        }
    }
}

#endif

template <typename T>
ColumnPtr ColumnVector<T>::replicate(const IColumn::Offsets & offsets) const
{
    const size_t size = data.size();
    if (size != offsets.size())
        throw Exception(ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH, "Size of offsets {} doesn't match size of column {}", offsets.size(), size);

    if (0 == size)
        return this->create();

    auto res = this->create(offsets.back());

#ifdef __SSE2__
    if constexpr (std::is_same_v<T, UInt32>)
    {
        replicateSSE2Int32(getData().data(), res->getData().data(), offsets);
        return res;
    }
#endif

    auto it = res->getData().begin(); // NOLINT
    for (size_t i = 0; i < size; ++i)
    {
        const auto span_end = res->getData().begin() + offsets[i]; // NOLINT
        for (; it != span_end; ++it)
            *it = data[i];
    }

    return res;
}

template <typename T>
void ColumnVector<T>::getExtremes(Field & min, Field & max) const
{
    size_t size = data.size();

    if (size == 0)
    {
        min = T(0);
        max = T(0);
        return;
    }

    bool has_value = false;

    /** Skip all NaNs in extremes calculation.
        * If all values are NaNs, then return NaN.
        * NOTE: There exist many different NaNs.
        * Different NaN could be returned: not bit-exact value as one of NaNs from column.
        */

    T cur_min = NaNOrZero<T>();
    T cur_max = NaNOrZero<T>();

    for (const T & x : data)
    {
        if (isNaN(x))
            continue;

        if (!has_value)
        {
            cur_min = x;
            cur_max = x;
            has_value = true;
            continue;
        }

        if (x < cur_min)
            cur_min = x;
        else if (x > cur_max)
            cur_max = x;
    }

    min = NearestFieldType<T>(cur_min);
    max = NearestFieldType<T>(cur_max);
}

template <typename T>
ColumnPtr ColumnVector<T>::compress(bool force_compression) const
{
    const size_t data_size = data.size();
    const size_t source_size = data_size * sizeof(T);

    /// Don't compress small blocks.
    if (source_size < 4096) /// A wild guess.
        return ColumnCompressed::wrap(this->getPtr());

    auto compressed = ColumnCompressed::compressBuffer(data.data(), source_size, force_compression);

    if (!compressed)
        return ColumnCompressed::wrap(this->getPtr());

    const size_t compressed_size = compressed->size();
    return ColumnCompressed::create(data_size, compressed_size,
        [my_compressed = std::move(compressed), column_size = data_size]
        {
            auto res = ColumnVector<T>::create(column_size);
            ColumnCompressed::decompressBuffer(
                my_compressed->data(), res->getData().data(), my_compressed->size(), column_size * sizeof(T));
            return res;
        });
}

template <typename T>
ColumnPtr ColumnVector<T>::createWithOffsets(const IColumn::Offsets & offsets, const ColumnConst & column_with_default_value, size_t total_rows, size_t shift) const
{
    if (offsets.size() + shift != size())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Incompatible sizes of offsets ({}), shift ({}) and size of column {}", offsets.size(), shift, size());

    auto res = this->create();
    auto & res_data = res->getData();

    T default_value = assert_cast<const ColumnVector<T> &>(column_with_default_value.getDataColumn()).getElement(0);
    res_data.resize_fill(total_rows, default_value);
    for (size_t i = 0; i < offsets.size(); ++i)
        res_data[offsets[i]] = data[i + shift];

    return res;
}

template <typename T>
void ColumnVector<T>::updateAt(const IColumn & src, size_t dst_pos, size_t src_pos)
{
    const auto & src_data = assert_cast<const Self &>(src).getData();
    data[dst_pos] = src_data[src_pos];
}

DECLARE_DEFAULT_CODE(
    template <typename Container, typename Type> void vectorIndexImpl(
    const Container & data, const PaddedPODArray<Type> & indexes, size_t limit, Container & res_data)
    {
        for (size_t i = 0; i < limit; ++i)
            res_data[i] = data[indexes[i]];
    }
);

DECLARE_AVX512VBMI_SPECIFIC_CODE(
    template <typename Container, typename Type>
    void vectorIndexImpl(const Container & data, const PaddedPODArray<Type> & indexes, size_t limit, Container & res_data)
    {
        static constexpr UInt64 MASK64 = 0xffffffffffffffff;
        const size_t limit64 = limit & ~63;
        size_t pos = 0;
        size_t data_size = data.size();

        auto data_pos = reinterpret_cast<const UInt8 *>(data.data());
        auto indexes_pos = reinterpret_cast<const UInt8 *>(indexes.data());
        auto res_pos = reinterpret_cast<UInt8 *>(res_data.data());

        if (limit == 0)
            return; /// nothing to do, just return

        if (data_size <= 64)
        {
            /// one single mask load for table size <= 64
            __mmask64 last_mask = MASK64 >> (64 - data_size);
            __m512i table1 = _mm512_maskz_loadu_epi8(last_mask, data_pos);

            /// 64 bytes table lookup using one single permutexvar_epi8
            while (pos < limit64)
            {
                __m512i vidx = _mm512_loadu_epi8(indexes_pos + pos);
                __m512i out = _mm512_permutexvar_epi8(vidx, table1);
                _mm512_storeu_epi8(res_pos + pos, out);
                pos += 64;
            }
            /// tail handling
            if (limit > limit64)
            {
                __mmask64 tail_mask = MASK64 >> (limit64 + 64 - limit);
                __m512i vidx = _mm512_maskz_loadu_epi8(tail_mask, indexes_pos + pos);
                __m512i out = _mm512_permutexvar_epi8(vidx, table1);
                _mm512_mask_storeu_epi8(res_pos + pos, tail_mask, out);
            }
        }
        else if (data_size <= 128)
        {
            /// table size (64, 128] requires 2 zmm load
            __mmask64 last_mask = MASK64 >> (128 - data_size);
            __m512i table1 = _mm512_loadu_epi8(data_pos);
            __m512i table2 = _mm512_maskz_loadu_epi8(last_mask, data_pos + 64);

            /// 128 bytes table lookup using one single permute2xvar_epi8
            while (pos < limit64)
            {
                __m512i vidx = _mm512_loadu_epi8(indexes_pos + pos);
                __m512i out = _mm512_permutex2var_epi8(table1, vidx, table2);
                _mm512_storeu_epi8(res_pos + pos, out);
                pos += 64;
            }
            if (limit > limit64)
            {
                __mmask64 tail_mask = MASK64 >> (limit64 + 64 - limit);
                __m512i vidx = _mm512_maskz_loadu_epi8(tail_mask, indexes_pos + pos);
                __m512i out = _mm512_permutex2var_epi8(table1, vidx, table2);
                _mm512_mask_storeu_epi8(res_pos + pos, tail_mask, out);
            }
        }
        else
        {
            if (data_size > 256)
            {
                /// byte index will not exceed 256 boundary.
                data_size = 256;
            }

            __m512i table1 = _mm512_loadu_epi8(data_pos);
            __m512i table2 = _mm512_loadu_epi8(data_pos + 64);
            __m512i table3;
            __m512i table4;
            if (data_size <= 192)
            {
                /// only 3 tables need to load if size <= 192
                __mmask64 last_mask = MASK64 >> (192 - data_size);
                table3 = _mm512_maskz_loadu_epi8(last_mask, data_pos + 128);
                table4 = _mm512_setzero_si512();
            }
            else
            {
                __mmask64 last_mask = MASK64 >> (256 - data_size);
                table3 = _mm512_loadu_epi8(data_pos + 128);
                table4 = _mm512_maskz_loadu_epi8(last_mask, data_pos + 192);
            }

            /// 256 bytes table lookup can use: 2 permute2xvar_epi8 plus 1 blender with MSB
            while (pos < limit64)
            {
                __m512i vidx = _mm512_loadu_epi8(indexes_pos + pos);
                __m512i tmp1 = _mm512_permutex2var_epi8(table1, vidx, table2);
                __m512i tmp2 = _mm512_permutex2var_epi8(table3, vidx, table4);
                __mmask64 msb = _mm512_movepi8_mask(vidx);
                __m512i out = _mm512_mask_blend_epi8(msb, tmp1, tmp2);
                _mm512_storeu_epi8(res_pos + pos, out);
                pos += 64;
            }
            if (limit > limit64)
            {
                __mmask64 tail_mask = MASK64 >> (limit64 + 64 - limit);
                __m512i vidx = _mm512_maskz_loadu_epi8(tail_mask, indexes_pos + pos);
                __m512i tmp1 = _mm512_permutex2var_epi8(table1, vidx, table2);
                __m512i tmp2 = _mm512_permutex2var_epi8(table3, vidx, table4);
                __mmask64 msb = _mm512_movepi8_mask(vidx);
                __m512i out = _mm512_mask_blend_epi8(msb, tmp1, tmp2);
                _mm512_mask_storeu_epi8(res_pos + pos, tail_mask, out);
            }
        }
    }
);

template <typename T>
template <typename Type>
ColumnPtr ColumnVector<T>::indexImpl(const PaddedPODArray<Type> & indexes, size_t limit) const
{
    chassert(limit <= indexes.size());

    auto res = this->create(limit);
    typename Self::Container & res_data = res->getData();
#if USE_MULTITARGET_CODE
    if constexpr (sizeof(T) == 1 && sizeof(Type) == 1)
    {
        /// VBMI optimization only applicable for (U)Int8 types
        if (isArchSupported(TargetArch::AVX512VBMI))
        {
            TargetSpecific::AVX512VBMI::vectorIndexImpl<Container, Type>(data, indexes, limit, res_data);
            return res;
        }
    }
#endif
    TargetSpecific::Default::vectorIndexImpl<Container, Type>(data, indexes, limit, res_data);

    return res;
}

/// Explicit template instantiations - to avoid code bloat in headers.
template class ColumnVector<UInt8>;
template class ColumnVector<UInt16>;
template class ColumnVector<UInt32>;
template class ColumnVector<UInt64>;
template class ColumnVector<UInt128>;
template class ColumnVector<UInt256>;
template class ColumnVector<Int8>;
template class ColumnVector<Int16>;
template class ColumnVector<Int32>;
template class ColumnVector<Int64>;
template class ColumnVector<Int128>;
template class ColumnVector<Int256>;
template class ColumnVector<BFloat16>;
template class ColumnVector<Float32>;
template class ColumnVector<Float64>;
template class ColumnVector<UUID>;
template class ColumnVector<IPv4>;
template class ColumnVector<IPv6>;

INSTANTIATE_INDEX_TEMPLATE_IMPL(ColumnVector)
/// Used by ColumnVariant.cpp
template ColumnPtr ColumnVector<UInt8>::indexImpl<UInt16>(const PaddedPODArray<UInt16> & indexes, size_t limit) const;
template ColumnPtr ColumnVector<UInt8>::indexImpl<UInt32>(const PaddedPODArray<UInt32> & indexes, size_t limit) const;
template ColumnPtr ColumnVector<UInt8>::indexImpl<UInt64>(const PaddedPODArray<UInt64> & indexes, size_t limit) const;
template ColumnPtr ColumnVector<UInt64>::indexImpl<UInt8>(const PaddedPODArray<UInt8> & indexes, size_t limit) const;
template ColumnPtr ColumnVector<UInt64>::indexImpl<UInt16>(const PaddedPODArray<UInt16> & indexes, size_t limit) const;
template ColumnPtr ColumnVector<UInt64>::indexImpl<UInt32>(const PaddedPODArray<UInt32> & indexes, size_t limit) const;

#if defined(OS_DARWIN)
template ColumnPtr ColumnVector<UInt8>::indexImpl<size_t>(const PaddedPODArray<size_t> & indexes, size_t limit) const;
template ColumnPtr ColumnVector<UInt64>::indexImpl<size_t>(const PaddedPODArray<size_t> & indexes, size_t limit) const;
#endif
}
