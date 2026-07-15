/**
 * Unit tests for TrySeqLock / ReleaseSeqLock / GetSeqCount FFI.
 *
 * Uses create_node_replicated (single-process, non-NUMA) with 1 replica so
 * the test runs on any machine without NUMA topology requirements.
 *
 * Each test prints PASS/FAIL and exits with code 1 on the first failure.
 */
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "nr_hashmap.hpp"

using namespace NrFfi;

#define ASSERT(cond, msg)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::cerr << "FAIL [" << __func__ << "]: " << (msg)       \
                      << " (line " << __LINE__ << ")\n";               \
            exit(1);                                                   \
        }                                                              \
    } while (0)

#define PASS() std::cout << "PASS [" << __func__ << "]\n"

/* ------------------------------------------------------------------ helpers */

static BlockId key(uint64_t id) { return BlockId(id); }

static GCDEntry empty_entry() {
    GCDEntry e;
    memset(&e, 0, sizeof(e));
    e.wmeta_idx_ = -1;
    for (int i = 0; i < NUM_NODE; ++i) {
        e.cn_array_[i].cn_ = -1;
        e.cn_array_[i].invalidate_ = false;
    }
    return e;
}

/* ------------------------------------------------------------------ tests */

static void test_nonexistent_key(NrMeta* meta) {
    BlockId k = key(0xDEAD);

    TrySeqLockResult r = TrySeqLock(meta, k);
    ASSERT(r.status_ == -1, "TrySeqLock on missing key should return -1");

    ssize_t rc = ReleaseSeqLock(meta, k);
    ASSERT(rc == -1, "ReleaseSeqLock on missing key should return -1");

    ssize_t sc = GetSeqCount(meta, k);
    ASSERT(sc == -1, "GetSeqCount on missing key should return -1");

    PASS();
}

static void test_acquire_release(NrMeta* meta) {
    BlockId k = key(1);
    GCDEntry e = empty_entry();
    e.cn_array_[0].cn_ = 42;

    Put(meta, k, e);

    /* first acquire */
    TrySeqLockResult r = TrySeqLock(meta, k);
    ASSERT(r.status_ == 1, "first TrySeqLock should succeed (status 1)");
    ASSERT(r.entry_.wmeta_.lock_ == true, "lock_ should be true after acquire");
    ASSERT(r.entry_.wmeta_.seqcount_ == 1, "seqcount should be 1 after first acquire");

    /* contended acquire while held */
    TrySeqLockResult r2 = TrySeqLock(meta, k);
    ASSERT(r2.status_ == 0, "second TrySeqLock should fail (status 0 = contended)");
    ASSERT(r2.entry_.wmeta_.lock_ == true, "lock_ still true");
    ASSERT(r2.entry_.wmeta_.seqcount_ == 1, "seqcount unchanged while contended");

    /* release */
    ssize_t sc = ReleaseSeqLock(meta, k);
    ASSERT(sc == 2, "seqcount should be 2 after release");

    /* GetSeqCount reads the committed value */
    ssize_t sc2 = GetSeqCount(meta, k);
    ASSERT(sc2 == 2, "GetSeqCount should return 2");

    PASS();
}

static void test_reacquire_after_release(NrMeta* meta) {
    BlockId k = key(2);
    GCDEntry e = empty_entry();
    e.cn_array_[0].cn_ = 7;

    Put(meta, k, e);

    /* cycle 1 */
    TrySeqLockResult r = TrySeqLock(meta, k);
    ASSERT(r.status_ == 1 && r.entry_.wmeta_.seqcount_ == 1,
           "cycle1 acquire: seqcount=1");
    ssize_t sc = ReleaseSeqLock(meta, k);
    ASSERT(sc == 2, "cycle1 release: seqcount=2");

    /* cycle 2 */
    r = TrySeqLock(meta, k);
    ASSERT(r.status_ == 1 && r.entry_.wmeta_.seqcount_ == 3,
           "cycle2 acquire: seqcount=3");
    sc = ReleaseSeqLock(meta, k);
    ASSERT(sc == 4, "cycle2 release: seqcount=4");

    ssize_t sc2 = GetSeqCount(meta, k);
    ASSERT(sc2 == 4, "GetSeqCount after two cycles: 4");

    PASS();
}

static void test_seqcount_reflects_lock_state(NrMeta* meta) {
    BlockId k = key(3);
    GCDEntry e = empty_entry();
    e.cn_array_[0].cn_ = 99;

    Put(meta, k, e);

    ssize_t before = GetSeqCount(meta, k);
    ASSERT(before == 0, "initial seqcount is 0");

    TrySeqLock(meta, k);
    ssize_t mid = GetSeqCount(meta, k);
    ASSERT(mid == 1, "seqcount is 1 while locked (odd = writer active)");

    ReleaseSeqLock(meta, k);
    ssize_t after = GetSeqCount(meta, k);
    ASSERT(after == 2, "seqcount is 2 after release (even = unlocked)");

    PASS();
}

/* ------------------------------------------------------------------ main */

int main() {
    constexpr uint64_t CAP = 1024;
    constexpr uint64_t N_REPLICAS = 1;

    NrRapper* nrht = create_node_replicated(CAP, N_REPLICAS);
    if (!nrht) {
        std::cerr << "create_node_replicated failed\n";
        return 1;
    }

    NrMeta* meta = register_node_replicated(nrht, 0);
    if (!meta) {
        std::cerr << "register_node_replicated failed\n";
        return 1;
    }

    test_nonexistent_key(meta);
    test_acquire_release(meta);
    test_reacquire_after_release(meta);
    test_seqcount_reflects_lock_state(meta);

    unregister_node_replicated(meta);
    free_node_replicated(nrht);

    std::cout << "All seqlock tests passed.\n";
    return 0;
}
