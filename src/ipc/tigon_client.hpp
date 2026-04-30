#pragma once

#include "absl/status/status.h"
#include "constants.h"
#include "prelude.h"
#include "shared_memory/region.h"

namespace rackobj::common {

class IpcClient {
    using IpcClientHandle = hostrpc::client<IpcBuffer, uint64_t, hostrpc::size_compiletime<kIpcSlots>>;

public:
    explicit IpcClient(uint64_t server_id);

    ~IpcClient() = default;

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&) = delete;
    IpcClient& operator=(IpcClient&&) = delete;

protected:
    template <typename T>
    absl::Status Call(uint64_t opcode, std::function<void(T*)> serializer, std::function<void(const T*)> deserializer);

private:
    SharedMemoryRegion shm_region_;

    IpcClientHandle handle_;
};

class TigonIPCClient {
    using IpcClientHandle = hostrpc::client<IpcBuffer, uint64_t, hostrpc::size_compiletime<kIpcSlots>>;

public:
    TigonIPCClient(void* server_buf) : shm_region_(server_buf, kIpcShmemSize, false) {
        uint8_t* shm_ptr = static_cast<uint8_t*>(shm_region_.GetRegion());
        uint8_t* client_inbox = shm_ptr + kIpcSharedBufferSize;
        uint8_t* client_outbox = client_inbox + kIpcInboxSize;
        uint8_t* client_locks = client_outbox + kIpcOutboxSize;

        handle_ =
            IpcClientHandle({}, hostrpc::careful_cast_to_bitmap<IpcClientHandle::lock_t>(client_locks, kIpcSlotsWords),
                            hostrpc::careful_cast_to_bitmap<IpcClientHandle::inbox_t>(client_inbox, kIpcSlotsWords),
                            hostrpc::careful_cast_to_bitmap<IpcClientHandle::outbox_t>(client_outbox, kIpcSlotsWords),
                            hostrpc::careful_array_cast<IpcBuffer>(shm_ptr, kIpcSlots));
    }

    ~TigonIPCClient() = default;

    TigonIPCClient(const TigonIPCClient&) = delete;
    TigonIPCClient& operator=(const TigonIPCClient&) = delete;
    TigonIPCClient(TigonIPCClient&&) = delete;
    TigonIPCClient& operator=(TigonIPCClient&&) = delete;

    // protected:
    template <typename T>
    absl::Status Call(uint64_t opcode, std::function<void(T*)> serializer, std::function<void(const T*)> deserializer) {
        static_assert(std::is_arithmetic<T>::value, "Not an arithmetic type");
        auto active_threads = platform::active_threads();
        auto maybe_port = handle_.rpc_try_open_typed_port(active_threads);
        if (!maybe_port) {
            return absl::Status(absl::StatusCode::kUnavailable, "Failed to open a port");
        }

        auto send_result =
            handle_.rpc_port_send(active_threads, maybe_port.value(), [&serializer, opcode](uint32_t, IpcBuffer* data) {
                data->buffer[0] = opcode;
                serializer(reinterpret_cast<T*>(data->buffer + 1));
            });

        auto recvd = handle_.rpc_port_wait(active_threads, hostrpc::cxx::move(send_result),
                                           [&deserializer](uint32_t, const IpcBuffer* data) {
                                               deserializer(reinterpret_cast<const T*>(data->buffer));
                                           });

        handle_.rpc_close_port(active_threads, hostrpc::cxx::move(recvd));
        return absl::OkStatus();
    }

private:
    SharedMemoryRegion shm_region_;

    IpcClientHandle handle_;
};
}  // namespace rackobj::common
