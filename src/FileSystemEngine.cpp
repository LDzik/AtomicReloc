#include "FileSystemEngine.h"

namespace FileSystemEngine
{

    // Helper: Normalized NT path prefix stripping (e.g. converting `\??\C:\App` or `\\?\C:\App` to `C:\App`)
    std::wstring NormalizePath(const std::wstring &path)
    {
        std::wstring normalized = path;

        // Convert any forward slashes to backslashes
        std::replace(normalized.begin(), normalized.end(), L'/', L'\\');

        // Check for NT namespaces
        if (normalized.rfind(L"\\??\\", 0) == 0)
        {
            normalized = normalized.substr(4);
        }
        else if (normalized.rfind(L"\\\\?\\", 0) == 0)
        {
            normalized = normalized.substr(4);
        }

        // Collapse double backslashes (e.g., E:\\ -> E:\), but preserve UNC/NT prefix if it wasn't stripped.
        size_t startPos = 0;
        if (normalized.rfind(L"\\\\", 0) == 0)
        {
            startPos = 2;
        }
        size_t pos;
        while ((pos = normalized.find(L"\\\\", startPos)) != std::wstring::npos)
        {
            normalized.replace(pos, 2, L"\\");
        }

        // Strip trailing backslashes unless it's a drive root (e.g. "C:\")
        while (normalized.length() > 3 && normalized.back() == L'\\')
        {
            normalized.pop_back();
        }

        return normalized;
    }

    std::wstring SanitizePath(const std::wstring &path)
    {
        std::wstring sanitized = path;

        // Convert any forward slashes to backslashes
        std::replace(sanitized.begin(), sanitized.end(), L'/', L'\\');

        // Collapse double backslashes (e.g., E:\\ -> E:\), but preserve UNC/NT prefix if it starts with \\ or \??\ prefix.
        size_t startPos = 0;
        if (sanitized.rfind(L"\\\\?\\", 0) == 0)
        {
            startPos = 4;
        }
        else if (sanitized.rfind(L"\\??\\", 0) == 0)
        {
            startPos = 4;
        }
        else if (sanitized.rfind(L"\\\\", 0) == 0)
        {
            startPos = 2;
        }

        size_t pos;
        while ((pos = sanitized.find(L"\\\\", startPos)) != std::wstring::npos)
        {
            sanitized.replace(pos, 2, L"\\");
        }

        // Strip trailing backslashes unless it's a drive root (e.g. "C:\")
        while (sanitized.length() > 3 && sanitized.back() == L'\\')
        {
            sanitized.pop_back();
        }

        return sanitized;
    }

    std::wstring JoinPath(const std::wstring &base, const std::wstring &child)
    {
        if (base.empty())
            return child;
        if (child.empty())
            return base;

        std::wstring sanitizedBase = SanitizePath(base);
        std::wstring sanitizedChild = child;

        // Strip leading backslash from child to avoid duplication
        while (!sanitizedChild.empty() && sanitizedChild.front() == L'\\')
        {
            sanitizedChild = sanitizedChild.substr(1);
        }

        if (sanitizedBase.back() == L'\\')
        {
            return sanitizedBase + sanitizedChild;
        }
        else
        {
            return sanitizedBase + L"\\" + sanitizedChild;
        }
    }

    bool IsDeveloperModeEnabled()
    {
        HKEY hKey = nullptr;
        // Strict read-only access KEY_READ prevents UAC access-denied failures for non-admin users
        LSTATUS status = ::RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
            0,
            KEY_READ,
            &hKey);

        if (status != ERROR_SUCCESS)
        {
            // If the registry key doesn't exist, Developer Mode is disabled by default
            return false;
        }

        DWORD dwValue = 0;
        DWORD dwType = REG_DWORD;
        DWORD cbData = sizeof(dwValue);

        status = ::RegQueryValueExW(
            hKey,
            L"AllowDevelopmentWithoutDevLicense",
            nullptr,
            &dwType,
            reinterpret_cast<LPBYTE>(&dwValue),
            &cbData);

        ::RegCloseKey(hKey);

        if (status == ERROR_SUCCESS && dwType == REG_DWORD)
        {
            return (dwValue == 1);
        }

        return false;
    }

    bool IsReparsePoint(const std::wstring &path)
    {
        DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            return false;
        }
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    bool IsSameVolume(const std::wstring &path1, const std::wstring &path2)
    {
        wchar_t vol1[MAX_PATH] = {0};
        wchar_t vol2[MAX_PATH] = {0};

        if (!::GetVolumePathNameW(path1.c_str(), vol1, MAX_PATH))
        {
            Logger::Log(Logger::Level::Error, L"Failed to resolve volume name for: " + path1 + L". Error: " + GetLastErrorAsString());
            return false;
        }

        if (!::GetVolumePathNameW(path2.c_str(), vol2, MAX_PATH))
        {
            Logger::Log(Logger::Level::Error, L"Failed to resolve volume name for: " + path2 + L". Error: " + GetLastErrorAsString());
            return false;
        }

        return (_wcsicmp(vol1, vol2) == 0);
    }

    bool GetJunctionTarget(const std::wstring &junctionPath, std::wstring &outTarget)
    {
        HANDLE hFile = ::CreateFileW(
            junctionPath.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            NULL);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            Logger::Log(Logger::Level::Error, L"Failed to open reparse point: " + junctionPath + L". Error: " + GetLastErrorAsString());
            return false;
        }

        // Safe vector-allocation for the maximum reparse data buffer limit
        std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE, 0);
        DWORD bytesReturned = 0;

        BOOL success = ::DeviceIoControl(
            hFile,
            FSCTL_GET_REPARSE_POINT,
            NULL,
            0,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesReturned,
            NULL);

        ::CloseHandle(hFile);

        if (!success)
        {
            Logger::Log(Logger::Level::Error, L"DeviceIoControl FSCTL_GET_REPARSE_POINT failed for: " + junctionPath + L". Error: " + GetLastErrorAsString());
            return false;
        }

        PREPARSE_DATA_BUFFER rdb = reinterpret_cast<PREPARSE_DATA_BUFFER>(buffer.data());

        // Standard Directory Junction (Mount Point) Check
        if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
        {
            USHORT subOffset = rdb->MountPointReparseBuffer.SubstituteNameOffset;
            USHORT subLen = rdb->MountPointReparseBuffer.SubstituteNameLength;

            // Defensive bounds-checking to prevent memory access violations
            size_t pathBufferMaxBytes = bytesReturned - offsetof(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer);
            if (static_cast<size_t>(subOffset + subLen) > pathBufferMaxBytes)
            {
                Logger::Log(Logger::Level::Error, L"Junction path buffer offset is out of bounds for: " + junctionPath);
                return false;
            }

            wchar_t *rawPath = rdb->MountPointReparseBuffer.PathBuffer + (subOffset / sizeof(wchar_t));
            size_t charCount = subLen / sizeof(wchar_t);

            std::wstring targetPath(rawPath, charCount);
            outTarget = NormalizePath(targetPath);
            return true;
        }
        // Fallback or secondary check: Symbolic Link
        else if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK)
        {
            USHORT subOffset = rdb->SymbolicLinkReparseBuffer.SubstituteNameOffset;
            USHORT subLen = rdb->SymbolicLinkReparseBuffer.SubstituteNameLength;

            size_t pathBufferMaxBytes = bytesReturned - offsetof(REPARSE_DATA_BUFFER, SymbolicLinkReparseBuffer.PathBuffer);
            if (static_cast<size_t>(subOffset + subLen) > pathBufferMaxBytes)
            {
                Logger::Log(Logger::Level::Error, L"Symlink path buffer offset is out of bounds for: " + junctionPath);
                return false;
            }

            wchar_t *rawPath = rdb->SymbolicLinkReparseBuffer.PathBuffer + (subOffset / sizeof(wchar_t));
            size_t charCount = subLen / sizeof(wchar_t);

            std::wstring targetPath(rawPath, charCount);
            outTarget = NormalizePath(targetPath);
            return true;
        }

        Logger::Log(Logger::Level::Warning, L"Path is a reparse point but not a supported Junction or Symlink: " + junctionPath);
        return false;
    }

    bool CreateJunction(const std::wstring &junctionPath, const std::wstring &targetPath)
    {
        // Enforce the creation of an empty directory structure first
        if (!DirectoryExists(junctionPath))
        {
            if (!::CreateDirectoryW(junctionPath.c_str(), NULL))
            {
                DWORD err = ::GetLastError();
                if (err != ERROR_ALREADY_EXISTS)
                {
                    Logger::Log(Logger::Level::Error, L"Failed to create empty directory for junction target: " + junctionPath + L". Error: " + GetLastErrorAsString(err));
                    return false;
                }
            }
        }

        // Open the directory handle for reparse modifications
        HANDLE hDir = ::CreateFileW(
            junctionPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            NULL);

        if (hDir == INVALID_HANDLE_VALUE)
        {
            Logger::Log(Logger::Level::Error, L"Failed to acquire folder handle for setting junction: " + junctionPath + L". Error: " + GetLastErrorAsString());
            return false;
        }

        // Set up kernel NT path namespaces
        std::wstring substituteName = L"\\??\\" + targetPath;
        std::wstring printName = targetPath;

        size_t substituteNameLenBytes = substituteName.length() * sizeof(wchar_t);
        size_t printNameLenBytes = printName.length() * sizeof(wchar_t);

        // Preallocate exact structure buffer size to fit strings & terminating nulls
        std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE, 0);
        PREPARSE_DATA_BUFFER rdb = reinterpret_cast<PREPARSE_DATA_BUFFER>(buffer.data());

        rdb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
        rdb->Reserved = 0;

        USHORT subOffset = 0;
        USHORT subLen = static_cast<USHORT>(substituteNameLenBytes);
        USHORT printOffset = static_cast<USHORT>(subLen + sizeof(wchar_t)); // Include space for null char
        USHORT printLen = static_cast<USHORT>(printNameLenBytes);

        // Verify structure bounds
        size_t totalStringBytes = printOffset + printLen + sizeof(wchar_t);
        if (totalStringBytes > (MAXIMUM_REPARSE_DATA_BUFFER_SIZE - offsetof(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer)))
        {
            ::CloseHandle(hDir);
            Logger::Log(Logger::Level::Error, L"Path is too long to fit in reparse buffer limits: " + targetPath);
            return false;
        }

        rdb->MountPointReparseBuffer.SubstituteNameOffset = subOffset;
        rdb->MountPointReparseBuffer.SubstituteNameLength = subLen;
        rdb->MountPointReparseBuffer.PrintNameOffset = printOffset;
        rdb->MountPointReparseBuffer.PrintNameLength = printLen;

        // Copy actual string paths into the variable PathBuffer array
        std::memcpy(rdb->MountPointReparseBuffer.PathBuffer + (subOffset / sizeof(wchar_t)), substituteName.c_str(), subLen);
        rdb->MountPointReparseBuffer.PathBuffer[subOffset / sizeof(wchar_t) + subLen / sizeof(wchar_t)] = L'\0';

        std::memcpy(rdb->MountPointReparseBuffer.PathBuffer + (printOffset / sizeof(wchar_t)), printName.c_str(), printLen);
        rdb->MountPointReparseBuffer.PathBuffer[printOffset / sizeof(wchar_t) + printLen / sizeof(wchar_t)] = L'\0';

        // MountPointReparseBuffer starts at 8th byte in REPARSE_DATA_BUFFER.
        // ReparseDataLength represents the bytes size from that point onwards.
        rdb->ReparseDataLength = static_cast<USHORT>(8 + totalStringBytes);

        DWORD bytesReturned = 0;
        BOOL success = ::DeviceIoControl(
            hDir,
            FSCTL_SET_REPARSE_POINT,
            rdb,
            rdb->ReparseDataLength + 8, // Total size of REPARSE_DATA_BUFFER
            NULL,
            0,
            &bytesReturned,
            NULL);

        ::CloseHandle(hDir);

        if (!success)
        {
            Logger::Log(Logger::Level::Error, L"Failed to write Junction reparse point to " + junctionPath + L". Error: " + GetLastErrorAsString());
            return false;
        }

        return true;
    }

    bool DeleteJunction(const std::wstring &junctionPath)
    {
        // Standard Windows NTFS behavior: calling RemoveDirectoryW on a Directory Junction
        // will remove the mount point folder structure cleanly, leaving the actual target contents fully untouched.
        ::SetFileAttributesW(junctionPath.c_str(), FILE_ATTRIBUTE_NORMAL);

        if (!::RemoveDirectoryW(junctionPath.c_str()))
        {
            Logger::Log(Logger::Level::Error, L"Failed to delete Junction folder: " + junctionPath + L". Error: " + GetLastErrorAsString());
            return false;
        }
        return true;
    }

    bool CreateSymlink(const std::wstring &symlinkPath, const std::wstring &targetPath, bool isDirectory)
    {
        DWORD dwFlags = isDirectory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;

        // Append Developer Mode unprivileged link flag if enabled by user/registry
        if (IsDeveloperModeEnabled())
        {
            dwFlags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        }

        if (!::CreateSymbolicLinkW(symlinkPath.c_str(), targetPath.c_str(), dwFlags))
        {
            DWORD err = ::GetLastError();
            Logger::Log(Logger::Level::Error, L"Failed to create Symbolic Link: " + symlinkPath + L" -> " + targetPath + L". Error: " + GetLastErrorAsString(err));
            return false;
        }
        return true;
    }

    bool CreateHardlink(const std::wstring &hardlinkPath, const std::wstring &targetPath)
    {
        if (!::CreateHardLinkW(hardlinkPath.c_str(), targetPath.c_str(), NULL))
        {
            Logger::Log(Logger::Level::Error, L"Failed to create Hard Link: " + hardlinkPath + L" -> " + targetPath + L". Error: " + GetLastErrorAsString());
            return false;
        }
        return true;
    }

    bool ResolveTruePath(const std::wstring &path, std::wstring &outTruePath, std::vector<std::wstring> &outGhostChain)
    {
        outGhostChain.clear();
        std::wstring currentPath = NormalizePath(path);
        int iteration = 0;

        while (IsReparsePoint(currentPath))
        {
            outGhostChain.push_back(currentPath); // Record the ghost

            std::wstring nextTarget;
            if (!GetJunctionTarget(currentPath, nextTarget))
            {
                Logger::Log(Logger::Level::Error, L"Unable to follow intermediate reparse point: " + currentPath);
                return false;
            }
            currentPath = NormalizePath(nextTarget);

            if (++iteration >= 10)
                return false;
        }

        outTruePath = currentPath;
        return true;
    }

    bool DirectoryExists(const std::wstring &path)
    {
        DWORD attributes = ::GetFileAttributesW(path.c_str());
        return (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
    }

    bool FileExists(const std::wstring &path)
    {
        DWORD attributes = ::GetFileAttributesW(path.c_str());
        return (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY));
    }

    bool CreateDirectoryRecursive(const std::wstring &path)
    {
        if (DirectoryExists(path))
        {
            return true;
        }

        std::wstring normalized = NormalizePath(path);
        size_t pos = 0;

        while ((pos = normalized.find(L'\\', pos + 1)) != std::wstring::npos)
        {
            std::wstring subPath = normalized.substr(0, pos);
            if (!DirectoryExists(subPath))
            {
                if (!::CreateDirectoryW(subPath.c_str(), NULL))
                {
                    DWORD err = ::GetLastError();
                    if (err != ERROR_ALREADY_EXISTS)
                    {
                        Logger::Log(Logger::Level::Error, L"Failed to recursively create subdirectory: " + subPath + L". Error: " + GetLastErrorAsString(err));
                        return false;
                    }
                }
            }
        }

        if (!::CreateDirectoryW(normalized.c_str(), NULL))
        {
            DWORD err = ::GetLastError();
            if (err != ERROR_ALREADY_EXISTS)
            {
                Logger::Log(Logger::Level::Error, L"Failed to create directory: " + normalized + L". Error: " + GetLastErrorAsString(err));
                return false;
            }
        }

        return DirectoryExists(normalized);
    }

    bool DeleteDirectoryRecursive(const std::wstring &path)
    {
        std::wstring searchPath = NormalizePath(path) + L"\\*";
        WIN32_FIND_DATAW findData;
        HANDLE hFind = ::FindFirstFileW(searchPath.c_str(), &findData);

        if (hFind == INVALID_HANDLE_VALUE)
        {
            DWORD err = ::GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            Logger::Log(Logger::Level::Error, L"Failed to start recursive directory search: " + path + L". Error: " + GetLastErrorAsString(err));
            return false;
        }

        bool success = true;

        do
        {
            std::wstring name = findData.cFileName;

            // Ignore standard self/parent folder descriptors
            if (name == L"." || name == L"..")
            {
                continue;
            }

            std::wstring fullItemPath = NormalizePath(path) + L"\\" + name;

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // If it is a reparse point directory junction, delete the link ONLY
                if (IsReparsePoint(fullItemPath))
                {
                    if (!DeleteJunction(fullItemPath))
                    {
                        success = false;
                    }
                }
                else
                {
                    if (!DeleteDirectoryRecursive(fullItemPath))
                    {
                        success = false;
                    }
                }
            }
            else
            {
                // Set to Normal to clear read-only constraints before unlinking
                ::SetFileAttributesW(fullItemPath.c_str(), FILE_ATTRIBUTE_NORMAL);

                if (!::DeleteFileW(fullItemPath.c_str()))
                {
                    Logger::Log(Logger::Level::Error, L"Failed to delete file: " + fullItemPath + L". Error: " + GetLastErrorAsString());
                    success = false;
                }
            }

        } while (::FindNextFileW(hFind, &findData));

        ::FindClose(hFind);

        if (!success)
        {
            return false;
        }

        // Delete the empty directory itself
        if (!::RemoveDirectoryW(path.c_str()))
        {
            Logger::Log(Logger::Level::Error, L"Failed to delete directory: " + path + L". Error: " + GetLastErrorAsString());
            return false;
        }

        return true;
    }

    std::wstring GetFullyResolvedPhysicalPath(const std::wstring &path)
    {
        if (path.empty())
            return L"";

        // 1. First get the normalized full path
        wchar_t fullPath[MAX_PATH * 2] = {0};
        DWORD len = ::GetFullPathNameW(path.c_str(), MAX_PATH * 2, fullPath, nullptr);
        std::wstring normalized = (len > 0) ? std::wstring(fullPath, len) : path;
        normalized = NormalizePath(normalized);

        // Strip trailing backslash for processing
        while (normalized.length() > 3 && normalized.back() == L'\\')
        {
            normalized.pop_back();
        }

        // 2. Try opening the path directly (if it exists)
        HANDLE hFile = ::CreateFileW(
            normalized.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, // required to open directories
            NULL);

        if (hFile != INVALID_HANDLE_VALUE)
        {
            wchar_t finalPath[MAX_PATH * 2] = {0};
            DWORD finalLen = ::GetFinalPathNameByHandleW(hFile, finalPath, MAX_PATH * 2, VOLUME_NAME_DOS);
            ::CloseHandle(hFile);
            if (finalLen > 0 && finalLen < MAX_PATH * 2)
            {
                normalized = NormalizePath(std::wstring(finalPath, finalLen));
            }
        }
        else
        {
            // 3. If it doesn't exist, try resolving parent directories recursively
            size_t lastSlash = normalized.find_last_of(L'\\');
            if (lastSlash != std::wstring::npos && lastSlash > 0)
            {
                std::wstring parent = normalized.substr(0, lastSlash);
                std::wstring child = normalized.substr(lastSlash + 1);

                std::wstring resolvedParent = GetFullyResolvedPhysicalPath(parent);
                normalized = NormalizePath(resolvedParent + L"\\" + child);
            }
        }

        // Collapse double backslashes in the resolved path (e.g. E:\\ -> E:\)
        size_t startPos = 0;
        if (normalized.rfind(L"\\\\", 0) == 0)
        {
            startPos = 2; // preserve UNC prefix
        }
        size_t pos;
        while ((pos = normalized.find(L"\\\\", startPos)) != std::wstring::npos)
        {
            normalized.replace(pos, 2, L"\\");
        }

        // Ensure drive root like C:\ is kept as C:\ (not C:)
        if (normalized.length() == 2 && normalized[1] == L':')
        {
            normalized += L"\\";
        }

        return normalized;
    }
}
