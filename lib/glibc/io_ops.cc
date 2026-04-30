#include <dlfcn.h>

#include "absl/log/log.h"
#include "file.h"
#include "globals.h"
#include "original_syscalls.h"

using rackobj::lib::RackOBJFile;
using std::shared_ptr;

ssize_t read(int fd, void* buf, size_t count) {
    if (fd == 0) [[unlikely]] {
        return rackobj::lib::original_syscalls.read(fd, buf, count);
    }

    // DLOG(INFO) << "read(" << fd << ", *, " << count << ")";

    if (!AreGlobalsInitialized()) [[unlikely]] {
        auto original_read = reinterpret_cast<original_read_t>(dlsym(RTLD_NEXT, "read"));
        return original_read(fd, buf, count);
    }

    shared_ptr<RackOBJFile> fp = FindFilePointer(fd);
    return fp == nullptr ? rackobj::lib::original_syscalls.read(fd, buf, count)
                         : fp->Read(buf, static_cast<uint64_t>(count));
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    if (offset < 0) {
        LOG(WARNING) << "pread called with offset < 0";
        return -1;
    }

    if (!AreGlobalsInitialized()) [[unlikely]] {
        auto original_pread = reinterpret_cast<original_pread_t>(dlsym(RTLD_NEXT, "pread"));
        return original_pread(fd, buf, count, offset);
    }

    shared_ptr<RackOBJFile> fp = FindFilePointer(fd);
    return fp == nullptr ? rackobj::lib::original_syscalls.read(fd, buf, count) : fp->Pread(buf, count, offset);
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (fd == 1 || fd == 2) [[unlikely]] {
        return rackobj::lib::original_syscalls.write(fd, buf, count);
    }

    // DLOG(INFO) << "write(" << fd << ", *, " << count << ")";

    if (!AreGlobalsInitialized()) [[unlikely]] {
        auto original_write = reinterpret_cast<original_write_t>(dlsym(RTLD_NEXT, "write"));
        return original_write(fd, buf, count);
    }

    shared_ptr<RackOBJFile> fp = FindFilePointer(fd);
    return fp == nullptr ? rackobj::lib::original_syscalls.write(fd, buf, count)
                         : fp->Write(buf, static_cast<uint64_t>(count));
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    if (offset < 0) {
        LOG(WARNING) << "pwrite called with offset < 0";
        return -1;
    }

    if (!AreGlobalsInitialized()) [[unlikely]] {
        auto original_pwrite = reinterpret_cast<original_pwrite_t>(dlsym(RTLD_NEXT, "pwrite"));
        return original_pwrite(fd, buf, count, offset);
    }

    shared_ptr<RackOBJFile> fp = FindFilePointer(fd);
    return fp == nullptr ? rackobj::lib::original_syscalls.pwrite(fd, buf, count, offset)
                         : fp->Pwrite(buf, count, offset);
}