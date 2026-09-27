#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "LockScanner.h"
#include "ModernTheme.h"
#include "WindowsIntegration.h"
#include "resource.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"GeniaUnlockerWindow";
constexpr wchar_t kWindowTitle[] = L"Genia Unlocker";
constexpr wchar_t kSettingsWindowClass[] = L"GeniaUnlockerSettingsWindow";
constexpr wchar_t kMutexName[] = L"Local\\GeniaUnlocker.Singleton.1";
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 10;
constexpr UINT WM_APP_TRAY = WM_APP + 11;

constexpr int IDC_TARGET = 1001;
constexpr int IDC_FILE = 1002;
constexpr int IDC_FOLDER = 1003;
constexpr int IDC_LIST = 1004;
constexpr int IDC_UNLOCK = 1005;
constexpr int IDC_TERMINATE = 1006;
constexpr int IDC_RETRY = 1007;
constexpr int IDC_STATUS = 1010;
constexpr int IDC_APP_TITLE = 1012;
constexpr int IDC_TARGET_LABEL = 1014;
constexpr int IDC_DELETE = 1015;
constexpr int IDC_UNLOCK_DELETE = 1016;
constexpr int IDC_SETTINGS = 1017;
constexpr int IDC_DETAILS = 1018;
constexpr int IDC_FORCE_UNLOCK = 1019;

constexpr int IDC_SETTINGS_TITLE = 2001;
constexpr int IDC_SETTINGS_SHELL = 2002;
constexpr int IDC_SETTINGS_AUTOSTART = 2003;
constexpr int IDC_SETTINGS_PERMANENT = 2004;
constexpr int IDC_SETTINGS_CLOSE = 2005;

constexpr UINT ID_TRAY_OPEN = 5001;
constexpr UINT ID_TRAY_FILE = 5002;
constexpr UINT ID_TRAY_EXIT = 5003;
constexpr UINT ID_PROCESS_FORCE_UNLOCK = 5101;
constexpr UINT ID_PROCESS_TERMINATE = 5102;
constexpr UINT ID_PROCESS_OPEN_EXE = 5103;
constexpr UINT ID_PROCESS_COPY_OBJECT = 5104;
constexpr UINT ID_PROCESS_COPY_PID = 5105;

// CreateWindow* uses the HMENU parameter as a child-control ID.
// On x64 HMENU is pointer-sized, so convert through INT_PTR explicitly.
HMENU ControlId(int id) noexcept {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

void SetButtonCheck(HWND button, UINT state) noexcept {
    SendMessageW(button, BM_SETCHECK, static_cast<WPARAM>(state), 0);
}

UINT GetButtonCheck(HWND button) noexcept {
    return static_cast<UINT>(SendMessageW(button, BM_GETCHECK, 0, 0));
}

struct ScanPayload {
    unsigned long long generation{};
    ScanResult result;
};

enum class PendingAction {
    None,
    VerifyUnlock,
    VerifyForceUnlock,
    UnlockDeleteAfterUnlock,
    UnlockDeleteAfterForceUnlock,
    UnlockDeleteAfterTerminate
};

struct AppState {
    HWND hwnd{};
    HWND targetEdit{};
    HWND list{};
    HWND status{};
    HWND settingsWindow{};
    HFONT font{};
    HFONT titleFont{};
    HBRUSH windowBrush{};
    HBRUSH surfaceBrush{};
    ModernTheme::Palette palette{};
    int dpi{96};
    HICON icon{};
    HIMAGELIST processImageList{};
    NOTIFYICONDATAW tray{};
    UINT taskbarCreated{};
    std::wstring target;
    std::vector<LockProcess> locks;
    std::atomic<unsigned long long> scanGeneration{0};
    DWORD lastInaccessibleProcessCount{};
    bool lastTargetExists{};
    bool lastTargetIsDirectory{};
    bool scanInProgress{};
    PendingAction pendingAction{PendingAction::None};
    std::wstring pendingUnlockError;
    std::wstring pendingForceUnlockDetails;
    std::wstring lastStatusDetails;
    bool permanentDeleteDefault{};
    bool exiting{};
};

struct ParsedArgs {
    bool tray{};
    bool installShell{};
    bool uninstallShell{};
    bool enableAutostart{};
    bool disableAutostart{};
    bool showHelp{};
    bool elevatedInstance{};
    DWORD elevatedKillPid{};
    std::vector<DWORD> elevatedKillPids;
    std::wstring elevatedForceUnlockTarget;
    std::wstring target;
};

std::wstring ErrorMessage(DWORD error) {
    LPWSTR buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result;
    if (len && buffer) {
        result.assign(buffer, len);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
        LocalFree(buffer);
    } else {
        result = L"Error " + std::to_wstring(error);
    }
    return result;
}

std::wstring GetPortableSettingsPath() {
    std::wstring exe = GetExecutablePath();
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"GeniaUnlocker.ini";
    }
    return exe.substr(0, slash + 1) + L"GeniaUnlocker.ini";
}

bool LoadPermanentDeleteDefault() {
    const std::wstring ini = GetPortableSettingsPath();
    return GetPrivateProfileIntW(L"Actions", L"PermanentDelete", 0, ini.c_str()) != 0;
}

bool SavePermanentDeleteDefault(bool enabled) {
    const std::wstring ini = GetPortableSettingsPath();
    return WritePrivateProfileStringW(L"Actions", L"PermanentDelete",
                                      enabled ? L"1" : L"0", ini.c_str()) != FALSE;
}

ParsedArgs ParseArguments() {
    ParsedArgs parsed;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return parsed;
    }

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (_wcsicmp(arg.c_str(), L"--tray") == 0) {
            parsed.tray = true;
        } else if (_wcsicmp(arg.c_str(), L"--target") == 0 && i + 1 < argc) {
            parsed.target = argv[++i];
        } else if (_wcsicmp(arg.c_str(), L"--install-shell") == 0) {
            parsed.installShell = true;
        } else if (_wcsicmp(arg.c_str(), L"--uninstall-shell") == 0) {
            parsed.uninstallShell = true;
        } else if (_wcsicmp(arg.c_str(), L"--enable-autostart") == 0) {
            parsed.enableAutostart = true;
        } else if (_wcsicmp(arg.c_str(), L"--disable-autostart") == 0) {
            parsed.disableAutostart = true;
        } else if (_wcsicmp(arg.c_str(), L"--elevated-instance") == 0) {
            parsed.elevatedInstance = true;
        } else if (_wcsicmp(arg.c_str(), L"--elevated-kill") == 0 && i + 1 < argc) {
            parsed.elevatedKillPid = wcstoul(argv[++i], nullptr, 10);
        } else if (_wcsicmp(arg.c_str(), L"--elevated-kill-list") == 0 && i + 1 < argc) {
            std::wstring list = argv[++i];
            size_t start = 0;
            while (start < list.size()) {
                size_t comma = list.find(L',', start);
                std::wstring token = list.substr(
                    start,
                    comma == std::wstring::npos ? std::wstring::npos : comma - start);
                DWORD pid = wcstoul(token.c_str(), nullptr, 10);
                if (pid != 0) {
                    parsed.elevatedKillPids.push_back(pid);
                }
                if (comma == std::wstring::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (_wcsicmp(arg.c_str(), L"--elevated-force-unlock") == 0 && i + 1 < argc) {
            parsed.elevatedForceUnlockTarget = argv[++i];
        } else if (_wcsicmp(arg.c_str(), L"--help") == 0 || _wcsicmp(arg.c_str(), L"-h") == 0) {
            parsed.showHelp = true;
        } else if (!arg.empty() && arg[0] != L'-' && parsed.target.empty()) {
            // Makes drag-and-drop onto GeniaUnlocker.exe useful too.
            parsed.target = arg;
        }
    }
    LocalFree(argv);
    return parsed;
}

void SetFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

// A classic Win32 EDIT control does not vertically center its text when the
// control is made taller than the font. Genia Unlocker uses 38 logical-pixel
// input fields, so without a formatting rectangle the caret hugs the top edge.
// We use a one-line multiline EDIT and explicitly center its formatting area.
void CenterEditContent(HWND edit, int dpi) {
    if (!edit) return;

    RECT client{};
    if (!GetClientRect(edit, &client)) return;

    HDC dc = GetDC(edit);
    if (!dc) return;

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) {
        oldFont = SelectObject(dc, font);
    }

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);

    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(edit, dc);

    const int clientHeight = client.bottom - client.top;
    const int lineHeight = (std::max)(1, static_cast<int>(tm.tmHeight));
    const int verticalPadding = (std::max)(0, (clientHeight - lineHeight) / 2);
    const int horizontalPadding = MulDiv(10, dpi > 0 ? dpi : 96, 96);

    RECT format{};
    format.left = horizontalPadding;
    format.top = verticalPadding;
    format.right = (std::max)(format.left + 1, client.right - horizontalPadding);
    format.bottom = (std::min)(client.bottom, format.top + lineHeight + 1);

    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
    InvalidateRect(edit, nullptr, TRUE);
}

void SetStatusWithDetails(AppState* state,
                          const std::wstring& visibleText,
                          const std::wstring& detailsText) {
    if (!state) return;
    state->lastStatusDetails = detailsText.empty() ? visibleText : detailsText;
    if (state->status) {
        SetWindowTextW(state->status, visibleText.c_str());
    }
    HWND details = state->hwnd ? GetDlgItem(state->hwnd, IDC_DETAILS) : nullptr;
    if (details) {
        EnableWindow(details, !state->lastStatusDetails.empty());
    }
}

void SetStatus(AppState* state, const std::wstring& text) {
    SetStatusWithDetails(state, text, text);
}

int Scale(const AppState* state, int logicalPixels) {
    return MulDiv(logicalPixels, state && state->dpi > 0 ? state->dpi : 96, 96);
}

HFONT CreateModernFont(int dpi, int pointSize, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pointSize, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void RecreateFonts(AppState* state) {
    if (!state) return;
    if (state->font) DeleteObject(state->font);
    if (state->titleFont) DeleteObject(state->titleFont);

    state->font = CreateModernFont(state->dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
    state->titleFont = CreateModernFont(state->dpi, 15, FW_SEMIBOLD, L"Segoe UI Variable Display");
}

void ApplyFonts(AppState* state) {
    if (!state || !state->hwnd) return;
    for (int id : {IDC_TARGET, IDC_FILE, IDC_FOLDER, IDC_LIST, IDC_UNLOCK, IDC_FORCE_UNLOCK,
                   IDC_TERMINATE, IDC_DELETE, IDC_UNLOCK_DELETE, IDC_RETRY, IDC_STATUS,
                   IDC_TARGET_LABEL, IDC_SETTINGS, IDC_DETAILS}) {
        SetFont(GetDlgItem(state->hwnd, id), state->font);
    }
    SetFont(GetDlgItem(state->hwnd, IDC_APP_TITLE), state->titleFont);
}

void UpdateListColumns(AppState* state, int width) {
    if (!state || !state->list || width <= 0) return;
    const int usable = width - Scale(state, 6);
    const int processW = (std::max)(Scale(state, 138), usable * 24 / 100);
    const int pidW = (std::max)(Scale(state, 52), usable * 8 / 100);
    const int detectedW = (std::max)(Scale(state, 92), usable * 14 / 100);
    const int executableW = (std::max)(Scale(state, 250), usable - processW - pidW - detectedW);
    ListView_SetColumnWidth(state->list, 0, processW);
    ListView_SetColumnWidth(state->list, 1, pidW);
    ListView_SetColumnWidth(state->list, 2, detectedW);
    ListView_SetColumnWidth(state->list, 3, executableW);
}

void UpdateTheme(AppState* state) {
    if (!state || !state->hwnd) return;

    state->palette = ModernTheme::QueryPalette();
    if (state->windowBrush) DeleteObject(state->windowBrush);
    if (state->surfaceBrush) DeleteObject(state->surfaceBrush);
    state->windowBrush = CreateSolidBrush(state->palette.window);
    state->surfaceBrush = CreateSolidBrush(state->palette.surface);

    ModernTheme::ApplyWindowChrome(state->hwnd, state->palette.dark);
    for (int id : {IDC_TARGET, IDC_LIST}) {
        ModernTheme::ApplyControlTheme(GetDlgItem(state->hwnd, id), state->palette.dark);
    }
    ModernTheme::ApplyControlTheme(ListView_GetHeader(state->list), state->palette.dark);

    ListView_SetBkColor(state->list, state->palette.surface);
    ListView_SetTextBkColor(state->list, state->palette.surface);
    ListView_SetTextColor(state->list, state->palette.text);

    RedrawWindow(state->hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

std::wstring PickPath(HWND owner, bool folder) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return {};
    }

    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Select folder to inspect");
    } else {
        options |= FOS_FILEMUSTEXIST;
        dialog->SetTitle(L"Select file to inspect");
    }
    dialog->SetOptions(options);

    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

void RecreateProcessImageList(AppState* state) {
    if (!state || !state->list) return;

    if (state->processImageList) {
        ListView_SetImageList(state->list, nullptr, LVSIL_SMALL);
        ImageList_Destroy(state->processImageList);
        state->processImageList = nullptr;
    }

    const int iconSize = Scale(state, 18);
    state->processImageList = ImageList_Create(
        iconSize, iconSize, ILC_COLOR32 | ILC_MASK, 8, 8);
    if (state->processImageList) {
        ListView_SetImageList(state->list, state->processImageList, LVSIL_SMALL);
    }
}

int AddProcessIcon(AppState* state, const std::wstring& executablePath) {
    if (!state || !state->processImageList) return -1;

    HICON icon = nullptr;
    if (!executablePath.empty()) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(executablePath.c_str(), 0, &info, sizeof(info),
                           SHGFI_ICON | SHGFI_SMALLICON) != 0) {
            icon = info.hIcon;
        }
    }

    const HICON iconToAdd = icon ? icon : state->icon;
    const int imageIndex = iconToAdd
        ? ImageList_AddIcon(state->processImageList, iconToAdd)
        : -1;
    if (icon) {
        DestroyIcon(icon);
    }
    return imageIndex;
}

void RevealProcessExecutable(AppState* state, int row) {
    if (!state || row < 0 || static_cast<size_t>(row) >= state->locks.size()) return;

    const std::wstring& path = state->locks[static_cast<size_t>(row)].path;
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(state->hwnd, L"The executable path is unavailable.",
                    kWindowTitle, MB_ICONINFORMATION);
        return;
    }

    std::wstring parameters = L"/select,\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(state->hwnd, L"open", L"explorer.exe",
                                     parameters.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(state->hwnd, L"Could not open the executable location in Explorer.",
                    kWindowTitle, MB_ICONERROR);
    }
}

std::wstring PrimaryLockedObject(const LockProcess& process) {
    if (process.lockedObjects.empty()) {
        return L"(target / object unavailable)";
    }
    std::wstring text = process.lockedObjects.front();
    if (process.lockedObjects.size() > 1) {
        text += L"  (+" + std::to_wstring(process.lockedObjects.size() - 1) + L")";
    }
    return text;
}

std::wstring BuildProcessInfoText(const LockProcess& process) {
    std::wstring text = process.name + L" (PID " + std::to_wstring(process.pid) + L")";
    if (!process.path.empty()) {
        text += L"\nExecutable: " + process.path;
    }
    if (!process.lockedObjects.empty()) {
        text += L"\n\nLocked objects:";
        const size_t limit = (std::min)(process.lockedObjects.size(), static_cast<size_t>(12));
        for (size_t i = 0; i < limit; ++i) {
            text += L"\n  • " + process.lockedObjects[i];
        }
        if (process.lockedObjects.size() > limit) {
            text += L"\n  • and " + std::to_wstring(process.lockedObjects.size() - limit) + L" more";
        }
    }
    return text;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(data, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void PopulateList(AppState* state) {
    ListView_DeleteAllItems(state->list);
    if (state->processImageList) {
        ImageList_RemoveAll(state->processImageList);
    }

    for (size_t i = 0; i < state->locks.size(); ++i) {
        const auto& process = state->locks[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(process.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        item.iImage = AddProcessIcon(state, process.path);
        int row = ListView_InsertItem(state->list, &item);

        std::wstring pid = std::to_wstring(process.pid);
        ListView_SetItemText(state->list, row, 1, pid.data());

        std::wstring source;
        if (process.foundByRestartManager) {
            source = L"Restart Manager";
        }
        if (process.foundByHandleScan) {
            if (!source.empty()) source += L" + ";
            source += L"Handle";
        }
        if (process.foundByProcessImageScan) {
            if (!source.empty()) source += L" + ";
            source += L"Image/module";
        }
        if (source.empty()) {
            source = L"Process scan";
        }
        ListView_SetItemText(state->list, row, 2, source.data());

        std::wstring lockedObject = PrimaryLockedObject(process);
        ListView_SetItemText(state->list, row, 3, lockedObject.data());
    }
}

void UpdateActionButtons(AppState* state) {
    if (!state || !state->hwnd) {
        return;
    }

    const bool busy = state->scanInProgress;
    const bool exists = state->lastTargetExists;
    const bool hasLocks = !state->locks.empty();
    const bool verifiedClean =
        exists && !hasLocks && state->lastInaccessibleProcessCount == 0;
    const int selectedRow = state->list
        ? ListView_GetNextItem(state->list, -1, LVNI_SELECTED)
        : -1;
    const bool hasSelection = selectedRow >= 0;
    bool selectedUnsafe = false;
    if (hasSelection && static_cast<size_t>(selectedRow) < state->locks.size()) {
        std::wstring reason;
        selectedUnsafe = IsUnsafeSystemProcess(
            state->locks[static_cast<size_t>(selectedRow)].pid, reason);
    }
    const bool hasForceableHandles = std::any_of(
        state->locks.begin(), state->locks.end(),
        [](const LockProcess& process) { return process.foundByHandleScan; });

    EnableWindow(GetDlgItem(state->hwnd, IDC_UNLOCK), exists && hasLocks && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_FORCE_UNLOCK),
                 exists && hasForceableHandles && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_TERMINATE),
                 exists && hasLocks && hasSelection && !selectedUnsafe && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_DELETE), verifiedClean && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_UNLOCK_DELETE),
                 exists && !verifiedClean && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_RETRY),
                 !state->target.empty() && !busy);

    // Keep only one destructive action visible in the compact main toolbar.
    ShowWindow(GetDlgItem(state->hwnd, IDC_DELETE), verifiedClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state->hwnd, IDC_UNLOCK_DELETE), verifiedClean ? SW_HIDE : SW_SHOW);
}

void StartScan(AppState* state) {
    if (!state || state->target.empty()) {
        if (state) {
            state->scanInProgress = false;
            state->lastTargetExists = false;
            state->lastTargetIsDirectory = false;
            state->locks.clear();
            PopulateList(state);
            SetStatus(state, L"Choose a file or folder, or drop it here.");
            UpdateActionButtons(state);
        }
        return;
    }

    const auto generation = ++state->scanGeneration;
    const std::wstring target = state->target;
    const HWND hwnd = state->hwnd;
    state->scanInProgress = true;
    SetStatusWithDetails(state, L"Scanning...",
        L"Scanning Restart Manager, process images/modules and system file handles...");
    SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY), L"Rescan");
    UpdateActionButtons(state);

    std::thread([hwnd, generation, target]() {
        auto payload = std::make_unique<ScanPayload>();
        payload->generation = generation;
        payload->result = ScanLocks(target);
        if (PostMessageW(hwnd, WM_APP_SCAN_DONE, 0, reinterpret_cast<LPARAM>(payload.get()))) {
            payload.release();
        }
    }).detach();
}

void ActivateMainWindow(AppState* state) {
    if (!state || !state->hwnd || !IsWindow(state->hwnd)) {
        return;
    }

    const HWND hwnd = state->hwnd;
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }

    // A context-menu launch can arrive while another application owns the
    // foreground. Pulse TOPMOST only long enough to bring Genia Unlocker to
    // the front, then immediately restore normal (non-always-on-top) behavior.
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
}

void SetTarget(AppState* state, const std::wstring& target, bool showWindow = true) {
    if (target.empty()) {
        return;
    }
    state->target = target;
    state->pendingAction = PendingAction::None;
    state->pendingUnlockError.clear();
    state->pendingForceUnlockDetails.clear();
    state->lastTargetExists = false;
    state->lastTargetIsDirectory = false;
    state->lastInaccessibleProcessCount = 0;
    SetWindowTextW(state->targetEdit, target.c_str());
    if (showWindow) {
        ActivateMainWindow(state);
    }
    StartScan(state);
}

int SelectedLockIndex(AppState* state) {
    return ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
}

void AddTrayIcon(AppState* state) {
    state->tray = {};
    state->tray.cbSize = sizeof(state->tray);
    state->tray.hWnd = state->hwnd;
    state->tray.uID = 1;
    state->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    state->tray.uCallbackMessage = WM_APP_TRAY;
    state->tray.hIcon = state->icon;
    wcscpy_s(state->tray.szTip, L"Genia Unlocker");
    Shell_NotifyIconW(NIM_ADD, &state->tray);
    state->tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &state->tray);
}

void RemoveTrayIcon(AppState* state) {
    Shell_NotifyIconW(NIM_DELETE, &state->tray);
}

void ShowTrayMenu(AppState* state) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open Genia Unlocker");
    AppendMenuW(menu, MF_STRING, ID_TRAY_FILE, L"Select file...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(state->hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, state->hwnd, nullptr);
    DestroyMenu(menu);
}

struct SettingsState {
    AppState* app{};
    HWND closeButton{};
    HWND shellCheck{};
    HWND autostartCheck{};
    HWND permanentDeleteCheck{};
    HFONT font{};
    HFONT titleFont{};
};

void SyncSettingsControls(SettingsState* settings) {
    if (!settings) return;
    SetButtonCheck(settings->shellCheck,
                   IsShellIntegrationEnabled() ? BST_CHECKED : BST_UNCHECKED);
    SetButtonCheck(settings->autostartCheck,
                   IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    SetButtonCheck(settings->permanentDeleteCheck,
                   settings->app && settings->app->permanentDeleteDefault
                       ? BST_CHECKED : BST_UNCHECKED);
}

LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* settings = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        settings = reinterpret_cast<SettingsState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(settings));
    }

    AppState* app = settings ? settings->app : nullptr;
    switch (msg) {
    case WM_CREATE: {
        if (!settings || !app) return -1;
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

        settings->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
        settings->titleFont = CreateModernFont(dpi, 15, FW_SEMIBOLD, L"Segoe UI Variable Display");

        // Compact custom caption for the Settings tool window.  The native
        // caption always places its Close button flush against the right frame;
        // using a client caption lets us keep a small, DPI-aware inset.
        settings->closeButton = CreateWindowW(
            L"BUTTON", L"×",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(368), sc(5), sc(24), sc(22), hwnd,
            ControlId(IDC_SETTINGS_CLOSE), nullptr, nullptr);

        HWND title = CreateWindowW(L"STATIC", L"Settings",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            sc(16), sc(42), sc(330), sc(28), hwnd,
            ControlId(IDC_SETTINGS_TITLE), nullptr, nullptr);
        settings->shellCheck = CreateWindowW(L"BUTTON", L"Explorer context menu",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            sc(16), sc(84), sc(320), sc(24), hwnd,
            ControlId(IDC_SETTINGS_SHELL), nullptr, nullptr);
        settings->autostartCheck = CreateWindowW(L"BUTTON", L"Start with Windows (tray)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            sc(16), sc(112), sc(340), sc(24), hwnd,
            ControlId(IDC_SETTINGS_AUTOSTART), nullptr, nullptr);
        settings->permanentDeleteCheck = CreateWindowW(
            L"BUTTON", L"Permanently delete by default (skip Recycle Bin)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            sc(16), sc(144), sc(350), sc(24), hwnd,
            ControlId(IDC_SETTINGS_PERMANENT), nullptr, nullptr);
        HWND note = CreateWindowW(
            L"STATIC", L"Portable preference is saved beside GeniaUnlocker.exe.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            sc(16), sc(179), sc(350), sc(22), hwnd,
            nullptr, nullptr, nullptr);

        SetFont(settings->closeButton, settings->font);
        SetFont(title, settings->titleFont);
        SetFont(settings->shellCheck, settings->font);
        SetFont(settings->autostartCheck, settings->font);
        SetFont(settings->permanentDeleteCheck, settings->font);
        SetFont(note, settings->font);
        ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->shellCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->autostartCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->permanentDeleteCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->closeButton, app->palette.dark);
        SyncSettingsControls(settings);
        return 0;
    }
    case WM_ERASEBKGND:
        if (app && app->windowBrush) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, app->windowBrush);
            return 1;
        }
        break;
    case WM_PAINT:
        if (settings && app) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
            RECT captionRc{MulDiv(10, dpi, 96), MulDiv(3, dpi, 96),
                           rc.right - MulDiv(48, dpi, 96), MulDiv(29, dpi, 96)};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            HGDIOBJ oldFont = settings->font ? SelectObject(dc, settings->font) : nullptr;
            DrawTextW(dc, L"Genia Unlocker Settings", -1, &captionRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            return reinterpret_cast<LRESULT>(app->windowBrush);
        }
        break;
    case WM_DRAWITEM:
        if (app) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw && draw->CtlType == ODT_BUTTON) {
                ModernTheme::DrawButton(*draw, app->palette, ModernTheme::ButtonKind::Secondary);
                return TRUE;
            }
        }
        break;
    case WM_COMMAND:
        if (!settings || !app) break;
        switch (LOWORD(wParam)) {
        case IDC_SETTINGS_CLOSE:
            if (HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
            }
            return 0;
        case IDC_SETTINGS_SHELL:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled = GetButtonCheck(settings->shellCheck) == BST_CHECKED;
                std::wstring error;
                if (!SetShellIntegrationEnabled(enabled, error)) {
                    MessageBoxW(hwnd, error.c_str(), kWindowTitle, MB_ICONERROR);
                }
                SyncSettingsControls(settings);
            }
            return 0;
        case IDC_SETTINGS_AUTOSTART:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled = GetButtonCheck(settings->autostartCheck) == BST_CHECKED;
                std::wstring error;
                if (!SetAutostartEnabled(enabled, error)) {
                    MessageBoxW(hwnd, error.c_str(), kWindowTitle, MB_ICONERROR);
                }
                SyncSettingsControls(settings);
            }
            return 0;
        case IDC_SETTINGS_PERMANENT:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool previous = app->permanentDeleteDefault;
                app->permanentDeleteDefault =
                    GetButtonCheck(settings->permanentDeleteCheck) == BST_CHECKED;
                if (!SavePermanentDeleteDefault(app->permanentDeleteDefault)) {
                    app->permanentDeleteDefault = previous;
                    MessageBoxW(
                        hwnd,
                        L"Could not write GeniaUnlocker.ini beside the executable. "
                        L"Move the portable EXE to a writable folder or adjust permissions.",
                        kWindowTitle,
                        MB_ICONERROR);
                }
                SyncSettingsControls(settings);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        // Make the custom caption draggable while keeping the inset Close
        // button clickable. Coordinates in lParam are screen coordinates.
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (settings && settings->closeButton) {
            RECT closeRc{};
            GetWindowRect(settings->closeButton, &closeRc);
            if (PtInRect(&closeRc, pt)) {
                return HTCLIENT;
            }
        }

        RECT windowRc{};
        GetWindowRect(hwnd, &windowRc);
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        const int captionHeight = MulDiv(32, dpi, 96);
        if (pt.y >= windowRc.top && pt.y < windowRc.top + captionHeight) {
            return HTCAPTION;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app && app->settingsWindow == hwnd) {
            app->settingsWindow = nullptr;
        }
        if (settings) {
            if (settings->font) DeleteObject(settings->font);
            if (settings->titleFont) DeleteObject(settings->titleFont);
        }
        delete settings;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowSettingsWindow(AppState* state) {
    if (!state || !state->hwnd) return;
    if (state->settingsWindow && IsWindow(state->settingsWindow)) {
        ShowWindow(state->settingsWindow, SW_RESTORE);
        SetForegroundWindow(state->settingsWindow);
        return;
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(state->hwnd, GWLP_HINSTANCE));
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWindowProc;
        wc.hInstance = instance;
        wc.hIcon = state->icon;
        wc.hIconSm = state->icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kSettingsWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(state->hwnd, L"Could not create the settings window.", kWindowTitle, MB_ICONERROR);
            return;
        }
        classReady = true;
    }

    const int dpi = GetDpiForWindow(state->hwnd) > 0 ? GetDpiForWindow(state->hwnd) : 96;
    const int width = MulDiv(400, dpi, 96);
    const int height = MulDiv(220, dpi, 96);
    RECT owner{};
    GetWindowRect(state->hwnd, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    auto* settings = new SettingsState{};
    settings->app = state;
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kSettingsWindowClass,
        L"Genia Unlocker Settings",
        WS_POPUP | WS_BORDER,
        x, y, width, height,
        state->hwnd, nullptr, instance, settings);
    if (!window) {
        delete settings;
        MessageBoxW(state->hwnd, L"Could not open settings.", kWindowTitle, MB_ICONERROR);
        return;
    }
    state->settingsWindow = window;
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
}

std::wstring FormatHResult(HRESULT hr) {
    wchar_t code[32]{};
    swprintf_s(code, _countof(code), L"0x%08lX", static_cast<unsigned long>(hr));
    std::wstring message = ErrorMessage(HRESULT_CODE(hr));
    if (message.empty()) {
        return std::wstring(L"HRESULT ") + code;
    }
    return message + L" (HRESULT " + code + L")";
}

bool DeleteTargetWithShell(const std::wstring& target,
                           bool permanent,
                           std::wstring& errorText) {
    errorText.clear();

    IFileOperation* operation = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));