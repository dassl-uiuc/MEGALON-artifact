#ifndef RACKOBJ_CONSTANTS_H
#define RACKOBJ_CONSTANTS_H

// evict_manager.cc
#define EVICT_MGR_SLEEP_INTERVAL_NS 500000000  // (500 ms)
#define EVICT_WATERMARK 90                     // 0-100

// flush_manager.cc
#define FLUSH_MGR_SLEEP_INTERVAL_NS 400000000
#define FLUSH_WATERMARK 70  // 0-100

// write_meta.cc
#define MAX_WMETA_SAMPLING_SIZE 10

// wmeta_manager.cc
#define WMETA_MGR_SLEEP_INTERVAL_NS 10000  // (10 us)
#define WMETA_WATERMARK 90                 // 0-100

// #ifdef DYN_WMETA
// #define WRITE_RATIO 15
// #else
// #define WRITE_RATIO 100  // 0-100 (ratio of cxl cache slots that can be RW shared)
// #endif                   /* DYN_WMETA */

// config.cc
#ifdef DYN_WMETA
#define DEFAULT_LOGICAL_SCR_SIZE 100 * 1024 * 1024UL  // scr size in MB
#else
#define DEFAULT_LOGICAL_SCR_SIZE 100000 * 1024 * 1024UL  // scr size in MB
#endif

// gcd_nr.h
#define LOGICAL_NODE_NUM 3  // this should match the rust library
#define NR_DEFAULT_NUM_ENTRIES 12000000

// local_work_allocator.h
#define REPL_WQ_SIZE 102400

// connection_pool.cc
#define HASH_SEED ((uint32_t)1234567890)

// globals.cc
#define FPS_SIZE 1024

// shm_obj_handle.h
#define INIT_LOCALSEQMAP_CAP 1024

// config.cc
#define DEFAULT_NCR_REGION_SIZE 56 * 1024 * 1024 * 1024UL    // 56GB
#define DEFAULT_SCR_REGION_SIZE 2 * 1024 * 1024 * 1024UL     // 2GB
#define DEFAULT_LOCAL_REGION_SIZE 20 * 1024 * 1024 * 1024UL  // 20GB

// helper.cc (multiple files)
#define CACHE_LINE_SIZE 64

#define NUM_NUMA 4  // number of NUMA nodes on the machine (including mem. node)

// gcd.h
#define NUMA_MEM 0
#define BUCKET_POW 10

#define RACKOBJ_CONFIG_ALLOWED_DIRTY_RATIO 100

// seqcount.h
#ifndef C3_RWLOCK
#define C3_RWLOCK 0
#endif

// seqcount.h
// MAX_SEQLOCK_VALUE <= 2^(32 - 2) and SEQLOCK_BITS <= 32, because the seqlock is a uint32_t
// Will not work for reader/write lock.
#define SEQLOCK_BITS 32

#ifdef FILE_INTERFACE
#define SLOT_SIZE 4096UL
#else
#define SLOT_SIZE 1024UL
#endif

#define CONSECUTIVE_ACCESS_THRESHOLD 5

#ifndef KEY_SIZE
#define KEY_SIZE 24
#endif

#define RANDSLOTALLOC 1  // reserve slot from random index in the array

// static double PARTITION_RATIO = 1.0;
// static size_t KEY_SPACE_STATIC = 0;

#endif  // RACKOBJ_CONSTANTS_H
