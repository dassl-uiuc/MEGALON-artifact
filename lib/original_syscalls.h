#pragma once

#include <dirent.h>
#include <stdio.h>
#include <sys/types.h>

using original_open_t = int (*)(const char *, int, ...);
using original_creat_t = int (*)(const char *, mode_t);
using original_openat_t = int (*)(int, const char *, int, ...);
using original_mkstemp_t = int (*)(char *);
using original_close_t = int (*)(int);
using original_read_t = ssize_t (*)(int, void *, size_t);
using original_write_t = ssize_t (*)(int, const void *, size_t);
using original_access_t = int (*)(const char *, int);
using original_mkdir_t = int (*)(const char *, mode_t);
using original_chmod_t = int (*)(const char *, mode_t);
using original_fchmod_t = int (*)(int, mode_t);

using original_fopen_t = FILE *(*)(const char *, const char *);
using original_fdopen_t = FILE *(*)(int, const char *);
using original_fclose_t = int (*)(FILE *);
using original_fileno_t = int (*)(FILE *);
using original_fsync_t = int (*)(int);
using original_fread_t = size_t (*)(void *, size_t, size_t, FILE *);
using original_fwrite_t = size_t (*)(const void *, size_t, size_t, FILE *);
using original_fflush_t = int (*)(FILE *);
using original_fprintf_t = int (*)(FILE *, const char *, ...);
using original_vfprintf_t = int (*)(FILE *, const char *, va_list);

using original_flockfile_t = void (*)(FILE *);
using original_funlockfile_t = void (*)(FILE *);
using original_ftrylockfile_t = int (*)(FILE *);
using original_ferror_t = int (*)(FILE *);
using original_clearerr_t = void (*)(FILE *);

using original_opendir_t = DIR *(*)(const char *);
using original_fdopendir_t = DIR *(*)(int);
using original_readdir_t = struct dirent *(*)(DIR *dirp);
using original_readdir64_t = struct dirent64 *(*)(DIR *dirp);
using original_closedir_t = int (*)(DIR *);
using original_dirfd_t = int (*)(DIR *);

using original_rename_t = int (*)(const char *, const char *);
using original_symlink_t = int (*)(const char *, const char *);
using original_readlink_t = ssize_t (*)(const char *, char *, size_t);
using original_unlink_t = int (*)(const char *);
using original_stat_t = int (*)(const char *, struct stat *);
using original_stat64_t = int (*)(const char *, struct stat64 *);
using original_fstat_t = int (*)(int, struct stat *);
using original_fstat64_t = int (*)(int, struct stat64 *);
using original_statx_t = int (*)(int, const char *, int, unsigned int, struct statx *);

// https://refspecs.linuxbase.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/baselib---xstat64.html
using original_xstat64_t = int (*)(int, const char *, struct stat64 *);
using original_fxstat64_t = int (*)(int, int, struct stat64 *);

using original_getdents64_t = ssize_t (*)(int, void *, size_t);
using original_pwrite_t = ssize_t (*)(int, const void *, size_t, off_t);
using original_pwritev_t = ssize_t (*)(int, const struct iovec *, int, off_t);
using original_pread_t = ssize_t (*)(int, void *, size_t, off_t);
using original_lseek_t = off_t (*)(int, off_t, int);
using original_ftruncate_t = int (*)(int, off_t);
using original_feof_t = int (*)(FILE *);
using original_fseek_t = int (*)(FILE *, long, int);
using original_ftell_t = int (*)(FILE *);
using original_fgets_t = char *(*)(char *, int, FILE *);

namespace rackobj::lib {

struct original_syscalls_t {
    original_open_t open;
    original_creat_t creat;
    original_openat_t openat;
    original_mkstemp_t mkstemp;
    original_close_t close;
    original_read_t read;
    original_write_t write;
    original_mkdir_t mkdir;
    original_access_t access;
    original_chmod_t chmod;
    original_fchmod_t fchmod;

    original_fopen_t fopen;
    original_fdopen_t fdopen;
    original_fclose_t fclose;
    original_fflush_t fflush;
    original_fileno_t fileno;
    original_fread_t fread;
    original_fwrite_t fwrite;
    original_fsync_t fsync;

    original_opendir_t opendir;
    original_fdopendir_t fdopendir;
    original_dirfd_t dirfd;
    original_readdir_t readdir;
    original_readdir64_t readdir64;
    original_closedir_t closedir;

    original_rename_t rename;
    original_symlink_t symlink;
    original_readlink_t readlink;
    original_unlink_t unlink;
    original_fstat_t fstat;
    original_fstat64_t fstat64;

    original_pwrite_t pwrite;
    original_pwritev_t pwritev;
    original_pread_t pread;
    original_lseek_t lseek;
    original_ftruncate_t ftruncate;
    original_feof_t feof;
    original_fseek_t fseek;
    original_ftell_t ftell;
    original_fgets_t fgets;

    bool initialized = false;

    bool IsInitialized() const { return initialized; }
};

extern const original_syscalls_t original_syscalls;

}  // namespace rackobj::lib