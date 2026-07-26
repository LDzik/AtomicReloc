#pragma once

#include "Common.h"
#include <vector>
#include <string>
#include <restartmanager.h>

// Direct native MSVC linker command to bind restart manager libraries
#pragma comment(lib, "rstrtmgr.lib")

struct ProcessLockInfo {
    DWORD dwProcessId = 0;
    std::wstring strAppName;
    std::wstring strServiceShortName;
    RM_APP_TYPE appType = RmUnknownApp;
};

class RestartManagerSession {
private:
    DWORD m_dwSessionHandle = 0xFFFFFFFF;
    WCHAR m_szSessionKey[CCH_RM_SESSION_KEY + 1] = {0};
    bool m_bSessionStarted = false;

public:
    RestartManagerSession() {
        DWORD dwSessionHandle = 0;
        // Start Restart Manager session
        DWORD dwError = ::RmStartSession(&dwSessionHandle, 0, m_szSessionKey);
        
        if (dwError == ERROR_SUCCESS) {
            m_dwSessionHandle = dwSessionHandle;
            m_bSessionStarted = true;
            Logger::Log(Logger::Level::Info, L"RestartManager: Successfully initialized session handle: " + std::to_wstring(dwSessionHandle));
        } else {
            Logger::Log(Logger::Level::Error, L"RestartManager: Failed to initialize session. Error: " + GetLastErrorAsString(dwError));
        }
    }

    // RAII destructor ensures no background handles are leaked under any condition
    ~RestartManagerSession() {
        Close();
    }

    void Close() {
        if (m_bSessionStarted && m_dwSessionHandle != 0xFFFFFFFF) {
            DWORD dwError = ::RmEndSession(m_dwSessionHandle);
            if (dwError == ERROR_SUCCESS) {
                Logger::Log(Logger::Level::Info, L"RestartManager: Ended session handle: " + std::to_wstring(m_dwSessionHandle));
            } else {
                Logger::Log(Logger::Level::Warning, L"RestartManager: Failed to end session cleanly. Error: " + GetLastErrorAsString(dwError));
            }
            m_dwSessionHandle = 0xFFFFFFFF;
            m_bSessionStarted = false;
        }
    }

    DWORD GetHandle() const { return m_dwSessionHandle; }
    bool IsValid() const { return m_bSessionStarted; }
};

namespace RestartManagerEngine {
    // Utility interface: recursively checks for background processes holding active locks inside scope
    bool GetProcessesLockingDirectory(
        const std::wstring& directoryPath, 
        std::vector<ProcessLockInfo>& outProcesses,
	HWND hwndUI = NULL
    );
}
