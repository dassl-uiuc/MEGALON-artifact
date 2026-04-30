#ifndef RACKOBJ_BITMAP_H
#define RACKOBJ_BITMAP_H

#include <bitset>
#include <cstring>

#include "arch/arch.h"
#include "common/helper.h"

namespace rackobj::common {

#define BITS_PER_BYTE 8
#define __KERNEL_DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))
#define BITS_PER_TYPE(type) (sizeof(type) * BITS_PER_BYTE)
#define BITS_TO_LONGS(nr) __KERNEL_DIV_ROUND_UP(nr, BITS_PER_TYPE(uint64_t))

#define BITMAP_SHIFT 6
#define BITMAP_MASK ((1UL << BITMAP_SHIFT) - 1)
#define BIT(nr) (1UL << (nr))

class BaseBitmap {
protected:
    constexpr size_t bitmap_index(size_t page_index) const { return page_index >> BITMAP_SHIFT; }

    constexpr uint32_t bitmap_offset(size_t page_index) const { return page_index & BITMAP_MASK; }

    static bool inline load_bit(uint64_t *bitmap_start_addr, size_t index, uint32_t offset) {
        uint64_t *target = &bitmap_start_addr[index];
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        uint64_t bitmap_entry = __atomic_load_n(target, __ATOMIC_ACQUIRE);
        return bitmap_entry & BIT(offset);
    }

    /**
     *  Try to set bit at @offset in @bitmap_start_addr[@index]
     *  Return &true if the bit is set, &false if CAS fails.
     */
    static bool inline try_setbit(uint64_t *bitmap_start_addr, size_t index, uint32_t offset) {
        uint64_t *target = &bitmap_start_addr[index];
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        uint64_t bitmap_entry = __atomic_load_n(target, __ATOMIC_RELAXED);

        bool already_set = bitmap_entry & BIT(offset);
        if (already_set) return true;

        uint64_t new_entry = bitmap_entry | BIT(offset);

#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        return __atomic_compare_exchange_n(target, &bitmap_entry, new_entry, false /* weak */, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    }

    static bool inline try_setbit_relaxed(uint64_t *bitmap_start_addr, size_t index, uint32_t offset) {
        uint64_t *target = &bitmap_start_addr[index];
        uint64_t bitmap_entry = __atomic_load_n(target, __ATOMIC_RELAXED);

        bool already_set = bitmap_entry & BIT(offset);
        if (already_set) return true;

        uint64_t new_entry = bitmap_entry | BIT(offset);

        return __atomic_compare_exchange_n(target, &bitmap_entry, new_entry, false /* weak */, __ATOMIC_RELAXED,
                                           __ATOMIC_RELAXED);
    }

    /**
     *  Try to set bit at @offset in @bitmap_start_addr[@index]
     *  Return &true if the bit is set by myself, &false if the bit is already set by others or CAS fails.
     */
    static bool try_setbit_exclusive(uint64_t *bitmap_start_addr, size_t index, uint32_t offset);

    /**
     *  Try to clear bit at @offset in @bitmap_start_addr[@index]
     *  Return &true if the bit is cleared, &false if CAS fails.
     */
    static bool inline try_clearbit(uint64_t *bitmap_start_addr, size_t index, uint32_t offset) {
        uint64_t *target = &bitmap_start_addr[index];
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        uint64_t bitmap_entry = __atomic_load_n(target, __ATOMIC_RELAXED);

        bool is_set = bitmap_entry & BIT(offset);
        if (!is_set) return true;

        uint64_t new_entry = bitmap_entry & ~BIT(offset);

#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        return __atomic_compare_exchange_n(target, &bitmap_entry, new_entry, false /* weak */, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    }

    /**
     *  Try to clear bit at @offset in @bitmap_start_addr[@index]
     *  Return &true if the bit is cleared by myself, &false if the bit is already cleared by others or CAS fails.
     */
    static bool inline try_clearbit_exclusive(uint64_t *bitmap_start_addr, size_t index, uint32_t offset) {
        uint64_t *target = &bitmap_start_addr[index];
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        uint64_t bitmap_entry = __atomic_load_n(target, __ATOMIC_RELAXED);

        bool is_set = bitmap_entry & BIT(offset);
        if (!is_set) return false;

        uint64_t new_entry = bitmap_entry & ~BIT(offset);

#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        return __atomic_compare_exchange_n(target, &bitmap_entry, new_entry, false /* weak */, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    }

    static bool inline try_clearbit_exclusive_relaxed(uint64_t *bitmap_start_addr, size_t index, uint32_t offset) {
        uint64_t *target = &bitmap_start_addr[index];
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        uint64_t bitmap_entry = __atomic_load_n(target, __ATOMIC_RELAXED);

        bool is_set = bitmap_entry & BIT(offset);
        if (!is_set) return false;

        uint64_t new_entry = bitmap_entry & ~BIT(offset);

#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
        return __atomic_compare_exchange_n(target, &bitmap_entry, new_entry, false /* weak */, __ATOMIC_RELAXED,
                                           __ATOMIC_RELAXED);
    }

    constexpr void bitmap_setbit(uint64_t *bitmap_start_addr, size_t cn_index) {
        size_t index = bitmap_index(cn_index);
        uint32_t offset = bitmap_offset(cn_index);

        while (!try_setbit(bitmap_start_addr, index, offset)) {
            cpu_relax();
        }
    }

    constexpr void bitmap_setbit_exclusive(uint64_t *bitmap_start_addr, size_t cn_index) {
        size_t index = bitmap_index(cn_index);
        uint32_t offset = bitmap_offset(cn_index);

        while (!try_setbit_exclusive(bitmap_start_addr, index, offset)) {
            cpu_relax();
        }
    }

    constexpr void bitmap_clearbit(uint64_t *bitmap_start_addr, size_t cn_index) {
        size_t index = bitmap_index(cn_index);
        uint32_t offset = bitmap_offset(cn_index);

        while (!try_clearbit(bitmap_start_addr, index, offset)) {
            cpu_relax();
        }
    }

    bool inline bitmap_testbit(uint64_t *bitmap_start_addr, size_t cn_index) const {
        size_t index = bitmap_index(cn_index);
        uint32_t offset = bitmap_offset(cn_index);
#ifdef NO_COHERENCE
        cache_flush(reinterpret_cast<char *>(&bitmap_start_addr[index]), sizeof(uint64_t));
#endif

        uint64_t bitmap_entry = __atomic_load_n(&bitmap_start_addr[index], __ATOMIC_SEQ_CST);

        return bitmap_entry & (1 << offset);
    }

    bool inline bitmap_find_setbit(uint64_t *bitmap_start_addr, size_t bitmap_len, size_t &index, uint32_t &offset,
                                   size_t start_line = 0) const {
        auto random_start_line = std::rand() % bitmap_len;
        for (size_t i = 0; i < bitmap_len; i++) {
            size_t row = (i + random_start_line) % bitmap_len;
#ifdef NO_COHERENCE
            cache_flush(reinterpret_cast<char *>(&bitmap_start_addr[row]), sizeof(uint64_t));
#endif
            uint64_t bitmap_entry = __atomic_load_n(&bitmap_start_addr[row], __ATOMIC_SEQ_CST);
            if (bitmap_entry != 0) {
                offset = static_cast<uint32_t>(ffsll(static_cast<int64_t>(bitmap_entry)) - 1);
                index = row;
                return true;
            }
        }
        return false;
    }

    bool inline bitmap_find_setbit_relexed(uint64_t *bitmap_start_addr, size_t bitmap_len, size_t &index,
                                           uint32_t &offset, size_t start_line = 0) const {
        for (size_t i = 0; i < bitmap_len; i++) {
            size_t row = (i + start_line) % bitmap_len;
#ifdef NO_COHERENCE
            cache_flush(reinterpret_cast<char *>(&bitmap_start_addr[row]), sizeof(uint64_t));
#endif
            uint64_t bitmap_entry = __atomic_load_n(&bitmap_start_addr[row], __ATOMIC_RELAXED);
            if (bitmap_entry != 0) {
                offset = static_cast<uint32_t>(ffsll(static_cast<int64_t>(bitmap_entry)) - 1);
                index = row;
                return true;
            }
        }
        return false;
    }
};

}  // namespace rackobj::common

#endif  // RACKOBJ_BITMAP_H