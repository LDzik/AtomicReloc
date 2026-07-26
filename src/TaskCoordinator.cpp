#include "TaskCoordinator.h"
#include <winioctl.h>
#include <commctrl.h>
#include <chrono>

static std::wstring GetFullPath(const std::wstring &path)
{
    wchar_t buffer[MAX_PATH * 2] = {0};
    DWORD len = ::GetFullPathNameW(path.c_str(), MAX_PATH * 2, buffer, nullptr);
    if (len > 0)
    {
        return std::wstring(buffer, len);
    }
    return path;
}

TaskCoordinator::TaskCoordinator(
    const std::wstring &sourceRoot,
    const std::wstring &destRoot,
    RedirectStrategy strategy,
    HWND uiWindow,
    const std::wstring &appDataPath) : m_sourceRoot(FileSystemEngine::NormalizePath(GetFullPath(sourceRoot))),
                                       m_destRoot(FileSystemEngine::NormalizePath(GetFullPath(destRoot))),
                                       m_strategy(strategy),
                                       m_uiWindow(uiWindow),
                                       m_appDataPath(appDataPath)
{
}

TaskCoordinator::~TaskCoordinator()
{
    CancelMigration();
    if (m_coordinatorThread.joinable())
    {
        m_coordinatorThread.join();
    }
}

bool TaskCoordinator::QueryVolumeSeekPenalty(const std::wstring &driveRoot, bool &outIncursSeekPenalty) const
{
    outIncursSeekPenalty = false;

    // Optical or floppy drives are assumed to incur seek penalty
    std::wstring drivePathWithSlash = driveRoot.back() == L'\\' ? driveRoot : driveRoot + L"\\";
    UINT driveType = ::GetDriveTypeW(drivePathWithSlash.c_str());
    if (driveType == DRIVE_CDROM || driveType == DRIVE_REMOVABLE)
    {
        outIncursSeekPenalty = true;
        return true;
    }

    std::wstring volumePath = L"\\\\.\\" + driveRoot;
    // Open the drive handle with 0 (No specific read/write access required for query info)
    // to bypass typical UAC administration permission barriers for non-elevated users.
    HANDLE hDevice = ::CreateFileW(
        volumePath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (hDevice == INVALID_HANDLE_VALUE)
    {
        Logger::Log(Logger::Level::Warning, L"Topology: Unable to open volume handle for: " + volumePath + L". Defaulting seek penalty checks.");
        return false;
    }

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor = {};
    DWORD bytesReturned = 0;

    BOOL success = ::DeviceIoControl(
        hDevice,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query,
        sizeof(query),
        &descriptor,
        sizeof(descriptor),
        &bytesReturned,
        NULL);

    ::CloseHandle(hDevice);

    if (success)
    {
        outIncursSeekPenalty = (descriptor.IncursSeekPenalty != FALSE);
        Logger::Log(Logger::Level::Info, L"Topology: Volume " + driveRoot + L" Seek Penalty Incurred: " + (outIncursSeekPenalty ? L"YES (HDD)" : L"NO (SSD/NVMe)"));
        return true;
    }

    Logger::Log(Logger::Level::Warning, L"Topology: Query properties failed for volume: " + driveRoot + L". Assuming standard SSD.");
    return false;
}

bool TaskCoordinator::AnalyzeTopology(bool &outSourceIsHDD, bool &outDestIsHDD, uint32_t &outThreadCount)
{
    outSourceIsHDD = false;
    outDestIsHDD = false;
    outThreadCount = 2; // Dynamic default fallback

    // Junction Re-Targeting: Resolve true physical source first to query its actual drive volume properties
    std::wstring trueSource;
    std::vector<std::wstring> ghostChain;
    if (!FileSystemEngine::ResolveTruePath(m_sourceRoot, trueSource, ghostChain))
    {
        Logger::Log(Logger::Level::Error, L"Topology Analysis: Unable to resolve true path for source: " + m_sourceRoot);
        return false;
    }

    // Extract drive letter structures (e.g. "C:")
    std::wstring srcDrive = trueSource.substr(0, 2);
    std::wstring destDrive = m_destRoot.substr(0, 2);

    if (srcDrive[1] != L':' || destDrive[1] != L':')
    {
        Logger::Log(Logger::Level::Error, L"Topology Analysis: Root paths must start with a valid drive letter: " + trueSource + L" | " + m_destRoot);
        return false;
    }

    QueryVolumeSeekPenalty(srcDrive, outSourceIsHDD);
    QueryVolumeSeekPenalty(destDrive, outDestIsHDD);

    // HDD Throttling Check: If either involves spinning platters, throttle thread counts to 1 to avoid thrashing
    if (outSourceIsHDD || outDestIsHDD)
    {
        outThreadCount = 1;
        Logger::Log(Logger::Level::Warning, L"Topology Coordinator: HDD detected in target pipeline! Throttling Swarm execution to 1 worker thread to avoid disk thrashing.");
    }
    else
    {
        // Multi-threaded SSD / NVMe dynamics scaling relative to cores
        uint32_t cores = std::thread::hardware_concurrency();
        outThreadCount = (cores > 2) ? (cores - 1) : 2;
        Logger::Log(Logger::Level::Success, L"Topology Coordinator: Solid-state storage chain detected. Scaling Swarm to: " + std::to_wstring(outThreadCount) + L" concurrent threads.");
    }

    return true;
}

bool TaskCoordinator::ScanDirectory(const std::wstring &path, const std::wstring &relPath)
{
    std::wstring searchPattern = FileSystemEngine::JoinPath(path, L"*");
    WIN32_FIND_DATAW findData;
    HANDLE hFind = ::FindFirstFileW(searchPattern.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE)
    {
        Logger::Log(Logger::Level::Error, L"Scanner: Failed to scan path: " + path + L". Error: " + GetLastErrorAsString());
        return false;
    }

    do
    {
        std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }

        // Avoid scanning any loose .mover_bak items
        if (name.rfind(L".mover_bak") != std::wstring::npos)
        {
            continue;
        }

        std::wstring fullItemPath = FileSystemEngine::JoinPath(path, name);
        std::wstring itemRelPath = relPath.empty() ? name : FileSystemEngine::JoinPath(relPath, name);

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            // Folders are recorded in m_subfolders so we can precreate them
            m_subfolders.push_back(itemRelPath);
            if (!ScanDirectory(fullItemPath, itemRelPath))
            {
                ::FindClose(hFind);
                return false;
            }
        }
        else
        {
            // File size aggregation
            uint64_t size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
            m_files.push_back(FileTask{itemRelPath, size});
            m_totalBytes += size;
        }

    } while (::FindNextFileW(hFind, &findData));

    ::FindClose(hFind);
    return true;
}

void TaskCoordinator::PostProgressUpdate(bool isComplete, bool isFailed, const std::wstring &errorMsg)
{
    if (!m_uiWindow)
    {
        return;
    }

    // Allocate Heap data to transfer across Win32 message borders safely
    MigrationProgressData *data = new MigrationProgressData{};
    data->totalBytes = m_totalBytes;
    data->bytesProcessed = m_bytesProcessed.load();
    data->totalFiles = static_cast<uint32_t>(m_files.size());
    data->filesProcessed = m_filesProcessed.load();
    data->currentSpeedMBs = m_speedMBs;
    data->isComplete = isComplete;
    data->isFailed = isFailed;

    {
        std::lock_guard<std::mutex> lock(m_fileNameMutex);
        size_t len = (std::min)(m_currentFileName.length(), static_cast<size_t>(MAX_PATH - 1));
        std::wcsncpy(data->currentFileName, m_currentFileName.c_str(), len);
        data->currentFileName[len] = L'\0';
    }

    if (!errorMsg.empty())
    {
        size_t len = (std::min)(errorMsg.length(), static_cast<size_t>(255));
        std::wcsncpy(data->errorMessage, errorMsg.c_str(), len);
        data->errorMessage[len] = L'\0';
    }

    ::PostMessageW(m_uiWindow, WM_USER_MIGRATION_PROGRESS, 0, reinterpret_cast<LPARAM>(data));
}

void TaskCoordinator::WorkerThreadProcedure()
{
    // Smart Junction Re-Targeting: Read files from pre-calculated physical source directory
    std::wstring trueSource = m_resolvedSourceRoot;

    while (true)
    {
        FileTask task;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this]()
                      { return !m_taskQueue.empty() || m_shutdown.load() || m_cancel.load(); });

            if (m_shutdown.load() || m_cancel.load() || m_workerFailed.load())
            {
                break;
            }

            task = m_taskQueue.front();
            m_taskQueue.pop();
        }

        std::wstring srcFile = FileSystemEngine::JoinPath(trueSource, task.relPath);
        std::wstring destFile = FileSystemEngine::JoinPath(m_destRoot, task.relPath);

        {
            std::lock_guard<std::mutex> nameLock(m_fileNameMutex);
            m_currentFileName = task.relPath;
        }

        // Execute safety pipeline steps
        bool success = JournalManager::PreallocateFile(destFile, task.fileSize);
        if (success)
        {
            // Optimized: Copy and compute MurmurHash3 on-the-fly and verify on flush completion
            success = JournalManager::StreamAndFlushFile(srcFile, destFile, m_bytesProcessed, m_cancel);
        }

        if (!success)
        {
            std::lock_guard<std::mutex> errLock(m_errorMutex);
            if (!m_workerFailed.load())
            {
                m_workerFailed = true;
                m_workerErrorMessage = L"Safety pipeline processing failed for file: " + task.relPath;
            }
            m_cv.notify_all();
            break;
        }

        m_filesProcessed.fetch_add(1);
    }
}

void TaskCoordinator::RunMigrationProcedure()
{
    m_coordinatorRunning = true;
    Logger::Log(Logger::Level::Info, L"Coordinator: Relocation process initialized.");

    // SAFETY GUARD: Resolve FULL PHYSICAL PATH of both source and destination
    std::wstring resolvedPhysicalSource = FileSystemEngine::GetFullyResolvedPhysicalPath(m_sourceRoot);
    std::wstring resolvedPhysicalDestination;

    if (m_strategy == RedirectStrategy::Restore)
    {
        // For restore, the destination is the junction point itself, but since we will delete the junction
        // and make it a normal directory under its parent, the physical destination will be under the parent's physical path.
        size_t lastSlash = m_destRoot.find_last_of(L'\\');
        if (lastSlash != std::wstring::npos && lastSlash > 0)
        {
            std::wstring parent = m_destRoot.substr(0, lastSlash);
            std::wstring child = m_destRoot.substr(lastSlash + 1);
            std::wstring resolvedParent = FileSystemEngine::GetFullyResolvedPhysicalPath(parent);
            resolvedPhysicalDestination = FileSystemEngine::NormalizePath(FileSystemEngine::JoinPath(resolvedParent, child));
        }
        else
        {
            resolvedPhysicalDestination = FileSystemEngine::GetFullyResolvedPhysicalPath(m_destRoot);
        }
    }
    else
    {
        resolvedPhysicalDestination = FileSystemEngine::GetFullyResolvedPhysicalPath(m_destRoot);
    }

    // Strip trailing slashes
    while (resolvedPhysicalSource.length() > 3 && resolvedPhysicalSource.back() == L'\\')
    {
        resolvedPhysicalSource.pop_back();
    }
    while (resolvedPhysicalDestination.length() > 3 && resolvedPhysicalDestination.back() == L'\\')
    {
        resolvedPhysicalDestination.pop_back();
    }

    if (_wcsicmp(resolvedPhysicalSource.c_str(), resolvedPhysicalDestination.c_str()) == 0)
    {
        // Abort the migration immediately with a TaskDialog warning
        TASKDIALOGCONFIG tdConfig = {};
        tdConfig.cbSize = sizeof(tdConfig);
        tdConfig.hwndParent = m_uiWindow;
        tdConfig.pszWindowTitle = L"AtomicReloc Safety Guard";
        tdConfig.pszMainInstruction = L"Source and Destination are the same location. No move required.";
        tdConfig.pszContent = L"The resolved physical source and destination paths are identical. Move aborted to prevent data loss.";
        tdConfig.pszMainIcon = TD_WARNING_ICON;
        tdConfig.dwCommonButtons = TDCBF_OK_BUTTON;

        ::TaskDialogIndirect(&tdConfig, NULL, NULL, NULL);

        PostProgressUpdate(false, true, L"Source and Destination are the same location. No move required.");
        m_coordinatorRunning = false;
        return;
    }

    bool srcHDD = false, destHDD = false;
    uint32_t threadCount = 1;

    if (!AnalyzeTopology(srcHDD, destHDD, threadCount))
    {
        PostProgressUpdate(false, true, L"Drive topology analysis failed.");
        m_coordinatorRunning = false;
        return;
    }

    std::wstring trueSource;
    std::vector<std::wstring> ghostChain;
    FileSystemEngine::ResolveTruePath(m_sourceRoot, m_resolvedSourceRoot, ghostChain);
    trueSource = m_resolvedSourceRoot;

    if (!ghostChain.empty())
    {
        Logger::Log(Logger::Level::Success, L"Smart Junction Re-Targeting: Source directory " + m_sourceRoot + L" is already redirecting to " + trueSource + L". Bypassing source drive write limits.");
    }

    // 1. Scan source directory
    Logger::Log(Logger::Level::Info, L"Coordinator: Scanning directories recursive: " + trueSource);
    m_files.clear();
    m_subfolders.clear();
    m_totalBytes = 0;

    if (!ScanDirectory(trueSource, L""))
    {
        PostProgressUpdate(false, true, L"Scanning source directories failed.");
        m_coordinatorRunning = false;
        return;
    }

    Logger::Log(Logger::Level::Info, L"Coordinator: Found " + std::to_wstring(m_files.size()) + L" files. Total Payload: " + std::to_wstring(m_totalBytes) + L" bytes.");

    // 2. Precreate folders on target
    Logger::Log(Logger::Level::Info, L"Coordinator: Pre-creating subdirectory structures on: " + m_destRoot);
    if (m_strategy == RedirectStrategy::Restore)
    {
        if (FileSystemEngine::IsReparsePoint(m_destRoot))
        {
            Logger::Log(Logger::Level::Info, L"Coordinator: Removing redirect link at restore destination: " + m_destRoot);
            ::RemoveDirectoryW(m_destRoot.c_str());
        }
    }
    if (!FileSystemEngine::CreateDirectoryRecursive(m_destRoot))
    {
        PostProgressUpdate(false, true, L"Unable to create destination root path: " + m_destRoot);
        m_coordinatorRunning = false;
        return;
    }

    for (const auto &sub : m_subfolders)
    {
        std::wstring destSub = FileSystemEngine::JoinPath(m_destRoot, sub);
        if (!FileSystemEngine::CreateDirectoryRecursive(destSub))
        {
            PostProgressUpdate(false, true, L"Failed creating target subdirectory: " + destSub);
            m_coordinatorRunning = false;
            return;
        }
    }

    // 3. Set up active Transaction Journal Log
    std::wstring txUuid;
    JournalManager jm(m_appDataPath);

    if (!jm.StartTransaction(m_sourceRoot, m_resolvedSourceRoot, m_destRoot, m_strategy, txUuid))
    {
        PostProgressUpdate(false, true, L"Failed to scaffold transactional journal log.");
        m_coordinatorRunning = false;
        return;
    }

    // Load file task queue
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (const auto &f : m_files)
        {
            m_taskQueue.push(f);
        }
    }

    // Spawn Swarm worker threads
    m_workers.clear();
    for (uint32_t i = 0; i < threadCount; ++i)
    {
        m_workers.push_back(std::thread(&TaskCoordinator::WorkerThreadProcedure, this));
    }

    // Throughput telemetry calculating thread
    uint64_t lastBytes = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        uint64_t currentBytes = m_bytesProcessed.load();
        m_speedMBs = static_cast<double>(currentBytes - lastBytes) / (1024.0 * 1024.0) / 0.5;
        lastBytes = currentBytes;

        PostProgressUpdate(false, false);

        // Terminate monitoring loops if worker failures or cancels arise
        if (m_cancel.load() || m_workerFailed.load())
        {
            break;
        }

        // Verify if queue is empty
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_taskQueue.empty() && m_filesProcessed.load() == m_files.size())
            {
                break;
            }
        }
    }

    // Join Swarm Workers
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_shutdown = true;
        m_cv.notify_all();
    }

    for (auto &w : m_workers)
    {
        if (w.joinable())
        {
            w.join();
        }
    }

    // Process rollbacks if pipeline faults or cancellations arise
    if (m_cancel.load() || m_workerFailed.load())
    {
        Logger::Log(Logger::Level::Warning, L"Coordinator: Transaction failed or cancelled. Initiating safety recovery rollback.");

        jm.AbortTransaction();

        std::wstring recoveryMsg = m_cancel.load() ? L"Process cancelled by user." : m_workerErrorMessage;
        jm.RecoverRollback(jm.GetCurrentTransaction(), nullptr);

        PostProgressUpdate(false, true, recoveryMsg);
        m_coordinatorRunning = false;
        return;
    }

    // 4. Update state to Copy Completed
    Logger::Log(Logger::Level::Success, L"Coordinator: All file tasks successfully copied and verified.");
    jm.UpdateState(TransactionState::CopyCompleted);

    // 5. Final Stage - Atomic Directory Swap
    if (m_strategy == RedirectStrategy::Restore)
    {
        Logger::Log(Logger::Level::Info, L"Coordinator: Finalizing restore by purging physical alternative source: " + m_resolvedSourceRoot);
        jm.UpdateState(TransactionState::PointerSwapped);

        std::wstring srcToPurge = m_resolvedSourceRoot;
        // FIX: Capture ghostChain so we can hunt down the orphaned middlemen (D:\)
        std::thread cleanupThread([srcToPurge, ghostChain]()
                                  {
            // 1. Purge the physical payload (F:\)
            FileSystemEngine::DeleteDirectoryRecursive(srcToPurge);
            
            // 2. Clean up intermediate ghosts (D:\)
            // Skip index 0 because that is the root (E:\) which we already handled
            for (size_t i = 1; i < ghostChain.size(); ++i) {
                if (FileSystemEngine::IsReparsePoint(ghostChain[i])) {
                    FileSystemEngine::DeleteJunction(ghostChain[i]);
                }
            }
            
            Logger::Log(Logger::Level::Success, L"Coordinator: Alternative physical source successfully purged."); });
        cleanupThread.detach();

        jm.CommitTransaction();
        PostProgressUpdate(true, false);
    }
    else
    {
        Logger::Log(Logger::Level::Info, L"Coordinator: Executing atomic directory swap redirects...");
        std::wstring backupPath;
        std::wstring swapErrorMsg;

        bool swapSuccess = JournalManager::AtomicSwapDirectory(
            m_sourceRoot,
            m_destRoot,
            m_strategy,
            backupPath,
            swapErrorMsg);

        if (swapSuccess)
        {
            // Redirections set! Swap states to PointerSwapped
            jm.UpdateState(TransactionState::PointerSwapped);
            Logger::Log(Logger::Level::Success, L"Coordinator: Directory redirect link created and validated successfully.");

            // Explicit Nested Cleanup Verification:
            // Ensure the new junction on E (m_sourceRoot) is verified to be working and points to F (m_destRoot)
            // BEFORE launching the background deletion of D (backupPath).
            std::wstring resolvedPath;
            std::vector<std::wstring> verifyChain;
            bool linkVerified = FileSystemEngine::ResolveTruePath(m_sourceRoot, resolvedPath, verifyChain) && (_wcsicmp(resolvedPath.c_str(), m_destRoot.c_str()) == 0);

            if (linkVerified)
            {
                // Asynchronously purge source backing directory
                Logger::Log(Logger::Level::Info, L"Coordinator: Redirection verified. Initiating source payload purge on background thread...");
                std::wstring bakPath = backupPath;
                std::wstring truePhysicalSource = m_resolvedSourceRoot;
                std::wstring sourceRootCopy = m_sourceRoot;
                std::thread cleanupThread([bakPath, ghostChain, truePhysicalSource, sourceRootCopy]()
                                          {
    
                    // 1. Delete the true physical source safely
                    if (_wcsicmp(sourceRootCopy.c_str(), truePhysicalSource.c_str()) != 0) {
                        FileSystemEngine::DeleteDirectoryRecursive(truePhysicalSource);
                    }
                    
                    // 2. Optionally clean up intermediate ghosts
                    // Start at index 1 to skip the root which was handled by AtomicSwap
                    for (size_t i = 1; i < ghostChain.size(); ++i) {
                        const auto& ghost = ghostChain[i];
                        if (FileSystemEngine::IsReparsePoint(ghost)) {
                            FileSystemEngine::DeleteJunction(ghost);
                            Logger::Log(Logger::Level::Info, L"Cleaned up intermediate ghost junction: " + ghost);
                        } else {
                            // SAFE: If it's not a reparse point anymore, leave it alone.
                            Logger::Log(Logger::Level::Warning, L"Expected ghost junction was modified, skipping cleanup: " + ghost);
                        }
                    }
                    
                    // 3. Delete the backup of the root pointer
                    FileSystemEngine::DeleteDirectoryRecursive(bakPath); });
                cleanupThread.detach();
            }
            else
            {
                Logger::Log(Logger::Level::Error, L"Coordinator: New junction verification failed. Skipping background payload purge to prevent data loss.");
            }

            // Finalize transaction
            jm.CommitTransaction();
            PostProgressUpdate(true, false);
        }
        else
        {
            Logger::Log(Logger::Level::Error, L"Coordinator: Atomic directory swap failed. Initiating transaction rollback.");

            jm.AbortTransaction();
            jm.RecoverRollback(jm.GetCurrentTransaction(), nullptr);

            PostProgressUpdate(false, true, swapErrorMsg);
        }
    }

    m_coordinatorRunning = false;
}

bool TaskCoordinator::StartMigration()
{
    if (m_coordinatorRunning.load())
    {
        return false;
    }

    m_cancel = false;
    m_shutdown = false;
    m_workerFailed = false;
    m_totalBytes = 0;
    m_bytesProcessed = 0;
    m_filesProcessed = 0;
    m_speedMBs = 0.0;

    if (m_coordinatorThread.joinable())
    {
        m_coordinatorThread.join();
    }

    m_coordinatorThread = std::thread(&TaskCoordinator::RunMigrationProcedure, this);
    return true;
}

void TaskCoordinator::CancelMigration()
{
    m_cancel = true;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_cv.notify_all();
    }
}

uint64_t TaskCoordinator::CalculateDirectorySize(const std::wstring &path)
{
    uint64_t totalSize = 0;
    std::wstring searchPattern = FileSystemEngine::JoinPath(path, L"*");
    WIN32_FIND_DATAW findData;
    HANDLE hFind = ::FindFirstFileW(searchPattern.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    do
    {
        std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }

        // Skip reparse points entirely (junctions, symlinks) during size calculation
        // to prevent infinite loops, double counting, or following external paths
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        {
            continue;
        }

        std::wstring fullItemPath = FileSystemEngine::JoinPath(path, name);

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            totalSize += CalculateDirectorySize(fullItemPath);
        }
        else
        {
            uint64_t size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
            totalSize += size;
        }

    } while (::FindNextFileW(hFind, &findData));

    ::FindClose(hFind);
    return totalSize;
}

void TaskCoordinator::CalculateDirectorySizeAsync(const std::wstring &path, HWND uiWindow)
{
    std::thread([path, uiWindow]()
                {
        uint64_t size = CalculateDirectorySize(path);
        
        FolderSizeData* data = new FolderSizeData{};
        size_t len = (std::min)(path.length(), static_cast<size_t>(MAX_PATH - 1));
        std::wcsncpy(data->folderPath, path.c_str(), len);
        data->folderPath[len] = L'\0';
        data->totalSize = size;
        
        ::PostMessageW(uiWindow, WM_USER_SIZE_CALCULATED, 0, reinterpret_cast<LPARAM>(data)); })
        .detach();
}
