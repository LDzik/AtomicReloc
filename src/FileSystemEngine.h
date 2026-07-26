#pragma once

#include "Common.h"

// Define REPARSE_DATA_BUFFER custom structure to prevent compilation errors
// when compiling on standard MSVC installations without the Windows Driver Kit (WDK).
#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x000900A8
#endif

#ifndef FSCTL_SET_REPARSE_POINT
#define FSCTL_SET_REPARSE_POINT 0x000900A4
#endif

#ifndef FSCTL_DELETE_REPARSE_POINT
#define FSCTL_DELETE_REPARSE_POINT 0x000900AC
#endif

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#endif

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif

// Reparse Data Buffer layout for mount points (junctions) and symbolic links
typedef struct _REPARSE_DATA_BUFFER
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union
    {
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
        struct
        {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    } DUMMYUNIONNAME;
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

// Allocation size for maximum reparse buffer limits
#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 * 1024)
#endif

enum class RedirectStrategy
{
    Junction,
    SymbolicLink,
    HardLink,
    Restore
};

namespace FileSystemEngine
{

    // Verifies if Developer Mode is enabled in Windows Registry using strict KEY_READ to avoid UAC blocks.
    bool IsDeveloperModeEnabled();

    // Verifies if a given path is an active Reparse Point (Junction or Symbolic Link).
    bool IsReparsePoint(const std::wstring &path);

    // Queries standard volume info to see if source and destination exist on the same physical volume.
    bool IsSameVolume(const std::wstring &path1, const std::wstring &path2);

    // Resolves a single junction's physical target.
    bool GetJunctionTarget(const std::wstring &junctionPath, std::wstring &outTarget);

    // Creates an NTFS Directory Junction pointing from junctionPath to targetPath.
    bool CreateJunction(const std::wstring &junctionPath, const std::wstring &targetPath);

    // Removes the Directory Junction safely by deleting the directory (standard NTFS behavior leaves target intact).
    bool DeleteJunction(const std::wstring &junctionPath);

    // Creates a Symbolic Link (directory or file) appending Developer Mode unprivileged flags if enabled.
    bool CreateSymlink(const std::wstring &symlinkPath, const std::wstring &targetPath, bool isDirectory);

    // Creates an NTFS Hard Link (valid only on the same volume).
    bool CreateHardlink(const std::wstring &hardlinkPath, const std::wstring &targetPath);

    // Smart Junction Chain Resolution
    bool ResolveTruePath(const std::wstring &path, std::wstring &outTruePath, std::vector<std::wstring> &outGhostChain);

    // Utility: Checks if a folder exists.
    bool DirectoryExists(const std::wstring &path);

    // Utility: Checks if a file exists.
    bool FileExists(const std::wstring &path);

    // Utility: Creates a folder and all its parent subdirectories recursively.
    bool CreateDirectoryRecursive(const std::wstring &path);

    // Utility: Recursively purges a folder and all its contents (used during bak cleanup / rollback).
    bool DeleteDirectoryRecursive(const std::wstring &path);

    // Helper: Normalized NT path prefix stripping (e.g. converting `\??\C:\App` or `\\?\C:\App` to `C:\App`).
    std::wstring NormalizePath(const std::wstring &path);

    // Collapses redundant backslashes, normalizes slashes, preserves UNC prefixes.
    std::wstring SanitizePath(const std::wstring &path);

    // Joins base and child paths safely. If base path ends in a backslash, avoids appending another one.
    std::wstring JoinPath(const std::wstring &base, const std::wstring &child);

    // Fully resolves any path to its absolute physical target, resolving junctions/symlinks at any level using GetFinalPathNameByHandleW.
    std::wstring GetFullyResolvedPhysicalPath(const std::wstring &path);
}
