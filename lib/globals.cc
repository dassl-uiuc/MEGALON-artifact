#include "globals.h"

#include <rackobj.h>

#include <shared_mutex>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "common/constants.h"
#include "common/helper.h"
#include "original_syscalls.h"

using rackobj::RackOBJPath;
using rackobj::lib::RackOBJFile;
using rackobj::lib::virtual_fd_t;
using std::shared_lock;
using std::shared_mutex;
using std::shared_ptr;
using std::unique_lock;

static std::vector<shared_ptr<RackOBJFile>> fps((uint64_t)FPS_SIZE);

static bool slowdown_remote_memcpy = false;

namespace rackobj {

namespace lib {

const RackOBJConfig client_cfg;

SharedMemoryObjectHandle<CurrPolicy> page_cache(NUMA_MEM, client_cfg);

bool ShouldSlowdownRemoteMemoryMemcpy() { return slowdown_remote_memcpy; }

thread_local ThreadLocalMeta<CurrPolicy> thread_local_meta = ThreadLocalMeta<CurrPolicy>();

}  // namespace lib

void EnableRemoteMemcpySlowdown(bool enable) { slowdown_remote_memcpy = enable; }

uint64_t GetReadCount() { return lib::thread_local_meta.pt_stat.read_cnt; }

uint64_t GetReadRetryCount() { return lib::thread_local_meta.pt_stat.read_retry_cnt; }

uint64_t GetReadRetryInvoc() { return lib::thread_local_meta.pt_stat.read_retry_invocation; }

uint64_t GetAdmitCount() { return lib::thread_local_meta.pt_stat.admit_cnt; }

void ClearMovementCounter() { lib::page_cache.ClearReclaimCount(); };

ssize_t Put(const uint8_t* buf, uint64_t count, off_t key) { return lib::RackOBJKV::Put(buf, count, key); }

ssize_t Get(uint8_t* buf, uint64_t count, off_t key) { return lib::RackOBJKV::Get(buf, count, key); }

void Register(int tid) {
    int rid = rackobj::TidToRid(tid);

    int physical_numa_node = rackobj::RidToNumaNode(rid);
    rackobj::pinThreadtoNumaNode(physical_numa_node);

#ifdef NR
    RegisterNRThread(rid);
#endif

    lib::thread_local_meta.file_backed_ = false;
    lib::thread_local_meta.logical_node_id_ = rid;

    LOG(INFO) << "logical thread " << tid << " mapped to (" << rid << ", " << physical_numa_node << ")";
}

void UnRegister() {
#ifdef NR
    UnRegisterNRThread();
#endif
}

void SetWmetaWaterMark(size_t wmeta_water_mark) { lib::page_cache.SetWmetaWaterMark(wmeta_water_mark); }

void StartReplMgr() { lib::page_cache.StartReplMgr(); }

size_t GetConfigSize_t(std::string c_name) {
    if (c_name == "key_space") {
        return lib::client_cfg.GetKeySpace();
    } else if (c_name == "num_exec_numa") {
        return static_cast<size_t>(NUM_NUMA) - 1;
    } else if (c_name == "num_logical_node") {
        return LOGICAL_NODE_NUM;
    } else if (c_name == "logical_nid") {
        return static_cast<size_t>(lib::thread_local_meta.logical_node_id_);
    }

    LOG(FATAL) << "GetConfigSize_t: " << c_name << " unrecognized";
    return 0;
}

double GetPartitionRatio() { return lib::client_cfg.GetPartitionRatio(); }

}  // namespace rackobj

static struct InitializeIndicator {
    bool initialized;
    InitializeIndicator() { initialized = true; }
} init_d;

bool AreGlobalsInitialized() { return init_d.initialized && rackobj::lib::original_syscalls.IsInitialized(); }

shared_ptr<RackOBJFile> FindFilePointer(int fd) { return fps.at(static_cast<size_t>(fd)); }

bool DoesFilePointerExist(RackOBJFile* fp) {
    return fps.at(static_cast<size_t>(fp->GetVirtualFileDescriptor())) != nullptr;
}

void InsertFilePointer(shared_ptr<RackOBJFile> fp) { fps.at(static_cast<size_t>(fp->GetVirtualFileDescriptor())) = fp; }

void EraseFilePointer(RackOBJFile* fp) { fps.at(static_cast<size_t>(fp->GetVirtualFileDescriptor())) = nullptr; }

#ifdef NR
void RegisterNRThread(int rid) {
    rackobj::lib::thread_local_meta.nr_meta_per_thread_ = rackobj::lib::page_cache.RegisterNRThread(rid);
}

void UnRegisterNRThread() {
    rackobj::lib::page_cache.UnRegisterNRThread(rackobj::lib::thread_local_meta.nr_meta_per_thread_);
}
#endif

void RegisterLocalMem() {
    // LOG(FATAL) << "not updated to logical node";
    rackobj::lib::thread_local_meta.file_backed_ = true;
    rackobj::lib::thread_local_meta.local_cache_ptr_ =
        rackobj::lib::page_cache.RegisterLocalMem(rackobj::lib::thread_local_meta.logical_node_id_);
}