#pragma once

#include "Common.h"
#include "FileSystemEngine.h"
#include "JournalManager.h"
#include <vector>
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

// User message identifier for non-blocking UI notifications
#define WM_USER_MIGRATION_PROGRESS (WM_USER + 100)
#define WM_USER_SIZE_CALCULATED (WM_USER + 101)

// Standard progress structure marshalled to the Win32 window loop
struct MigrationProgressData {
    uint64_t totalBytes;
    uint64_t bytesProcessed;
    uint32_t totalFiles;
    uint32_t filesProcessed;
    double currentSpeedMBs;
    wchar_t currentFileName[MAX_PATH];
    bool isComplete;
    bool isFailed;
    wchar_t errorMessage[256];
};

struct FolderSizeData {
    wchar_t folderPath[MAX_PATH];
    uint64_t totalSize;
};

struct FileTask {
    std::wstring relPath; // Relative path from root
    uint64_t fileSize;
};

class TaskCoordinator {
private:
    std::wstring m_sourceRoot;
    std::wstring m_destRoot;
    RedirectStrategy m_strategy;

    std::vector<FileTask> m_files;
    std::vector<std::wstring> m_subfolders;
    uint64_t m_totalBytes = 0;

    // The Swarm: Multi-threaded Work Queue
    std::vector<std::thread> m_workers;
    std::queue<FileTask> m_taskQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    std::atomic<bool> m_shutdown = false;
    std::atomic<bool> m_cancel = false;
    std::atomic<bool> m_workerFailed = false;
    std::wstring m_workerErrorMessage;
    std::mutex m_errorMutex;

    std::atomic<uint64_t> m_bytesProcessed = 0;
    std::atomic<uint32_t> m_filesProcessed = 0;

    std::wstring m_currentFileName;
    std::mutex m_fileNameMutex;

    HWND m_uiWindow = nullptr;
    std::wstring m_appDataPath;
    std::wstring m_resolvedSourceRoot;
    double m_speedMBs = 0.0;

    // Async thread for running coordination and reporting
    std::thread m_coordinatorThread;
    std::atomic<bool> m_coordinatorRunning = false;

    // Direct helper: scans drive seek topology properties via Win32 DeviceIoControl
    bool QueryVolumeSeekPenalty(const std::wstring& driveRoot, bool& outIncursSeekPenalty) const;

    // Scanning directories recursively to map files/folders
    bool ScanDirectory(const std::wstring& path, const std::wstring& relPath);

    // Thread pool worker body
    void WorkerThreadProcedure();

    // The main coordination engine
    void RunMigrationProcedure();

    // Asynchronously dispatch progress telemetry to the Win32 message loop
    void PostProgressUpdate(bool isComplete, bool isFailed, const std::wstring& errorMsg = L"");

public:
    TaskCoordinator(
        const std::wstring& sourceRoot,
        const std::wstring& destRoot,
        RedirectStrategy strategy,
        HWND uiWindow,
        const std::wstring& appDataPath
    );

    ~TaskCoordinator();

    // The Hive: Storage topology checker. Checks for seek penalties and scales thread counts.
    bool AnalyzeTopology(bool& outSourceIsHDD, bool& outDestIsHDD, uint32_t& outThreadCount);

    // Launches asynchronous multi-threaded pipeline
    bool StartMigration();

    // Set cancellation flag to abort active copy queues
    void CancelMigration();

    // Async size calculation helpers
    static uint64_t CalculateDirectorySize(const std::wstring& path);
    static void CalculateDirectorySizeAsync(const std::wstring& path, HWND uiWindow);

    // Read-only getters
    uint64_t GetTotalBytes() const { return m_totalBytes; }
    uint32_t GetTotalFiles() const { return static_cast<uint32_t>(m_files.size()); }
    bool IsRunning() const { return m_coordinatorRunning.load(); }
};
