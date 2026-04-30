#include "original_syscalls.h"

#include <dlfcn.h>

namespace rackobj::lib {

const original_syscalls_t original_syscalls = {
    .open = reinterpret_cast<original_open_t>(dlsym(RTLD_NEXT, "open")),
    .creat = reinterpret_cast<original_creat_t>(dlsym(RTLD_NEXT, "creat")),
    .openat = reinterpret_cast<original_openat_t>(dlsym(RTLD_NEXT, "openat")),
    .mkstemp = reinterpret_cast<original_mkstemp_t>(dlsym(RTLD_NEXT, "mkstemp")),
    .close = reinterpret_cast<original_close_t>(dlsym(RTLD_NEXT, "close")),
    .read = reinterpret_cast<original_read_t>(dlsym(RTLD_NEXT, "read")),
    .write = reinterpret_cast<original_write_t>(dlsym(RTLD_NEXT, "write")),
    .mkdir = reinterpret_cast<original_mkdir_t>(dlsym(RTLD_NEXT, "mkdir")),
    .access = reinterpret_cast<original_access_t>(dlsym(RTLD_NEXT, "access")),
    .chmod = reinterpret_cast<original_chmod_t>(dlsym(RTLD_NEXT, "chmod")),
    .fchmod = reinterpret_cast<original_fchmod_t>(dlsym(RTLD_NEXT, "fchmod")),
    .fopen = reinterpret_cast<original_fopen_t>(dlsym(RTLD_NEXT, "fopen")),
    .fdopen = reinterpret_cast<original_fdopen_t>(dlsym(RTLD_NEXT, "fdopen")),
    .fclose = reinterpret_cast<original_fclose_t>(dlsym(RTLD_NEXT, "fclose")),
    .fflush = reinterpret_cast<original_fflush_t>(dlsym(RTLD_NEXT, "fflush")),
    .fileno = reinterpret_cast<original_fileno_t>(dlsym(RTLD_NEXT, "fileno")),
    .fread = reinterpret_cast<original_fread_t>(dlsym(RTLD_NEXT, "fread")),
    .fwrite = reinterpret_cast<original_fwrite_t>(dlsym(RTLD_NEXT, "fwrite")),
    .fsync = reinterpret_cast<original_fsync_t>(dlsym(RTLD_NEXT, "fsync")),
    .opendir = reinterpret_cast<original_opendir_t>(dlsym(RTLD_NEXT, "opendir")),
    .fdopendir = reinterpret_cast<original_fdopendir_t>(dlsym(RTLD_NEXT, "fdopendir")),
    .dirfd = reinterpret_cast<original_dirfd_t>(dlsym(RTLD_NEXT, "dirfd")),
    .readdir = reinterpret_cast<original_readdir_t>(dlsym(RTLD_NEXT, "readdir")),
    .readdir64 = reinterpret_cast<original_readdir64_t>(dlsym(RTLD_NEXT, "readdir64")),
    .closedir = reinterpret_cast<original_closedir_t>(dlsym(RTLD_NEXT, "closedir")),
    .rename = reinterpret_cast<original_rename_t>(dlsym(RTLD_NEXT, "rename")),
    .symlink = reinterpret_cast<original_symlink_t>(dlsym(RTLD_NEXT, "symlink")),
    .readlink = reinterpret_cast<original_readlink_t>(dlsym(RTLD_NEXT, "readlink")),
    .unlink = reinterpret_cast<original_unlink_t>(dlsym(RTLD_NEXT, "unlink")),
    .fstat = reinterpret_cast<original_fstat_t>(dlsym(RTLD_NEXT, "fstat")),
    .fstat64 = reinterpret_cast<original_fstat64_t>(dlsym(RTLD_NEXT, "fstat64")),
    .pwrite = reinterpret_cast<original_pwrite_t>(dlsym(RTLD_NEXT, "pwrite")),
    .pwritev = reinterpret_cast<original_pwritev_t>(dlsym(RTLD_NEXT, "pwritev")),
    .pread = reinterpret_cast<original_pread_t>(dlsym(RTLD_NEXT, "pread")),
    .lseek = reinterpret_cast<original_lseek_t>(dlsym(RTLD_NEXT, "lseek")),
    .ftruncate = reinterpret_cast<original_ftruncate_t>(dlsym(RTLD_NEXT, "ftruncate")),
    .feof = reinterpret_cast<original_feof_t>(dlsym(RTLD_NEXT, "feof")),
    .fseek = reinterpret_cast<original_fseek_t>(dlsym(RTLD_NEXT, "fseek")),
    .ftell = reinterpret_cast<original_ftell_t>(dlsym(RTLD_NEXT, "ftell")),
    .fgets = reinterpret_cast<original_fgets_t>(dlsym(RTLD_NEXT, "fgets")),
    .initialized = true};

}  // namespace rackobj::lib