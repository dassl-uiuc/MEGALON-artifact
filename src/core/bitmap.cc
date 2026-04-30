#include "bitmap.h"

namespace rackobj::common {

bool BaseBitmap::try_setbit_exclusive(uint64_t *bitmap_start_addr, size_t index, uint32_t offset) {
    uint64_t *target = &bitmap_start_addr[index];
#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
    uint64_t bitmap_entry = __atomic_load_n(target, __ATOMIC_RELAXED);

    bool already_set = bitmap_entry & BIT(offset);
    if (already_set) return false;

    uint64_t new_entry = bitmap_entry | BIT(offset);

#ifdef NO_COHERENCE
    cache_flush(reinterpret_cast<char *>(target), sizeof(uint64_t));
#endif
    return __atomic_compare_exchange_n(target, &bitmap_entry, new_entry, false /* weak */, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

}  // namespace rackobj::common