#include "JournalManager.h"
#include <objbase.h>
#include <sstream>

// Helper to parse the journal key-value content in Unicode
static bool ParseJournalFile(const std::wstring &path, JournalData &outData)
{
    HANDLE hFile = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD size = ::GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE || size == 0)
    {
        ::CloseHandle(hFile);
        return false;
    }

    std::vector<char> rawBuffer(size + 1, 0);
    DWORD bytesRead = 0;
    BOOL success = ::ReadFile(hFile, rawBuffer.data(), size, &bytesRead, NULL);
    ::CloseHandle(hFile);

    if (!success || bytesRead == 0)
    {
        return false;
    }

    // Convert UTF-8 stream to Wide String
    int wsize = ::MultiByteToWideChar(CP_UTF8, 0, rawBuffer.data(), -1, NULL, 0);
    if (wsize <= 0)
    {
        return false;
    }

    std::vector<wchar_t> wBuffer(wsize + 1, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, rawBuffer.data(), -1, wBuffer.data(), wsize);

    std::wstring content(wBuffer.data());
    std::wstringstream wss(content);
    std::wstring line;

    auto trim = [](std::wstring &s)
    {
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t'))
            s.erase(s.begin());
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n'))
            s.pop_back();
    };

    while (std::getline(wss, line))
    {
        size_t colon = line.find(L':');
        if (colon == std::wstring::npos)
        {
            continue;
        }

        std::wstring key = line.substr(0, colon);
        std::wstring val = line.substr(colon + 1);

        trim(key);
        trim(val);

        if (key == L"UUID")
        {
            outData.uuid = val;
        }
        else if (key == L"SourcePath")
        {
            outData.sourcePath = val;
        }
        else if (key == L"PhysicalSourcePath")
        {
            outData.physicalSourcePath = val;
        }
        else if (key == L"DestPath")
        {
            outData.destPath = val;
        }
        else if (key == L"State")
        {
            if (val == L"Pending")
            {
                outData.state = TransactionState::Pending;
            }
            else if (val == L"CopyCompleted")
            {
                outData.state = TransactionState::CopyCompleted;
            }
            else if (val == L"PointerSwapped")
            {
                outData.state = TransactionState::PointerSwapped;
            }
            else if (val == L"Aborted")
            {
                outData.state = TransactionState::Aborted;
            }
            else
            {
                outData.state = TransactionState::None;
            }
        }
        else if (key == L"Strategy")
        {
            if (val == L"Junction")
            {
                outData.strategy = RedirectStrategy::Junction;
            }
            else if (val == L"SymbolicLink")
            {
                outData.strategy = RedirectStrategy::SymbolicLink;
            }
            else if (val == L"HardLink")
            {
                outData.strategy = RedirectStrategy::HardLink;
            }
            else if (val == L"Restore")
            {
                outData.strategy = RedirectStrategy::Restore;
            }
        }
    }

    return !outData.uuid.empty() && !outData.sourcePath.empty() && !outData.destPath.empty();
}

JournalManager::JournalManager(const std::wstring &appDataPath)
{
    m_journalDirectory = appDataPath + L"\\journals";
    FileSystemEngine::CreateDirectoryRecursive(m_journalDirectory);
}

std::wstring JournalManager::GetJournalPathForUuid(const std::wstring &uuid) const
{
    return m_journalDirectory + L"\\" + uuid + L".mover-journal";
}

std::wstring JournalManager::GetTemporaryJournalPathForUuid(const std::wstring &uuid) const
{
    return m_journalDirectory + L"\\" + uuid + L".mover-journal.tmp";
}

bool JournalManager::WriteJournalAtomically(const JournalData &data)
{
    std::wstring journalPath = GetJournalPathForUuid(data.uuid);
    std::wstring tmpPath = GetTemporaryJournalPathForUuid(data.uuid);

    // Format content stream in Unicode
    std::wstringstream wss;
    wss << L"UUID: " << data.uuid << L"\n";
    wss << L"SourcePath: " << data.sourcePath << L"\n";
    wss << L"PhysicalSourcePath: " << data.physicalSourcePath << L"\n";
    wss << L"DestPath: " << data.destPath << L"\n";

    std::wstring stateStr = L"None";
    switch (data.state)
    {
    case TransactionState::Pending:
        stateStr = L"Pending";
        break;
    case TransactionState::CopyCompleted:
        stateStr = L"CopyCompleted";
        break;
    case TransactionState::PointerSwapped:
        stateStr = L"PointerSwapped";
        break;
    case TransactionState::Aborted:
        stateStr = L"Aborted";
        break;
    default:
        break;
    }
    wss << L"State: " << stateStr << L"\n";

    std::wstring stratStr = L"Junction";
    switch (data.strategy)
    {
    case RedirectStrategy::Junction:
        stratStr = L"Junction";
        break;
    case RedirectStrategy::SymbolicLink:
        stratStr = L"SymbolicLink";
        break;
    case RedirectStrategy::HardLink:
        stratStr = L"HardLink";
        break;
    case RedirectStrategy::Restore:
        stratStr = L"Restore";
        break;
    }
    wss << L"Strategy: " << stratStr << L"\n";

    std::wstring content = wss.str();

    // Map to UTF-8
    int sizeNeeded = ::WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, NULL, 0, NULL, NULL);
    std::vector<char> utf8Buffer(sizeNeeded);
    ::WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, utf8Buffer.data(), sizeNeeded, NULL, NULL);

    // 1. Write data to temporary journal file
    HANDLE hTmp = ::CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hTmp == INVALID_HANDLE_VALUE)
    {
        Logger::Log(Logger::Level::Error, L"Journal: Failed to create temp journal file: " + tmpPath + L". Error: " + GetLastErrorAsString());
        return false;
    }

    DWORD bytesWritten = 0;
    // Write size excluding terminating null
    BOOL success = ::WriteFile(hTmp, utf8Buffer.data(), static_cast<DWORD>(utf8Buffer.size() - 1), &bytesWritten, NULL);

    if (success)
    {
        // 2. Commit blocks down to physical disk cache
        ::FlushFileBuffers(hTmp);
    }

    ::CloseHandle(hTmp);

    if (!success)
    {
        Logger::Log(Logger::Level::Error, L"Journal: Failed to write to temporary journal: " + tmpPath);
        ::DeleteFileW(tmpPath.c_str());
        return false;
    }

    // 3. Swap files atomically
    if (!::MoveFileExW(tmpPath.c_str(), journalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        Logger::Log(Logger::Level::Error, L"Journal: Atomic replacement swap failed. Target: " + journalPath + L". Error: " + GetLastErrorAsString());
        ::DeleteFileW(tmpPath.c_str());
        return false;
    }

    return true;
}

bool JournalManager::FindActiveJournals(std::vector<JournalData> &outJournals)
{
    std::wstring searchPath = m_journalDirectory + L"\\*.mover-journal";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = ::FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE)
    {
        DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        {
            return true;
        }
        Logger::Log(Logger::Level::Error, L"Journal: Failed to scan journal directory. Error: " + GetLastErrorAsString(err));
        return false;
    }

    do
    {
        std::wstring name = findData.cFileName;
        std::wstring fullPath = m_journalDirectory + L"\\" + name;

        JournalData jd;
        if (ParseJournalFile(fullPath, jd))
        {
            outJournals.push_back(jd);
        }

    } while (::FindNextFileW(hFind, &findData));

    ::FindClose(hFind);
    return true;
}

bool JournalManager::StartTransaction(const std::wstring &sourcePath, const std::wstring &physicalSourcePath, const std::wstring &destPath, RedirectStrategy strategy, std::wstring &outUuid)
{
    GUID guid;
    HRESULT hr = ::CoCreateGuid(&guid);
    if (FAILED(hr))
    {
        Logger::Log(Logger::Level::Error, L"Journal: CoCreateGuid failed to generate a transaction ID.");
        return false;
    }

    wchar_t szGuid[40] = {0};
    if (::StringFromGUID2(guid, szGuid, 40) == 0)
    {
        Logger::Log(Logger::Level::Error, L"Journal: StringFromGUID2 failed to format GUID.");
        return false;
    }

    outUuid = szGuid;
    m_currentData.uuid = outUuid;
    m_currentData.sourcePath = FileSystemEngine::NormalizePath(sourcePath);
    m_currentData.physicalSourcePath = FileSystemEngine::NormalizePath(physicalSourcePath);
    m_currentData.destPath = FileSystemEngine::NormalizePath(destPath);
    m_currentData.state = TransactionState::Pending;
    m_currentData.strategy = strategy;

    m_currentJournalPath = GetJournalPathForUuid(outUuid);

    return WriteJournalAtomically(m_currentData);
}

bool JournalManager::UpdateState(TransactionState newState)
{
    m_currentData.state = newState;
    return WriteJournalAtomically(m_currentData);
}

bool JournalManager::CommitTransaction()
{
    if (m_currentJournalPath.empty())
    {
        return true;
    }

    if (!::DeleteFileW(m_currentJournalPath.c_str()))
    {
        Logger::Log(Logger::Level::Warning, L"Journal: Failed to delete transaction journal during commit: " + m_currentJournalPath + L". Error: " + GetLastErrorAsString());
    }

    m_currentJournalPath.clear();
    m_currentData = JournalData{};
    return true;
}

bool JournalManager::AbortTransaction()
{
    m_currentData.state = TransactionState::Aborted;
    return WriteJournalAtomically(m_currentData);
}

bool JournalManager::LoadJournal(const std::wstring &uuid, JournalData &outData)
{
    std::wstring path = GetJournalPathForUuid(uuid);
    return ParseJournalFile(path, outData);
}

// --- Decoupled 4-Stage Safety Pipeline Implementation ---

bool JournalManager::PreallocateFile(const std::wstring &destPath, uint64_t fileSize)
{
    // Stage 1: Preallocate physical space on destination disk
    HANDLE hDest = ::CreateFileW(
        destPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hDest == INVALID_HANDLE_VALUE)
    {
        Logger::Log(Logger::Level::Error, L"Pipeline Allocation: Failed to pre-allocate file footprint: " + destPath + L". Error: " + GetLastErrorAsString());
        return false;
    }

    if (fileSize > 0)
    {
        LARGE_INTEGER liSize;
        liSize.QuadPart = static_cast<LONGLONG>(fileSize);

        if (!::SetFilePointerEx(hDest, liSize, NULL, FILE_BEGIN))
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Allocation: SetFilePointerEx failed for size: " + std::to_wstring(fileSize) + L". Error: " + GetLastErrorAsString());
            ::CloseHandle(hDest);
            return false;
        }

        if (!::SetEndOfFile(hDest))
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Allocation: SetEndOfFile failed. Error: " + GetLastErrorAsString());
            ::CloseHandle(hDest);
            return false;
        }
    }

    ::CloseHandle(hDest);
    return true;
}

bool JournalManager::StreamAndFlushFile(
    const std::wstring &srcPath,
    const std::wstring &destPath,
    std::atomic<uint64_t> &progressBytes,
    std::atomic<bool> &cancelFlag)
{
    // Stage 2: Stream in optimized 1MB block buffers, hash on-the-fly, and force flushes
    HANDLE hSrc = ::CreateFileW(
        srcPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        NULL);

    if (hSrc == INVALID_HANDLE_VALUE)
    {
        Logger::Log(Logger::Level::Error, L"Pipeline Stream: Failed to open source file: " + srcPath + L". Error: " + GetLastErrorAsString());
        return false;
    }

    HANDLE hDest = ::CreateFileW(
        destPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_FLAG_WRITE_THROUGH,
        NULL);

    if (hDest == INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(hSrc);
        Logger::Log(Logger::Level::Error, L"Pipeline Stream: Failed to open destination for stream write: " + destPath + L". Error: " + GetLastErrorAsString());
        return false;
    }

    // Set destination cursor to start (pre-allocation moved pointer to EOF)
    LARGE_INTEGER liZero = {0};
    ::SetFilePointerEx(hDest, liZero, NULL, FILE_BEGIN);

    const DWORD blockSize = 1024 * 1024; // 1MB sequential buffer
    std::vector<BYTE> buffer(blockSize);

    Murmur3Hasher hasherSrc;
    Murmur3Hasher hasherDest;

    DWORD bytesRead = 0;
    DWORD bytesWritten = 0;
    bool success = true;

    while (::ReadFile(hSrc, buffer.data(), blockSize, &bytesRead, NULL) && bytesRead > 0)
    {
        if (cancelFlag.load())
        {
            Logger::Log(Logger::Level::Warning, L"Pipeline Stream: Copy cancelled by thread Coordinator.");
            success = false;
            break;
        }

        if (!::WriteFile(hDest, buffer.data(), bytesRead, &bytesWritten, NULL) || bytesWritten != bytesRead)
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Stream: Failed writing block stream. Error: " + GetLastErrorAsString());
            success = false;
            break;
        }

        // On-The-Fly Hashing Optimization: Feed blocks progressively as they are read and written
        hasherSrc.Update(buffer.data(), bytesRead);
        hasherDest.Update(buffer.data(), bytesWritten);

        progressBytes.fetch_add(bytesWritten);
    }

    // Commit changes down to physical platter sectors/flash cells
    if (success)
    {
        if (!::FlushFileBuffers(hDest))
        {
            Logger::Log(Logger::Level::Warning, L"Pipeline Stream: FlushFileBuffers failed for: " + destPath + L". Error: " + GetLastErrorAsString());
        }

        // Stage 3: Immediate Verification - Compare hashes right after cache flush
        Murmur3Hasher::Hash128 hashSrc = hasherSrc.Finalize();
        Murmur3Hasher::Hash128 hashDest = hasherDest.Finalize();

        if (hashSrc != hashDest)
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Stream Integrity: Hash checksum mismatch during copy streaming.");
            Logger::Log(Logger::Level::Error, L"Source Hash:      " + hashSrc.ToString());
            Logger::Log(Logger::Level::Error, L"Destination Hash: " + hashDest.ToString());
            success = false;
        }
    }

    ::CloseHandle(hSrc);
    ::CloseHandle(hDest);

    return success;
}

bool JournalManager::VerifyFileIntegrity(const std::wstring &srcPath, const std::wstring &destPath)
{
    // Stage 3: Double read checksum comparison using incremental MurmurHash3
    HANDLE hSrc = ::CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hSrc == INVALID_HANDLE_VALUE)
    {
        Logger::Log(Logger::Level::Error, L"Pipeline Verification: Failed to open source for validation: " + srcPath + L". Error: " + GetLastErrorAsString());
        return false;
    }

    HANDLE hDest = ::CreateFileW(destPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hDest == INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(hSrc);
        Logger::Log(Logger::Level::Error, L"Pipeline Verification: Failed to open destination for validation: " + destPath + L". Error: " + GetLastErrorAsString());
        return false;
    }

    const DWORD blockSize = 1024 * 1024; // 1MB block buffers
    std::vector<BYTE> bufferSrc(blockSize);
    std::vector<BYTE> bufferDest(blockSize);

    Murmur3Hasher hasherSrc;
    Murmur3Hasher hasherDest;

    DWORD bytesReadSrc = 0;
    DWORD bytesReadDest = 0;
    bool success = true;

    while (true)
    {
        BOOL readSrc = ::ReadFile(hSrc, bufferSrc.data(), blockSize, &bytesReadSrc, NULL);
        BOOL readDest = ::ReadFile(hDest, bufferDest.data(), blockSize, &bytesReadDest, NULL);

        if (!readSrc || !readDest)
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Verification: I/O Read failure during validation pass.");
            success = false;
            break;
        }

        if (bytesReadSrc != bytesReadDest)
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Verification: Size mismatch detected during validation pass.");
            success = false;
            break;
        }

        if (bytesReadSrc == 0)
        {
            break; // Finished parsing streams
        }

        hasherSrc.Update(bufferSrc.data(), bytesReadSrc);
        hasherDest.Update(bufferDest.data(), bytesReadDest);
    }

    ::CloseHandle(hSrc);
    ::CloseHandle(hDest);

    if (!success)
    {
        return false;
    }

    Murmur3Hasher::Hash128 hashSrc = hasherSrc.Finalize();
    Murmur3Hasher::Hash128 hashDest = hasherDest.Finalize();

    if (hashSrc != hashDest)
    {
        Logger::Log(Logger::Level::Error, L"Pipeline Verification: Integrity validation failed! Hash checksum mismatch.");
        Logger::Log(Logger::Level::Error, L"Source Hash:      " + hashSrc.ToString());
        Logger::Log(Logger::Level::Error, L"Destination Hash: " + hashDest.ToString());
        return false;
    }

    return true;
}

bool JournalManager::AtomicSwapDirectory(
    const std::wstring &srcPath,
    const std::wstring &destPath,
    RedirectStrategy strategy,
    std::wstring &outBackupPath,
    std::wstring &outErrorMsg)
{
    // Stage 4: Atomic Swap & Link Redirection Verification
    if (FileSystemEngine::IsReparsePoint(srcPath))
    {
        // Resolve the existing physical target
        std::wstring oldTarget;
        if (!FileSystemEngine::GetJunctionTarget(srcPath, oldTarget))
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Swap: Failed to resolve current target of existing junction: " + srcPath);
            outErrorMsg = L"Failed to resolve current target of existing junction: " + srcPath;
            return false;
        }

        outBackupPath = oldTarget;

        // Remove existing pointer cleanly using DeleteJunction
        if (!FileSystemEngine::DeleteJunction(srcPath))
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Swap: Failed to delete existing junction: " + srcPath);
            outErrorMsg = L"Failed to delete existing junction: " + srcPath;
            return false;
        }

        // Establish the NTFS redirection link
        bool linkCreated = false;
        if (strategy == RedirectStrategy::Junction)
        {
            linkCreated = FileSystemEngine::CreateJunction(srcPath, destPath);
        }
        else if (strategy == RedirectStrategy::SymbolicLink)
        {
            linkCreated = FileSystemEngine::CreateSymlink(srcPath, destPath, true);
        }

        if (!linkCreated)
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Swap: Failed to establish redirect pointer link. Rolling back to old target.");
            outErrorMsg = L"Failed to establish redirect pointer link.";
            FileSystemEngine::CreateJunction(srcPath, oldTarget);
            return false;
        }

        // Validate redirection link loops successfully to dest
        std::wstring resolvedPath;
        std::vector<std::wstring> verifyChain;
        if (!FileSystemEngine::ResolveTruePath(srcPath, resolvedPath, verifyChain) || verifyChain.empty() || _wcsicmp(resolvedPath.c_str(), destPath.c_str()) != 0)
        {
            Logger::Log(Logger::Level::Error, L"Pipeline Swap: Redirection path verification failed (Link targets: " + resolvedPath + L"). Rolling back.");
            outErrorMsg = L"Redirection path verification failed (step 0). Expected: " + destPath + L" Got: " + resolvedPath;
            if (strategy == RedirectStrategy::Junction)
            {
                FileSystemEngine::DeleteJunction(srcPath);
            }
            else
            {
                ::RemoveDirectoryW(srcPath.c_str());
            }
            FileSystemEngine::CreateJunction(srcPath, oldTarget);
            return false;
        }

        return true;
    }

    outBackupPath = srcPath + L".mover_bak";

    // Clean any prior backup leftovers
    if (FileSystemEngine::DirectoryExists(outBackupPath))
    {
        FileSystemEngine::DeleteDirectoryRecursive(outBackupPath);
    }

    // Step 1: Atomic directory rename
    if (!::MoveFileExW(srcPath.c_str(), outBackupPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        outErrorMsg = L"Pipeline Swap: Failed to relocate folder structure atomically to backup path: " + outBackupPath + L". Error: " + GetLastErrorAsString();
        Logger::Log(Logger::Level::Error, outErrorMsg);
        return false;
    }

    // Step 2: Establish the NTFS redirection link
    bool linkCreated = false;
    if (strategy == RedirectStrategy::Junction)
    {
        linkCreated = FileSystemEngine::CreateJunction(srcPath, destPath);
    }
    else if (strategy == RedirectStrategy::SymbolicLink)
    {
        linkCreated = FileSystemEngine::CreateSymlink(srcPath, destPath, true);
    }

    if (!linkCreated)
    {
        Logger::Log(Logger::Level::Error, L"Pipeline Swap: Failed to establish redirect pointer link. Initiating rollback.");
        outErrorMsg = L"Failed to establish redirect pointer link.";
        ::MoveFileExW(outBackupPath.c_str(), srcPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        return false;
    }

    // Step 3: Validate redirection link loops successfully to dest
    std::wstring resolvedPath;
    std::vector<std::wstring> verifyChain;
    if (!FileSystemEngine::ResolveTruePath(srcPath, resolvedPath, verifyChain) || verifyChain.empty() || _wcsicmp(resolvedPath.c_str(), destPath.c_str()) != 0)
    {
        Logger::Log(Logger::Level::Error, L"Pipeline Swap: Redirection path verification failed (Link targets: " + resolvedPath + L"). Rolling back.");
        outErrorMsg = L"Redirection path verification failed (step 3). Expected: " + destPath + L" Got: " + resolvedPath;

        // Remove link
        if (strategy == RedirectStrategy::Junction)
        {
            FileSystemEngine::DeleteJunction(srcPath);
        }
        else
        {
            ::RemoveDirectoryW(srcPath.c_str());
        }

        // Restore backup folder
        ::MoveFileExW(outBackupPath.c_str(), srcPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        return false;
    }

    return true;
}

// --- Automated Crash Recovery Routine ---

bool JournalManager::RecoverRollback(const JournalData &journal, void (*progressCallback)(const std::wstring &))
{
    if (progressCallback)
        progressCallback(L"Starting automated transaction rollback...");

    std::wstring backupPath = journal.sourcePath + L".mover_bak";

    // 1. If target exists, purge it to reclaim space
    if (FileSystemEngine::DirectoryExists(journal.destPath))
    {
        if (progressCallback)
            progressCallback(L"Purging partially copied destination: " + journal.destPath);
        FileSystemEngine::DeleteDirectoryRecursive(journal.destPath);
    }

    if (journal.strategy == RedirectStrategy::Restore)
    {
        if (progressCallback)
            progressCallback(L"Recreating redirection link for destination...");
        FileSystemEngine::CreateJunction(journal.destPath, journal.sourcePath);
        // Clear active journal file
        std::wstring path = GetJournalPathForUuid(journal.uuid);
        ::DeleteFileW(path.c_str());
        if (progressCallback)
            progressCallback(L"Rollback complete. System returned to pre-transaction state.");
        return true;
    }

    // 2. If swap ran but backup is left, swap back
    if (!FileSystemEngine::DirectoryExists(journal.sourcePath) && FileSystemEngine::DirectoryExists(backupPath))
    {
        if (progressCallback)
            progressCallback(L"Restoring original source folder structure from backup...");
        ::MoveFileExW(backupPath.c_str(), journal.sourcePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    else if (FileSystemEngine::IsReparsePoint(journal.sourcePath) && FileSystemEngine::DirectoryExists(backupPath))
    {
        if (progressCallback)
            progressCallback(L"Removing redirection link and restoring original source folder structure...");
        if (journal.strategy == RedirectStrategy::Junction)
        {
            FileSystemEngine::DeleteJunction(journal.sourcePath);
        }
        else
        {
            ::RemoveDirectoryW(journal.sourcePath.c_str());
        }
        ::MoveFileExW(backupPath.c_str(), journal.sourcePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    // Clean any loose backup folder
    if (FileSystemEngine::DirectoryExists(backupPath))
    {
        if (progressCallback)
            progressCallback(L"Purging temporary backup folder...");
        FileSystemEngine::DeleteDirectoryRecursive(backupPath);
    }

    // 3. Clear active journal file
    std::wstring path = GetJournalPathForUuid(journal.uuid);
    ::DeleteFileW(path.c_str());

    if (progressCallback)
        progressCallback(L"Rollback complete. System returned to pre-transaction state.");
    return true;
}

bool JournalManager::RecoverResume(const JournalData &journal, void (*progressCallback)(const std::wstring &))
{
    if (progressCallback)
        progressCallback(L"Starting transaction resume...");

    if (journal.strategy == RedirectStrategy::Restore)
    {
        if (journal.state == TransactionState::CopyCompleted)
        {
            if (progressCallback)
                progressCallback(L"Source files were fully copied. Finalizing restore by purging physical source...");
            FileSystemEngine::DeleteDirectoryRecursive(journal.physicalSourcePath); // Use physical!
        }
        std::wstring path = GetJournalPathForUuid(journal.uuid);
        ::DeleteFileW(path.c_str());
        if (progressCallback)
            progressCallback(L"Resume complete. System state finalized.");
        return true;
    }

    if (journal.state == TransactionState::CopyCompleted)
    {
        if (progressCallback)
            progressCallback(L"Source files were fully copied. Relocating folders and creating links...");

        std::wstring outBackup;
        std::wstring outError;
        if (!AtomicSwapDirectory(journal.sourcePath, journal.destPath, journal.strategy, outBackup, outError))
        {
            if (progressCallback)
                progressCallback(L"Atomic swap failed during recovery resume. Rolling back instead...");
            return RecoverRollback(journal, progressCallback);
        }

        // Force state forward so the cleanup block below catches it
        UpdateState(TransactionState::PointerSwapped);
    }

    // If pointer swapped is active (or we just forced it active), perform the final backup purge stage
    if (journal.state == TransactionState::PointerSwapped || m_currentData.state == TransactionState::PointerSwapped)
    {

        // 1. Delete physical payload ONLY for chained moves
        if (_wcsicmp(journal.sourcePath.c_str(), journal.physicalSourcePath.c_str()) != 0)
        {
            if (FileSystemEngine::DirectoryExists(journal.physicalSourcePath))
            {
                if (progressCallback)
                    progressCallback(L"Purging old physical payload from chained move...");
                FileSystemEngine::DeleteDirectoryRecursive(journal.physicalSourcePath);
            }
        }

        // 2. Delete the .mover_bak (which is the actual payload for normal moves)
        std::wstring activeBackup = journal.sourcePath + L".mover_bak";
        if (FileSystemEngine::DirectoryExists(activeBackup))
        {
            if (progressCallback)
                progressCallback(L"Asynchronously purging backup file payloads...");
            FileSystemEngine::DeleteDirectoryRecursive(activeBackup);
        }
    }

    // Clear journal
    std::wstring path = GetJournalPathForUuid(journal.uuid);
    ::DeleteFileW(path.c_str());

    if (progressCallback)
        progressCallback(L"Resume complete. System state finalized.");
    return true;
}
