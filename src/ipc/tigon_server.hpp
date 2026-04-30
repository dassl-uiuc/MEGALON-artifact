#pragma once
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include "constants.h"
#include "globals.h"
#include "prelude.h"
#include "shared_memory/region.h"
#include "shm_obj_handle.h"

namespace rackobj::common {

class IpcServer {
    using IpcServerHandle = hostrpc::server<IpcBuffer, uint64_t, hostrpc::size_compiletime<kIpcSlots>>;

public:
    explicit IpcServer(uint64_t server_id);

    virtual ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;

    void Run(const uint16_t nthreads);

    void Shutdown();

protected:
    /* HandleRpc is called when a client sends an RPC to the server. The user
     * is responsible for deserializing the data and calling the appropriate
     * function. The request is placed in the buffer `payload`. The reply must
     * be serialized into the buffer `response`.
     */
    virtual void HandleRpc(uint64_t opcode, const uint64_t* payload, size_t payload_buffer_len, uint64_t* response,
                           size_t response_buffer_len) = 0;

private:
    /* Memory is laid out as follows (low address on the left, high address on
     * the right):
     *
     * `[ Shared Buffer | Client Inbox | Client Outbox | Client Locks ]`
     *
     * The server's inbox is the client's outbox, and vice versa. The shared
     * buffer is used for the actual data transfer. The client is responsible
     * for allocating and freeing the physical shared memory. Both are
     * responsible for managing the virtual memory mapping of the shared memory.
     *
     * The length of the shared buffer is defined by
     * `rackobj::common::kIpcSharedBufferSize`. The length of the client inbox,
     * outbox, and locks are defined in `rackobj::common::kIpcInboxSize`,
     * `rackobj::common::kIpcOutboxSize`, and `rackobj::common::kIpcLocksSize`,
     * respectively. The length of the entire shared memory region is defined in
     * `rackobj::common::kIpcShmemSize`.
     */
    AllocatableSharedMemoryRegion shm_region_;

    IpcServerHandle server_handle_;

    void* server_locks_;

    std::atomic_bool stop_flag_;
};

template <typename Policy>
class TigonIPCServer {
    using IpcServerHandle = hostrpc::server<IpcBuffer, uint64_t, hostrpc::size_compiletime<kIpcSlots>>;

public:
    TigonIPCServer(const TigonIPCServer&) = delete;
    TigonIPCServer& operator=(const TigonIPCServer&) = delete;
    TigonIPCServer(TigonIPCServer&&) = delete;
    TigonIPCServer& operator=(TigonIPCServer&&) = delete;

    TigonIPCServer(lib::SharedMemoryObjectHandle<Policy>* pg_cache_handle, int exec_node,
                   std::shared_ptr<AllocatableLocalMemoryRegion> cxl_scr_mem)
        : pg_cache(pg_cache_handle),
          shm_region_(cxl_scr_mem),
          logical_exec_node_(static_cast<size_t>(exec_node)),
          server_locks_(aligned_alloc(64, kIpcSlotsBytes)) {
        /**
         * The assumption here is that a shared memory region will be reserved for the buffer
         * of each server and that clients will be created with these same memory regions.
         */

        LOG(INFO) << "Starting IPC server on node " << logical_exec_node_;
        PCHECK(server_locks_ != nullptr) << "aligned_alloc(64, " << kIpcSlotsBytes << ") failed";
        std::memset(server_locks_, 0, kIpcSlotsBytes);

        shm_ptr_ = shm_region_->Allocate<MemoryAlignment::kPageAlign>(kIpcShmemSize);
        std::memset((void*)shm_ptr_, 0, kIpcShmemSize);

        CHECK(shm_ptr_ != nullptr) << "failed to allocate local memory region for ipc";

        uint8_t* client_inbox = shm_ptr_ + kIpcSharedBufferSize;
        uint8_t* client_outbox = client_inbox + kIpcInboxSize;

        server_handle_ =
            IpcServerHandle({}, hostrpc::careful_cast_to_bitmap<IpcServerHandle::lock_t>(server_locks_, kIpcSlotsWords),
                            hostrpc::careful_cast_to_bitmap<IpcServerHandle::inbox_t>(client_outbox, kIpcSlotsWords),
                            hostrpc::careful_cast_to_bitmap<IpcServerHandle::outbox_t>(client_inbox, kIpcSlotsWords),
                            hostrpc::careful_array_cast<IpcBuffer>(shm_ptr_, kIpcSlots));
    }

    ~TigonIPCServer() { free(server_locks_); }

    void Run() {
        thread_ = std::thread([this]() {
            int physical_numa = RidToNumaNode(static_cast<int>(logical_exec_node_));
            LOG(INFO) << "Running Server on logical node " << logical_exec_node_ << ", on physical node "
                      << physical_numa;
            pinThreadtoNumaNode(physical_numa);
            bool did_work = false;
            while (!stop_flag_.load()) {
                did_work = rpc_handle(
                    &server_handle_,
                    [&](uint32_t, IpcBuffer* data) {
                        HandleRpc(data->buffer[0],
                                  /*reinterpret_cast<uint8_t*>(*/ data->buffer + 1 /*)*/,
                                  sizeof(IpcBuffer) - sizeof(data->buffer[0]),
                                  /*reinterpret_cast<uint8_t*>(*/ data->buffer /*) */, sizeof(IpcBuffer));
                        return true;
                    },
                    [](uint32_t, IpcBuffer* data) {
                        std::memset(data->buffer, 0, sizeof(IpcBuffer));
                        return true;
                    });

                if (!did_work) {
                    platform::sleep_briefly();
                }
            }
        });
    }

    void Shutdown() {
        stop_flag_.store(true);
        thread_.join();
    }

    uint8_t* GetMemRegion() { return shm_ptr_; }

    int GetExecNode() { return logical_exec_node_; }

protected:
    /* HandleRpc is called when a client sends an RPC to the server. The user
     * is responsible for deserializing the data and calling the appropriate
     * function. The request is placed in the buffer `payload`. The reply must
     * be serialized into the buffer `response`.
     */
    void HandleRpc(uint64_t opcode, const uint64_t* payload, size_t payload_buffer_len, uint64_t* response,
                   size_t response_buffer_len) {
        (void)payload_buffer_len;
        switch (opcode) {
            case TigonIPCOp::RequestShareCreate:
                handleRequestShare((ino_t)payload[0], (off_t)payload[1], response, response_buffer_len);
                break;
            case TigonIPCOp::RequestShareGet:
                handleRequestShareGet((ino_t)payload[0], (off_t)payload[1], response, response_buffer_len);
                break;
            default:
                LOG(FATAL) << "unrecognized ipc opcode " << opcode << " at node " << logical_exec_node_;
        }
    }

private:
    void handleRequestShare(ino_t inode, off_t key, uint64_t* response, size_t response_buffer_len) {
        (void)response_buffer_len;
        // 3. Use some function to share the key
        BlockId block(logical_exec_node_, inode, key);
        // Move partitioned page to shared and get indices
        auto [new_wmeta_idx, cn_idx] = pg_cache->PartitionToShared(block, logical_exec_node_);
        //* 4. Mark IPC as completed in response buffer
        response[0] = new_wmeta_idx;
        response[1] = cn_idx;
    }

    void handleRequestShareGet(ino_t inode, off_t key, uint64_t* response, size_t response_buffer_len) {
        (void)response_buffer_len;
        // 3. Use some function to share the key
        BlockId block(logical_exec_node_, inode, key);
        // Move partitioned page to shared and get indices
        auto [new_wmeta_idx, cn_idx] = pg_cache->PartitionToSharedGet(block, logical_exec_node_);
        //* 4. Mark IPC as completed in response buffer
        response[0] = new_wmeta_idx;
        response[1] = cn_idx;
    }

    /* Memory is laid out as follows (low address on the left, high address on
     * the right):
     *
     * `[ Shared Buffer | Client Inbox | Client Outbox | Client Locks ]`
     *
     * The server's inbox is the client's outbox, and vice versa. The shared
     * buffer is used for the actual data transfer. The client is responsible
     * for allocating and freeing the physical shared memory. Both are
     * responsible for managing the virtual memory mapping of the shared memory.
     *
     * The length of the shared buffer is defined by
     * `rackobj::common::kIpcSharedBufferSize`. The length of the client inbox,
     * outbox, and locks are defined in `rackobj::common::kIpcInboxSize`,
     * `rackobj::common::kIpcOutboxSize`, and `rackobj::common::kIpcLocksSize`,
     * respectively. The length of the entire shared memory region is defined in
     * `rackobj::common::kIpcShmemSize`.
     */

    lib::SharedMemoryObjectHandle<Policy>* pg_cache;
    std::shared_ptr<AllocatableLocalMemoryRegion> shm_region_;
    uint8_t* shm_ptr_;

    size_t logical_exec_node_;  // logical exec node

    IpcServerHandle server_handle_;

    void* server_locks_;

    std::thread thread_;

    std::atomic_bool stop_flag_;
};

}  // namespace rackobj::common