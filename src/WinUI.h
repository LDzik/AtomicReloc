#pragma once

#include "Common.h"
#include "TaskCoordinator.h"
#include "RestartManager.h"
#include "JournalManager.h"
#include <commctrl.h>
#include <memory>

// Ensure common controls version 6 is linked for visual styles
#pragma comment(lib, "comctl32.lib")

class AtomicRelocUI
{
private:
    HWND m_hwnd = nullptr;
    HFONT m_hFont = nullptr;
    HFONT m_hFontBold = nullptr;
    HFONT m_hFontHeader = nullptr;

    struct DiscoveredFolder
    {
        std::wstring name;
        std::wstring fullSourcePath;
        std::wstring physicalPath;
        uint64_t size = 0;
        bool sizeCalculated = false;
        bool isJunction = false;
        bool isSymlink = false;
    };

    // Control Windows Handles
    HWND m_hEditSrc = nullptr;
    HWND m_hEditDst = nullptr;
    HWND m_hComboStrategy = nullptr;
    HWND m_hBtnSrc = nullptr;
    HWND m_hBtnDst = nullptr;
    HWND m_hBtnScan = nullptr;
    HWND m_hBtnMigrate = nullptr;
    HWND m_hBtnCancel = nullptr;
    HWND m_hBtnRefresh = nullptr;
    HWND m_hProgressBar = nullptr;
    HWND m_hLabelStatus = nullptr;
    HWND m_hLabelSpeed = nullptr;
    HWND m_hListViewLog = nullptr;
    HWND m_hListViewFolders = nullptr;

    // Additional Control Handles for Polish Pass
    HWND m_hEditSearch = nullptr;
    HWND m_hChkShowHidden = nullptr;
    HWND m_hChkShowSystem = nullptr;
    HWND m_hLabelSrc = nullptr;
    HWND m_hLabelDst = nullptr;
    HWND m_hLabelStrategy = nullptr;
    HWND m_hLabelSearch = nullptr;
    HWND m_hSeparator1 = nullptr;
    HWND m_hSeparator2 = nullptr;

    std::unique_ptr<TaskCoordinator> m_coordinator;
    std::wstring m_appDataPath;
    bool m_isMigrating = false;

    std::vector<DiscoveredFolder> m_folders;

    // Search and Sort State
    bool m_showHidden = false;
    bool m_showSystem = false;
    std::wstring m_searchQuery;
    int m_sortColumn = -1;
    bool m_sortAscending = true;
    std::wstring m_originalLocation;
    std::wstring m_newLocation;
    bool m_inLayout = false; // Recursion guard for sizing updates

    // GUI Identifiers
    static constexpr int ID_BTN_SRC = 1001;
    static constexpr int ID_BTN_DST = 1002;
    static constexpr int ID_BTN_SCAN = 1003;
    static constexpr int ID_BTN_MIGRATE = 1004;
    static constexpr int ID_BTN_CANCEL = 1005;
    static constexpr int ID_COMBO_STRAT = 1006;
    static constexpr int ID_BTN_REFRESH = 1007;
    static constexpr int ID_EDIT_SEARCH = 1008;
    static constexpr int ID_CHK_HIDDEN = 1009;
    static constexpr int ID_EDIT_SRC = 1010;
    static constexpr int ID_CHK_SYSTEM = 1011;
    static constexpr int ID_EDIT_DST = 1012;

    // Internal layouts and event callbacks
    void CreateChildControls();
    void SaveSettings();
    void LoadSettings();
    void UpdateButtonState();
    void ApplySegoeUIFont();
    void BrowseFolder(HWND hEditTarget);
    void HandleScanAndLockCheck();
    void HandleStartMigration();
    void HandleCancelMigration();

    void SetControlsEnabled(bool enabled);
    void UpdateLayout();
    void ScanRootLibrary();
    std::wstring FormatBytes(uint64_t bytes);

    // Dynamic UI Helper Methods
    void ApplySearchFilter();
    void SortFolders(int column);
    void CopySelectedLogsToClipboard();
    static int CALLBACK CompareFolders(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort);

public:
    explicit AtomicRelocUI(const std::wstring &appDataPath);
    ~AtomicRelocUI();

    // Setup window loops and bindings
    bool Initialize(HINSTANCE hInst, int nCmdShow);

    // Non-blocking WndProc message routes
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    // Global WndProc router
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void AddLogEntry(const std::wstring &message, Logger::Level level = Logger::Level::Info);
};
