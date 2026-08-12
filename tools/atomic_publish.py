#!/usr/bin/env python3
"""Publish one staged file or directory with an atomic rename."""

from __future__ import annotations

import argparse
import ctypes
import errno
import os
import stat
import sys
from pathlib import Path

RENAME_NOREPLACE = 1
RENAME_EXCL = 0x00000004


def call_rename(function: object, *arguments: object) -> None:
    """Call a C rename function and raise its system error."""
    result = function(*arguments)  # type: ignore[operator]
    if result == 0:
        return
    error = ctypes.get_errno()
    raise OSError(error, os.strerror(error))


def posix_publish(source: Path, destination: Path, replace: bool) -> None:
    """Publish through bound POSIX directory descriptors."""
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
    source_directory = os.open(source.parent, directory_flags)
    try:
        destination_directory = os.open(destination.parent, directory_flags)
        try:
            source_status = os.stat(
                source.name,
                dir_fd=source_directory,
                follow_symlinks=False,
            )
            source_is_file = stat.S_ISREG(source_status.st_mode)
            source_is_directory = stat.S_ISDIR(source_status.st_mode)
            if not source_is_file and not source_is_directory:
                raise ValueError("the staged object must be a regular file or directory")
            if replace and not source_is_file:
                raise ValueError("replacement publication supports only regular files")
            if source_status.st_dev != os.fstat(destination_directory).st_dev:
                raise ValueError("the staged object and destination must use one filesystem")

            source_name = os.fsencode(source.name)
            destination_name = os.fsencode(destination.name)
            flags = 0 if replace else RENAME_NOREPLACE
            if sys.platform.startswith("linux"):
                library = ctypes.CDLL(None, use_errno=True)
                function = getattr(library, "renameat2", None)
                if function is None:
                    raise OSError(errno.ENOTSUP, "renameat2 is unavailable")
                function.argtypes = [
                    ctypes.c_int,
                    ctypes.c_char_p,
                    ctypes.c_int,
                    ctypes.c_char_p,
                    ctypes.c_uint,
                ]
                function.restype = ctypes.c_int
                call_rename(
                    function,
                    source_directory,
                    source_name,
                    destination_directory,
                    destination_name,
                    flags,
                )
                return

            if sys.platform == "darwin":
                library = ctypes.CDLL(None, use_errno=True)
                function = getattr(library, "renameatx_np", None)
                if function is None:
                    raise OSError(errno.ENOTSUP, "renameatx_np is unavailable")
                function.argtypes = [
                    ctypes.c_int,
                    ctypes.c_char_p,
                    ctypes.c_int,
                    ctypes.c_char_p,
                    ctypes.c_uint,
                ]
                function.restype = ctypes.c_int
                call_rename(
                    function,
                    source_directory,
                    source_name,
                    destination_directory,
                    destination_name,
                    0 if replace else RENAME_EXCL,
                )
                return

            if replace:
                os.replace(
                    source.name,
                    destination.name,
                    src_dir_fd=source_directory,
                    dst_dir_fd=destination_directory,
                )
                return
            raise OSError(errno.ENOTSUP, "atomic no-replace rename is unavailable")
        finally:
            os.close(destination_directory)
    finally:
        os.close(source_directory)


def windows_publish(source: Path, destination: Path, replace: bool) -> None:
    """Publish through bound Windows handles."""
    from ctypes import wintypes

    delete_access = 0x00010000
    file_list_directory = 0x0001
    file_read_attributes = 0x0080
    file_share_read = 0x00000001
    file_share_write = 0x00000002
    file_share_delete = 0x00000004
    open_existing = 3
    file_attribute_directory = 0x00000010
    file_attribute_reparse_point = 0x00000400
    file_type_disk = 0x0001
    file_flag_backup_semantics = 0x02000000
    file_flag_open_reparse_point = 0x00200000
    file_rename_info_class = 3
    invalid_handle = ctypes.c_void_p(-1).value

    class FileInformation(ctypes.Structure):
        _fields_ = [
            ("attributes", wintypes.DWORD),
            ("creation_time", wintypes.FILETIME),
            ("access_time", wintypes.FILETIME),
            ("write_time", wintypes.FILETIME),
            ("volume_serial", wintypes.DWORD),
            ("size_high", wintypes.DWORD),
            ("size_low", wintypes.DWORD),
            ("links", wintypes.DWORD),
            ("index_high", wintypes.DWORD),
            ("index_low", wintypes.DWORD),
        ]

    class RenameInformation(ctypes.Structure):
        _fields_ = [
            ("replace", wintypes.BOOL),
            ("root_directory", wintypes.HANDLE),
            ("name_length", wintypes.DWORD),
            ("name", wintypes.WCHAR * 1),
        ]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    create_file = kernel.CreateFileW
    create_file.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    create_file.restype = wintypes.HANDLE
    get_information = kernel.GetFileInformationByHandle
    get_information.argtypes = [wintypes.HANDLE, ctypes.POINTER(FileInformation)]
    get_information.restype = wintypes.BOOL
    get_file_type = kernel.GetFileType
    get_file_type.argtypes = [wintypes.HANDLE]
    get_file_type.restype = wintypes.DWORD
    set_information = kernel.SetFileInformationByHandle
    set_information.argtypes = [
        wintypes.HANDLE,
        ctypes.c_int,
        wintypes.LPVOID,
        wintypes.DWORD,
    ]
    set_information.restype = wintypes.BOOL
    close_handle = kernel.CloseHandle
    close_handle.argtypes = [wintypes.HANDLE]
    close_handle.restype = wintypes.BOOL

    directory_handle = create_file(
        str(destination.parent),
        file_list_directory | file_read_attributes,
        file_share_read | file_share_write | file_share_delete,
        None,
        open_existing,
        file_flag_backup_semantics | file_flag_open_reparse_point,
        None,
    )
    if directory_handle == invalid_handle:
        raise ctypes.WinError(ctypes.get_last_error())
    source_handle = create_file(
        str(source),
        delete_access | file_read_attributes,
        file_share_read,
        None,
        open_existing,
        file_flag_backup_semantics | file_flag_open_reparse_point,
        None,
    )
    if source_handle == invalid_handle:
        error = ctypes.get_last_error()
        close_handle(directory_handle)
        raise ctypes.WinError(error)
    try:
        source_information = FileInformation()
        directory_information = FileInformation()
        if not get_information(source_handle, ctypes.byref(source_information)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not get_information(directory_handle, ctypes.byref(directory_information)):
            raise ctypes.WinError(ctypes.get_last_error())
        if source_information.attributes & file_attribute_reparse_point:
            raise ValueError("the staged object must not be a Windows reparse point")
        if directory_information.attributes & file_attribute_reparse_point:
            raise ValueError("the destination directory must not be a Windows reparse point")
        if not directory_information.attributes & file_attribute_directory:
            raise ValueError("the publication destination parent must be a directory")
        if get_file_type(source_handle) != file_type_disk:
            raise ValueError("the staged object must be a disk file or directory")
        source_is_directory = bool(source_information.attributes & file_attribute_directory)
        if replace and source_is_directory:
            raise ValueError("replacement publication supports only regular files")
        if source_information.volume_serial != directory_information.volume_serial:
            raise ValueError("the staged object and destination must use one filesystem")

        encoded_name = destination.name.encode("utf-16-le")
        name_offset = RenameInformation.name.offset
        information_size = name_offset + len(encoded_name)
        storage = ctypes.create_string_buffer(
            max(ctypes.sizeof(RenameInformation), information_size)
        )
        rename_information = RenameInformation.from_buffer(storage)
        rename_information.replace = replace
        rename_information.root_directory = directory_handle
        rename_information.name_length = len(encoded_name)
        ctypes.memmove(ctypes.addressof(storage) + name_offset, encoded_name, len(encoded_name))
        if not set_information(
            source_handle,
            file_rename_info_class,
            storage,
            information_size,
        ):
            error = ctypes.get_last_error()
            if error in {80, 183}:
                raise FileExistsError(error, "the publication destination already exists")
            raise ctypes.WinError(error)
    finally:
        close_handle(source_handle)
        close_handle(directory_handle)


def publish(source: Path, destination: Path, replace: bool) -> None:
    """Validate and publish one staged object."""
    if source.name in {"", ".", ".."} or destination.name in {"", ".", ".."}:
        raise ValueError("the source and destination must name one object")
    if os.name == "nt":
        if ":" in source.name or ":" in destination.name:
            raise ValueError("Windows alternate data streams are not valid publication paths")
        windows_publish(source, destination, replace)
        return
    if os.name == "posix":
        posix_publish(source, destination, replace)
        return
    raise OSError(errno.ENOTSUP, "atomic publication is unavailable")


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="staged file or directory")
    parser.add_argument("destination", type=Path, help="final path")
    parser.add_argument(
        "--replace",
        action="store_true",
        help="atomically replace one regular destination file",
    )
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    options = parse_args(arguments)
    try:
        publish(options.source, options.destination, options.replace)
    except FileExistsError:
        print("error: the publication destination already exists", file=sys.stderr)
        return 3
    except (OSError, ValueError) as error:
        print(f"error: atomic publication failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
