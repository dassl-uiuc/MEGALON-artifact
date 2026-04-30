#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

#include "common/expected.h"
#include "common/path.h"
#include "common/types.h"
#include "core/blockid.h"

namespace rackobj::lib {

// A file descriptor returned to the user application
typedef int virtual_fd_t;

class RackOBJFile {
public:
    /*
     * Create a file managed by the kernel space (i.e. not managed by RackOBJ).
     * This is used in the case that the file is not opened with O_RACKOBJ in the
     * flags argument to open(). We need to keep track of kernel-managed files
     * since we are hijacking the file descriptor space and allocating our own
     * file descriptors and need to differentiate between kernel file
     * descriptors and rackobj file descriptors in other libc functions.
     *
     * @param rfd the file descriptor created by the kernel
     */
    RackOBJFile(int rfd, RackOBJPath&& path) noexcept;

    RackOBJFile(RackOBJPath&& path, int rfd, uint64_t server_id, ino_t inode) noexcept;

    ~RackOBJFile();

    RackOBJFile& operator=(RackOBJFile&& other) = default;

    RackOBJFile(const RackOBJFile&) = delete;

    RackOBJFile& operator=(const RackOBJFile&) = delete;

    virtual_fd_t GetVirtualFileDescriptor() const { return vfd_; }

    constexpr bool IsKernelManaged() const noexcept { return fh_.index() == 0; }

    constexpr bool IsRackOBJFile() const noexcept { return fh_.index() == 1; }

    // constexpr bool IsRackOBJDirent() const noexcept { return fh_.index() == 2;
    // }

    constexpr const char* GetFileTypeString() const noexcept {
        if (IsKernelManaged()) {
            return "kernel managed";
        } else if (IsRackOBJFile()) {
            return "local disk";
        } else {
            return "directory";
        }
    }

    const RackOBJPath& GetPath() const { return path_; }

    // ssize_t Write(const void* buf, size_t count);
    ssize_t Write(const void* buf, size_t count);

    ssize_t Pwrite(const void* buf, size_t count, off_t offset);

    ssize_t Read(void* buf, uint64_t count);

    ssize_t Pread(void* buf, uint64_t count, off_t offset);

    // static size_t Get(void* buf, uint64_t count, off_t offset);

    // static size_t Put(const void* buf, uint64_t count, off_t offset);

    int Close();

    // int Fsync();

    long Tell() const;

    // struct dirent* GetDirent(size_t index);

    // void AdvanceDirent();

    // void RewindDirent();

    void Lockfile();

    void Unlockfile();

    int TryLockfile();

    int GetError() const;

    void SetError(int error_code);

    void ClearError() { SetError(0); }

private:
    struct KernelManagedFileHandle {
        int fd_;

        explicit KernelManagedFileHandle(int fd) noexcept : fd_(fd) {}
    };

    class FetchBlockFunctor {
    public:
        FetchBlockFunctor(int fd) noexcept : fs_interface_(fd) {}

        [[nodiscard]] expected<size_t, PosixError> operator()(const common::BlockId& block_id, void* write_location);

    private:
        int fs_interface_;
    };

    struct RackOBJFileHandle {
        // The ID of the server who's disk this file resides on
        uint64_t server_id_;

        // The inode of the file on disk
        ino_t inode_;

        // The current offset of the file descriptor
        off_t file_offset_;

        // File locking information to serialize process-local operations on
        // this file descriptor
        size_t lock_count_;
        std::shared_ptr<std::mutex> lock_count_mutex_;
        std::shared_ptr<std::condition_variable> lock_count_cv_;

        // A functor with which we can fetch blocks from the underlying storage,
        // whether it be a local or remote disk
        FetchBlockFunctor fetch_block_fn_;

        int error_code_;

        int fd_;

        RackOBJFileHandle(int fd, uint64_t server_id, ino_t inode) noexcept;

        bool Lockfile();

        void Unlockfile();

        size_t Pread(uint8_t* buf, uint64_t count, off_t offset);

        size_t Pwrite(const uint8_t* buf, uint64_t count, off_t offset);
    };

    std::variant<KernelManagedFileHandle, RackOBJFileHandle> fh_;

    virtual_fd_t vfd_;

    RackOBJPath path_;
};

}  // namespace rackobj::lib