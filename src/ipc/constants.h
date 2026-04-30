#pragma once

#include <string>

namespace rackobj::common {

struct IpcBuffer {
    alignas(4096) uint64_t buffer[512];
};
static_assert(sizeof(IpcBuffer) == 4096, "");

static constexpr size_t kIpcSlots = 512;  // number of slots, usually in bits
static constexpr size_t kIpcSlotsBytes = kIpcSlots / 8;
static constexpr size_t kIpcSlotsWords = kIpcSlotsBytes / sizeof(uint64_t);

static constexpr size_t kIpcSharedBufferSize = kIpcSlots * sizeof(IpcBuffer);
static constexpr size_t kIpcInboxSize = kIpcSlotsBytes;
static constexpr size_t kIpcOutboxSize = kIpcSlotsBytes;
static constexpr size_t kIpcLocksSize = kIpcSlotsBytes;
static constexpr size_t kIpcShmemSize = kIpcSharedBufferSize + kIpcInboxSize + kIpcOutboxSize + kIpcLocksSize;

static_assert(kIpcInboxSize % 64 == 0, "IPC inbox must be a multiple of the cache line size");
static_assert(kIpcOutboxSize % 64 == 0, "IPC outbox must be a multiple of the cache line size");
static_assert(kIpcLocksSize % 64 == 0, "IPC client locks array must be a multiple of the cache line size");

/**
 * The 0th byte in the IPC message is reserved for the RPC opcode, so count it
 * out of the maximum payload size
 */
static constexpr size_t kMaxIpcPayloadSize = sizeof(IpcBuffer) - sizeof(uint8_t);

// inline std::string GetIpcShmemPath(uint64_t server_id) { return "/rackfs_ipc_" + std::to_string(server_id); }

enum TigonIPCOp {
    RequestShareCreate,
    RequestShareGet,
};

}  // namespace rackobj::common