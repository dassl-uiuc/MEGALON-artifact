#ifndef RACKOBJ_CACHE_NODE_HANDLE_H
#define RACKOBJ_CACHE_NODE_HANDLE_H

#include <iostream>

#include "cc_primitive.h"
#include "common/expected.h"
#include "common/helper.h"
#include "index/gcd.h"
#include "index/gcd_nr.h"
#include "index/lcd.h"
#include "manager/wmeta_manager.h"
#include "object_slot.h"
#include "scr_bitmap.h"
#include "write_meta.h"

namespace rackobj::common {

enum class ReadErrno {
    NO_ERROR,
    PAGE_NOT_FOUND,
    META_OUTDATE,
    PAGE_INCOHERENT,
};

enum class ReadLocation {
    INVALID,
    NO_ENTRY,
    CXL_SHARED,
    LOCAL_NODE,
    REMOTE_NODE,
};

enum class WriteErrno {
    NO_ERROR,
    PAGE_NOT_FOUND,
    META_OUTDATE,
    PAGE_RO,
    MULTIPLE_REPLICA,
    MULTIPLE_REPLICA_NOT_ON_CXL,
    PAGE_ON_REMOTE_NODE,
};

std::ostream &operator<<(std::ostream &os, WriteErrno e);

struct ReadHandle {
    int64_t seqcount;
    std::optional<common::GCDEntry> entry_optional;
    std::optional<std::reference_wrapper<common::LCDEntry>> lmeta;
    common::BlockId key;
    std::optional<size_t> cn_index;
    int current_nid;
    int remote_nid;
    bool from_cxl = false;
    ReadErrno rh_errno;
    bool is_local_cpu_cached = false;
};

struct WriteHandle {
    std::optional<common::GCDEntry> entry_optional;
    std::optional<std::reference_wrapper<common::LCDEntry>> lmeta;
    common::BlockId key;
    std::optional<size_t> cn_index;
    int current_nid;
    bool from_cxl = false;
    WriteErrno wh_errno;
};

/* Consistent Coherent CXL Page nOtification Layer*/

class C3PO {
    using GcdHandle = common::GlobalCacheDirectoryHandle;

    friend class C3POHandle;
    friend class WriteMetadataManager;

public:
    C3PO(const size_t num_entries, void *map_address, const int node, const uint8_t *base_addr,
         const std::shared_ptr<common::AllocatableLocalMemoryRegion> &sc_shm_region, const size_t logical_scr_size);
    ~C3PO() = default;

    [[nodiscard]] bool WLock(size_t wm_index) { return scr_meta_->GetWmeta(wm_index)->WSeqBegin(); }

    [[nodiscard]] unsigned int WUnlock(size_t wm_index) { return scr_meta_->GetWmeta(wm_index)->WSeqEnd(); }

private:
    std::unique_ptr<GcdHandle> gcd_;

    std::unique_ptr<PartitionMetadata> scr_meta_;  // resides on SCR

    SharedBitmap *scr_bitmap_ptr_;

    size_t logical_scr_size_;
};

class WriteMetadataManagerHandle;

class C3POHandle {
    friend class WriteMetadataManager;

public:
    static std::unique_ptr<C3POHandle> CreateOrMap(
        size_t num_entries, void *map_address, int cxl_nid, const uint8_t *base_addr,
        const std::shared_ptr<common::AllocatableLocalMemoryRegion> &sc_shm_region, const size_t logical_scr_size);

    explicit C3POHandle(C3PO *c3po, int cxl_nid) noexcept;
    ~C3POHandle() = default;

    void InitSharedMetadata(size_t num_slots,
                            const std::shared_ptr<common::AllocatableLocalMemoryRegion> &sc_shm_region,
                            SharedBitmap *bitmap);

    void CreateLocalSeqMap(int nid, const std::shared_ptr<common::AllocatableLocalMemoryRegion> &local_region);

    void SetSharedPageCache(const std::shared_ptr<common::BasePageCache> &pcache_p);

    C3PO::GcdHandle *Gcd() { return c3po_->gcd_.get(); }

    PartitionMetadata *Scr_meta() { return c3po_->scr_meta_.get(); }

    static inline bool ExistOnArrayIdx(const std::optional<common::GCDEntry> &entry_optional, int idx) {
        return entry_optional.has_value() && entry_optional->cn_array_[idx].cn_idx_.has_value() &&
               !entry_optional->cn_array_[idx].invalidate_;
    }

    bool CheckCoherence(const common::BlockId &block_id, uint32_t nid
#ifdef NR
                        ,
                        const NrFfi::NrMeta *nr_meta
#endif
    );

    bool CheckCoherence(const std::optional<size_t> &cn_index, uint32_t nid
#ifdef NR
                        ,
                        const NrFfi::NrMeta *nr_meta
#endif
    );

    bool CheckNotification(
#ifdef NR
        const NrFfi::NrMeta *nr_meta
#endif
    );

    bool CheckNotificationWrite(
#ifdef NR
        const NrFfi::NrMeta *nr_meta
#endif
    );

    expected<std::optional<ssize_t>, NrGcdDeleteError> Delete(const BlockId &to_remove
#ifdef NR
                                                              ,
                                                              const NrFfi::NrMeta *nr_meta
#endif
    );

    static inline bool IsEntryEmpty(const std::optional<common::GCDEntry> &entry_optional) {
        if (!entry_optional.has_value()) return true;
        for (int i = 0; i < LOGICAL_NODE_NUM + 1; i++) {
            if (ExistOnArrayIdx(entry_optional, i)) return false;
        }
        return true;
    }

    static inline bool IsRO(const std::optional<common::GCDEntry> &entry_optional) {
        return entry_optional.has_value() && !entry_optional->wmeta_idx_.has_value();
    }

    std::optional<size_t> CacheNodeIndexOnNode(const std::optional<common::GCDEntry> &entry_optional, int nid);

    std::optional<size_t> CacheNodeIndexOnCxl(const std::optional<common::GCDEntry> &entry_optional);

    bool read_seq_start(ReadHandle &rh);

    bool read_seq_end(ReadHandle &rh);

    /** flush sequence is a vairant of read sequence */
    bool flush_seq_start(ReadHandle &rh);

    bool flush_seq_end(ReadHandle &rh);

    bool write_seq_start(WriteHandle &wh
#ifdef NR
                         ,
                         const NrFfi::NrMeta *nr_meta
#endif
    );

    void write_seq_end(WriteHandle &wh);

    [[nodiscard]] bool WLock(size_t wm_idx) { return c3po_->WLock(wm_idx); }

    [[nodiscard]] unsigned int WUnlock(size_t wm_idx) { return c3po_->WUnlock(wm_idx); }

    size_t WmetaOverThreshold() { return c3po_->scr_meta_->WmetaOverThreshold(); }

    void SetWmetaWaterMark(size_t watermark) {
        size_t threshold = c3po_->scr_meta_->GetWmetaSlotLen() * watermark / 100;
        c3po_->scr_meta_->SetWmetaThreshold(threshold);
    }

    WriteMetadata *GetWmeta(size_t wm_idx) { return c3po_->scr_meta_->GetWmeta(wm_idx); }

    inline void cache_flush(char *addr, size_t size) { rackobj::cache_flush(addr, size); }

    inline void write_flush(char *addr, size_t size) { rackobj::write_flush(addr, size); }

    void StartManager();

    void StopManager();

private:
    C3PO *c3po_;
    int cxl_nid_;

    std::unique_ptr<common::WriteMetadataManagerHandle> wmeta_mgr_;
};

static inline void set_read_handle_errno(struct ReadHandle &rh, ReadErrno rh_errno) { rh.rh_errno = rh_errno; }

static inline void set_write_handle_errno(struct WriteHandle &wh, WriteErrno wh_errno) { wh.wh_errno = wh_errno; }

static inline void init_read_handle(struct ReadHandle &rh, const common::BlockId &key, uint8_t logical_node) {
    memset(&rh, 0, sizeof(rh));
    rh.seqcount = -1;
    rh.key = key;
    rh.current_nid = static_cast<int>(logical_node);
    rh.remote_nid = -1;
    rh.from_cxl = true;
    rh.rh_errno = ReadErrno::NO_ERROR;
}

static inline void update_read_handle(struct ReadHandle &rh, const common::BlockId &key,
                                      const std::optional<common::GCDEntry> &entry_optional) {
    rh.key = key;
    rh.entry_optional = entry_optional;
    rh.from_cxl = true;
}

static inline void update_read_handle(struct ReadHandle &rh, const common::BlockId &key, common::LCDEntry &lmeta) {
    rh.lmeta = std::ref(lmeta);
    rh.key = key;
    rh.from_cxl = false;
}

static inline void init_write_handle(struct WriteHandle &wh, const common::BlockId &key, uint8_t logical_node) {
    memset(&wh, 0, sizeof(wh));
    wh.key = key;
    wh.current_nid = static_cast<int>(logical_node);
    wh.from_cxl = true;
    wh.wh_errno = WriteErrno::NO_ERROR;
}

static inline void update_write_handle(struct WriteHandle &wh, const common::BlockId &key,
                                       const std::optional<common::GCDEntry> &entry_optional) {
    wh.key = key;
    wh.entry_optional = entry_optional;
    wh.from_cxl = true;
}

static inline void update_write_handle(struct WriteHandle &wh, const common::BlockId &key, common::LCDEntry &lmeta) {
    wh.key = key;
    wh.lmeta = std::ref(lmeta);
    wh.from_cxl = false;
}

// C3PO API

}  // namespace rackobj::common

#endif  // RACKOBJ_CACHE_NODE_HANDLE_H