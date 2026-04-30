#include "file.h"

#include <sys/types.h>
#include <unistd.h>

#include <mutex>
#include <queue>
#include <string>

#include "absl/log/log.h"
#include "globals.h"
#include "memcpy.h"
#include "original_syscalls.h"

// How we keep track of available file descriptors
static int fd_incr = 4;
static std::priority_queue<int, std::vector<int>, std::greater<int>> used_fds;
static std::mutex used_fds_mu;

namespace rackobj::lib {

using rackobj::common::BlockId;
using std::make_shared;
using std::shared_ptr;
using std::string;

static virtual_fd_t AllocateVirtualDescriptor() {
    std::lock_guard<std::mutex> guard(used_fds_mu);
    if (!used_fds.empty()) {
        int fd = used_fds.top();
        used_fds.pop();
        return fd;
    } else {
        int fd = fd_incr;
        ++fd_incr;
        return fd;
    }
}

static size_t GetThreadId() {
#ifdef __APPLE__
    ino_t tid;
    pthread_threadid_np(nullptr, &tid);
    return static_cast<size_t>(tid);
#else
    pid_t tid = gettid();
    return static_cast<size_t>(tid);
#endif
}

template <typename T>
static constexpr T round_down_to_block_size(T val) {
    return val - (val % static_cast<T>(common::BlockId::kBlockSize));
}

// static_assert(round_down_to_block_size(0) == 0);
// static_assert(round_down_to_block_size(1) == 0);
// static_assert(round_down_to_block_size(4095) == 0);
// static_assert(round_down_to_block_size(4096) == 4096);
// static_assert(round_down_to_block_size(4097) == 4096);
// static_assert(round_down_to_block_size(8191) == 4096);
// static_assert(round_down_to_block_size(8192) == 8192);

RackOBJFile::RackOBJFileHandle::RackOBJFileHandle(int fd, uint64_t server_id, ino_t inode) noexcept
    : server_id_(server_id),
      inode_(inode),
      file_offset_(0),
      lock_count_(0),
      lock_count_mutex_(make_shared<std::mutex>()),
      lock_count_cv_(make_shared<std::condition_variable>()),
      fetch_block_fn_(fd),
      error_code_(0),
      fd_(fd) {}

bool RackOBJFile::RackOBJFileHandle::Lockfile() {
    std::unique_lock<std::mutex> lock(*lock_count_mutex_);
    lock_count_cv_->wait(lock, [this] { return lock_count_ == 0 || lock_count_ == GetThreadId(); });

    if (lock_count_ == 0) {
        lock_count_ = GetThreadId();
        return true;
    }

    return false;
}

void RackOBJFile::RackOBJFileHandle::Unlockfile() {
    std::unique_lock<std::mutex> lock(*lock_count_mutex_);
    lock_count_ = 0;
    lock_count_cv_->notify_one();
}

size_t RackOBJFile::RackOBJFileHandle::Pread(uint8_t* buf, uint64_t count, off_t offset) {
    BlockId block_id(static_cast<uint64_t>(fd_), inode_, round_down_to_block_size(offset));
    auto offset_into_page = static_cast<size_t>(offset - block_id.GetOffset());
    size_t bytes_copied = 0;

    while (bytes_copied < count) {
        DLOG(INFO) << "reading from block " << block_id << " at offset " << offset_into_page << " with "
                   << (count - bytes_copied) << " bytes remaining";

        size_t available = std::min(count - bytes_copied, common::BlockId::kBlockSize - offset_into_page);

        size_t bytes_read =
            page_cache.Read(block_id, buf + bytes_copied, available, offset_into_page, &thread_local_meta);

        bytes_copied += bytes_read;
        if (bytes_read < available) {
            DLOG(INFO) << "Read all bytes available in page; stop (rc=" << bytes_copied << ")";
            break;
        }

        block_id.IncrementPage();
        offset_into_page = 0;
    }

    DLOG(INFO) << "bytes_copied = " << bytes_copied;
    return bytes_copied;
}

size_t RackOBJFile::RackOBJFileHandle::Pwrite(const uint8_t* buf, uint64_t count, off_t offset) {
    // TODO: take care of append (write beyong current file size)
    BlockId block_id(static_cast<uint64_t>(fd_), inode_, round_down_to_block_size(offset));
    auto offset_into_page = static_cast<size_t>(offset - block_id.GetOffset());
    size_t bytes_copied = 0;

    while (bytes_copied < count) {
        DLOG(INFO) << "writing to block " << block_id << " at offset " << offset_into_page << " with "
                   << (count - bytes_copied) << " bytes remaining";

        size_t available = std::min(count - bytes_copied, common::BlockId::kBlockSize - offset_into_page);

        size_t bytes_read =
            page_cache.Write(block_id, buf + bytes_copied, available, offset_into_page, &thread_local_meta);

        bytes_copied += bytes_read;
        if (bytes_read < available) {
            DLOG(INFO) << "Read all bytes available in page; stop (rc=" << bytes_copied << ")";
            break;
        }

        block_id.IncrementPage();
        offset_into_page = 0;
    }

    return bytes_copied;
}

expected<size_t, PosixError> RackOBJFile::FetchBlockFunctor::operator()(const common::BlockId& block_id,
                                                                        void* write_location) {
    ssize_t rc =
        original_syscalls.pread(fs_interface_, write_location, common::BlockId::kBlockSize, block_id.GetOffset());
    if (rc != -1) {
        return static_cast<size_t>(rc);
    } else {
        return unexpected<PosixError>(static_cast<PosixError>(rc));
    }
}

RackOBJFile::RackOBJFile(int rfd, RackOBJPath&& path) noexcept
    : fh_(KernelManagedFileHandle(rfd)), vfd_(AllocateVirtualDescriptor()), path_(std::forward<RackOBJPath>(path)) {}

RackOBJFile::RackOBJFile(RackOBJPath&& path, int rfd, uint64_t server_id, ino_t inode) noexcept
    : fh_(RackOBJFileHandle(rfd, server_id, inode)),
      vfd_(AllocateVirtualDescriptor()),
      path_(std::forward<RackOBJPath>(path)) {}

RackOBJFile::~RackOBJFile() {
    std::lock_guard<std::mutex> guard(used_fds_mu);
    used_fds.push(vfd_);
}

ssize_t RackOBJFile::Read(void* buf, uint64_t count) {
    if (auto* fptr = std::get_if<RackOBJFileHandle>(&fh_)) [[likely]] {
        fptr->Lockfile();
        auto copied = fptr->Pread(static_cast<uint8_t*>(buf), count, fptr->file_offset_);
        fptr->file_offset_ += static_cast<off_t>(copied);
        fptr->Unlockfile();

        return static_cast<ssize_t>(copied);
    } else if (const auto* kfptr = std::get_if<KernelManagedFileHandle>(&fh_)) {
        return original_syscalls.read(kfptr->fd_, buf, count);
    } else {
        errno = EISDIR;
        return -1;
    }
}

ssize_t RackOBJFile::Pread(void* buf, uint64_t count, off_t offset) {
    if (auto* fptr = std::get_if<RackOBJFileHandle>(&fh_)) [[likely]] {
        auto copied = fptr->Pread(static_cast<uint8_t*>(buf), count, offset);
        return static_cast<ssize_t>(copied);
    } else if (const auto* kfptr = std::get_if<KernelManagedFileHandle>(&fh_)) {
        return original_syscalls.pread(kfptr->fd_, buf, count, static_cast<off_t>(offset));
    } else {
        errno = EISDIR;
        return -1;
    }
}

ssize_t RackOBJFile::Write(const void* buf, uint64_t count) {
    if (auto* fptr = std::get_if<RackOBJFileHandle>(&fh_)) [[likely]] {
        fptr->Lockfile();
        auto copied = fptr->Pwrite(static_cast<const uint8_t*>(buf), count, fptr->file_offset_);
        fptr->file_offset_ += static_cast<off_t>(copied);
        fptr->Unlockfile();

        return static_cast<ssize_t>(copied);
    } else if (const auto* kfptr = std::get_if<KernelManagedFileHandle>(&fh_)) {
        return original_syscalls.write(kfptr->fd_, buf, count);
    } else {
        errno = EISDIR;
        return -1;
    }
}

ssize_t RackOBJFile::Pwrite(const void* buf, uint64_t count, off_t offset) {
    if (auto* fptr = std::get_if<RackOBJFileHandle>(&fh_)) [[likely]] {
        auto copied = fptr->Pwrite(static_cast<const uint8_t*>(buf), count, offset);
        return static_cast<ssize_t>(copied);
    } else if (const auto* kfptr = std::get_if<KernelManagedFileHandle>(&fh_)) {
        return original_syscalls.pwrite(kfptr->fd_, buf, count, static_cast<off_t>(offset));
    } else {
        errno = EISDIR;
        return -1;
    }
}

int RackOBJFile::Close() {
    return std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, KernelManagedFileHandle>) {
                return original_syscalls.close(arg.fd_);
            } else {
                // TODO: else if rackobj local disk file: close
                return 0;
            }
        },
        fh_);
}

void RackOBJFile::Lockfile() {
    std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, RackOBJFileHandle>) {
                arg.Lockfile();
            } else {
                LOG(FATAL) << "Lockfile called on non-RackOBJ file";
            }
        },
        fh_);
}

void RackOBJFile::Unlockfile() {
    std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, RackOBJFileHandle>) {
                arg.Unlockfile();
            } else {
                LOG(FATAL) << "Unlockfile called on non-RackOBJ file";
            }
        },
        fh_);
}

int RackOBJFile::TryLockfile() {
    return std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, RackOBJFileHandle>) {
                std::unique_lock<std::mutex> lock(*arg.lock_count_mutex_);
                if (arg.lock_count_ == 0) {
                    arg.lock_count_ = GetThreadId();
                    return 0;
                } else if (arg.lock_count_ == GetThreadId()) {
                    return EDEADLK;
                } else {
                    return EBUSY;
                }
            } else {
                LOG(FATAL) << "TryLockfile called on non-RackOBJ file";
                return -1;
            }
        },
        fh_);
}

long RackOBJFile::Tell() const {
    return std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            /*if constexpr (std::is_same_v<T, RackOBJDirentHandle>) {
                return static_cast<long>(arg.index_);
            } else */
            if constexpr (std::is_same_v<T, KernelManagedFileHandle>) {
                return original_syscalls.lseek(arg.fd_, 0, SEEK_CUR);
            } else {
                return arg.file_offset_;
            }
        },
        fh_);
}

int RackOBJFile::GetError() const {
    return std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, RackOBJFileHandle>) {
                return arg.error_code_;
            } else {
                return 0;
            }
        },
        fh_);
}

void RackOBJFile::SetError(int error_code) {
    std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, RackOBJFileHandle>) {
                arg.error_code_ = error_code;
            }
        },
        fh_);
}

}  // namespace rackobj::lib