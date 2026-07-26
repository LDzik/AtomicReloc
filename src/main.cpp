#include "Common.h"
#include "WinUI.h"
#include "JournalManager.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellscalingapi.h>

#pragma comment(lib, "comctl32.lib")

// Inline recovery helper callback
static void RecoveryProgressCallback(const std::wstring& progressMessage) {
    Logger::Log(Logger::Level::Info, L"Recovery Service: " + progressMessage);
}

// Get the local AppData directory path for AtomicReloc
static std::wstring GetApplicationAppDataPath() {
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(::SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path))) {
        std::wstring appPath = std::wstring(path) + L"\\AtomicReloc";
        // Ensure directories are created
        ::CreateDirectoryW(appPath.c_str(), NULL);
        return appPath;
    }
    return L".";
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 1. Initialize High-DPI scaling context if available (PerMonitorV2)
    // Using SetProcessDpiAwarenessContext is modern, with fallback to SetProcessDPIAware
    HMODULE hUser32 = ::GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
        auto pfnSetProcessDpiAwarenessContext = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            ::GetProcAddress(hUser32, "SetProcessDpiAwarenessContext")
        );
        if (pfnSetProcessDpiAwarenessContext) {
            pfnSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            ::SetProcessDPIAware();
        }
    } else {
        ::SetProcessDPIAware();
    }

    // 2. Initialize Win32 Common Controls
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    if (!::InitCommonControlsEx(&icex)) {
        ::MessageBoxW(NULL, L"Failed to initialize Windows Common Controls (comctl32.dll).", L"Critical Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 3. Set up Roaming/Local AppData Directory
    std::wstring appDataPath = GetApplicationAppDataPath();
    Logger::Log(Logger::Level::Info, L"AtomicReloc AppData path: " + appDataPath);

    // 4. Automated Startup Crash Recovery Sequence
    JournalManager jm(appDataPath);
    std::vector<JournalData> activeJournals;
    
    if (jm.FindActiveJournals(activeJournals) && !activeJournals.empty()) {
        Logger::Log(Logger::Level::Warning, L"Discovered " + std::to_wstring(activeJournals.size()) + L" interrupted transaction logs.");

        for (const auto& journal : activeJournals) {
            std::wstring stratStr = (journal.strategy == RedirectStrategy::SymbolicLink) ? L"Symbolic Link" : L"Directory Junction";
            
            std::wstring content = L"AtomicReloc detected an interrupted folder migration:\n\n"
                                   L"Source: " + journal.sourcePath + L"\n"
                                   L"Destination: " + journal.destPath + L"\n"
                                   L"Strategy: " + stratStr + L"\n\n"
                                   L"This can happen if the system crashed, lost power, or the application was terminated during relocation. Choose an action to restore safety:";

            TASKDIALOGCONFIG tdConfig = {};
            tdConfig.cbSize = sizeof(tdConfig);
            tdConfig.hwndParent = NULL;
            tdConfig.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
            tdConfig.pszWindowTitle = L"AtomicReloc - Crash Recovery";
            tdConfig.pszMainInstruction = L"Unfinished Migration Transaction Detected!";
            tdConfig.pszContent = content.c_str();
            tdConfig.pszMainIcon = TD_WARNING_ICON;

            TASKDIALOG_BUTTON buttons[3] = {};
            buttons[0].nButtonID = 1000;
            buttons[0].pszButtonText = L"Resume Relocation\nAttempts to complete the redirection link and finalize backup folder removal.";
            buttons[1].nButtonID = 1001;
            buttons[1].pszButtonText = L"Rollback Relocation (Recommended)\nRestores original folder from backup and purges destination files.";
            buttons[2].nButtonID = 1002;
            buttons[2].pszButtonText = L"Discard Journal Log\nDeletes the journal file, leaving files untouched. Use only if manually recovered.";

            tdConfig.cButtons = 3;
            tdConfig.pButtons = buttons;

            int selectedButton = 0;
            HRESULT hr = ::TaskDialogIndirect(&tdConfig, &selectedButton, NULL, NULL);

            if (SUCCEEDED(hr)) {
                if (selectedButton == 1000) {
                    Logger::Log(Logger::Level::Info, L"User selected: Resume Transaction " + journal.uuid);
                    if (jm.RecoverResume(journal, RecoveryProgressCallback)) {
                        ::MessageBoxW(NULL, L"Successfully completed and resumed migration transaction.", L"Recovery Success", MB_OK | MB_ICONINFORMATION);
                    } else {
                        ::MessageBoxW(NULL, L"Failed to resume migration transaction cleanly. Original backups are safe; rollback recommended.", L"Recovery Failure", MB_OK | MB_ICONERROR);
                    }
                }
                else if (selectedButton == 1001) {
                    Logger::Log(Logger::Level::Info, L"User selected: Rollback Transaction " + journal.uuid);
                    if (jm.RecoverRollback(journal, RecoveryProgressCallback)) {
                        ::MessageBoxW(NULL, L"Successfully rolled back relocation transaction. Original state restored.", L"Recovery Success", MB_OK | MB_ICONINFORMATION);
                    } else {
                        ::MessageBoxW(NULL, L"Failed to rollback relocation transaction cleanly.", L"Recovery Failure", MB_OK | MB_ICONERROR);
                    }
                }
                else if (selectedButton == 1002) {
                    Logger::Log(Logger::Level::Warning, L"User selected: Discard Journal " + journal.uuid);
                    std::wstring path = appDataPath + L"\\journals\\" + journal.uuid + L".mover-journal";
                    ::DeleteFileW(path.c_str());
                    ::MessageBoxW(NULL, L"Journal log discarded. System state left unchanged.", L"Recovery Warning", MB_OK | MB_ICONWARNING);
                }
            }
        }
    }

    // 5. Spin up standard UI frame
    AtomicRelocUI ui(appDataPath);
    if (!ui.Initialize(hInstance, nCmdShow)) {
        return 1;
    }

    // 6. Classic Win32 Message Loop
    MSG msg = {};
    while (::GetMessageW(&msg, NULL, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
