#include "RestartManager.h"
#include "FileSystemEngine.h"
#include "WinUI.h"
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <RestartManager.h>
#include <set>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "rstrtmgr.lib")

#define WM_USER_LOCK_SCAN_PROGRESS (WM_USER + 4)

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

#define SystemExtendedHandleInformation 64

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX, *PSYSTEM_HANDLE_INFORMATION_EX;

typedef NTSTATUS(NTAPI* fNtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

extern AtomicRelocUI* g_pAppUI;

namespace RestartManagerEngine {

    void TraceUI(Logger::Level level, const std::wstring& message) {
        if (g_pAppUI) {
            g_pAppUI->AddLogEntry(message, level);
        }
    }

    std::wstring NormalizeDirectoryPath(const std::wstring& path) {
        HANDLE hDir = ::CreateFileW(path.c_str(), GENERIC_READ, 
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hDir == INVALID_HANDLE_VALUE) {
            return L"";
        }

        std::vector<wchar_t> buffer(MAX_PATH);
        DWORD res = ::GetFinalPathNameByHandleW(hDir, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        
        if (res > buffer.size()) {
            buffer.resize(res);
            res = ::GetFinalPathNameByHandleW(hDir, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        ::CloseHandle(hDir);

        if (res == 0) return L"";

        std::wstring result(buffer.data(), res);
        std::transform(result.begin(), result.end(), result.begin(), ::towlower);
        
        if (!result.empty() && result.back() != L'\\') {
            result += L'\\';
        }
        return result;
    }

    // NEW: Queries Restart Manager for rich data on a SINGLE process, which is instant.
    void EnhanceProcessInfoViaRM(DWORD pid, HANDLE hProcess, ProcessLockInfo& info) {
        // Set basic fallbacks
        info.appType = RmUnknownApp; // RmUnknownApp
        info.strServiceShortName = L"";

        FILETIME creationTime, exitTime, kernelTime, userTime;
        if (!::GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime, &userTime)) {
            return;
        }

        RM_UNIQUE_PROCESS rmUniqueProc = {0};
        rmUniqueProc.dwProcessId = pid;
        rmUniqueProc.ProcessStartTime = creationTime;

        DWORD dwSession;
        WCHAR szSessionKey[CCH_RM_SESSION_KEY + 1] = { 0 };
        
        if (::RmStartSession(&dwSession, 0, szSessionKey) == ERROR_SUCCESS) {
            // Register just the ONE process we know is locking the folder
            if (::RmRegisterResources(dwSession, 0, NULL, 1, &rmUniqueProc, 0, NULL) == ERROR_SUCCESS) {
                UINT nProcInfoNeeded = 0;
                UINT nProcInfo = 0;
                DWORD dwRebootReasons = RmRebootReasonNone;

                DWORD dwError = ::RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo, NULL, &dwRebootReasons);
                if (dwError == ERROR_MORE_DATA || dwError == ERROR_SUCCESS) {
                    nProcInfo = nProcInfoNeeded;
                    std::vector<RM_PROCESS_INFO> rmInfos(nProcInfo);
                    
                    if (::RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo, rmInfos.data(), &dwRebootReasons) == ERROR_SUCCESS) {
                        if (nProcInfo > 0) {
                            // Successfully retrieved Restart Manager data!
                            info.appType = rmInfos[0].ApplicationType;
                            info.strServiceShortName = rmInfos[0].strServiceShortName;
                            
                            // Prefer RM's localized app name over the raw executable name if available
                            if (wcslen(rmInfos[0].strAppName) > 0) {
                                info.strAppName = rmInfos[0].strAppName;
                            }
                        }
                    }
                }
            }
            ::RmEndSession(dwSession);
        }
    }

    bool GetProcessesLockingDirectory(
        const std::wstring& directoryPath, 
        std::vector<ProcessLockInfo>& outProcesses,
        HWND hwndUI
    ) {
        ::SetLastError(ERROR_SUCCESS);
        outProcesses.clear();

        if (directoryPath.empty()) {
            TraceUI(Logger::Level::Error, L"[RM Trace] Target path is empty.");
            return false;
        }

        std::wstring targetPath = NormalizeDirectoryPath(directoryPath);
        if (targetPath.empty()) {
            TraceUI(Logger::Level::Error, L"[RM Trace] Failed to normalize directory path.");
            return false;
        }

        HMODULE hNtDll = ::GetModuleHandleW(L"ntdll.dll");
        if (!hNtDll) return false;

        auto NtQuerySystemInfo = reinterpret_cast<fNtQuerySystemInformation>(
            ::GetProcAddress(hNtDll, "NtQuerySystemInformation")
        );
        if (!NtQuerySystemInfo) return false;

        ULONG bufferSize = 4 * 1024 * 1024; 
        std::vector<BYTE> buffer(bufferSize);
        NTSTATUS status;

        while ((status = NtQuerySystemInfo(SystemExtendedHandleInformation, buffer.data(), bufferSize, &bufferSize)) == STATUS_INFO_LENGTH_MISMATCH) {
            buffer.resize(bufferSize);
        }

        if (status != STATUS_SUCCESS) return false;

        auto* handleInfo = reinterpret_cast<PSYSTEM_HANDLE_INFORMATION_EX>(buffer.data());
        std::set<DWORD> seenPids;

        if (hwndUI) ::PostMessageW(hwndUI, WM_USER_LOCK_SCAN_PROGRESS, 10, 0);

        for (ULONG_PTR i = 0; i < handleInfo->NumberOfHandles; ++i) {
            if (i % 20000 == 0 && hwndUI) {
                int percent = 10 + static_cast<int>((i * 90) / handleInfo->NumberOfHandles);
                ::PostMessageW(hwndUI, WM_USER_LOCK_SCAN_PROGRESS, percent, 0);
            }

            auto& handleEntry = handleInfo->Handles[i];
            DWORD pid = static_cast<DWORD>(handleEntry.UniqueProcessId);

            if (pid <= 4) continue;
            if (seenPids.count(pid)) continue;
            if (handleEntry.GrantedAccess == 0x0012019f) continue;

            HANDLE hProcess = ::OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!hProcess) continue;

            HANDLE hDup = NULL;
            if (::DuplicateHandle(hProcess, reinterpret_cast<HANDLE>(handleEntry.HandleValue), 
                                  ::GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                
                if (::GetFileType(hDup) == FILE_TYPE_DISK) {
                    std::vector<wchar_t> pathBuffer(MAX_PATH);
                    DWORD res = ::GetFinalPathNameByHandleW(hDup, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                    
                    if (res > pathBuffer.size()) {
                        pathBuffer.resize(res);
                        res = ::GetFinalPathNameByHandleW(hDup, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                    }

                    if (res > 0 && res <= pathBuffer.size()) {
                        std::wstring handlePath(pathBuffer.data(), res);
                        std::transform(handlePath.begin(), handlePath.end(), handlePath.begin(), ::towlower);

                        if (handlePath.find(targetPath) == 0 || handlePath == targetPath.substr(0, targetPath.length() - 1)) {
                            seenPids.insert(pid);

                            ProcessLockInfo info;
                            info.dwProcessId = pid;
                            
                            // Base executable name fallback
                            wchar_t processName[MAX_PATH] = L"<unknown>";
                            if (::GetModuleBaseNameW(hProcess, NULL, processName, MAX_PATH) > 0) {
                                info.strAppName = processName;
                            } else {
                                info.strAppName = L"Unknown Process";
                            }

                            // Fill appType and strServiceShortName using the Restart Manager
                            EnhanceProcessInfoViaRM(pid, hProcess, info);

                            outProcesses.push_back(info);
                        }
                    }
                }
                ::CloseHandle(hDup);
            }
            ::CloseHandle(hProcess);
        }

        if (hwndUI) ::PostMessageW(hwndUI, WM_USER_LOCK_SCAN_PROGRESS, 100, 0);
        return true;
    }
}