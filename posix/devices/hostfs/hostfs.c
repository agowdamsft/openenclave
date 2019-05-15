// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#define _GNU_SOURCE

// clang-format off
#include <openenclave/enclave.h>
// clang-format on

#include <openenclave/internal/posix/device.h>
#include <openenclave/corelibc/limits.h>
#include <openenclave/internal/posix/fsops.h>
#include <openenclave/bits/safemath.h>
#include <openenclave/internal/calls.h>
#include <openenclave/internal/thread.h>
#include <openenclave/internal/print.h>
#include "posix_t.h"
#include <openenclave/corelibc/dirent.h>
#include <openenclave/corelibc/stdlib.h>
#include <openenclave/corelibc/string.h>
#include <openenclave/corelibc/fcntl.h>
#include <openenclave/bits/module.h>
#include <openenclave/internal/trace.h>
#include <openenclave/internal/posix/raise.h>
#include <openenclave/bits/safecrt.h>

/*
**==============================================================================
**
** hostfs operations:
**
**==============================================================================
*/

#define FS_MAGIC 0x5f35f964
#define FILE_MAGIC 0xfe48c6ff
#define DIR_MAGIC 0x8add1b0b

typedef struct _fs
{
    struct _oe_device base;
    uint32_t magic;
    unsigned long mount_flags;
    char mount_source[OE_PATH_MAX];
} fs_t;

typedef struct _file
{
    struct _oe_device base;
    uint32_t magic;
    oe_host_fd_t host_fd;
    uint32_t ready_mask;
    oe_device_t* dir;
} file_t;

typedef struct _dir
{
    struct _oe_device base;
    uint32_t magic;
    uint64_t host_dir;
    struct oe_dirent entry;
} dir_t;

static int _get_open_access_mode(int flags)
{
    return (flags & 000000003);
}

static fs_t* _cast_fs(const oe_device_t* device)
{
    fs_t* fs = (fs_t*)device;

    if (fs == NULL || fs->magic != FS_MAGIC)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return fs;
}

static file_t* _cast_file(const oe_device_t* device)
{
    file_t* file = (file_t*)device;

    if (file == NULL || file->magic != FILE_MAGIC)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return file;
}

static dir_t* _cast_dir(const oe_device_t* device)
{
    dir_t* dir = (dir_t*)device;

    if (dir == NULL || dir->magic != DIR_MAGIC)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return dir;
}

OE_INLINE bool _is_rdonly(const fs_t* fs)
{
    return (fs->mount_flags & OE_MS_RDONLY);
}

OE_INLINE bool _is_root(const char* path)
{
    return path[0] == '/' && path[1] == '\0';
}

/* Expand path to include the mount_source (needed by host side) */
static int _expand_path(
    const fs_t* fs,
    const char* suffix,
    char path[OE_PATH_MAX])
{
    const size_t n = OE_PATH_MAX;
    int ret = -1;

    if (_is_root(fs->mount_source))
    {
        if (oe_strlcpy(path, suffix, OE_PATH_MAX) >= n)
            OE_RAISE_ERRNO(OE_ENAMETOOLONG);
    }
    else
    {
        if (oe_strlcpy(path, fs->mount_source, OE_PATH_MAX) >= n)
            OE_RAISE_ERRNO(OE_ENAMETOOLONG);

        if (!_is_root(suffix))
        {
            if (oe_strlcat(path, "/", OE_PATH_MAX) >= n)
                OE_RAISE_ERRNO(OE_ENAMETOOLONG);

            if (oe_strlcat(path, suffix, OE_PATH_MAX) >= n)
                OE_RAISE_ERRNO(OE_ENAMETOOLONG);
        }
    }

    ret = 0;

done:
    return ret;
}

static int _hostfs_mount(
    oe_device_t* dev,
    const char* source,
    const char* target,
    unsigned long flags)
{
    int ret = -1;
    fs_t* fs = _cast_fs(dev);

    if (!fs || !source || !target)
        OE_RAISE_ERRNO(OE_EINVAL);

    fs->mount_flags = flags;
    oe_strlcpy(fs->mount_source, source, sizeof(fs->mount_source));

    ret = 0;

done:
    return ret;
}

static int _hostfs_unmount(oe_device_t* dev, const char* target)
{
    int ret = -1;
    fs_t* fs = _cast_fs(dev);

    if (!fs || !target)
        OE_RAISE_ERRNO(OE_EINVAL);

    ret = 0;

done:
    return ret;
}

static ssize_t _hostfs_read(oe_device_t*, void* buf, size_t count);

static int _hostfs_close(oe_device_t*);

static int _hostfs_clone(oe_device_t* device, oe_device_t** new_device)
{
    int ret = -1;
    fs_t* fs = _cast_fs(device);
    fs_t* new_fs = NULL;

    if (!fs || !new_device)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (!(new_fs = oe_calloc(1, sizeof(fs_t))))
        OE_RAISE_ERRNO(OE_ENOMEM);

    *new_fs = *fs;
    *new_device = &new_fs->base;
    ret = 0;

done:
    return ret;
}

static int _hostfs_release(oe_device_t* device)
{
    int ret = -1;
    fs_t* fs = _cast_fs(device);

    if (!fs)
        OE_RAISE_ERRNO(OE_EINVAL);

    oe_free(fs);
    ret = 0;
done:
    return ret;
}

static int _hostfs_shutdown(oe_device_t* device)
{
    int ret = -1;
    fs_t* fs = _cast_fs(device);

    if (!fs)
        OE_RAISE_ERRNO(OE_EINVAL);

    ret = 0;

done:
    return ret;
}

static oe_device_t* _hostfs_open_file(
    oe_device_t* fs_,
    const char* pathname,
    int flags,
    oe_mode_t mode)
{
    oe_device_t* ret = NULL;
    fs_t* fs = _cast_fs(fs_);
    file_t* file = NULL;
    char full_pathname[OE_PATH_MAX];
    oe_host_fd_t retval = -1;

    oe_errno = 0;

    /* Check parameters */
    if (!fs || !pathname)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Fail if attempting to write to a read-only file system. */
    if (_is_rdonly(fs) && _get_open_access_mode(flags) != OE_O_RDONLY)
        OE_RAISE_ERRNO(OE_EPERM);

    /* Create new file struct. */
    {
        if (!(file = oe_calloc(1, sizeof(file_t))))
            OE_RAISE_ERRNO(OE_ENOMEM);

        file->base.type = OE_DEVICE_TYPE_FILE;
        file->base.name = OE_DEVICE_NAME_HOST_FILE_SYSTEM;
        file->magic = FILE_MAGIC;
        file->base.ops.fs = fs->base.ops.fs;
    }

    /* Call */
    {
        if (_expand_path(fs, pathname, full_pathname) != 0)
            OE_RAISE_ERRNO_MSG(oe_errno, "pathname=%s", pathname);

        if (oe_posix_open_ocall(&retval, full_pathname, flags, mode) != OE_OK)
            OE_RAISE_ERRNO(OE_EINVAL);

        if (retval < 0)
            goto done;

        file->host_fd = retval;
    }

    ret = &file->base;
    file = NULL;

done:

    if (file)
        oe_free(file);

    return ret;
}

static oe_device_t* _hostfs_opendir(oe_device_t* fs, const char* name);
static int _hostfs_closedir(oe_device_t* file);

static oe_device_t* _hostfs_open_directory(
    oe_device_t* fs_,
    const char* pathname,
    int flags,
    oe_mode_t mode)
{
    oe_device_t* ret = NULL;
    fs_t* fs = _cast_fs(fs_);
    file_t* file = NULL;
    oe_device_t* dir = NULL;

    oe_errno = 0;

    OE_UNUSED(mode);

    /* Check parameters */
    if (!fs || !pathname || !(flags & OE_O_DIRECTORY))
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Directories can only be opened for read access. */
    if (_get_open_access_mode(flags) != OE_O_RDONLY)
        OE_RAISE_ERRNO(OE_EACCES);

    /* Attempt to open the directory. */
    if (!(dir = _hostfs_opendir(fs_, pathname)))
        OE_RAISE_ERRNO_MSG(oe_errno, "pathname=%s", pathname);

    /* Allocate and initialize the file struct. */
    {
        if (!(file = oe_calloc(1, sizeof(file_t))))
            OE_RAISE_ERRNO(OE_ENOMEM);

        file->base.type = OE_DEVICE_TYPE_FILE;
        file->base.name = OE_DEVICE_NAME_HOST_FILE_SYSTEM;
        file->magic = FILE_MAGIC;
        file->base.ops.fs = fs->base.ops.fs;
        file->host_fd = -1;
        file->dir = dir;
    }

    ret = &file->base;
    file = NULL;
    dir = NULL;

done:

    if (file)
        oe_free(file);

    if (dir)
        _hostfs_closedir(dir);

    return ret;
}

static oe_device_t* _hostfs_open(
    oe_device_t* fs,
    const char* pathname,
    int flags,
    oe_mode_t mode)
{
    if ((flags & OE_O_DIRECTORY))
    {
        return _hostfs_open_directory(fs, pathname, flags, mode);
    }
    else
    {
        return _hostfs_open_file(fs, pathname, flags, mode);
    }
}

static int _hostfs_dup(oe_device_t* file_, oe_device_t** new_file)
{
    int ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    /* Check parameters. */
    if (!file)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Call */
    {
        oe_host_fd_t retval = -1;

        if (oe_posix_dup_ocall(&retval, file->host_fd) != OE_OK)
            OE_RAISE_ERRNO(OE_EINVAL);

        if (retval == -1)
            OE_RAISE_ERRNO(oe_errno);

        {
            file_t* f = NULL;
            _hostfs_clone(file_, (oe_device_t**)&f);

            if (!f)
                OE_RAISE_ERRNO(oe_errno);

            f->host_fd = retval;
            *new_file = (oe_device_t*)f;
        }
    }

    ret = 0;

done:
    return ret;
}

static ssize_t _hostfs_read(oe_device_t* file_, void* buf, size_t count)
{
    ssize_t ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    if (!file)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Call the host. */
    if (oe_posix_read_ocall(&ret, file->host_fd, buf, count) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return ret;
}

static struct oe_dirent* _hostfs_readdir(oe_device_t* dir_);

static int _hostfs_getdents(
    oe_device_t* file_,
    struct oe_dirent* dirp,
    unsigned int count)
{
    int ret = -1;
    int bytes = 0;
    file_t* file = _cast_file(file_);
    unsigned int i;
    unsigned int n = count / sizeof(struct oe_dirent);

    oe_errno = 0;

    if (!file || !file->dir || !dirp)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Read the entries one-by-one. */
    for (i = 0; i < n; i++)
    {
        oe_errno = 0;

        struct oe_dirent* ent;

        if (!(ent = _hostfs_readdir(file->dir)))
        {
            if (oe_errno)
            {
                OE_RAISE_ERRNO(oe_errno);
                goto done;
            }

            break;
        }

        *dirp = *ent;
        bytes += (int)sizeof(struct oe_dirent);
        dirp++;
    }

    ret = bytes;

done:
    return ret;
}

static ssize_t _hostfs_write(oe_device_t* file, const void* buf, size_t count)
{
    ssize_t ret = -1;
    file_t* f = _cast_file(file);

    oe_errno = 0;

    /* Check parameters. */
    if (!f || (count && !buf))
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Call the host. */
    if (oe_posix_write_ocall(&ret, f->host_fd, buf, count) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return ret;
}

static oe_off_t _hostfs_lseek_file(
    oe_device_t* file,
    oe_off_t offset,
    int whence)
{
    oe_off_t ret = -1;
    file_t* f = _cast_file(file);

    oe_errno = 0;

    if (!file)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (oe_posix_lseek_ocall(&ret, f->host_fd, offset, whence) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return ret;
}

static int _hostfs_rewinddir(oe_device_t* dir_)
{
    int ret = -1;
    dir_t* dir = _cast_dir(dir_);

    if (!dir)
        OE_RAISE_ERRNO(OE_EINVAL);

    oe_posix_rewinddir_ocall(dir->host_dir);

    ret = 0;

done:
    return ret;
}

static oe_off_t _hostfs_lseek_dir(
    oe_device_t* file_,
    oe_off_t offset,
    int whence)
{
    oe_off_t ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    if (!file || !file->dir || offset != 0 || whence != OE_SEEK_SET)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (_hostfs_rewinddir(file->dir) != 0)
        OE_RAISE_ERRNO(oe_errno);

    ret = 0;

done:
    return ret;
}

static oe_off_t _hostfs_lseek(oe_device_t* file_, oe_off_t offset, int whence)
{
    oe_off_t ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    if (!file)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (file->dir)
        ret = _hostfs_lseek_dir(file_, offset, whence);
    else
        ret = _hostfs_lseek_file(file_, offset, whence);

done:
    return ret;
}

static int _hostfs_close_file(oe_device_t* file)
{
    int ret = -1;
    file_t* f = _cast_file(file);

    oe_errno = 0;

    if (!f)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (oe_posix_close_ocall(&ret, f->host_fd) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (ret == 0)
        oe_free(file);

    ret = 0;

done:
    return ret;
}

static int _hostfs_close_directory(oe_device_t* file_)
{
    int ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    /* Check parameters. */
    if (!file || !file->dir)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Release the directory object. */
    if (_hostfs_closedir(file->dir) != 0)
        OE_RAISE_ERRNO(oe_errno);

    /* Release the file object. */
    oe_free(file);

    ret = 0;

done:
    return ret;
}

static int _hostfs_close(oe_device_t* file_)
{
    int ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    if (!file)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (file->dir)
        ret = _hostfs_close_directory(file_);
    else
        ret = _hostfs_close_file(file_);

done:
    return ret;
}

static int _hostfs_ioctl(
    oe_device_t* file_,
    unsigned long request,
    uint64_t arg)
{
    int ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    if (!file)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (oe_posix_ioctl_ocall(&ret, file->host_fd, request, arg) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return ret;
}

static int _hostfs_fcntl(oe_device_t* file_, int cmd, uint64_t arg)
{
    int ret = -1;
    file_t* file = _cast_file(file_);

    oe_errno = 0;

    if (!file)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (oe_posix_fcntl_ocall(&ret, file->host_fd, cmd, arg) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:
    return ret;
}

static oe_device_t* _hostfs_opendir(oe_device_t* fs_, const char* name)
{
    oe_device_t* ret = NULL;
    fs_t* fs = _cast_fs(fs_);
    dir_t* dir = NULL;
    char full_name[OE_PATH_MAX];
    uint64_t retval = 0;

    if (!fs || !name)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (_expand_path(fs, name, full_name) != 0)
        OE_RAISE_ERRNO_MSG(oe_errno, "name=%s", name);

    if (oe_posix_opendir_ocall(&retval, full_name) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

    {
        if (!(dir = oe_calloc(1, sizeof(dir_t))))
            OE_RAISE_ERRNO(OE_ENOMEM);

        dir->base.type = OE_DEVICE_TYPE_DIRECTORY;
        dir->base.name = OE_DEVICE_NAME_HOST_FILE_SYSTEM;
        dir->magic = DIR_MAGIC;
        dir->base.ops.fs = fs->base.ops.fs;
        dir->host_dir = retval;
    }

    ret = &dir->base;
    dir = NULL;

done:

    if (dir)
        oe_free(dir);

    return ret;
}

static struct oe_dirent* _hostfs_readdir(oe_device_t* dir_)
{
    struct oe_dirent* ret = NULL;
    dir_t* dir = _cast_dir(dir_);
    int retval = -1;

    oe_errno = 0;

    if (!dir)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Call the host. */
    {
        if (oe_posix_readdir_ocall(
                &retval,
                dir->host_dir,
                &dir->entry.d_ino,
                &dir->entry.d_off,
                &dir->entry.d_reclen,
                &dir->entry.d_type,
                dir->entry.d_name,
                sizeof(dir->entry.d_name)) != OE_OK)
        {
            OE_RAISE_ERRNO(OE_EINVAL);
        }

        /* Fix up the record length. */
        if (retval == 0)
            dir->entry.d_reclen = sizeof(struct oe_dirent);
    }

    /* Handle any error. */
    if (retval == -1)
    {
        const size_t num_bytes = sizeof(dir->entry);

        if (oe_memset_s(&dir->entry, num_bytes, 0, num_bytes) != OE_OK)
            OE_RAISE_ERRNO(OE_EINVAL);

        if (oe_errno)
            OE_RAISE_ERRNO(oe_errno);

        goto done;
    }

    ret = &dir->entry;

done:

    return ret;
}

static int _hostfs_closedir(oe_device_t* dir)
{
    int ret = -1;
    dir_t* d = _cast_dir(dir);
    int retval = -1;

    oe_errno = 0;

    if (!d)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (oe_posix_closedir_ocall(&retval, d->host_dir) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (retval == 0)
        ret = 0;

    oe_free(dir);

done:

    return ret;
}

static int _hostfs_stat(
    oe_device_t* fs_,
    const char* pathname,
    struct oe_stat* buf)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_pathname[OE_PATH_MAX];

    if (buf)
    {
        if (oe_memset_s(buf, sizeof(*buf), 0, sizeof(*buf)) != OE_OK)
            OE_RAISE_ERRNO(OE_EINVAL);
    }

    if (!fs || !pathname || !buf)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (_expand_path(fs, pathname, full_pathname) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_stat_ocall(&ret, full_pathname, buf) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static int _hostfs_access(oe_device_t* fs_, const char* pathname, int mode)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_pathname[OE_PATH_MAX];
    const uint32_t MASK = (OE_R_OK | OE_W_OK | OE_X_OK);

    if (!fs || !pathname || ((uint32_t)mode & ~MASK))
        OE_RAISE_ERRNO(OE_EINVAL);

    if (_expand_path(fs, pathname, full_pathname) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_access_ocall(&ret, full_pathname, mode) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static int _hostfs_link(
    oe_device_t* fs_,
    const char* oldpath,
    const char* newpath)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_oldpath[OE_PATH_MAX];
    char full_newpath[OE_PATH_MAX];

    if (!fs || !oldpath || !newpath)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Fail if attempting to write to a read-only file system. */
    if (_is_rdonly(fs))
        OE_RAISE_ERRNO(OE_EPERM);

    if (_expand_path(fs, oldpath, full_oldpath) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (_expand_path(fs, newpath, full_newpath) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_link_ocall(&ret, full_oldpath, full_newpath) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static int _hostfs_unlink(oe_device_t* fs_, const char* pathname)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_pathname[OE_PATH_MAX];

    if (!fs)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Fail if attempting to write to a read-only file system. */
    if (_is_rdonly(fs))
        OE_RAISE_ERRNO(OE_EPERM);

    if (_expand_path(fs, pathname, full_pathname) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_unlink_ocall(&ret, full_pathname) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static int _hostfs_rename(
    oe_device_t* fs_,
    const char* oldpath,
    const char* newpath)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_oldpath[OE_PATH_MAX];
    char full_newpath[OE_PATH_MAX];

    if (!fs || !oldpath || !newpath)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (_is_rdonly(fs))
        OE_RAISE_ERRNO(OE_EPERM);

    if (_expand_path(fs, oldpath, full_oldpath) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (_expand_path(fs, newpath, full_newpath) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_rename_ocall(&ret, full_oldpath, full_newpath) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static int _hostfs_truncate(oe_device_t* fs_, const char* path, oe_off_t length)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_path[OE_PATH_MAX];

    if (!fs)
        OE_RAISE_ERRNO(OE_EINVAL);

    if (_is_rdonly(fs))
        OE_RAISE_ERRNO(OE_EPERM);

    if (_expand_path(fs, path, full_path) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_truncate_ocall(&ret, full_path, length) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static int _hostfs_mkdir(oe_device_t* fs_, const char* pathname, oe_mode_t mode)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_pathname[OE_PATH_MAX];

    if (!fs)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Fail if attempting to write to a read-only file system. */
    if (_is_rdonly(fs))
        OE_RAISE_ERRNO(OE_EPERM);

    if (_expand_path(fs, pathname, full_pathname) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_mkdir_ocall(&ret, full_pathname, mode) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static int _hostfs_rmdir(oe_device_t* fs_, const char* pathname)
{
    int ret = -1;
    fs_t* fs = _cast_fs(fs_);
    char full_pathname[OE_PATH_MAX];

    if (!fs)
        OE_RAISE_ERRNO(OE_EINVAL);

    /* Fail if attempting to write to a read-only file system. */
    if (_is_rdonly(fs))
        OE_RAISE_ERRNO(OE_EPERM);

    if (_expand_path(fs, pathname, full_pathname) != 0)
        OE_RAISE_ERRNO(oe_errno);

    if (oe_posix_rmdir_ocall(&ret, full_pathname) != OE_OK)
        OE_RAISE_ERRNO(OE_EINVAL);

done:

    return ret;
}

static oe_host_fd_t _hostfs_gethostfd(oe_device_t* file_)
{
    file_t* f = _cast_file(file_);

    if (f->magic == FILE_MAGIC)
    {
        return f->host_fd;
    }

    return -1;
}

static oe_fs_ops_t _ops = {
    .base.dup = _hostfs_dup,
    .base.shutdown = _hostfs_shutdown,
    .base.ioctl = _hostfs_ioctl,
    .base.fcntl = _hostfs_fcntl,
    .clone = _hostfs_clone,
    .release = _hostfs_release,
    .mount = _hostfs_mount,
    .unmount = _hostfs_unmount,
    .open = _hostfs_open,
    .base.read = _hostfs_read,
    .base.write = _hostfs_write,
    .base.get_host_fd = _hostfs_gethostfd,
    .lseek = _hostfs_lseek,
    .base.close = _hostfs_close,
    .getdents = _hostfs_getdents,
    .stat = _hostfs_stat,
    .access = _hostfs_access,
    .link = _hostfs_link,
    .unlink = _hostfs_unlink,
    .rename = _hostfs_rename,
    .truncate = _hostfs_truncate,
    .mkdir = _hostfs_mkdir,
    .rmdir = _hostfs_rmdir,
};

static fs_t _hostfs = {
    .base.type = OE_DEVICE_TYPE_FILESYSTEM,
    .base.name = OE_DEVICE_NAME_HOST_FILE_SYSTEM,
    .base.ops.fs = &_ops,
    .magic = FS_MAGIC,
    .mount_flags = 0,
    .mount_source = {'/'},
};

oe_device_t* oe_get_hostfs_device(void)
{
    return &_hostfs.base;
}

static oe_once_t _once = OE_ONCE_INITIALIZER;
static bool _loaded;

static void _load_once(void)
{
    oe_result_t result = OE_FAILURE;
    const uint64_t devid = OE_DEVID_HOST_FILE_SYSTEM;

    if (oe_set_device(devid, oe_get_hostfs_device()) != 0)
        OE_RAISE_ERRNO(oe_errno);

    result = OE_OK;

done:

    if (result == OE_OK)
        _loaded = true;
}

oe_result_t oe_load_module_host_file_system(void)
{
    if (oe_once(&_once, _load_once) != OE_OK || !_loaded)
        return OE_FAILURE;

    return OE_OK;
}