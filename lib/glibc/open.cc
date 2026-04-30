#include <dlfcn.h>
#include <fcntl.h>

#include <cstdarg>
#include <memory>
#include <optional>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "common/path.h"
#include "file.h"
#include "globals.h"
#include "original_syscalls.h"

using rackobj::RackOBJPath;
using rackobj::lib::RackOBJFile;
using std::make_shared;
using std::optional;
using std::quoted;
using std::shared_ptr;
using std::string;

// This identifier is not defined on macOS, so we define it here.
// Source: https://elixir.bootlin.com/glibc/latest/source/io/fcntl.h#L39-L44
#ifndef __OPEN_NEEDS_MODE
#ifdef __O_TMPFILE
#define __OPEN_NEEDS_MODE(oflag) (((oflag)&O_CREAT) != 0 || ((oflag)&__O_TMPFILE) == __O_TMPFILE)
#else
#define __OPEN_NEEDS_MODE(oflag) (((oflag)&O_CREAT) != 0)
#endif
#endif

static RackOBJFile* rackobj_open(const char* pathname, int flags, mode_t mode) {
    CHECK(SLOT_SIZE == 4096) << "SLOT_SIZE must be 4096";
    shared_ptr<RackOBJFile> fp = nullptr;
    RackOBJPath path(rackobj::lib::client_cfg, pathname);

    // If the file is listed as excluded, we still need to keep track of the
    // file descriptor to differentiate between kernel file descriptors and
    // virtual file descriptors. To make things easy, we wrap every single
    // descriptor with a NaiveRackOBJFile pointer.
    if (rackobj::lib::client_cfg.IsPathExcluded(pathname)) {
        int fd = rackobj::lib::original_syscalls.open(pathname, flags, mode);
        if (fd == -1) {
            // errno will be set by the original open function
            DLOG(WARNING) << "Failed to open kernel managed " << quoted(pathname);
            return nullptr;
        }

        fp = make_shared<RackOBJFile>(fd, std::move(path));
        DLOG(INFO) << "rackobj_open(" << pathname << ", " << std::hex << flags << ", " << std::oct << mode << std::dec
                   << ") --> " << fp->GetFileTypeString() << ", real fd=" << fd
                   << ", vfd=" << fp->GetVirtualFileDescriptor() << ", fp=" << fp;
    } else {
        uint64_t server_id = rackobj::lib::client_cfg.GetServerId();

        int fd = rackobj::lib::original_syscalls.open(path.GetRealPath().c_str(), O_DIRECT | O_RDWR | (flags & O_CREAT),
                                                      0666);

        if (fd == -1) {
            LOG(ERROR) << "Failed to open " << quoted(pathname);
            return nullptr;
        }

        struct stat stbuf;
        int rc = fstat(fd, &stbuf);
        if (rc == -1) {
            LOG(ERROR) << "fstat " << quoted(pathname) << " failed";
            return nullptr;
        }

        // char* buf;
        // int ret = posix_memalign((void**)&buf, 4096, SLOT_SIZE);
        // ssize_t rc_access = rackobj::lib::original_syscalls.pread(fd, buf, SLOT_SIZE, 0);
        // if (rc_access == -1) {
        //     LOG(ERROR) << "pread " << quoted(pathname) << " failed";
        //     return nullptr;
        // }
        // if (rc_access == 0) {
        //     LOG(ERROR) << "pread " << quoted(pathname) << " failed";
        //     return nullptr;
        // }

        fp = make_shared<RackOBJFile>(std::move(path), fd, server_id, stbuf.st_ino);

        // LOG(INFO) << "rackobj_open(" << pathname << ", " << std::hex << flags << ", " << std::oct << mode << std::dec
        //           << ") --> " << fp->GetFileTypeString() << ", real fd=" << fd
        //           << ", vfd=" << fp->GetVirtualFileDescriptor() << ", fp=" << fp;
    }

    RegisterLocalMem();

    InsertFilePointer(fp);
    return fp.get();
}

int open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    if (__OPEN_NEEDS_MODE(flags)) {
        va_list arg;
        va_start(arg, flags);
        mode = va_arg(arg, mode_t);
        va_end(arg);
    }

    DLOG(INFO) << "open(" << pathname << ", " << std::oct << flags << ", " << std::oct << mode << ")";

    RackOBJFile* fp = rackobj_open(pathname, flags, mode);
    return fp == nullptr ? -1 : fp->GetVirtualFileDescriptor();
}

int close(int fd) {
    // DLOG(INFO) << "close(" << fd << ")";
    if (!AreGlobalsInitialized()) [[unlikely]] {
        auto original_close = reinterpret_cast<original_close_t>(dlsym(RTLD_NEXT, "close"));
        return original_close(fd);
    }

    shared_ptr<RackOBJFile> fp = FindFilePointer(fd);
    if (fp == nullptr) {
        return rackobj::lib::original_syscalls.close(fd);
    }

    int result = fp->Close();
    EraseFilePointer(fp.get());
#ifdef NR
    UnRegisterNRThread();
#endif
    return result;
}
