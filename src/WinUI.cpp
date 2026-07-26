#include "WinUI.h"
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>
#include <thread>

#pragma comment(lib, "shell32.lib")

#define WM_USER_LOCK_SCAN_COMPLETE (WM_USER + 3)
#define WM_USER_LOCK_SCAN_PROGRESS (WM_USER + 4)

struct LockScanResult
{
    bool success;
    std::wstring srcPath;
    std::vector<ProcessLockInfo> lockingProcesses;
};

// Global pointer helper for WndProc mapping
AtomicRelocUI *g_pAppUI = nullptr;

namespace
{
    std::wstring GetFullyResolvedPath(const std::wstring &p)
    {
        return FileSystemEngine::GetFullyResolvedPhysicalPath(p);
    }
}

AtomicRelocUI::AtomicRelocUI(const std::wstring &appDataPath) : m_appDataPath(appDataPath)
{
    g_pAppUI = this;
}

AtomicRelocUI::~AtomicRelocUI()
{
    if (m_hFont)
        ::DeleteObject(m_hFont);
    if (m_hFontBold)
        ::DeleteObject(m_hFontBold);
    if (m_hFontHeader)
        ::DeleteObject(m_hFontHeader);
}

bool AtomicRelocUI::Initialize(HINSTANCE hInst, int nCmdShow)
{
    // 1. Register main Window Class
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = AtomicRelocUI::WndProc;
    wcex.hInstance = hInst;
    wcex.hCursor = ::LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"AtomicRelocWindowClass";
    wcex.hIcon = ::LoadIcon(hInst, MAKEINTRESOURCE(1));
    wcex.hIconSm = ::LoadIcon(hInst, MAKEINTRESOURCE(1));

    if (!::RegisterClassExW(&wcex))
    {
        Logger::Log(Logger::Level::Error, L"UI: Failed to register window class.");
        return false;
    }

    // Allocate coordinates for perfect display centering
    int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);
    int winWidth = 800;
    int winHeight = 650;
    int posX = (screenWidth - winWidth) / 2;
    int posY = (screenHeight - winHeight) / 2;

    // 2. Create the window
    m_hwnd = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"AtomicRelocWindowClass",
        L"AtomicReloc",
        WS_OVERLAPPEDWINDOW,
        posX, posY, winWidth, winHeight,
        NULL, NULL, hInst, NULL);

    if (!m_hwnd)
    {
        Logger::Log(Logger::Level::Error, L"UI: Failed to create main window.");
        return false;
    }

    ::ShowWindow(m_hwnd, nCmdShow);
    ::UpdateWindow(m_hwnd);

    AddLogEntry(L"AtomicReloc Engine successfully initialized.", Logger::Level::Success);
    AddLogEntry(L"Ready to ingest folder migration tasks.", Logger::Level::Info);

    return true;
}

void AtomicRelocUI::SaveSettings()
{
    HKEY hKey;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\AtomicReloc", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        // Save Original Location
        wchar_t src[MAX_PATH] = {0};
        ::GetWindowTextW(m_hEditSrc, src, MAX_PATH);
        ::RegSetValueExW(hKey, L"OriginalLocation", 0, REG_SZ, reinterpret_cast<const BYTE *>(src), static_cast<DWORD>((wcslen(src) + 1) * sizeof(wchar_t)));

        // Save New Location
        wchar_t dst[MAX_PATH] = {0};
        ::GetWindowTextW(m_hEditDst, dst, MAX_PATH);
        ::RegSetValueExW(hKey, L"NewLocation", 0, REG_SZ, reinterpret_cast<const BYTE *>(dst), static_cast<DWORD>((wcslen(dst) + 1) * sizeof(wchar_t)));

        // Save Checkbox States
        DWORD hidden = (::SendMessageW(m_hChkShowHidden, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
        ::RegSetValueExW(hKey, L"ShowHidden", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&hidden), sizeof(hidden));

        DWORD system = (::SendMessageW(m_hChkShowSystem, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
        ::RegSetValueExW(hKey, L"ShowSystem", 0, REG_DWORD, reinterpret_cast<const BYTE *>(&system), sizeof(system));

        ::RegCloseKey(hKey);
    }
}

void AtomicRelocUI::LoadSettings()
{
    HKEY hKey;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AtomicReloc", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t buffer[MAX_PATH] = {0};
        DWORD bufSize;

        // Load Original Location
        bufSize = sizeof(buffer);
        if (::RegQueryValueExW(hKey, L"OriginalLocation", NULL, NULL, reinterpret_cast<LPBYTE>(buffer), &bufSize) == ERROR_SUCCESS)
        {
            ::SetWindowTextW(m_hEditSrc, buffer);
        }

        // Load New Location
        bufSize = sizeof(buffer);
        if (::RegQueryValueExW(hKey, L"NewLocation", NULL, NULL, reinterpret_cast<LPBYTE>(buffer), &bufSize) == ERROR_SUCCESS)
        {
            ::SetWindowTextW(m_hEditDst, buffer);
        }

        // Load Checkbox States
        DWORD val = 0;
        DWORD valSize = sizeof(val);
        if (::RegQueryValueExW(hKey, L"ShowHidden", NULL, NULL, reinterpret_cast<LPBYTE>(&val), &valSize) == ERROR_SUCCESS)
        {
            ::SendMessageW(m_hChkShowHidden, BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
            m_showHidden = (val != 0);
        }

        valSize = sizeof(val);
        if (::RegQueryValueExW(hKey, L"ShowSystem", NULL, NULL, reinterpret_cast<LPBYTE>(&val), &valSize) == ERROR_SUCCESS)
        {
            ::SendMessageW(m_hChkShowSystem, BM_SETCHECK, val ? BST_CHECKED : BST_UNCHECKED, 0);
            m_showSystem = (val != 0);
        }

        ::RegCloseKey(hKey);
    }

    // Auto-scan if an original location was loaded
    if (::GetWindowTextLengthW(m_hEditSrc) > 0)
    {
        ScanRootLibrary();
    }
}

void AtomicRelocUI::CreateChildControls()
{
    ApplySegoeUIFont();

    // Helper lambda to create labels and apply Segoe UI Variable font explicitly
    auto CreateLabel = [&](const wchar_t *text, int x, int y, int w, int h)
    {
        HWND hLabel = ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h, m_hwnd, NULL, NULL, NULL);
        if (hLabel)
        {
            ::SendMessageW(hLabel, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
        }
        return hLabel;
    };

    // Helper lambda to create buttons and apply Segoe UI Variable font explicitly
    auto CreateButton = [&](const wchar_t *text, int x, int y, int w, int h, int id, bool isDefault = false)
    {
        DWORD style = WS_CHILD | WS_VISIBLE | (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
        HWND hBtn = ::CreateWindowExW(0, L"BUTTON", text, style, x, y, w, h, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), NULL, NULL);
        if (hBtn)
        {
            ::SendMessageW(hBtn, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
        }
        return hBtn;
    };

    m_hLabelSrc = CreateLabel(L"Original Location:", 0, 0, 10, 10);
    m_hEditSrc = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_SRC)), NULL, NULL);
    ::SendMessageW(m_hEditSrc, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    m_hBtnSrc = CreateButton(L"Browse...", 0, 0, 10, 10, ID_BTN_SRC);

    m_hLabelDst = CreateLabel(L"New Location:", 0, 0, 10, 10);
    m_hEditDst = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_DST)), NULL, NULL);
    ::SendMessageW(m_hEditDst, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    m_hBtnDst = CreateButton(L"Browse...", 0, 0, 10, 10, ID_BTN_DST);

    m_hLabelStrategy = CreateLabel(L"Connection Method:", 0, 0, 10, 10);
    m_hComboStrategy = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_COMBO_STRAT)), NULL, NULL);
    ::SendMessageW(m_hComboStrategy, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    ::SendMessageW(m_hComboStrategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Directory Junction"));
    ::SendMessageW(m_hComboStrategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Symbolic Link"));
    ::SendMessageW(m_hComboStrategy, CB_SETCURSEL, 0, 0);

    m_hBtnRefresh = CreateButton(L"Refresh", 0, 0, 10, 10, ID_BTN_REFRESH);
    m_hBtnScan = CreateButton(L"Check Locks", 0, 0, 10, 10, ID_BTN_SCAN);
    ::EnableWindow(m_hBtnScan, FALSE);

    m_hBtnMigrate = CreateButton(L"Relocate", 0, 0, 10, 10, ID_BTN_MIGRATE, true);
    ::EnableWindow(m_hBtnMigrate, FALSE);

    m_hBtnCancel = CreateButton(L"Cancel", 0, 0, 10, 10, ID_BTN_CANCEL);
    ::EnableWindow(m_hBtnCancel, FALSE);

    // Separators
    m_hSeparator1 = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 0, 10, 10, m_hwnd, NULL, NULL, NULL);

    // Search Controls
    m_hLabelSearch = CreateLabel(L"Search Folders:", 0, 0, 10, 10);
    m_hEditSearch = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_SEARCH)), NULL, NULL);
    ::SendMessageW(m_hEditSearch, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);

    m_hChkShowHidden = ::CreateWindowExW(0, L"BUTTON", L"Show Hidden Folders", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHK_HIDDEN)), NULL, NULL);
    ::SendMessageW(m_hChkShowHidden, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);

    m_hChkShowSystem = ::CreateWindowExW(0, L"BUTTON", L"Show System Folders", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 10, 10, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHK_SYSTEM)), NULL, NULL);
    ::SendMessageW(m_hChkShowSystem, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);

    // Folder ListView
    m_hListViewFolders = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 10, 10,
        m_hwnd, NULL, NULL, NULL);
    ::SendMessageW(m_hListViewFolders, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    ::SendMessageW(m_hListViewFolders, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    // Insert Columns in Folder ListView
    LVCOLUMNW lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    lvc.cx = 200;
    lvc.pszText = const_cast<LPWSTR>(L"Folder Name");
    ::SendMessageW(m_hListViewFolders, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&lvc));

    lvc.cx = 120;
    lvc.pszText = const_cast<LPWSTR>(L"Status");
    ::SendMessageW(m_hListViewFolders, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&lvc));

    lvc.cx = 300;
    lvc.pszText = const_cast<LPWSTR>(L"Current Location");
    ::SendMessageW(m_hListViewFolders, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&lvc));

    lvc.cx = 120;
    lvc.pszText = const_cast<LPWSTR>(L"Size");
    ::SendMessageW(m_hListViewFolders, LVM_INSERTCOLUMNW, 3, reinterpret_cast<LPARAM>(&lvc));

    lvc.cx = 100;
    lvc.pszText = const_cast<LPWSTR>(L"Method");
    ::SendMessageW(m_hListViewFolders, LVM_INSERTCOLUMNW, 4, reinterpret_cast<LPARAM>(&lvc));

    HWND hHeaderFolders = reinterpret_cast<HWND>(::SendMessageW(m_hListViewFolders, LVM_GETHEADER, 0, 0));
    if (hHeaderFolders)
    {
        ::SendMessageW(hHeaderFolders, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    }

    // Status and Progress Labels
    m_hLabelStatus = CreateLabel(L"Idle. Select an Original Location and New Location to begin.", 0, 0, 10, 10);
    m_hProgressBar = ::CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10, 10, m_hwnd, NULL, NULL, NULL);
    m_hLabelSpeed = CreateLabel(L"0 MB/s - 0 bytes remaining", 0, 0, 10, 10);

    m_hSeparator2 = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 0, 10, 10, m_hwnd, NULL, NULL, NULL);

    // Log ListView
    m_hListViewLog = ::CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT,
        0, 0, 10, 10,
        m_hwnd, NULL, NULL, NULL);
    ::SendMessageW(m_hListViewLog, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    ::SendMessageW(m_hListViewLog, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    // Insert Columns in Log ListView
    LVCOLUMNW lvcLog = {};
    lvcLog.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    lvcLog.cx = 600;
    lvcLog.pszText = const_cast<LPWSTR>(L"Operation Details");
    ::SendMessageW(m_hListViewLog, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&lvcLog));

    lvcLog.cx = 130;
    lvcLog.pszText = const_cast<LPWSTR>(L"Time");
    ::SendMessageW(m_hListViewLog, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&lvcLog));

    HWND hHeaderLog = reinterpret_cast<HWND>(::SendMessageW(m_hListViewLog, LVM_GETHEADER, 0, 0));
    if (hHeaderLog)
    {
        ::SendMessageW(hHeaderLog, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFont), TRUE);
    }

    UpdateLayout();

    LoadSettings();
}

void AtomicRelocUI::ApplySegoeUIFont()
{
    // Generate clean Segoe UI Variable bindings
    m_hFont = ::CreateFontW(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable");

    m_hFontBold = ::CreateFontW(
        -12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable");

    m_hFontHeader = ::CreateFontW(
        -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable");
}

void AtomicRelocUI::BrowseFolder(HWND hEditTarget)
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = m_hwnd;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    bi.lpszTitle = L"Select folder to load into AtomicReloc:";

    PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderW(&bi);
    if (pidl != nullptr)
    {
        wchar_t path[MAX_PATH] = {0};
        if (::SHGetPathFromIDListW(pidl, path))
        {
            ::SetWindowTextW(hEditTarget, path);
            if (hEditTarget == m_hEditSrc)
            {
                ScanRootLibrary();
            }
            else if (hEditTarget == m_hEditDst)
            {
                UpdateButtonState();
            }
        }
        ::CoTaskMemFree(pidl);
    }
}

void AtomicRelocUI::AddLogEntry(const std::wstring &message, Logger::Level level)
{
    if (!m_hListViewLog)
        return;

    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT;
    lvi.pszText = const_cast<LPWSTR>(L"");
    lvi.iItem = static_cast<int>(::SendMessageW(m_hListViewLog, LVM_GETITEMCOUNT, 0, 0));
    lvi.iSubItem = 0;

    // Inset item
    int index = static_cast<int>(::SendMessageW(m_hListViewLog, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvi)));

    // Format message string
    std::wstring levelStr;
    switch (level)
    {
    case Logger::Level::Info:
        levelStr = L"";
        break;
    case Logger::Level::Warning:
        levelStr = L"[!] ";
        break;
    case Logger::Level::Error:
        levelStr = L"[ERROR] ";
        break;
    case Logger::Level::Success:
        levelStr = L"[SUCCESS] ";
        break;
    }
    std::wstring text = levelStr + message;
    ::SendMessageW(m_hListViewLog, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&(lvi.pszText = const_cast<LPWSTR>(text.c_str()), lvi.iSubItem = 0, &lvi)));

    // Add current timestamp column
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t timeBuf[32];
    ::StringCchPrintfW(timeBuf, 32, L"%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    ::SendMessageW(m_hListViewLog, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&(lvi.pszText = timeBuf, lvi.iSubItem = 1, &lvi)));

    // Auto-scroll listview down
    ::SendMessageW(m_hListViewLog, LVM_ENSUREVISIBLE, index, FALSE);
}

void AtomicRelocUI::SetControlsEnabled(bool enabled)
{
    ::EnableWindow(m_hBtnSrc, enabled ? TRUE : FALSE);
    ::EnableWindow(m_hBtnDst, enabled ? TRUE : FALSE);
    ::EnableWindow(m_hComboStrategy, enabled ? TRUE : FALSE);
    ::EnableWindow(m_hBtnRefresh, enabled ? TRUE : FALSE);
    ::EnableWindow(m_hEditSearch, enabled ? TRUE : FALSE);
    ::EnableWindow(m_hChkShowHidden, enabled ? TRUE : FALSE);
    ::EnableWindow(m_hChkShowSystem, enabled ? TRUE : FALSE);

    if (enabled)
    {
        int selectedIndex = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        ::EnableWindow(m_hBtnScan, selectedIndex != -1 ? TRUE : FALSE);
        ::EnableWindow(m_hBtnMigrate, selectedIndex != -1 ? TRUE : FALSE);
    }
    else
    {
        ::EnableWindow(m_hBtnScan, FALSE);
        ::EnableWindow(m_hBtnMigrate, FALSE);
    }

    ::EnableWindow(m_hBtnCancel, enabled ? FALSE : TRUE);
}

void AtomicRelocUI::ScanRootLibrary()
{
    m_folders.clear();
    ::SendMessageW(m_hListViewFolders, LVM_DELETEALLITEMS, 0, 0);
    ::EnableWindow(m_hBtnScan, FALSE);
    ::EnableWindow(m_hBtnMigrate, FALSE);
    ::SetWindowTextW(m_hBtnMigrate, L"Relocate \u2192");
    ::InvalidateRect(m_hBtnMigrate, NULL, TRUE);
    ::UpdateWindow(m_hBtnMigrate);

    wchar_t src[MAX_PATH] = {0};
    ::GetWindowTextW(m_hEditSrc, src, MAX_PATH);
    m_originalLocation = FileSystemEngine::SanitizePath(src);

    if (m_originalLocation.empty() || !FileSystemEngine::DirectoryExists(m_originalLocation))
    {
        return;
    }

    std::wstring searchPattern = FileSystemEngine::JoinPath(m_originalLocation, L"*");
    WIN32_FIND_DATAW findData;
    HANDLE hFind = ::FindFirstFileW(searchPattern.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE)
    {
        AddLogEntry(L"Scan: Failed to open root directory " + m_originalLocation, Logger::Level::Error);
        return;
    }

    do
    {
        std::wstring name = findData.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }

        // System Garbage Filters
        bool isHidden = (findData.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        bool isSystem = (findData.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
        bool startsWithDollar = (name.rfind(L"$", 0) == 0);

        if (isHidden && !m_showHidden)
        {
            continue;
        }
        if ((isSystem || startsWithDollar) && !m_showSystem)
        {
            continue;
        }

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            std::wstring fullPath = FileSystemEngine::JoinPath(m_originalLocation, name);

            // Avoid scanning loose .mover_bak items
            if (name.rfind(L".mover_bak") != std::wstring::npos)
            {
                continue;
            }

            DiscoveredFolder folder;
            folder.name = name;
            folder.fullSourcePath = fullPath;
            folder.size = 0;
            folder.sizeCalculated = false;
            folder.isJunction = FileSystemEngine::IsReparsePoint(fullPath);
            folder.isSymlink = false;

            if (folder.isJunction)
            {
                if (findData.dwReserved0 == IO_REPARSE_TAG_SYMLINK)
                {
                    folder.isSymlink = true;
                }
                std::wstring target;
                std::vector<std::wstring> dummyChain;
                if (FileSystemEngine::ResolveTruePath(fullPath, target, dummyChain))
                {
                    folder.physicalPath = target;
                }
                else
                {
                    folder.physicalPath = fullPath;
                }
            }
            else
            {
                folder.physicalPath = fullPath;
            }

            m_folders.push_back(folder);

            // Spawn async size calculation thread targeting physical path of folder
            TaskCoordinator::CalculateDirectorySizeAsync(folder.physicalPath, m_hwnd);
        }
    } while (::FindNextFileW(hFind, &findData));

    ::FindClose(hFind);

    ApplySearchFilter();

    AddLogEntry(L"Discovered " + std::to_wstring(m_folders.size()) + L" folders in library root.", Logger::Level::Info);
}

void AtomicRelocUI::ApplySearchFilter()
{
    int selectedIndex = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    std::wstring selectedFolderName;
    if (selectedIndex != -1)
    {
        LVITEMW lvi = {};
        lvi.mask = LVIF_PARAM;
        lvi.iItem = selectedIndex;
        if (::SendMessageW(m_hListViewFolders, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lvi)))
        {
            size_t idx = static_cast<size_t>(lvi.lParam);
            if (idx < m_folders.size())
            {
                selectedFolderName = m_folders[idx].name;
            }
        }
    }

    ::SendMessageW(m_hListViewFolders, LVM_DELETEALLITEMS, 0, 0);

    wchar_t searchBuf[256] = {0};
    ::GetWindowTextW(m_hEditSearch, searchBuf, 256);
    std::wstring query = searchBuf;

    std::wstring queryLower = query;
    std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::towlower);

    int insertedCount = 0;
    for (size_t i = 0; i < m_folders.size(); ++i)
    {
        const auto &folder = m_folders[i];

        std::wstring nameLower = folder.name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);

        if (!queryLower.empty() && nameLower.find(queryLower) == std::wstring::npos)
        {
            continue;
        }

        // Add row to ListView
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = insertedCount;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<LPWSTR>(folder.name.c_str());
        lvi.lParam = static_cast<LPARAM>(i);

        int index = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvi)));

        // Column 2: Status Arrow
        std::wstring statusStr = folder.isJunction ? L"\u2190 Redirected" : L"\u2192 Source";
        ::SendMessageW(m_hListViewFolders, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&(lvi.pszText = const_cast<LPWSTR>(statusStr.c_str()), lvi.iSubItem = 1, &lvi)));

        // Column 3: Current Location
        ::SendMessageW(m_hListViewFolders, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&(lvi.pszText = const_cast<LPWSTR>(folder.physicalPath.c_str()), lvi.iSubItem = 2, &lvi)));

        // Column 4: Size
        std::wstring sizeStr = folder.sizeCalculated ? FormatBytes(folder.size) : L"Calculating...";
        ::SendMessageW(m_hListViewFolders, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&(lvi.pszText = const_cast<LPWSTR>(sizeStr.c_str()), lvi.iSubItem = 3, &lvi)));

        // Column 5: Method
        std::wstring methodStr = L"";
        if (folder.isJunction)
        {
            methodStr = folder.isSymlink ? L"Symlink" : L"Junction";
        }
        ::SendMessageW(m_hListViewFolders, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&(lvi.pszText = const_cast<LPWSTR>(methodStr.c_str()), lvi.iSubItem = 4, &lvi)));

        // Restore selection if matching
        if (!selectedFolderName.empty() && folder.name == selectedFolderName)
        {
            LVITEMW selItem = {};
            selItem.state = LVIS_SELECTED | LVIS_FOCUSED;
            selItem.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            ::SendMessageW(m_hListViewFolders, LVM_SETITEMSTATE, index, reinterpret_cast<LPARAM>(&selItem));
        }

        insertedCount++;
    }

    if (m_sortColumn != -1)
    {
        ::SendMessageW(m_hListViewFolders, LVM_SORTITEMS, reinterpret_cast<WPARAM>(this), reinterpret_cast<LPARAM>(CompareFolders));
    }
    UpdateButtonState();
}

void AtomicRelocUI::UpdateButtonState()
{
    int selectedIndex = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (selectedIndex == -1)
    {
        ::EnableWindow(m_hBtnScan, FALSE);
        ::EnableWindow(m_hBtnMigrate, FALSE);
        ::SetWindowTextW(m_hBtnMigrate, L"Relocate \u2192");
        ::InvalidateRect(m_hBtnMigrate, NULL, TRUE);
        ::UpdateWindow(m_hBtnMigrate);
        return;
    }

    LVITEMW lvi = {};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = selectedIndex;
    if (!::SendMessageW(m_hListViewFolders, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lvi)))
    {
        ::EnableWindow(m_hBtnScan, FALSE);
        ::EnableWindow(m_hBtnMigrate, FALSE);
        return;
    }
    size_t folderIndex = static_cast<size_t>(lvi.lParam);
    if (folderIndex >= m_folders.size())
    {
        ::EnableWindow(m_hBtnScan, FALSE);
        ::EnableWindow(m_hBtnMigrate, FALSE);
        return;
    }

    const auto &folder = m_folders[folderIndex];
    ::EnableWindow(m_hBtnScan, TRUE);

    wchar_t dstBuf[MAX_PATH] = {0};
    ::GetWindowTextW(m_hEditDst, dstBuf, MAX_PATH);
    m_newLocation = FileSystemEngine::SanitizePath(dstBuf);

    std::wstring expectedDest = m_newLocation.empty() ? L"" : FileSystemEngine::JoinPath(m_newLocation, folder.name);

    std::wstring resolvedTarget = FileSystemEngine::GetFullyResolvedPhysicalPath(folder.physicalPath);
    std::wstring resolvedNewLocation = m_newLocation.empty() ? L"" : FileSystemEngine::GetFullyResolvedPhysicalPath(expectedDest);

    std::wstring buttonLabel;
    bool isRestore = false;

    if (!folder.isJunction)
    {
        buttonLabel = L"Relocate \u2192";
    }
    else
    {
        if (m_newLocation.empty() || _wcsicmp(resolvedTarget.c_str(), resolvedNewLocation.c_str()) == 0)
        {
            buttonLabel = L"Restore \u2190";
            isRestore = true;
        }
        else
        {
            buttonLabel = L"Chained Move \u2192";
        }
    }

    ::SetWindowTextW(m_hBtnMigrate, buttonLabel.c_str());
    ::InvalidateRect(m_hBtnMigrate, NULL, TRUE);
    ::UpdateWindow(m_hBtnMigrate);

    // Disable button if resolved physical source equals resolved physical destination
    std::wstring resolvedPhysicalSource = FileSystemEngine::GetFullyResolvedPhysicalPath(folder.physicalPath);
    std::wstring resolvedPhysicalDestination;

    if (isRestore)
    {
        size_t lastSlash = folder.fullSourcePath.find_last_of(L'\\');
        if (lastSlash != std::wstring::npos && lastSlash > 0)
        {
            std::wstring parent = folder.fullSourcePath.substr(0, lastSlash);
            std::wstring child = folder.fullSourcePath.substr(lastSlash + 1);
            std::wstring resolvedParent = FileSystemEngine::GetFullyResolvedPhysicalPath(parent);
            resolvedPhysicalDestination = FileSystemEngine::NormalizePath(FileSystemEngine::JoinPath(resolvedParent, child));
        }
        else
        {
            resolvedPhysicalDestination = FileSystemEngine::GetFullyResolvedPhysicalPath(folder.fullSourcePath);
        }
    }
    else
    {
        resolvedPhysicalDestination = FileSystemEngine::GetFullyResolvedPhysicalPath(expectedDest);
    }

    // Strip trailing slashes for comparison
    while (resolvedPhysicalSource.length() > 3 && resolvedPhysicalSource.back() == L'\\')
    {
        resolvedPhysicalSource.pop_back();
    }
    while (resolvedPhysicalDestination.length() > 3 && resolvedPhysicalDestination.back() == L'\\')
    {
        resolvedPhysicalDestination.pop_back();
    }

    bool disableButton = false;
    if (m_newLocation.empty() && !isRestore)
    {
        disableButton = true;
    }
    else if (_wcsicmp(resolvedPhysicalSource.c_str(), resolvedPhysicalDestination.c_str()) == 0)
    {
        disableButton = true;
    }

    ::EnableWindow(m_hBtnMigrate, disableButton ? FALSE : TRUE);
}

void AtomicRelocUI::SortFolders(int column)
{
    if (column < 0 || column > 4)
        return;

    if (m_sortColumn == column)
    {
        m_sortAscending = !m_sortAscending;
    }
    else
    {
        m_sortColumn = column;
        m_sortAscending = true;
    }

    // Apply sort arrow markers to header control using native HDM_SETITEMW messages
    HWND hHeader = reinterpret_cast<HWND>(::SendMessageW(m_hListViewFolders, LVM_GETHEADER, 0, 0));
    if (hHeader)
    {
        int colCount = static_cast<int>(::SendMessageW(hHeader, HDM_GETITEMCOUNT, 0, 0));
        for (int i = 0; i < colCount; ++i)
        {
            HDITEMW hdi = {};
            hdi.mask = HDI_FORMAT;
            if (::SendMessageW(hHeader, HDM_GETITEMW, i, reinterpret_cast<LPARAM>(&hdi)))
            {
                // Clear existing sort flags
                hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);

                // Add sort flag for active column
                if (i == m_sortColumn)
                {
                    hdi.fmt |= (m_sortAscending ? HDF_SORTUP : HDF_SORTDOWN);
                }
                ::SendMessageW(hHeader, HDM_SETITEMW, i, reinterpret_cast<LPARAM>(&hdi));
            }
        }
    }

    ::SendMessageW(m_hListViewFolders, LVM_SORTITEMS, reinterpret_cast<WPARAM>(this), reinterpret_cast<LPARAM>(CompareFolders));
}

void AtomicRelocUI::CopySelectedLogsToClipboard()
{
    if (!m_hListViewLog)
        return;

    std::wstring clipboardText;
    int count = static_cast<int>(::SendMessageW(m_hListViewLog, LVM_GETITEMCOUNT, 0, 0));
    bool first = true;

    for (int i = 0; i < count; ++i)
    {
        UINT state = static_cast<UINT>(::SendMessageW(m_hListViewLog, LVM_GETITEMSTATE, i, LVIS_SELECTED));
        if (state & LVIS_SELECTED)
        {
            wchar_t detailBuf[1024] = {0};
            wchar_t timeBuf[128] = {0};

            LVITEMW lvi = {};
            lvi.iSubItem = 0;
            lvi.cchTextMax = 1024;
            lvi.pszText = detailBuf;
            ::SendMessageW(m_hListViewLog, LVM_GETITEMTEXTW, i, reinterpret_cast<LPARAM>(&lvi));

            lvi.iSubItem = 1;
            lvi.cchTextMax = 128;
            lvi.pszText = timeBuf;
            ::SendMessageW(m_hListViewLog, LVM_GETITEMTEXTW, i, reinterpret_cast<LPARAM>(&lvi));

            if (!first)
            {
                clipboardText += L"\r\n";
            }
            clipboardText += std::wstring(timeBuf) + L" - " + std::wstring(detailBuf);
            first = false;
        }
    }

    if (clipboardText.empty())
    {
        return;
    }

    if (::OpenClipboard(m_hwnd))
    {
        ::EmptyClipboard();
        size_t sizeInBytes = (clipboardText.length() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, sizeInBytes);
        if (hMem)
        {
            void *pMem = ::GlobalLock(hMem);
            if (pMem)
            {
                std::memcpy(pMem, clipboardText.c_str(), sizeInBytes);
                ::GlobalUnlock(hMem);
                ::SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
        ::CloseClipboard();
    }
    else
    {
        AddLogEntry(L"Failed to open clipboard.", Logger::Level::Error);
    }
}

void AtomicRelocUI::UpdateLayout()
{
    if (m_inLayout)
        return;
    m_inLayout = true;

    if (!m_hwnd)
    {
        m_inLayout = false;
        return;
    }

    RECT rect;
    ::GetClientRect(m_hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    if (width <= 0 || height <= 0)
    {
        m_inLayout = false;
        return;
    }

    int margin = 12;
    int rowHeight = 24;
    int spacing = 8;
    int labelWidth = 140;
    int browseWidth = 80;

    // Disable redrawing during atomic repositioning to prevent resizing stutter
    ::SendMessageW(m_hListViewFolders, WM_SETREDRAW, FALSE, 0);

    HDWP hdwp = ::BeginDeferWindowPos(23);
    if (!hdwp)
    {
        ::SendMessageW(m_hListViewFolders, WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(m_hListViewFolders, NULL, TRUE);
        m_inLayout = false;
        return;
    }

    auto DeferPos = [&](HWND hwnd, int x, int y, int w, int h)
    {
        if (hwnd && hdwp)
        {
            HDWP hdwpNew = ::DeferWindowPos(hdwp, hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            if (hdwpNew)
            {
                hdwp = hdwpNew;
            }
            else
            {
                hdwp = nullptr;
            }
        }
    };

    // Row 1: Original Location
    int y = margin;
    DeferPos(m_hLabelSrc, margin, y + 4, labelWidth, rowHeight);

    int editWidth = width - 2 * margin - labelWidth - spacing - browseWidth - spacing;
    DeferPos(m_hEditSrc, margin + labelWidth + spacing, y, editWidth, rowHeight);
    DeferPos(m_hBtnSrc, width - margin - browseWidth, y, browseWidth, rowHeight);

    // Row 2: New Location
    y += rowHeight + spacing;
    DeferPos(m_hLabelDst, margin, y + 4, labelWidth, rowHeight);
    DeferPos(m_hEditDst, margin + labelWidth + spacing, y, editWidth, rowHeight);
    DeferPos(m_hBtnDst, width - margin - browseWidth, y, browseWidth, rowHeight);

    // Row 3: Connection Method and Action Buttons
    y += rowHeight + spacing;
    DeferPos(m_hLabelStrategy, margin, y + 4, labelWidth, rowHeight);
    DeferPos(m_hComboStrategy, margin + labelWidth + spacing, y, 130, 150);

    // Dynamic sizing/anchoring of action buttons from the right
    int cancelWidth = 80;
    int migrateWidth = 110;
    int scanWidth = 100;
    int refreshWidth = 80;

    int rightX = width - margin;

    rightX -= cancelWidth;
    DeferPos(m_hBtnCancel, rightX, y, cancelWidth, rowHeight);

    rightX -= (spacing + migrateWidth);
    DeferPos(m_hBtnMigrate, rightX, y, migrateWidth, rowHeight);

    rightX -= (spacing + scanWidth);
    DeferPos(m_hBtnScan, rightX, y, scanWidth, rowHeight);

    rightX -= (spacing + refreshWidth);
    DeferPos(m_hBtnRefresh, rightX, y, refreshWidth, rowHeight);

    // Separator 1
    y += rowHeight + spacing + 2;
    DeferPos(m_hSeparator1, margin, y, width - 2 * margin, 2);

    // Row 4: Search & Filtering
    y += spacing + 4;
    DeferPos(m_hLabelSearch, margin, y + 4, labelWidth, rowHeight);
    DeferPos(m_hEditSearch, margin + labelWidth + spacing, y, 200, rowHeight);
    DeferPos(m_hChkShowHidden, margin + labelWidth + spacing + 200 + spacing, y, 160, rowHeight);
    DeferPos(m_hChkShowSystem, margin + labelWidth + spacing + 200 + spacing + 160 + spacing, y, 160, rowHeight);

    // Bottom Area Layout Calculations (Log Height at least 160px or 100px on compact)
    int logHeight = 160;
    if (height < 600)
        logHeight = 100; // Make log smaller on compact windows

    int bottomY = height - margin;

    bottomY -= logHeight;
    DeferPos(m_hListViewLog, margin, bottomY, width - 2 * margin, logHeight);
    if (m_hListViewLog)
    {
        // Resize log columns
        int logWidth = width - 2 * margin;
        ::SendMessageW(m_hListViewLog, LVM_SETCOLUMNWIDTH, 0, logWidth - 130);
        ::SendMessageW(m_hListViewLog, LVM_SETCOLUMNWIDTH, 1, 130);
    }

    bottomY -= (spacing + 2);
    DeferPos(m_hSeparator2, margin, bottomY, width - 2 * margin, 2);

    bottomY -= (spacing + 18);
    DeferPos(m_hLabelSpeed, margin, bottomY, width - 2 * margin, 18);

    bottomY -= (spacing + 16);
    DeferPos(m_hProgressBar, margin, bottomY, width - 2 * margin, 16);

    bottomY -= (spacing + 18);
    DeferPos(m_hLabelStatus, margin, bottomY, width - 2 * margin, 18);

    // Middle Area: Folder ListView takes all remaining space
    int topY = y + rowHeight + spacing;
    int listHeight = bottomY - spacing - topY;
    if (listHeight < 80)
        listHeight = 80;

    DeferPos(m_hListViewFolders, margin, topY, width - 2 * margin, listHeight);
    if (m_hListViewFolders)
    {
        // Dynamic column scaling for 5 columns
        int listWidth = width - 2 * margin;
        int col0 = listWidth * 20 / 100;                      // Folder Name
        int col1 = listWidth * 15 / 100;                      // Status
        int col2 = listWidth * 35 / 100;                      // Current Location
        int col3 = listWidth * 15 / 100;                      // Size
        int col4 = listWidth - col0 - col1 - col2 - col3 - 4; // Method
        if (col4 < 80)
            col4 = 80;

        ::SendMessageW(m_hListViewFolders, LVM_SETCOLUMNWIDTH, 0, col0);
        ::SendMessageW(m_hListViewFolders, LVM_SETCOLUMNWIDTH, 1, col1);
        ::SendMessageW(m_hListViewFolders, LVM_SETCOLUMNWIDTH, 2, col2);
        ::SendMessageW(m_hListViewFolders, LVM_SETCOLUMNWIDTH, 3, col3);
        ::SendMessageW(m_hListViewFolders, LVM_SETCOLUMNWIDTH, 4, col4);
    }

    if (hdwp)
    {
        ::EndDeferWindowPos(hdwp);
    }

    // Re-enable redrawing and trigger high-priority repaint to finish resizing cleanly
    ::SendMessageW(m_hListViewFolders, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(m_hListViewFolders, NULL, TRUE);
    m_inLayout = false;
}

std::wstring AtomicRelocUI::FormatBytes(uint64_t bytes)
{
    double size = static_cast<double>(bytes);
    const wchar_t *units[] = {L"Bytes", L"KB", L"MB", L"GB", L"TB"};
    int unitIndex = 0;
    while (size >= 1024.0 && unitIndex < 4)
    {
        size /= 1024.0;
        unitIndex++;
    }
    wchar_t buf[64];
    if (unitIndex == 0)
    {
        ::StringCchPrintfW(buf, 64, L"%u Bytes", static_cast<uint32_t>(bytes));
    }
    else
    {
        ::StringCchPrintfW(buf, 64, L"%.2f %s", size, units[unitIndex]);
    }
    return buf;
}

void AtomicRelocUI::HandleScanAndLockCheck()
{
    int selectedIndex = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (selectedIndex == -1)
    {
        ::MessageBoxW(m_hwnd, L"Please select a folder from the list first.", L"Warning", MB_ICONWARNING | MB_OK);
        return;
    }

    LVITEMW lvi = {};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = selectedIndex;
    if (!::SendMessageW(m_hListViewFolders, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lvi)))
    {
        return;
    }
    size_t folderIndex = static_cast<size_t>(lvi.lParam);
    if (folderIndex >= m_folders.size())
    {
        return;
    }

    const auto &folder = m_folders[folderIndex];
    std::wstring srcPath = folder.physicalPath;

    AddLogEntry(L"Initiating deep lock scan for: " + srcPath, Logger::Level::Info);

    // 1. Disable the UI controls and show a loading message so the user knows it's working
    SetControlsEnabled(false);
    ::SetWindowTextW(m_hLabelStatus, L"Deep scanning thousands of files for locks... Please wait.");

    HWND hwnd = m_hwnd;

    ::SendMessageW(m_hProgressBar, PBM_SETPOS, 0, 0);

    // 2. Spin up a detached background thread for the heavy kernel queries
    std::thread([srcPath, hwnd]()
                {
        auto* result = new LockScanResult();
        result->srcPath = srcPath;
	result->success = RestartManagerEngine::GetProcessesLockingDirectory(srcPath, result->lockingProcesses, hwnd);
        
        // 3. Post the memory pointer safely back to the Main UI Thread when completely finished
        ::PostMessageW(hwnd, WM_USER_LOCK_SCAN_COMPLETE, 0, reinterpret_cast<LPARAM>(result)); })
        .detach();
}

void AtomicRelocUI::HandleStartMigration()
{
    int selectedIndex = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (selectedIndex == -1)
    {
        ::MessageBoxW(m_hwnd, L"Please select a folder from the list to relocate/restore.", L"Warning", MB_ICONWARNING | MB_OK);
        return;
    }

    LVITEMW lvi = {};
    lvi.mask = LVIF_PARAM;
    lvi.iItem = selectedIndex;
    if (!::SendMessageW(m_hListViewFolders, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lvi)))
    {
        return;
    }
    size_t folderIndex = static_cast<size_t>(lvi.lParam);
    if (folderIndex >= m_folders.size())
    {
        return;
    }

    const auto &folder = m_folders[folderIndex];

    std::wstring srcPath = folder.fullSourcePath;
    std::wstring dstPath;
    RedirectStrategy strat = RedirectStrategy::Junction;
    bool isChainedMove = false;

    wchar_t btnText[64] = {0};
    ::GetWindowTextW(m_hBtnMigrate, btnText, 64);
    std::wstring label = btnText;

    wchar_t dst[MAX_PATH] = {0};
    ::GetWindowTextW(m_hEditDst, dst, MAX_PATH);
    m_newLocation = FileSystemEngine::SanitizePath(dst);

    std::wstring expectedDest = m_newLocation.empty() ? L"" : FileSystemEngine::JoinPath(m_newLocation, folder.name);

    if (label.rfind(L"Restore", 0) == 0)
    {
        dstPath = folder.fullSourcePath;
        strat = RedirectStrategy::Restore;
        isChainedMove = false;
    }
    else if (label.rfind(L"Chained Move", 0) == 0)
    {
        dstPath = expectedDest;
        int sel = static_cast<int>(::SendMessageW(m_hComboStrategy, CB_GETCURSEL, 0, 0));
        strat = (sel == 1) ? RedirectStrategy::SymbolicLink : RedirectStrategy::Junction;
        isChainedMove = true;
    }
    else
    {
        dstPath = expectedDest;
        int sel = static_cast<int>(::SendMessageW(m_hComboStrategy, CB_GETCURSEL, 0, 0));
        strat = (sel == 1) ? RedirectStrategy::SymbolicLink : RedirectStrategy::Junction;
        isChainedMove = false;
    }

    // Safety check - resolve absolute physical paths
    std::wstring finalSource = FileSystemEngine::GetFullyResolvedPhysicalPath(folder.physicalPath);
    std::wstring finalDest;

    if (strat == RedirectStrategy::Restore)
    {
        size_t lastSlash = dstPath.find_last_of(L'\\');
        if (lastSlash != std::wstring::npos && lastSlash > 0)
        {
            std::wstring parent = dstPath.substr(0, lastSlash);
            std::wstring child = dstPath.substr(lastSlash + 1);
            std::wstring resolvedParent = FileSystemEngine::GetFullyResolvedPhysicalPath(parent);
            finalDest = FileSystemEngine::NormalizePath(FileSystemEngine::JoinPath(resolvedParent, child));
        }
        else
        {
            finalDest = FileSystemEngine::GetFullyResolvedPhysicalPath(dstPath);
        }
    }
    else
    {
        finalDest = FileSystemEngine::GetFullyResolvedPhysicalPath(dstPath);
    }

    // Strip trailing slashes for comparison
    while (finalSource.length() > 3 && finalSource.back() == L'\\')
    {
        finalSource.pop_back();
    }
    while (finalDest.length() > 3 && finalDest.back() == L'\\')
    {
        finalDest.pop_back();
    }

    if (_wcsicmp(finalSource.c_str(), finalDest.c_str()) == 0)
    {
        TASKDIALOGCONFIG tdConfig = {};
        tdConfig.cbSize = sizeof(tdConfig);
        tdConfig.hwndParent = m_hwnd;
        tdConfig.pszWindowTitle = L"AtomicReloc Safety Guard";
        tdConfig.pszMainInstruction = L"Source and Destination are the same location. No move required.";
        tdConfig.pszContent = L"The resolved physical source and destination paths are identical. Move aborted to prevent data loss.";
        tdConfig.pszMainIcon = TD_WARNING_ICON;
        tdConfig.dwCommonButtons = TDCBF_OK_BUTTON;

        ::TaskDialogIndirect(&tdConfig, NULL, NULL, NULL);
        return;
    }

    if (strat != RedirectStrategy::Restore)
    {
        if (!FileSystemEngine::DirectoryExists(srcPath) && !isChainedMove)
        {
            ::MessageBoxW(m_hwnd, L"Source folder does not exist.", L"Error", MB_ICONERROR | MB_OK);
            return;
        }

        if (FileSystemEngine::DirectoryExists(dstPath) && !FileSystemEngine::IsSameVolume(srcPath, dstPath))
        {
            AddLogEntry(L"Target path already exists. Pre-allocation will proceed.", Logger::Level::Warning);
        }
    }

    std::wstring stratStr;
    if (strat == RedirectStrategy::Restore)
    {
        stratStr = L"Restore to Original Location";
    }
    else if (strat == RedirectStrategy::SymbolicLink)
    {
        stratStr = L"Symbolic Link";
    }
    else
    {
        stratStr = L"Directory Junction";
    }

    std::wstring content;
    if (strat == RedirectStrategy::Restore)
    {
        content = L"You are about to restore:\n" + srcPath + L"\n\nFrom Alternative physical location:\n" + folder.physicalPath + L"\n\nStrategy: " + stratStr;
    }
    else if (isChainedMove)
    {
        content = L"You are about to relocate already-junctioned folder:\n" + srcPath + L"\n\nDirectly from current target:\n" + folder.physicalPath + L"\n\nTo new destination:\n" + dstPath + L"\n\nStrategy: " + stratStr;
    }
    else
    {
        content = L"You are about to move:\n" + srcPath + L"\n\nTo Destination:\n" + dstPath + L"\n\nStrategy: " + stratStr;
    }

    TASKDIALOGCONFIG tdConfig = {};
    tdConfig.cbSize = sizeof(tdConfig);
    tdConfig.hwndParent = m_hwnd;
    tdConfig.dwFlags = TDF_USE_COMMAND_LINKS;
    tdConfig.pszWindowTitle = (strat == RedirectStrategy::Restore) ? L"Confirm Restore" : L"Confirm Migration";
    tdConfig.pszMainInstruction = (strat == RedirectStrategy::Restore) ? L"Begin Restore Pipeline?" : L"Begin Application Migration?";
    tdConfig.pszContent = content.c_str();
    tdConfig.pszMainIcon = TD_INFORMATION_ICON;

    TASKDIALOG_BUTTON buttons[2] = {};
    buttons[0].nButtonID = IDOK;
    if (strat == RedirectStrategy::Restore)
    {
        buttons[0].pszButtonText = L"Start restore pipeline\nCopies files back, verifies integrity, and removes redirection link.";
    }
    else
    {
        buttons[0].pszButtonText = L"Start migration pipeline\nPre-allocates, copies, verifies on-the-fly and atomically swaps directory.";
    }
    buttons[1].nButtonID = IDCANCEL;
    buttons[1].pszButtonText = L"Cancel and return\nDoes not alter any files.";

    tdConfig.cButtons = 2;
    tdConfig.pButtons = buttons;

    int selectedButton = 0;
    ::TaskDialogIndirect(&tdConfig, &selectedButton, NULL, NULL);

    if (selectedButton != IDOK)
    {
        return;
    }

    m_isMigrating = true;
    SetControlsEnabled(false);

    ::SendMessageW(m_hProgressBar, PBM_SETPOS, 0, 0);
    ::SetWindowTextW(m_hLabelStatus, L"Scanning volume topologies...");
    ::SetWindowTextW(m_hLabelSpeed, L"Analyzing disk write latency...");

    m_coordinator = std::make_unique<TaskCoordinator>(srcPath, dstPath, strat, m_hwnd, m_appDataPath);

    AddLogEntry(L"Launching thread Swarm " + std::wstring(strat == RedirectStrategy::Restore ? L"restore" : L"migration") + L" pipeline.", Logger::Level::Info);
    if (!m_coordinator->StartMigration())
    {
        AddLogEntry(L"Failed starting Swarm pipeline.", Logger::Level::Error);
        m_isMigrating = false;
        SetControlsEnabled(true);
    }
}

void AtomicRelocUI::HandleCancelMigration()
{
    if (m_coordinator && m_isMigrating)
    {
        AddLogEntry(L"Cancellation requested by user. Terminating threads...", Logger::Level::Warning);
        m_coordinator->CancelMigration();
        ::EnableWindow(m_hBtnCancel, FALSE);
    }
}

LRESULT AtomicRelocUI::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
    {
        CreateChildControls();
        return 0;
    }

    case WM_SIZE:
    {
        UpdateLayout();
        return 0;
    }

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);
        if (wmId == ID_EDIT_SRC && wmEvent == EN_KILLFOCUS)
        {
            ScanRootLibrary();
        }
        else if (wmId == ID_EDIT_DST && wmEvent == EN_CHANGE)
        {
            UpdateButtonState();
        }
        else if (wmId == ID_EDIT_SEARCH && wmEvent == EN_CHANGE)
        {
            ApplySearchFilter();
        }
        else if (wmId == ID_CHK_HIDDEN)
        {
            LRESULT checked = ::SendMessageW(m_hChkShowHidden, BM_GETCHECK, 0, 0);
            m_showHidden = (checked == BST_CHECKED);
            ScanRootLibrary();
        }
        else if (wmId == ID_CHK_SYSTEM)
        {
            LRESULT checked = ::SendMessageW(m_hChkShowSystem, BM_GETCHECK, 0, 0);
            m_showSystem = (checked == BST_CHECKED);
            ScanRootLibrary();
        }
        else
        {
            switch (wmId)
            {
            case ID_BTN_SRC:
            {
                BrowseFolder(m_hEditSrc);
                break;
            }
            case ID_BTN_DST:
            {
                BrowseFolder(m_hEditDst);
                break;
            }
            case ID_BTN_REFRESH:
            {
                ScanRootLibrary();
                break;
            }
            case ID_BTN_SCAN:
            {
                HandleScanAndLockCheck();
                break;
            }
            case ID_BTN_MIGRATE:
            {
                HandleStartMigration();
                break;
            }
            case ID_BTN_CANCEL:
            {
                HandleCancelMigration();
                break;
            }
            default:
                break;
            }
        }
        return 0;
    }
    case WM_USER_LOCK_SCAN_PROGRESS:
    {
        // wParam contains the 0-100 percentage sent from RestartManager.cpp
        ::SendMessageW(m_hProgressBar, PBM_SETPOS, wParam, 0);
        return 0;
    }
    case WM_USER_LOCK_SCAN_COMPLETE:
    {
        LockScanResult *result = reinterpret_cast<LockScanResult *>(lParam);
        if (!result)
            return 0;

        // Re-enable the UI
        SetControlsEnabled(true);
        ::SetWindowTextW(m_hLabelStatus, L"Lock scan completed.");

        // Process the Task Dialogs back on the safe UI thread
        if (result->success)
        {
            if (result->lockingProcesses.empty())
            {
                TASKDIALOGCONFIG tdConfig = {};
                tdConfig.cbSize = sizeof(tdConfig);
                tdConfig.hwndParent = m_hwnd;
                tdConfig.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
                tdConfig.pszWindowTitle = L"AtomicReloc Lock Detector";
                tdConfig.pszMainInstruction = L"No Active File Locks Discovered";
                tdConfig.pszContent = L"All files are fully unlocked. The directory is safe to relocate.";
                tdConfig.pszMainIcon = TD_INFORMATION_ICON;
                tdConfig.dwCommonButtons = TDCBF_OK_BUTTON;

                ::TaskDialogIndirect(&tdConfig, NULL, NULL, NULL);
                AddLogEntry(L"No locking processes detected. Scope is ready to move.", Logger::Level::Success);
            }
            else
            {
                std::wstringstream wss;
                wss << L"The following processes are locking files inside the source scope:\n\n";

                bool hasCriticalProcess = false;

                for (const auto &proc : result->lockingProcesses)
                {
                    wss << L"  - " << proc.strAppName << L" (PID: " << proc.dwProcessId << L")";
                    if (!proc.strServiceShortName.empty())
                    {
                        wss << L" [Service: " << proc.strServiceShortName << L"]";
                    }

                    // Identify if this is a system-critical process
                    if (proc.appType == RmCritical || proc.appType == RmExplorer || _wcsicmp(proc.strAppName.c_str(), L"explorer.exe") == 0)
                    {
                        wss << L" (CRITICAL SYSTEM PROCESS)";
                        hasCriticalProcess = true;
                    }

                    wss << L"\n";
                }
                wss << L"\nWould you like AtomicReloc to terminate these applications gracefully?";

                // FIX 1: Store the string in a persistent local variable so .c_str() does not dangle!
                std::wstring dialogContent = wss.str();

                TASKDIALOGCONFIG tdConfig = {};
                tdConfig.cbSize = sizeof(tdConfig);
                tdConfig.hwndParent = m_hwnd;
                tdConfig.dwFlags = TDF_USE_COMMAND_LINKS;
                tdConfig.pszWindowTitle = L"AtomicReloc Lock Detector";
                tdConfig.pszMainInstruction = L"Active File Locks Detected!";
                tdConfig.pszContent = dialogContent.c_str(); // Safe pointer usage
                tdConfig.pszMainIcon = TD_WARNING_ICON;

                TASKDIALOG_BUTTON buttons[2] = {};
                buttons[0].nButtonID = 100;
                buttons[0].pszButtonText = L"Gracefully terminate conflicting processes\nCloses processes using standard system termination.";
                buttons[1].nButtonID = 101;
                buttons[1].pszButtonText = L"Cancel and close\nLeaves processes active.";

                tdConfig.cButtons = 2;
                tdConfig.pButtons = buttons;

                int selectedButton = 0;
                ::TaskDialogIndirect(&tdConfig, &selectedButton, NULL, NULL);

                if (selectedButton == 100)
                {
                    AddLogEntry(L"Attempting process terminations...", Logger::Level::Warning);
                    int terminated = 0;
                    int skipped = 0;

                    for (const auto &proc : result->lockingProcesses)
                    {
                        // FIX 2: SAFETY GUARD - Never terminate the Windows Shell or Critical System Processes
                        if (proc.appType == RmCritical || proc.appType == RmExplorer ||
                            _wcsicmp(proc.strAppName.c_str(), L"explorer.exe") == 0 ||
                            _wcsicmp(proc.strAppName.c_str(), L"svchost.exe") == 0)
                        {
                            skipped++;
                            continue;
                        }

                        HANDLE hProcess = ::OpenProcess(PROCESS_TERMINATE, FALSE, proc.dwProcessId);
                        if (hProcess != NULL)
                        {
                            if (::TerminateProcess(hProcess, 1))
                            {
                                terminated++;
                            }
                            ::CloseHandle(hProcess);
                        }
                    }

                    std::wstring logMsg = L"Successfully terminated " + std::to_wstring(terminated) + L" locking processes.";
                    if (skipped > 0)
                    {
                        logMsg += L" (Skipped " + std::to_wstring(skipped) + L" critical system processes).";
                    }
                    AddLogEntry(logMsg, Logger::Level::Success);
                }
            }
        }
        else
        {
            AddLogEntry(L"Lock Check Failed entirely. Check trace logs.", Logger::Level::Error);
        }

        // Prevent memory leaks
        delete result;
        return 0;
    }
    case WM_USER_SIZE_CALCULATED:
    {
        FolderSizeData *data = reinterpret_cast<FolderSizeData *>(lParam);
        if (data == nullptr)
        {
            return 0;
        }

        for (size_t i = 0; i < m_folders.size(); ++i)
        {
            if (_wcsicmp(m_folders[i].physicalPath.c_str(), data->folderPath) == 0)
            {
                m_folders[i].size = data->totalSize;
                m_folders[i].sizeCalculated = true;

                std::wstring sizeStr = FormatBytes(data->totalSize);

                // Find the ListView item matching lParam == i
                int itemCount = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_GETITEMCOUNT, 0, 0));
                for (int lvIdx = 0; lvIdx < itemCount; ++lvIdx)
                {
                    LVITEMW lviQuery = {};
                    lviQuery.mask = LVIF_PARAM;
                    lviQuery.iItem = lvIdx;
                    if (::SendMessageW(m_hListViewFolders, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lviQuery)))
                    {
                        if (static_cast<size_t>(lviQuery.lParam) == i)
                        {
                            LVITEMW lvi = {};
                            lvi.mask = LVIF_TEXT;
                            lvi.iItem = lvIdx;
                            lvi.iSubItem = 3;
                            lvi.pszText = const_cast<LPWSTR>(sizeStr.c_str());
                            ::SendMessageW(m_hListViewFolders, LVM_SETITEMTEXTW, lvIdx, reinterpret_cast<LPARAM>(&lvi));
                            break;
                        }
                    }
                }
                break;
            }
        }

        delete data;
        return 0;
    }

    case WM_USER_MIGRATION_PROGRESS:
    {
        MigrationProgressData *data = reinterpret_cast<MigrationProgressData *>(lParam);
        if (data == nullptr)
        {
            return 0;
        }

        if (data->isComplete)
        {
            ::SendMessageW(m_hProgressBar, PBM_SETPOS, 100, 0);
            ::SetWindowTextW(m_hLabelStatus, L"Operation finished successfully!");
            ::SetWindowTextW(m_hLabelSpeed, (L"Total bytes processed: " + std::to_wstring(data->totalBytes)).c_str());

            AddLogEntry(L"Operation successfully completed and verified.", Logger::Level::Success);
            AddLogEntry(L"Transaction committed.", Logger::Level::Success);

            TASKDIALOGCONFIG tdConfig = {};
            tdConfig.cbSize = sizeof(tdConfig);
            tdConfig.hwndParent = m_hwnd;
            tdConfig.pszWindowTitle = L"Operation Complete";
            tdConfig.pszMainInstruction = L"Success!";
            tdConfig.pszContent = L"The folder operation completed successfully and transaction records were committed.";
            tdConfig.pszMainIcon = TD_SHIELD_ICON;
            tdConfig.dwCommonButtons = TDCBF_OK_BUTTON;
            ::TaskDialogIndirect(&tdConfig, NULL, NULL, NULL);

            m_isMigrating = false;
            SetControlsEnabled(true);
            m_coordinator.reset();

            // Fresh scan to update visual state, sizes, and arrows
            ScanRootLibrary();
        }
        else if (data->isFailed)
        {
            ::SendMessageW(m_hProgressBar, PBM_SETPOS, 0, 0);
            ::SetWindowTextW(m_hLabelStatus, L"Operation failed!");
            ::SetWindowTextW(m_hLabelSpeed, data->errorMessage);

            AddLogEntry(data->errorMessage, Logger::Level::Error);
            AddLogEntry(L"Safety rollback completed. System state restored.", Logger::Level::Warning);

            TASKDIALOGCONFIG tdConfig = {};
            tdConfig.cbSize = sizeof(tdConfig);
            tdConfig.hwndParent = m_hwnd;
            tdConfig.pszWindowTitle = L"Pipeline Failure";
            tdConfig.pszMainInstruction = L"Transaction Aborted!";
            tdConfig.pszContent = data->errorMessage;
            tdConfig.pszMainIcon = TD_ERROR_ICON;
            tdConfig.dwCommonButtons = TDCBF_OK_BUTTON;
            ::TaskDialogIndirect(&tdConfig, NULL, NULL, NULL);

            m_isMigrating = false;
            SetControlsEnabled(true);
            m_coordinator.reset();

            // Fresh scan to restore visible states
            ScanRootLibrary();
        }
        else
        {
            int percent = 0;
            if (data->totalBytes > 0)
            {
                percent = static_cast<int>((data->bytesProcessed * 100) / data->totalBytes);
            }

            ::SendMessageW(m_hProgressBar, PBM_SETPOS, percent, 0);

            std::wstring statusText = L"Relocating file: " + std::wstring(data->currentFileName);
            ::SetWindowTextW(m_hLabelStatus, statusText.c_str());

            wchar_t speedBuf[256];
            double remainingGB = static_cast<double>(data->totalBytes - data->bytesProcessed) / (1024.0 * 1024.0 * 1024.0);

            ::StringCchPrintfW(speedBuf, 256, L"%.1f MB/s - %.2f GB remaining (File %u/%u)",
                               data->currentSpeedMBs, remainingGB, data->filesProcessed, data->totalFiles);
            ::SetWindowTextW(m_hLabelSpeed, speedBuf);

            if (data->filesProcessed > 0 && percent % 10 == 0)
            {
                AddLogEntry(L"Transferred file " + std::to_wstring(data->filesProcessed) + L"/" + std::to_wstring(data->totalFiles) + L" (" + std::to_wstring(percent) + L"%)", Logger::Level::Info);
            }
        }

        delete data;
        return 0;
    }

    case WM_NOTIFY:
    {
        LPNMHDR pnmh = reinterpret_cast<LPNMHDR>(lParam);
        if (pnmh->hwndFrom == m_hListViewFolders)
        {
            if (pnmh->code == NM_CUSTOMDRAW)
            {
                LPNMLVCUSTOMDRAW pCustomDraw = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
                if (pCustomDraw->nmcd.dwDrawStage == CDDS_PREPAINT)
                {
                    return CDRF_NOTIFYITEMDRAW;
                }
                else if (pCustomDraw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    return CDRF_NOTIFYSUBITEMDRAW;
                }
                else if (pCustomDraw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
                {
                    bool isSelected = (::SendMessageW(m_hListViewFolders, LVM_GETITEMSTATE, pCustomDraw->nmcd.dwItemSpec, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                    if (!isSelected)
                    {
                        LVITEMW lvi = {};
                        lvi.mask = LVIF_PARAM;
                        lvi.iItem = static_cast<int>(pCustomDraw->nmcd.dwItemSpec);
                        if (::SendMessageW(m_hListViewFolders, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lvi)))
                        {
                            size_t folderIndex = static_cast<size_t>(lvi.lParam);
                            if (folderIndex < m_folders.size() && m_folders[folderIndex].isJunction)
                            {
                                pCustomDraw->clrTextBk = RGB(220, 250, 220);
                                pCustomDraw->clrText = RGB(0, 80, 0);
                                return CDRF_NEWFONT;
                            }
                        }
                    }
                    return CDRF_DODEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            else if (pnmh->code == LVN_ITEMCHANGED)
            {
                UpdateButtonState();
            }
            else if (pnmh->code == LVN_COLUMNCLICK)
            {
                LPNMLISTVIEW pnmlv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                SortFolders(pnmlv->iSubItem);
            }
            else if (pnmh->code == NM_RCLICK)
            {
                int selectedIndex = static_cast<int>(::SendMessageW(m_hListViewFolders, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
                if (selectedIndex != -1)
                {
                    LVITEMW lvi = {};
                    lvi.mask = LVIF_PARAM;
                    lvi.iItem = selectedIndex;
                    if (::SendMessageW(m_hListViewFolders, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lvi)))
                    {
                        size_t folderIndex = static_cast<size_t>(lvi.lParam);
                        if (folderIndex < m_folders.size())
                        {
                            const auto &folder = m_folders[folderIndex];
                            HMENU hMenu = ::CreatePopupMenu();
                            if (hMenu)
                            {
                                ::AppendMenuW(hMenu, MF_STRING, 2003, L"Open Original Location in Explorer");
                                if (folder.isJunction)
                                {
                                    ::AppendMenuW(hMenu, MF_STRING, 2004, L"Open Physical Location in Explorer");
                                    ::AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                                    ::AppendMenuW(hMenu, MF_STRING, 2002, L"Force Restore to Original \u2190");
                                }

                                POINT pt;
                                ::GetCursorPos(&pt);

                                int selection = ::TrackPopupMenu(
                                    hMenu,
                                    TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                    pt.x, pt.y,
                                    0,
                                    m_hwnd,
                                    NULL);

                                ::DestroyMenu(hMenu);

                                if (selection == 2003)
                                {
                                    ::ShellExecuteW(NULL, L"open", folder.fullSourcePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                }
                                else if (selection == 2004)
                                {
                                    ::ShellExecuteW(NULL, L"open", folder.physicalPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                }
                                else if (selection == 2002)
                                {
                                    ::SetWindowTextW(m_hBtnMigrate, L"Restore \u2190");
                                    ::InvalidateRect(m_hBtnMigrate, NULL, TRUE);
                                    ::UpdateWindow(m_hBtnMigrate);
                                    HandleStartMigration();
                                }
                            }
                        }
                    }
                }
            }
        }
        else if (pnmh->hwndFrom == m_hListViewLog)
        {
            if (pnmh->code == LVN_KEYDOWN)
            {
                LPNMLVKEYDOWN pnkd = reinterpret_cast<LPNMLVKEYDOWN>(lParam);
                if (pnkd->wVKey == 'C' && (::GetKeyState(VK_CONTROL) < 0))
                {
                    CopySelectedLogsToClipboard();
                    return TRUE;
                }
            }
            else if (pnmh->code == NM_RCLICK)
            {
                HMENU hMenu = ::CreatePopupMenu();
                if (hMenu)
                {
                    ::AppendMenuW(hMenu, MF_STRING, 2001, L"Copy Selected Logs");

                    POINT pt;
                    ::GetCursorPos(&pt);

                    int selection = ::TrackPopupMenu(
                        hMenu,
                        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                        pt.x, pt.y,
                        0,
                        m_hwnd,
                        NULL);

                    ::DestroyMenu(hMenu);

                    if (selection == 2001)
                    {
                        CopySelectedLogsToClipboard();
                    }
                }
                return TRUE;
            }
        }
        return 0;
    }

    case WM_DESTROY:
    {
        SaveSettings();
        ::PostQuitMessage(0);
        return 0;
    }

    default:
        return ::DefWindowProcW(m_hwnd, uMsg, wParam, lParam);
    }
}

LRESULT CALLBACK AtomicRelocUI::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_NCCREATE)
    {
        LPCREATESTRUCTW pcs = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pcs->lpCreateParams));
    }

    AtomicRelocUI *pUI = g_pAppUI;
    if (pUI)
    {
        pUI->m_hwnd = hwnd;
        return pUI->HandleMessage(uMsg, wParam, lParam);
    }

    return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int CALLBACK AtomicRelocUI::CompareFolders(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
    AtomicRelocUI *pUI = reinterpret_cast<AtomicRelocUI *>(lParamSort);
    if (!pUI)
        return 0;

    size_t idx1 = static_cast<size_t>(lParam1);
    size_t idx2 = static_cast<size_t>(lParam2);

    if (idx1 >= pUI->m_folders.size() || idx2 >= pUI->m_folders.size())
    {
        return 0;
    }

    const auto &a = pUI->m_folders[idx1];
    const auto &b = pUI->m_folders[idx2];

    int cmp = 0;
    int column = pUI->m_sortColumn;

    if (column == 0)
    { // Name
        cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
    }
    else if (column == 1)
    { // Status (Junction vs Source)
        int aJunc = a.isJunction ? 1 : 0;
        int bJunc = b.isJunction ? 1 : 0;
        cmp = aJunc - bJunc;
        if (cmp == 0)
        {
            cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
        }
    }
    else if (column == 2)
    { // Current Location
        cmp = _wcsicmp(a.physicalPath.c_str(), b.physicalPath.c_str());
    }
    else if (column == 3)
    { // Size
        if (a.size < b.size)
            cmp = -1;
        else if (a.size > b.size)
            cmp = 1;
        else
            cmp = 0;
        if (cmp == 0)
        {
            cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
        }
    }
    else if (column == 4)
    { // Method
        std::wstring methodA = a.isJunction ? (a.isSymlink ? L"Symlink" : L"Junction") : L"";
        std::wstring methodB = b.isJunction ? (b.isSymlink ? L"Symlink" : L"Junction") : L"";
        cmp = _wcsicmp(methodA.c_str(), methodB.c_str());
        if (cmp == 0)
        {
            cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
        }
    }

    return pUI->m_sortAscending ? cmp : -cmp;
}
