#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline structured FS prototype, not a CCCI daemon or wire codec.

Only caller-provisioned private working copies are accepted. Never point this at
Android mounts or original backups: no import/copy/restore operation is provided.
The marker is an explicit acknowledgement, not proof of snapshot provenance.
No operation prints paths, payloads or file contents. Calls must be serialized.
Move implements established copy/deletion flags, never source deletion.
FindFirst/Next/Close retain native directory streams and byte-widen names;
DOS timestamps deliberately use the documented Bionic shared-mtime policy.
XDelete preflights private regular-file/directory trees before mutation.
CMPT_Read accepts only all-five-step
requests with read-only 0x700 flags. Uninitialized vendor failure outputs are
zeroed; GetFileDetail returns only the established three timestamp words.
GetDrive accepts exactly the two observed query tuples, never arbitrary flags.
All other operation codes, directory opens, OTP/device/ioctl access, restart
recovery, copying originals and wire dispatch remain outside this prototype.
An UnsupportedOperation is a local readiness failure, not a vendor response.

Move, detail and CMPT contracts were checked against same-board ccci_mdinit
SHA256 0740831c74b699ee802a7e7c0ccb25266a29f4105a5dc233203ef9dc2ff71a4e
(ELF functions 0x11e420, 0x122a98 and 0x1199fc respectively).
"""

import errno
import os
import stat
import struct
import tempfile
import time

MARKER = '.mt6893-fs-working-copy'
ACKNOWLEDGEMENT = b'PRIVATE DISPOSABLE MODEM FS WORKING COPY\n'
DRIVES = frozenset('ZXYWVUTSRQ')
MAX_HANDLES = 0x80
MAX_IO = 0x4000
MAX_PATH = 1024
MAX_TREE_DEPTH = 64


class _Search:
    def __init__(self, fd, iterator, pattern, required, excluded):
        self.fd = fd
        self.iterator = iterator
        self.pattern = pattern
        self.required = required
        self.excluded = excluded


def fs_error(exc):
    """The vendor errors established by static analysis; no host text leaks."""
    return {errno.ENOENT: -9, errno.EACCES: -16, errno.EPERM: -16,
            errno.ELOOP: -16, errno.EMFILE: -5}.get(exc.errno, -18)


class UnsupportedOperation(Exception):
    """Internal readiness failure; must not be serialized as a vendor errno."""


class WorkingCopy:
    """Directory-fd rooted engine; generation is checked on every request.

    Provision root mode 0700, marker mode 0600 with ACKNOWLEDGEMENT contents,
    and zero or more uppercase drive directories. Missing drives stay missing.
    Original backups must live outside root and must not share file hardlinks.
    Returns tuples matching operation response arguments, without wire packing
    except CMPT's documented eight-byte (completed bitmap, error) argument.
    """

    def __init__(self, root, generation):
        if not isinstance(generation, int) or generation <= 0:
            raise ValueError('positive generation required')
        self.root = -1
        self.handles = {}
        self.searches = {}
        self.generation = generation
        self.active = False
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                     os.O_CLOEXEC)
        try:
            info = os.fstat(fd)
            if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
                raise ValueError('working root must be owned and private')
            marker = os.open(MARKER, os.O_RDONLY | os.O_NOFOLLOW |
                             os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=fd)
            try:
                info = os.fstat(marker)
                if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
                        info.st_uid != os.geteuid() or
                        stat.S_IMODE(info.st_mode) != 0o600 or
                        os.read(marker, len(ACKNOWLEDGEMENT) + 1) !=
                        ACKNOWLEDGEMENT):
                    raise ValueError('explicit working-copy marker required')
            finally:
                os.close(marker)
        except BaseException:
            os.close(fd)
            raise
        self.root = fd
        self.active = True

    @staticmethod
    def _path(path):
        if not isinstance(path, bytes) or not path or len(path) > MAX_PATH:
            raise PermissionError(errno.EACCES, 'invalid path')
        try:
            text = path.decode('utf-16le', errors='strict')
        except UnicodeError:
            raise PermissionError(errno.EACCES, 'invalid path') from None
        # One optional terminator, never silently truncate embedded NULs.
        if text.endswith('\0'):
            text = text[:-1]
        text = text.replace('\\', '/')
        if ('\0' in text or '..' in text or len(text) < 3 or
                text[0] not in DRIVES or text[1:3] != ':/'):
            raise PermissionError(errno.EACCES, 'invalid path')
        # The MD spells a drive-relative path with a doubled separator
        # (X://name); POSIX collapses repeated separators, so only empty
        # components are dropped. A bare drive root is not a file path.
        parts = [part for part in text[3:].split('/') if part]
        if not parts or any(part == '.' or ':' in part for part in parts):
            raise PermissionError(errno.EACCES, 'invalid path')
        return [text[0], *parts]

    def _parent(self, path):
        parts = self._path(path)
        fd = os.dup(self.root)
        try:
            for part in parts[:-1]:
                next_fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY |
                                  os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
                os.close(fd)
                fd = next_fd
            return fd, parts[-1]
        except BaseException:
            os.close(fd)
            raise

    @staticmethod
    def _regular(fd):
        info = os.fstat(fd)
        # Also prevents FIFO/device access and modification of hardlinked seeds.
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise PermissionError(errno.EACCES, 'non-private regular file')
        return info

    @staticmethod
    def _flags(flags):
        if not isinstance(flags, int) or flags < 0 or flags > 0xffffffff:
            raise ValueError('invalid open flags')
        result = os.O_RDONLY if flags & 0x100 else os.O_RDWR
        if flags & 0x10000:
            result = os.O_RDWR | os.O_CREAT
        if flags & 0x20000:
            # Defer truncation until fstat rejects hardlinks/special files.
            result = os.O_RDWR | os.O_CREAT
        if flags & 0x10000000:
            result |= os.O_NONBLOCK
        return result | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK

    def _open(self, path, flags):
        if len(self.handles) + len(self.searches) == MAX_HANDLES:
            return -5
        native_flags = self._flags(flags)
        parent, name = self._parent(path)
        directory = False
        try:
            try:
                info = os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                if not native_flags & os.O_CREAT:
                    raise
            else:
                if stat.S_ISDIR(info.st_mode):
                    # The MD opens NVRAM directories read-only; nothing is
                    # readable through a directory fd, so allow exactly that.
                    if native_flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT |
                                       os.O_TRUNC):
                        raise PermissionError(errno.EACCES, 'directory write rejected')
                    directory = True
                    native_flags = (os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                                    os.O_CLOEXEC | os.O_NONBLOCK)
                elif not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                    raise PermissionError(errno.EACCES, 'non-private file')
            fd = os.open(name, native_flags, 0o660, dir_fd=parent)
        finally:
            os.close(parent)
        try:
            if not directory:
                self._regular(fd)
                if flags & 0x20000:
                    os.ftruncate(fd, 0)
        except BaseException:
            os.close(fd)
            raise
        handle = next(i for i in range(1, MAX_HANDLES + 1)
                      if i not in self.handles and i not in self.searches)
        self.handles[handle] = fd, flags
        return handle

    def _close(self, handle, sync=True):
        search = self.searches.pop(handle, None)
        if search is not None:
            result = 0
            try:
                search.iterator.close()
            except OSError as exc:
                result = fs_error(exc)
            finally:
                try:
                    os.close(search.fd)
                except OSError as exc:
                    if not result:
                        result = fs_error(exc)
            return result
        entry = self.handles.pop(handle, None)
        if entry is None:
            return -10
        fd, flags = entry
        result = 0
        try:
            if sync and flags & 0x1030000:
                os.fsync(fd)
        except OSError as exc:
            result = fs_error(exc)
        finally:
            try:
                os.close(fd)
            except OSError as exc:
                if not result:
                    result = fs_error(exc)
        return result

    @staticmethod
    def _drive_root(path):
        """Return the drive letter when the request names only the root."""
        if not isinstance(path, bytes) or not path or len(path) > MAX_PATH:
            raise PermissionError(errno.EACCES, 'invalid path')
        try:
            text = path.decode('utf-16le', errors='strict')
        except UnicodeError:
            raise PermissionError(errno.EACCES, 'invalid path') from None
        if text.endswith('\0'):
            text = text[:-1]
        text = text.replace('\\', '/').rstrip('/')
        if len(text) == 2 and text[1] == ':' and text[0] in DRIVES:
            return text[0]
        return None

    def _attributes(self, path):
        """Same-board GetAttributes word: read-only 0x1, directory 0x10."""
        root = self._drive_root(path)
        if root is not None:
            info = os.stat(root, dir_fd=self.root, follow_symlinks=False)
        else:
            parent, name = self._parent(path)
            try:
                info = os.stat(name, dir_fd=parent, follow_symlinks=False)
            finally:
                os.close(parent)
        return (0x1 if self._readonly(info) else 0) | \
               (0x10 if stat.S_ISDIR(info.st_mode) else 0)

    def _restore(self, path, unused1, unused2):
        """Restore needs the vendor bin-region backup this provider never has.

        Same-board fsd_main returns the NVM_RestoreFromBinRegion_OneFile status
        and maps a failed restore to -18; nothing is written on failure.
        """
        self._path(path)
        return -18

    @staticmethod
    def _drive(path):
        """Drive-root spelling for GetDiskInfo: one letter, optional colon."""
        if not isinstance(path, bytes) or not path or len(path) > MAX_PATH:
            raise PermissionError(errno.EACCES, 'invalid path')
        try:
            text = path.decode('utf-16le', errors='strict')
        except UnicodeError:
            raise PermissionError(errno.EACCES, 'invalid path') from None
        if text.endswith('\0'):
            text = text[:-1]
        text = text.replace('\\', '/')
        if ('\0' in text or '..' in text or len(text) < 2 or
                text[0] not in DRIVES or text[1] != ':'):
            raise PermissionError(errno.EACCES, 'invalid path')
        return text[0]

    def _disk_info(self, path, unused):
        """Same-board GetDiskInfo reply: 84-byte record, four set fields.

        The vendor helper statfs()es the mapped drive and writes sector size,
        total 512-byte sectors and free blocks at 0x30/0x34/0x38/0x40; every
        other byte of the 84-byte record stays zero.
        """
        drive = self._drive(path)
        try:
            fd = os.open(drive, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                         os.O_CLOEXEC, dir_fd=self.root)
        except FileNotFoundError:
            fd = os.dup(self.root)
        try:
            info = os.fstatvfs(fd)
        finally:
            os.close(fd)
        record = bytearray(84)
        struct.pack_into('<I', record, 0x30, 512)
        struct.pack_into('<I', record, 0x34, (info.f_blocks >> 9) & 0xffffffff)
        struct.pack_into('<I', record, 0x38, info.f_bfree & 0xffffffff)
        struct.pack_into('<I', record, 0x40, 512)
        return 0, bytes(record)

    @staticmethod
    def _length(length):
        if not isinstance(length, int) or not 0 <= length <= MAX_IO:
            raise ValueError('I/O limit exceeded')

    def _move_info(self, path):
        try:
            parent, name = self._parent(path)
        except FileNotFoundError:
            return None
        try:
            try:
                info = os.stat(name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                return None
            if stat.S_ISLNK(info.st_mode):
                raise PermissionError(errno.EACCES, 'symlink rejected')
            return info
        finally:
            os.close(parent)

    @staticmethod
    def _readonly(info):
        # Exact FS_IsReadOnly mode test, not access(W_OK), including for root.
        return info is not None and info.st_mode & 0xdb == 0 and info.st_mode & 0x124 != 0

    def _move(self, source, destination, flags):
        source_parts, destination_parts = self._path(source), self._path(destination)
        if source_parts == destination_parts:
            return -2
        if not isinstance(flags, int) or flags & ~0x10003 or not flags & 3:
            return -2
        if flags & 0x10000 and source_parts[:len(destination_parts)] == destination_parts:
            raise PermissionError(errno.EACCES, 'overlapping Move tree rejected')
        if flags & 2 and self._readonly(self._move_info(source)):
            return -45
        result = -18
        if flags & 0x10000:
            info = self._move_info(destination)
            if self._readonly(info):
                return -45
            if info is not None:
                if stat.S_ISDIR(info.st_mode):
                    result = self._xdelete(destination, 0x1c)
                    if result < 0:
                        return result
                else:
                    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                        raise PermissionError(errno.EACCES, 'non-private destination')
                    parent, name = self._parent(destination)
                    try:
                        os.unlink(name, dir_fd=parent)
                    finally:
                        os.close(parent)
                    result = 0
        # Vendor bit1 only logs; it never unlinks the source (even on success).
        if not flags & 1:
            return result
        source_fd = destination_fd = None
        try:
            parent, name = self._parent(source)
            try:
                info = os.stat(name, dir_fd=parent, follow_symlinks=False)
                if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                    raise PermissionError(errno.EACCES, 'non-private source')
                source_fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW |
                                    os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=parent)
            finally:
                os.close(parent)
            self._regular(source_fd)
            size = os.lseek(source_fd, 0, os.SEEK_END)
            os.lseek(source_fd, 0, os.SEEK_SET)
            # Vendor reads the WHOLE source before creat(destination). Stage
            # anonymously under the private root, using bounded memory. This
            # preserves destination ordering and does not claim atomic replace.
            with tempfile.TemporaryFile(dir=f'/proc/self/fd/{self.root}') as staging:
                remaining = size
                while remaining:
                    count = min(remaining, MAX_IO)
                    data = os.read(source_fd, count)
                    if len(data) != count or staging.write(data) != count:
                        return -1
                    remaining -= count
                staging.seek(0)
                parent, name = self._parent(destination)
                try:
                    try:
                        info = os.stat(name, dir_fd=parent, follow_symlinks=False)
                    except FileNotFoundError:
                        pass
                    else:
                        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                            raise PermissionError(errno.EACCES, 'non-private destination')
                    destination_fd = os.open(name, os.O_WRONLY | os.O_CREAT |
                                             os.O_NOFOLLOW | os.O_NONBLOCK |
                                             os.O_CLOEXEC, 0o777, dir_fd=parent)
                finally:
                    os.close(parent)
                self._regular(destination_fd)
                os.ftruncate(destination_fd, 0)
                remaining = size
                while remaining:
                    count = min(remaining, MAX_IO)
                    data = staging.read(count)
                    if len(data) != count or os.write(destination_fd, data) != count:
                        return -1
                    remaining -= count
            return 0
        finally:
            for fd in (source_fd, destination_fd):
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError:
                        pass

    @staticmethod
    def _match(pattern, name):
        # Same language as PatMatch: case-sensitive bytes, '*' and '?' only.
        # Iterative backtracking avoids vendor recursion on adversarial stars.
        p = n = 0
        star = -1
        restart = 0
        while n < len(name):
            if p < len(pattern) and (pattern[p] == 63 or pattern[p] == name[n]):
                p += 1
                n += 1
            elif p < len(pattern) and pattern[p] == 42:
                star = p
                p += 1
                restart = n
            elif star >= 0:
                p = star + 1
                restart += 1
                n = restart
            else:
                return False
        while p < len(pattern) and pattern[p] == 42:
            p += 1
        return p == len(pattern)

    @staticmethod
    def _packed_time(value):
        local = time.localtime(value)
        # Deliberate vendor quirks: year since1900, unhalved seconds and day
        # used for BOTH customary month/day fields. All arithmetic is u32.
        packed = ((local.tm_sec & 31) | ((local.tm_min & 63) << 5) |
                  ((local.tm_hour & 31) << 11) | 0x60000000)
        packed = (packed + ((local.tm_year - 1900) << 25)) & 0xffffffff
        return ((packed & 0xfe000000) | ((local.tm_mday & 15) << 21) |
                (packed & 0xffff) | ((local.tm_mday & 31) << 16))

    def _dos_entry(self, info, raw_name, capacity):
        value = bytearray(52)
        value[11] = int(self._readonly(info)) | (0x10 if stat.S_ISDIR(info.st_mode) else 0)
        value[12] = int(len(raw_name) < capacity)
        # Bionic localtime returns shared static storage: the second (mtime)
        # call overwrites the first (atime) before either pointer is read.
        packed = self._packed_time(info.st_mtime_ns // 1000000000)
        struct.pack_into('<I', value, 14, packed)
        struct.pack_into('<I', value, 22, packed)
        struct.pack_into('<I', value, 28, info.st_size & 0xffffffff)
        return bytes(value)

    @staticmethod
    def _search_capacity(capacity):
        if type(capacity) is not int or not 1 <= capacity <= 0xffffffff:
            raise ValueError('Find capacity must be a positive u32')

    def _search_result(self, search, capacity, status):
        for entry in search.iterator:
            raw_name = os.fsencode(entry.name)
            if not self._match(search.pattern, raw_name):
                continue
            info = os.stat(entry.name, dir_fd=search.fd, follow_symlinks=False)
            if (not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)) or
                    (stat.S_ISREG(info.st_mode) and info.st_nlink != 1)):
                raise PermissionError(errno.EACCES, 'non-private search entry')
            attr = int(self._readonly(info)) | (0x10 if stat.S_ISDIR(info.st_mode) else 0)
            if search.required & ~attr or attr & search.excluded:
                continue
            name = raw_name[:capacity - 1]
            # Exact byte widening, not a claim of Unicode transcoding. fsd
            # sends zero bytes if the converter returned zero characters.
            wide = b''.join(bytes((byte if byte != 47 else 92, 0)) for byte in name)
            if name:
                wide += b'\0\0'
            return status, self._dos_entry(info, raw_name, capacity), wide
        return -6, bytes(52), b''

    def _find_first(self, path, required, excluded, capacity):
        self._search_capacity(capacity)
        if any(type(value) is not int or not 0 <= value <= 255
               for value in (required, excluded)):
            raise ValueError('Find attributes must be bytes')
        fd, pattern = self._parent(path)
        search = None
        try:
            search = _Search(fd, os.scandir(fd), os.fsencode(pattern), required, excluded)
            result = self._search_result(search, capacity, 0)
            if result[0] < 0:
                return result
            handle = next((i for i in range(1, MAX_HANDLES + 1)
                           if i not in self.handles and i not in self.searches), None)
            if handle is None:
                return -5, bytes(52), b''
            self.searches[handle] = search
            search = None
            fd = -1
            return handle, result[1], result[2]
        finally:
            try:
                if search is not None:
                    search.iterator.close()
            finally:
                if fd >= 0:
                    os.close(fd)

    def _find_next(self, handle, capacity):
        self._search_capacity(capacity)
        search = self.searches.get(handle)
        if search is None:
            return -10, bytes(52), b''
        return self._search_result(search, capacity, 0)

    def _tree_info(self, fd, name):
        info = os.stat(name, dir_fd=fd, follow_symlinks=False)
        if (info.st_dev != os.fstat(self.root).st_dev or
                not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)) or
                (stat.S_ISREG(info.st_mode) and info.st_nlink != 1)):
            raise PermissionError(errno.EACCES, 'non-private deletion tree')
        return info

    @staticmethod
    def _open_child(fd, name, info):
        child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                        os.O_CLOEXEC, dir_fd=fd)
        try:
            actual = os.fstat(child)
            if (actual.st_ino, actual.st_dev) != (info.st_ino, info.st_dev):
                raise PermissionError(errno.EACCES, 'deletion tree changed')
        except BaseException:
            os.close(child)
            raise
        return child

    def _tree_preflight(self, fd, depth):
        if depth >= MAX_TREE_DEPTH:
            raise UnsupportedOperation('deletion tree exceeds bounded depth')
        with os.scandir(fd) as entries:
            for entry in entries:
                info = self._tree_info(fd, entry.name)
                if stat.S_ISDIR(info.st_mode):
                    child = self._open_child(fd, entry.name, info)
                    try:
                        self._tree_preflight(child, depth + 1)
                    finally:
                        os.close(child)

    def _recursive_delete(self, fd, flags):
        os.lseek(fd, 0, os.SEEK_SET)
        count = 0
        with os.scandir(fd) as entries:
            for entry in entries:
                info = self._tree_info(fd, entry.name)
                if stat.S_ISDIR(info.st_mode):
                    if flags & 16:
                        child = self._open_child(fd, entry.name, info)
                        try:
                            count += self._recursive_delete(child, flags)
                        finally:
                            os.close(child)
                    if flags & 8:
                        os.rmdir(entry.name, dir_fd=fd)
                        count += 1
                elif flags & 4:
                    os.unlink(entry.name, dir_fd=fd)
                    count += 1
        return count

    def _xdelete(self, path, flags):
        if not isinstance(flags, int) or not flags & 0x1c or flags & ~0x1c:
            return -2
        if len(self._path(path)) <= 1:
            raise PermissionError(errno.EACCES, 'drive root deletion rejected')
        parent, name = self._parent(path)
        child = None
        try:
            info = self._tree_info(parent, name)
            if not stat.S_ISDIR(info.st_mode):
                return -2
            child = self._open_child(parent, name, info)
            self._tree_preflight(child, 0)
            count = self._recursive_delete(child, flags)
            if flags & 0x18 == 0x18:
                os.rmdir(name, dir_fd=parent)
                count += 1
            return count
        finally:
            if child is not None:
                os.close(child)
            os.close(parent)

    def _cmpt(self, path, control):
        if not isinstance(control, bytes) or len(control) != 40:
            raise ValueError('CMPT_Read requires ten u32 words')
        words = struct.unpack('<10I', control)
        mask, flags = words[0], words[3]
        if mask & ~0x1f:
            raise UnsupportedOperation('CMPT_Read mask outside the known stages')
        # Bit 0 is mandatory in the vendor helper; without it the constant
        # {completed=0, error=-18} is serialized and no file is touched.
        if not mask & 1:
            return struct.pack('<Ii', 0, -18), 0, 0, b''
        completed, size, data, handle = 0, 0, b'', None
        error = 0
        try:
            handle = self._open(path, flags)
            if handle < 0:
                error = handle
            else:
                completed |= 1
                fd = self.handles[handle][0]
                if mask & 2:
                    size = self._regular(fd).st_size
                    if size > 0xffffffff:
                        raise OSError(errno.EOVERFLOW, 'size exceeds wire field')
                    completed |= 2
                if mask & 4:
                    offset = struct.unpack('<i', control[20:24])[0]
                    os.lseek(fd, offset, words[6])
                    completed |= 4
                if mask & 8:
                    data = os.read(fd, words[8])
                    completed |= 8
        except OSError as exc:
            error = fs_error(exc)
        finally:
            if handle is not None and handle > 0:
                close_error = self._close(handle)
                if not error and mask & 16:
                    if close_error:
                        error = close_error
                    else:
                        completed |= 16
        return struct.pack('<Ii', completed, error), size, len(data), data

    def operate(self, generation, op, *args):
        """Structured API. Invalid input raises; known host errors become FS codes.

        A stale/stopped generation always returns (-10,) without I/O. Unknown
        semantics raise UnsupportedOperation so a future transport must stop,
        rather than pretend that a guessed vendor response was sent.
        """
        if not self.active or generation != self.generation:
            return (-10,)
        try:
            if op == 0x1001:
                return (self._open(*args),)
            if op == 0x1005:
                return (self._close(*args),)
            if op == 0x1014:
                return (self._close(*args, sync=False),)
            if op == 0x1013:
                return self._find_next(*args)
            if op == 0x1018:
                return (self._xdelete(*args),)
            if op in (0x1002, 0x1003, 0x1004, 0x1009):
                handle, *rest = args
                if handle not in self.handles:
                    return {0x1003: (-10, 0, b''),
                            0x1004: (-10, 0),
                            0x1009: (-10, 0)}.get(op, (-10,))
                fd = self.handles[handle][0]
                if op == 0x1002:
                    offset, whence = rest
                    if whence not in (0, 1, 2) or not -(1 << 31) <= offset < (1 << 31):
                        raise ValueError('invalid seek')
                    position = os.lseek(fd, offset, whence)
                    if position > 0x7fffffff:
                        raise OSError(errno.EOVERFLOW, 'seek exceeds signed reply')
                    return (position,)
                if op == 0x1003:
                    length, = rest
                    self._length(length)
                    data = os.read(fd, length)
                    return 0, len(data), data
                if op == 0x1004:
                    data, length = rest
                    self._length(length)
                    if not isinstance(data, bytes) or length != len(data):
                        raise ValueError('write length mismatch')
                    return 0, os.write(fd, data)
                if rest:
                    raise ValueError('unexpected size arguments')
                size = os.fstat(fd).st_size
                if size > 0xffffffff:
                    raise OSError(errno.EOVERFLOW, 'size exceeds wire field')
                return 0, size
            if op == 0x100e:
                return self._disk_info(*args)
            if op == 0x1010:
                path, = args
                return (self._attributes(path),)
            if op == 0x1021:
                return (self._restore(*args),)
            if op == 0x1007:
                path, = args
                root = self._drive_root(path)
                if root is not None:
                    os.mkdir(root, 0o770, dir_fd=self.root)
                    return (0,)
                parent, name = self._parent(path)
                try:
                    os.mkdir(name, 0o770, dir_fd=parent)
                finally:
                    os.close(parent)
                return (0,)
            if op == 0x101a:
                if args == (4, 2, 12):
                    return (ord('Z'),)
                if args == (8, 2, 12):
                    return (-2,)
                raise UnsupportedOperation('GetDrive query tuple unresolved')
            if op == 0x101c:
                if args:
                    raise ValueError('SetDiskFlag has no arguments')
                return (0,)
            if op == 0x1022:
                return self._cmpt(*args)
            if op == 0x1025:
                path, = args
                parent, name = self._parent(path)
                try:
                    info = os.stat(name, dir_fd=parent, follow_symlinks=False)
                finally:
                    os.close(parent)
                if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
                    raise PermissionError(errno.EACCES, 'non-private file')
                times = (info.st_mtime_ns // 1000000000,
                         info.st_atime_ns // 1000000000,
                         info.st_ctime_ns // 1000000000)
                return 0, struct.pack('<3Q', *(value & 0xffffffffffffffff
                                              for value in times))
            if op == 0x1012:
                return self._find_first(*args)
            if op == 0x100c:
                return (self._move(*args),)
            raise UnsupportedOperation('operation not implemented')
        except OSError as exc:
            error = fs_error(exc)
            if op == 0x1003:
                return error, 0, b''
            if op in (0x1004, 0x1009):
                return error, 0
            if op == 0x1025:
                return error, bytes(24)
            if op in (0x1012, 0x1013):
                return error, bytes(52), b''
            return (error,)

    def shutdown(self):
        """Revoke generation first, close/fsync every handle, return first error."""
        self.active = False
        error = 0
        for handle in list(self.handles) + list(self.searches):
            result = self._close(handle)
            if not error:
                error = result
        if self.root >= 0:
            os.close(self.root)
            self.root = -1
        return error
