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
constexpr wchar_t kAppVersionDisplay[] = L"0.5.0 Preview 1";
constexpr wchar_t kRepositoryUrl[] = L"https://github.com/geniasoftwin/Genia-Unlocker";
constexpr wchar_t kIssuesUrl[] = L"https://github.com/geniasoftwin/Genia-Unlocker/issues/new";
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
constexpr int IDC_COPY_REPORT = 1020;

constexpr int IDC_SETTINGS_TITLE = 2001;
constexpr int IDC_SETTINGS_SHELL = 2002;
constexpr int IDC_SETTINGS_AUTOSTART = 2003;
constexpr int IDC_SETTINGS_PERMANENT = 2004;
constexpr int IDC_SETTINGS_CLOSE = 2005;
constexpr int IDC_SETTINGS_ABOUT = 2006;

constexpr UINT ID_TRAY_OPEN = 5001;
constexpr UINT ID_TRAY_FILE = 5002;
constexpr UINT ID_TRAY_EXIT = 5003;
constexpr UINT ID_TRAY_ABOUT = 5004;
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
    DWORD lastRestartManagerError{};
    DWORD lastInspectedDiskHandleCount{};
    DWORD lastDeleteShareProbeError{};
    bool lastHandleTypeFilterAvailable{};
    bool lastDeleteShareProbeSucceeded{};
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

void OpenExternalUrl(HWND owner, const wchar_t* url) {
    if (!url || !*url) return;
    HINSTANCE result = ShellExecuteW(owner, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(owner, L"Windows could not open the requested link.", kWindowTitle, MB_ICONERROR);
    }
}

void ShowAboutDialog(AppState* state) {
    if (!state) return;

    TASKDIALOG_BUTTON buttons[] = {
        { 6001, L"GitHub" },
        { 6002, L"Report issue" },
        { 6003, L"Copy version info" },
        { IDCANCEL, L"Close" }
    };

    std::wstring content =
        L"Lightweight native Windows file/folder unlocker.\n\n"
        L"Version: " + std::wstring(kAppVersionDisplay) +
        L"\nAuthor: GeniaSoftWin"
        L"\nLicense: MIT"
        L"\nPlatform: Windows x64 · Native Win32 C++"
        L"\n\nSource: github.com/geniasoftwin/Genia-Unlocker"
        L"\n\nCopyright © 2026 GeniaSoftWin";

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = state->hwnd;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW |
                     TDF_SIZE_TO_CONTENT |
                     TDF_USE_HICON_MAIN;
    config.pszWindowTitle = L"About Genia Unlocker";
    config.pszMainInstruction = L"Genia Unlocker";
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(_countof(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    config.hMainIcon = state->icon;

    int button = IDCANCEL;
    HRESULT hr = TaskDialogIndirect(&config, &button, nullptr, nullptr);
    if (FAILED(hr)) {
        std::wstring fallback =
            L"Genia Unlocker\nVersion " + std::wstring(kAppVersionDisplay) +
            L"\n\nAuthor: GeniaSoftWin\nLicense: MIT"
            L"\nSource: github.com/geniasoftwin/Genia-Unlocker";
        MessageBoxW(state->hwnd, fallback.c_str(), L"About Genia Unlocker", MB_ICONINFORMATION);
        return;
    }

    if (button == 6001) {
        OpenExternalUrl(state->hwnd, kRepositoryUrl);
    } else if (button == 6002) {
        OpenExternalUrl(state->hwnd, kIssuesUrl);
    } else if (button == 6003) {
        std::wstring info =
            L"Genia Unlocker " + std::wstring(kAppVersionDisplay) +
            L"\r\nAuthor: GeniaSoftWin"
            L"\r\nLicense: MIT"
            L"\r\nSource: " + std::wstring(kRepositoryUrl);
        if (!CopyTextToClipboard(state->hwnd, info)) {
            MessageBoxW(state->hwnd, L"Could not copy version information.", kWindowTitle, MB_ICONERROR);
        }
    }
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
    state->lastRestartManagerError = ERROR_SUCCESS;
    state->lastInspectedDiskHandleCount = 0;
    state->lastDeleteShareProbeError = ERROR_SUCCESS;
    state->lastHandleTypeFilterAvailable = false;
    state->lastDeleteShareProbeSucceeded = false;
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
    AppendMenuW(menu, MF_STRING, ID_TRAY_ABOUT, L"About Genia Unlocker");
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
    HWND aboutButton{};
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
            sc(16), sc(177), sc(350), sc(22), hwnd,
            nullptr, nullptr, nullptr);
        settings->aboutButton = CreateWindowW(
            L"BUTTON", L"About Genia Unlocker",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(16), sc(204), sc(154), sc(28), hwnd,
            ControlId(IDC_SETTINGS_ABOUT), nullptr, nullptr);

        SetFont(settings->closeButton, settings->font);
        SetFont(title, settings->titleFont);
        SetFont(settings->shellCheck, settings->font);
        SetFont(settings->autostartCheck, settings->font);
        SetFont(settings->permanentDeleteCheck, settings->font);
        SetFont(settings->aboutButton, settings->font);
        SetFont(note, settings->font);
        ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->shellCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->autostartCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->permanentDeleteCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->aboutButton, app->palette.dark);
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
        case IDC_SETTINGS_ABOUT:
            if (HIWORD(wParam) == BN_CLICKED) {
                ShowAboutDialog(app);
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
    const int height = MulDiv(250, dpi, 96);
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
    if (FAILED(hr) || !operation) {
        errorText = L"Cannot initialize the Windows file operation service: " + FormatHResult(hr);
        return false;
    }

    DWORD flags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (!permanent) {
        flags |= FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE;
    }
    hr = operation->SetOperationFlags(flags);
    if (FAILED(hr)) {
        operation->Release();
        errorText = L"Cannot configure the delete operation: " + FormatHResult(hr);
        return false;
    }

    IShellItem* item = nullptr;
    hr = SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || !item) {
        operation->Release();
        errorText = L"Cannot open the selected item for deletion: " + FormatHResult(hr);
        return false;
    }

    hr = operation->DeleteItem(item, nullptr);
    item->Release();
    if (FAILED(hr)) {
        operation->Release();
        errorText = L"Windows rejected the delete request: " + FormatHResult(hr);
        return false;
    }

    hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);
    operation->Release();

    if (FAILED(hr)) {
        errorText = L"Delete failed: " + FormatHResult(hr);
        return false;
    }
    if (aborted) {
        errorText = L"The delete operation was aborted.";
        return false;
    }

    const DWORD verifyAttributes = GetFileAttributesW(target.c_str());
    if (verifyAttributes != INVALID_FILE_ATTRIBUTES) {
        errorText = L"The item still exists after Windows completed the delete operation.";
        return false;
    }

    const DWORD verifyError = GetLastError();
    if (verifyError != ERROR_FILE_NOT_FOUND && verifyError != ERROR_PATH_NOT_FOUND) {
        errorText = L"Windows reported deletion, but Genia Unlocker could not verify that the item is gone: " +
                    ErrorMessage(verifyError);
        return false;
    }
    return true;
}

bool IsVolumeRootTarget(const std::wstring& target) {
    if (target.empty()) {
        return false;
    }

    wchar_t volumeRoot[32768]{};
    if (!GetVolumePathNameW(target.c_str(), volumeRoot, static_cast<DWORD>(_countof(volumeRoot)))) {
        return false;
    }

    auto normalize = [](std::wstring value) {
        std::replace(value.begin(), value.end(), L'/', L'\\');
        while (value.size() > 1 && value.back() == L'\\') {
            value.pop_back();
        }
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    };

    return normalize(target) == normalize(volumeRoot);
}

bool ConfirmDelete(AppState* state, const wchar_t* title) {
    if (!state || state->target.empty()) {
        return false;
    }

    DWORD attrs = GetFileAttributesW(state->target.c_str());
    const bool isDirectory = state->lastTargetExists
        ? state->lastTargetIsDirectory
        : (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);

    if (isDirectory && IsVolumeRootTarget(state->target)) {
        MessageBoxW(state->hwnd,
                    L"Deleting a drive or volume root is intentionally disabled.",
                    kWindowTitle,
                    MB_ICONINFORMATION);
        return false;
    }

    std::wstring prompt;
    if (state->permanentDeleteDefault) {
        prompt = isDirectory
            ? L"Permanently delete this folder and everything inside it?\n\n"
            : L"Permanently delete this file?\n\n";
        prompt += state->target;
        prompt += L"\n\nThis action cannot be undone.";
    } else {
        prompt = isDirectory
            ? L"Move this folder and everything inside it to the Recycle Bin?\n\n"
            : L"Move this file to the Recycle Bin?\n\n";
        prompt += state->target;
        prompt += L"\n\nYou can change the default delete mode in Settings.";
    }

    return MessageBoxW(state->hwnd,
                       prompt.c_str(),
                       title,
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

bool ScheduleDeleteOnRebootRecursive(const std::wstring& path, std::wstring& errorText) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        std::wstring pattern = path;
        if (!pattern.empty() && pattern.back() != L'\\') {
            pattern.push_back(L'\\');
        }
        pattern += L"*";

        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD findError = GetLastError();
            if (findError != ERROR_FILE_NOT_FOUND && findError != ERROR_PATH_NOT_FOUND) {
                errorText = L"Could not enumerate the folder for delete-on-reboot: " +
                            ErrorMessage(findError);
                return false;
            }
        } else {
            do {
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                    continue;
                }
                std::wstring child = path;
                if (!child.empty() && child.back() != L'\\') {
                    child.push_back(L'\\');
                }
                child += data.cFileName;
                if (!ScheduleDeleteOnRebootRecursive(child, errorText)) {
                    FindClose(find);
                    return false;
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }

    if (!MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        const DWORD error = GetLastError();
        errorText = L"Could not schedule deletion at reboot: " + ErrorMessage(error);
        return false;
    }
    return true;
}

bool OfferDeleteOnReboot(AppState* state) {
    if (!state || state->target.empty() || IsVolumeRootTarget(state->target)) {
        return false;
    }
    const int answer = MessageBoxW(
        state->hwnd,
        L"Windows could not delete the item now.\n\n"
        L"Schedule it for permanent deletion at the next Windows startup?\n\n"
        L"This fallback does not use the Recycle Bin and cannot be undone after restart.",
        L"Delete on reboot",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        return false;
    }

    std::wstring error;
    if (!ScheduleDeleteOnRebootRecursive(state->target, error)) {
        if (!error.empty()) {
            MessageBoxW(state->hwnd, error.c_str(), L"Delete on reboot", MB_ICONERROR);
        }
        return false;
    }
    SetStatus(state, L"Scheduled for permanent deletion at the next Windows startup.");
    return true;
}

bool TryDeleteCurrentTarget(AppState* state, const std::wstring& successText) {
    if (!state || state->target.empty()) {
        return false;
    }

    SetStatus(state, state->permanentDeleteDefault
        ? L"Permanently deleting the selected item..."
        : L"Moving the selected item to the Recycle Bin...");
    std::wstring error;
    constexpr int kDeleteAttempts = 4;
    for (int attempt = 1; attempt <= kDeleteAttempts; ++attempt) {
        if (DeleteTargetWithShell(state->target, state->permanentDeleteDefault, error)) {
            state->lastTargetExists = false;
            state->lastTargetIsDirectory = false;
            state->lastInaccessibleProcessCount = 0;
            state->locks.clear();
            PopulateList(state);
            SetStatus(state, successText);
            UpdateActionButtons(state);
            return true;
        }

        if (attempt < kDeleteAttempts) {
            const DWORD attrs = GetFileAttributesW(state->target.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                const DWORD verifyError = GetLastError();
                if (verifyError == ERROR_FILE_NOT_FOUND || verifyError == ERROR_PATH_NOT_FOUND) {
                    state->lastTargetExists = false;
                    state->lastTargetIsDirectory = false;
                    state->lastInaccessibleProcessCount = 0;
                    state->locks.clear();
                    PopulateList(state);
                    SetStatus(state, successText);
                    UpdateActionButtons(state);
                    return true;
                }
            }

            // A process can release/reopen its final handle just after the rescan.
            // Give Windows a short grace period before escalating to delete-on-reboot.
            static constexpr DWORD kRetryDelaysMs[] = { 120, 220, 360 };
            Sleep(kRetryDelaysMs[attempt - 1]);
            error.clear();
        }
    }

    if (!error.empty()) {
        MessageBoxW(state->hwnd, error.c_str(), L"Delete failed", MB_ICONERROR);
    }
    if (OfferDeleteOnReboot(state)) {
        return false;
    }
    SetStatus(state, L"Delete failed. Rescanning to identify what is still blocking the item...");
    state->pendingAction = PendingAction::None;
    StartScan(state);
    return false;
}

std::wstring BuildBlockingProcessPrompt(const AppState* state) {
    std::wstring prompt =
        L"The graceful unlock request did not release the item.\n\n"
        L"Blocking processes:\n";

    const size_t limit = (std::min)(state->locks.size(), static_cast<size_t>(6));
    for (size_t i = 0; i < limit; ++i) {
        const auto& process = state->locks[i];
        prompt += L"  • " + process.name + L" (PID " + std::to_wstring(process.pid) + L")\n";
    }
    if (state->locks.size() > limit) {
        prompt += L"  • and " + std::to_wstring(state->locks.size() - limit) + L" more\n";
    }

    prompt += state->permanentDeleteDefault
        ? L"\nTerminate the detected blocking processes and continue with permanent deletion?\n\n"
          L"Unsaved data in those processes can be lost."
        : L"\nTerminate the detected blocking processes and continue by moving the item to the Recycle Bin?\n\n"
          L"Unsaved data in those processes can be lost.";
    return prompt;
}

bool TerminateDetectedForDelete(AppState* state, std::wstring& errorText) {
    errorText.clear();
    std::vector<DWORD> needElevation;

    for (const auto& process : state->locks) {
        std::wstring unsafeReason;
        if (IsUnsafeSystemProcess(process.pid, unsafeReason)) {
            errorText = L"Genia Unlocker refuses to terminate " + process.name +
                        L" (PID " + std::to_wstring(process.pid) + L"): " + unsafeReason + L".";
            return false;
        }

        DWORD error = ERROR_SUCCESS;
        if (TerminateProcessByPid(process.pid, error)) {
            continue;
        }

        // A process can disappear between the scan and the termination pass.
        if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
            continue;
        }

        if (error == ERROR_ACCESS_DENIED && !IsRunningElevated()) {
            needElevation.push_back(process.pid);
            continue;
        }

        errorText = L"Cannot terminate " + process.name + L" (PID " +
                    std::to_wstring(process.pid) + L"): " + ErrorMessage(error);
        return false;
    }

    if (!needElevation.empty()) {
        DWORD helperExitCode = ERROR_GEN_FAILURE;
        if (!RunElevatedKillListHelper(needElevation, helperExitCode, errorText)) {
            if (errorText.empty()) {
                errorText = L"Administrator-assisted termination failed: " +
                            ErrorMessage(helperExitCode);
            }
            return false;
        }
    }

    return true;
}

bool StartTerminateThenDelete(AppState* state) {
    const std::wstring prompt = BuildBlockingProcessPrompt(state);
    if (MessageBoxW(state->hwnd,
                    prompt.c_str(),
                    L"Unlock & Delete",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        state->pendingAction = PendingAction::None;
        SetStatus(state, L"Deletion cancelled. The item is still locked.");
        return false;
    }

    std::wstring error;
    if (!TerminateDetectedForDelete(state, error)) {
        state->pendingAction = PendingAction::None;
        if (!error.empty()) {
            MessageBoxW(state->hwnd, error.c_str(), L"Unlock & Delete", MB_ICONERROR);
        }
        SetStatus(state, L"Could not terminate all blocking processes. The item was not deleted.");
        return false;
    }

    SetStatus(state, L"Blocking processes terminated. Verifying the item before deletion...");
    state->pendingAction = PendingAction::UnlockDeleteAfterTerminate;
    Sleep(300);
    StartScan(state);
    return true;
}

void DeleteOnly(AppState* state) {
    if (!state || state->target.empty() || !state->lastTargetExists) {
        return;
    }
    if (!ConfirmDelete(state, L"Delete")) {
        return;
    }
    TryDeleteCurrentTarget(state, state->permanentDeleteDefault
        ? L"Permanently deleted successfully."
        : L"Moved to the Recycle Bin successfully.");
}

std::wstring FormatForceUnlockResult(const ForceUnlockResult& result) {
    std::wstring text = L"Force Unlock closed " + std::to_wstring(result.handlesClosed) +
        (result.handlesClosed == 1 ? L" matching file handle" : L" matching file handles") +
        L" in " + std::to_wstring(result.processesAffected) +
        (result.processesAffected == 1 ? L" process." : L" processes.");
    if (result.failedHandleCount != 0) {
        text += L" " + std::to_wstring(result.failedHandleCount) + L" matching handles could not be closed.";
    }
    if (result.inaccessibleProcessCount != 0) {
        text += L" " + std::to_wstring(result.inaccessibleProcessCount) +
            L" file-handle owners require higher permissions.";
    }
    if (result.skippedProtectedProcessCount != 0) {
        text += L" " + std::to_wstring(result.skippedProtectedProcessCount) +
            L" critical Windows processes were intentionally skipped.";
    }
    return text;
}

bool ExecuteForceUnlock(AppState* state, bool allowElevation, std::wstring& details) {
    details.clear();
    if (!state || state->target.empty()) {
        return false;
    }

    ForceUnlockResult result = ForceUnlockHandles(state->target);
    details = FormatForceUnlockResult(result);

    if (result.inaccessibleProcessCount != 0 && !IsRunningElevated() && allowElevation) {
        const int answer = MessageBoxW(
            state->hwnd,
            L"Some matching file-handle owners could not be modified with current permissions.\n\n"
            L"Run the Force Unlock pass once as administrator?",
            L"Force Unlock",
            MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1);
        if (answer == IDYES) {
            DWORD helperCode = ERROR_GEN_FAILURE;
            std::wstring helperError;
            if (!RunElevatedForceUnlockHelper(state->target, helperCode, helperError)) {
                if (!helperError.empty()) {
                    MessageBoxW(state->hwnd, helperError.c_str(), L"Force Unlock", MB_ICONERROR);
                }
                return false;
            }
            details += L" Administrator-assisted Force Unlock pass completed.";
        }
    }
    return true;
}

void ForceUnlockCurrent(AppState* state) {
    if (!state || state->target.empty() || state->locks.empty()) {
        return;
    }
    if (IsVolumeRootTarget(state->target)) {
        MessageBoxW(
            state->hwnd,
            L"Force Unlock is intentionally disabled for an entire drive/volume root. "
            L"Choose a specific file or folder instead.",
            L"Force Unlock",
            MB_ICONWARNING);
        return;
    }

    bool hasHandleLocks = std::any_of(
        state->locks.begin(), state->locks.end(),
        [](const LockProcess& process) { return process.foundByHandleScan; });
    if (!hasHandleLocks) {
        MessageBoxW(
            state->hwnd,
            L"No matching file handles are currently available to close.\n\n"
            L"The item may be locked by a loaded executable/module mapping. In that case, "
            L"Terminate is the remaining user-mode option.",
            L"Force Unlock",
            MB_ICONINFORMATION);
        return;
    }

    const int answer = MessageBoxW(
        state->hwnd,
        L"Force Unlock closes only matching file handles inside other processes instead of "
        L"terminating the whole process.\n\n"
        L"This is more invasive than normal Unlock: an application may become unstable if it "
        L"expects the handle to remain valid. Critical Windows processes are never modified.\n\n"
        L"Continue?",
        L"Force Unlock",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        return;
    }

    SetStatus(state, L"Force-closing matching file handles...");
    state->pendingAction = PendingAction::VerifyForceUnlock;
    std::wstring details;
    if (!ExecuteForceUnlock(state, true, details)) {
        state->pendingAction = PendingAction::None;
        SetStatus(state, L"Force Unlock was not completed.");
        return;
    }
    state->pendingForceUnlockDetails = std::move(details);
    Sleep(200);
    StartScan(state);
}

void GracefulUnlock(AppState* state) {
    if (state->target.empty()) {
        return;
    }
    if (state->locks.empty()) {
        MessageBoxW(state->hwnd,
                    L"No locking processes are currently detected.",
                    kWindowTitle,
                    MB_ICONINFORMATION);
        return;
    }

    int answer = MessageBoxW(
        state->hwnd,
        L"Genia Unlocker will ask applications locking this item to close gracefully.\n\n"
        L"After the request, Genia Unlocker will rescan the item and report whether the "
        L"lock was actually released.\n\n"
        L"Unsaved work in those applications may still be affected. Continue?",
        L"Unlock",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) {
        return;
    }

    SetStatus(state, L"Requesting applications to release the resource...");
    state->pendingAction = PendingAction::VerifyUnlock;
    state->pendingUnlockError.clear();
    std::wstring error;
    RequestGracefulUnlock(state->target, error);
    state->pendingUnlockError = std::move(error);
    Sleep(350);
    StartScan(state);
}

void UnlockAndDelete(AppState* state) {
    if (!state || state->target.empty() || !state->lastTargetExists) {
        return;
    }
    if (!ConfirmDelete(state, L"Unlock & Delete")) {
        return;
    }

    // If no blocker matching this target is currently detected, try deletion
    // immediately. inaccessibleProcessCount is intentionally NOT a veto here:
    // it means some system processes could not be inspected, not that they are
    // proven to hold this target. The Windows delete operation itself is the
    // authoritative final check and will fail safely if a hidden lock remains.
    if (state->locks.empty()) {
        TryDeleteCurrentTarget(state, state->permanentDeleteDefault
            ? L"Permanently deleted successfully."
            : L"Moved to the Recycle Bin successfully.");
        return;
    }

    SetStatus(state, L"Trying a graceful unlock before deletion...");
    state->pendingAction = PendingAction::UnlockDeleteAfterUnlock;
    state->pendingUnlockError.clear();
    std::wstring error;
    RequestGracefulUnlock(state->target, error);
    state->pendingUnlockError = std::move(error);
    Sleep(350);
    StartScan(state);
}

void TerminateSelected(AppState* state) {
    int row = SelectedLockIndex(state);
    if (row < 0 || static_cast<size_t>(row) >= state->locks.size()) {
        MessageBoxW(state->hwnd, L"Select a process first.", kWindowTitle, MB_ICONINFORMATION);
        return;
    }

    const LockProcess process = state->locks[static_cast<size_t>(row)];
    std::wstring unsafeReason;
    if (IsUnsafeSystemProcess(process.pid, unsafeReason)) {
        std::wstring message = L"Genia Unlocker will not terminate " + process.name +
            L" because it is a " + unsafeReason + L".";
        MessageBoxW(state->hwnd, message.c_str(), L"Protected process", MB_ICONWARNING);
        return;
    }

    std::wstring prompt = L"Terminate " + process.name + L" (PID " + std::to_wstring(process.pid) +
                          L")?\n\nUnsaved data in this process can be lost.";
    if (MessageBoxW(state->hwnd, prompt.c_str(), L"Terminate process",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    DWORD error = ERROR_SUCCESS;
    if (!TerminateProcessByPid(process.pid, error)) {
        if (error == ERROR_ACCESS_DENIED && !IsRunningElevated()) {
            DWORD helperCode = ERROR_GEN_FAILURE;
            std::wstring elevateError;
            if (!RunElevatedKillHelper(process.pid, helperCode, elevateError)) {
                MessageBoxW(state->hwnd, elevateError.c_str(), kWindowTitle, MB_ICONERROR);
                return;
            }
        } else {
            std::wstring message = L"Cannot terminate the process: " + ErrorMessage(error);
            MessageBoxW(state->hwnd, message.c_str(), kWindowTitle, MB_ICONERROR);
            return;
        }
    }

    Sleep(250);
    StartScan(state);
}

void ShowProcessContextMenu(AppState* state, int row, POINT screenPoint) {
    if (!state || row < 0 || static_cast<size_t>(row) >= state->locks.size()) {
        return;
    }

    ListView_SetItemState(state->list, row,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    const LockProcess& process = state->locks[static_cast<size_t>(row)];

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    UINT forceFlags = MF_STRING;
    UINT terminateFlags = MF_STRING;
    std::wstring unsafeReason;
    if (!process.foundByHandleScan || IsUnsafeSystemProcess(process.pid, unsafeReason)) {
        forceFlags |= MF_GRAYED;
    }
    if (!unsafeReason.empty()) {
        terminateFlags |= MF_GRAYED;
    }

    AppendMenuW(menu, forceFlags, ID_PROCESS_FORCE_UNLOCK, L"Force Unlock target");
    AppendMenuW(menu, terminateFlags, ID_PROCESS_TERMINATE, L"Terminate process");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_PROCESS_OPEN_EXE, L"Open executable location");
    AppendMenuW(menu, MF_STRING, ID_PROCESS_COPY_OBJECT, L"Copy locked object path(s)");
    AppendMenuW(menu, MF_STRING, ID_PROCESS_COPY_PID, L"Copy PID");

    SetForegroundWindow(state->hwnd);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        screenPoint.x, screenPoint.y, 0, state->hwnd, nullptr);
    DestroyMenu(menu);

    switch (command) {
    case ID_PROCESS_FORCE_UNLOCK:
        ForceUnlockCurrent(state);
        break;
    case ID_PROCESS_TERMINATE:
        TerminateSelected(state);
        break;
    case ID_PROCESS_OPEN_EXE:
        RevealProcessExecutable(state, row);
        break;
    case ID_PROCESS_COPY_OBJECT: {
        std::wstring text;
        for (const auto& object : process.lockedObjects) {
            if (!text.empty()) text += L"\r\n";
            text += object;
        }
        if (text.empty()) text = state->target;
        CopyTextToClipboard(state->hwnd, text);
        break;
    }
    case ID_PROCESS_COPY_PID:
        CopyTextToClipboard(state->hwnd, std::to_wstring(process.pid));
        break;
    default:
        break;
    }
}

void RetryScan(AppState* state) {
    if (!state || state->target.empty()) {
        StartScan(state);
        return;
    }

    const bool unidentifiedSharingLock =
        state->locks.empty() &&
        state->lastDeleteShareProbeError == ERROR_SHARING_VIOLATION;
    if ((state->lastInaccessibleProcessCount != 0 || unidentifiedSharingLock) &&
        !IsRunningElevated()) {
        std::wstring prompt;
        if (unidentifiedSharingLock) {
            prompt =
                L"Windows confirms that this target is blocked by file sharing, "
                L"but the owning process was not identified with current permissions.\n\n"
                L"Restart Genia Unlocker as administrator and rescan this target?";
        } else {
            prompt =
                L"Some processes that own file handles could not be inspected with current permissions.\n\n"
                L"Restart Genia Unlocker as administrator and rescan this target?";
        }
        const int answer = MessageBoxW(
            state->hwnd,
            prompt.c_str(),
            L"Scan as administrator",
            MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1);
        if (answer == IDYES) {
            std::wstring error;
            if (RunElevatedScanInstance(state->target, error)) {
                state->exiting = true;
                DestroyWindow(state->hwnd);
                return;
            }
            if (!error.empty()) {
                MessageBoxW(state->hwnd, error.c_str(), kWindowTitle, MB_ICONERROR);
            }
            return;
        }
    }

    StartScan(state);
}

bool HandlePendingActionAfterScan(AppState* state, const ScanResult& result) {
    if (!state || state->pendingAction == PendingAction::None) {
        return false;
    }

    const PendingAction action = state->pendingAction;
    const bool noDetectedLocks = result.targetExists && state->locks.empty();
    // A global count of inaccessible file-handle owners is not evidence that
    // any of them owns this target. Verify the target itself with a harmless
    // DELETE-access/share probe instead.
    const bool verifiedUnlocked =
        noDetectedLocks &&
        result.deleteShareProbeSucceeded;

    if (action == PendingAction::VerifyUnlock) {
        state->pendingAction = PendingAction::None;

        if (!result.targetExists) {
            SetStatus(state, L"The selected path no longer exists.");
            return true;
        }
        if (verifiedUnlocked) {
            SetStatus(state, L"Unlocked successfully — verified by a clean rescan.");
            return true;
        }
        if (!state->locks.empty()) {
            std::wstring text = L"Unlock request completed, but the item is still locked by " +
                std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" process." : L" processes.");
            if (!state->pendingUnlockError.empty()) {
                text += L" Windows did not confirm a graceful release.";
            }
            SetStatus(state, text);
            return true;
        }

        std::wstring text =
            L"Unlock could not be verified because some file-handle owners could not be inspected.";
        if (!IsRunningElevated()) {
            text += L" Use Scan as Admin and try again.";
        }
        SetStatus(state, text);
        return true;
    }

    if (action == PendingAction::VerifyForceUnlock) {
        state->pendingAction = PendingAction::None;
        if (!result.targetExists) {
            SetStatusWithDetails(state, L"The selected path no longer exists.",
                                 state->pendingForceUnlockDetails);
            return true;
        }
        if (verifiedUnlocked) {
            SetStatusWithDetails(state, L"Force Unlock succeeded — verified by a clean rescan.",
                                 state->pendingForceUnlockDetails);
            return true;
        }

        std::wstring shortText;
        if (!state->locks.empty()) {
            shortText = L"Force Unlock completed, but " + std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" blocker remains." : L" blockers remain.");
        } else {
            shortText = L"Force Unlock completed, but the result needs an administrator rescan.";
        }
        std::wstring details = state->pendingForceUnlockDetails;
        if (!details.empty()) details += L" ";
        details += L"Loaded executable/module mappings cannot be released by closing ordinary file handles.";
        SetStatusWithDetails(state, shortText, details);
        return true;
    }

    if (action == PendingAction::UnlockDeleteAfterUnlock) {
        if (!result.targetExists) {
            state->pendingAction = PendingAction::None;
            SetStatus(state, L"The selected item no longer exists.");
            return true;
        }

        if (noDetectedLocks) {
            state->pendingAction = PendingAction::None;
            TryDeleteCurrentTarget(state, state->permanentDeleteDefault
                ? L"Unlocked and permanently deleted successfully."
                : L"Unlocked and moved to the Recycle Bin successfully.");
            return true;
        }

        const bool hasHandleLocks = std::any_of(
            state->locks.begin(), state->locks.end(),
            [](const LockProcess& process) { return process.foundByHandleScan; });
        if (hasHandleLocks) {
            const int forceAnswer = MessageBoxW(
                state->hwnd,
                L"Normal Unlock did not release the item.\n\n"
                L"Try Force Unlock next? Genia Unlocker will close only file handles that match "
                L"this target, without terminating the whole process. Critical Windows processes "
                L"are skipped.\n\n"
                L"An application can become unstable if it expects a closed handle to remain valid.",
                L"Unlock & Delete — Force Unlock",
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1);
            if (forceAnswer == IDYES) {
                state->pendingAction = PendingAction::UnlockDeleteAfterForceUnlock;
                SetStatus(state, L"Trying Force Unlock before process termination...");
                std::wstring details;
                if (!ExecuteForceUnlock(state, true, details)) {
                    state->pendingAction = PendingAction::None;
                    SetStatus(state, L"Force Unlock was not completed. The item was not deleted.");
                    return true;
                }
                state->pendingForceUnlockDetails = std::move(details);
                Sleep(250);
                StartScan(state);
                return true;
            }
        }

        StartTerminateThenDelete(state);
        return true;
    }

    if (action == PendingAction::UnlockDeleteAfterForceUnlock) {
        if (!result.targetExists) {
            state->pendingAction = PendingAction::None;
            SetStatusWithDetails(state, L"The selected item no longer exists.",
                                 state->pendingForceUnlockDetails);
            return true;
        }
        if (noDetectedLocks) {
            state->pendingAction = PendingAction::None;
            TryDeleteCurrentTarget(state, state->permanentDeleteDefault
                ? L"Force-unlocked and permanently deleted successfully."
                : L"Force-unlocked and moved to the Recycle Bin successfully.");
            return true;
        }
        state->lastStatusDetails = state->pendingForceUnlockDetails;
        StartTerminateThenDelete(state);
        return true;
    }

    if (action == PendingAction::UnlockDeleteAfterTerminate) {
        state->pendingAction = PendingAction::None;

        if (!result.targetExists) {
            SetStatus(state, L"The selected item no longer exists.");
            return true;
        }
        if (noDetectedLocks) {
            TryDeleteCurrentTarget(state, state->permanentDeleteDefault
                ? L"Blocking processes terminated and item permanently deleted."
                : L"Blocking processes terminated and item moved to the Recycle Bin.");
            return true;
        }

        std::wstring text =
            L"The item is still locked after process termination, so it was not deleted.";
        if (result.inaccessibleProcessCount != 0 && !IsRunningElevated()) {
            text += L" Use Scan as Admin for a deeper rescan.";
        }
        SetStatus(state, text);
        return true;
    }

    state->pendingAction = PendingAction::None;
    return false;
}

std::wstring BuildCurrentDetails(const AppState* state) {
    if (!state) return {};
    std::wstring text = state->lastStatusDetails;
    if (!state->locks.empty()) {
        if (!text.empty()) text += L"\n\n";
        text += L"Detected blockers:";
        for (const auto& process : state->locks) {
            text += L"\n\n" + BuildProcessInfoText(process);
        }
    }
    if (text.empty()) {
        text = L"No additional scan details are available.";
    }
    return text;
}

std::wstring BuildDiagnosticReport(const AppState* state) {
    if (!state) return {};

    std::wstring targetType = L"(unknown)";
    if (state->lastTargetExists) {
        targetType = state->lastTargetIsDirectory ? L"Directory" : L"File";
    }

    std::wstring deleteShare;
    if (state->lastDeleteShareProbeSucceeded) {
        deleteShare = L"Available";
    } else if (state->lastDeleteShareProbeError == ERROR_SHARING_VIOLATION) {
        deleteShare = L"Blocked (sharing violation)";
    } else if (state->lastTargetExists) {
        deleteShare = L"Unavailable: " + ErrorMessage(state->lastDeleteShareProbeError);
    } else {
        deleteShare = L"Not tested";
    }

    std::wstring report =
        L"Genia Unlocker diagnostic report\r\n"
        L"Version: " + std::wstring(kAppVersionDisplay) +
        L"\r\nElevated: " + std::wstring(IsRunningElevated() ? L"Yes" : L"No") +
        L"\r\nTarget: " + (state->target.empty() ? std::wstring(L"(none)") : state->target) +
        L"\r\nTarget exists: " + std::wstring(state->lastTargetExists ? L"Yes" : L"No") +
        L"\r\nTarget type: " + targetType +
        L"\r\nBlocking processes: " + std::to_wstring(state->locks.size()) +
        L"\r\nDelete-share probe: " + deleteShare +
        L"\r\nRestart Manager: " +
            std::wstring(state->lastRestartManagerError == ERROR_SUCCESS
                ? L"OK"
                : ErrorMessage(state->lastRestartManagerError)) +
        L"\r\nDisk handles inspected: " + std::to_wstring(state->lastInspectedDiskHandleCount) +
        L"\r\nHandle type filter: " +
            std::wstring(state->lastHandleTypeFilterAvailable ? L"File ObjectTypeIndex detected" : L"Unavailable / partial scan") +
        L"\r\nInaccessible file-handle owners: " +
            std::to_wstring(state->lastInaccessibleProcessCount);

    if (!state->lastStatusDetails.empty()) {
        report += L"\r\n\r\nStatus details:\r\n" + state->lastStatusDetails;
    }

    if (!state->locks.empty()) {
        report += L"\r\n\r\nDetected blockers:";
        for (const auto& process : state->locks) {
            report += L"\r\n\r\n" + process.name +
                      L" (PID " + std::to_wstring(process.pid) + L")";
            if (!process.path.empty()) {
                report += L"\r\nExecutable: " + process.path;
            }

            std::wstring method;
            if (process.foundByRestartManager) method += L"Restart Manager";
            if (process.foundByHandleScan) {
                if (!method.empty()) method += L" + ";
                method += L"Handle";
            }
            if (process.foundByProcessImageScan) {
                if (!method.empty()) method += L" + ";
                method += L"Image/Module";
            }
            if (!method.empty()) {
                report += L"\r\nDetection: " + method;
            }

            if (!process.lockedObjects.empty()) {
                report += L"\r\nLocked objects:";
                for (const auto& object : process.lockedObjects) {
                    report += L"\r\n  - " + object;
                }
            }
        }
    }

    report += L"\r\n\r\nSource: " + std::wstring(kRepositoryUrl);
    return report;
}

void LayoutControls(AppState* state, int width, int height) {
    const int margin = Scale(state, 14);
    const int gap = Scale(state, 7);
    const int buttonH = Scale(state, 30);
    const int chooseFileW = Scale(state, 78);
    const int chooseFolderW = Scale(state, 86);
    const int settingsW = Scale(state, 88);

    int y = Scale(state, 10);
    MoveWindow(GetDlgItem(state->hwnd, IDC_APP_TITLE), margin, y,
               width - margin * 2 - settingsW - gap, Scale(state, 26), TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_SETTINGS), width - margin - settingsW, y,
               settingsW, Scale(state, 26), TRUE);

    y += Scale(state, 32);
    MoveWindow(GetDlgItem(state->hwnd, IDC_TARGET_LABEL), margin, y,
               Scale(state, 86), Scale(state, 16), TRUE);
    y += Scale(state, 17);

    int targetW = width - margin * 2 - chooseFileW - chooseFolderW - gap * 2;
    if (targetW < Scale(state, 210)) targetW = Scale(state, 210);
    MoveWindow(state->targetEdit, margin, y, targetW, buttonH, TRUE);
    CenterEditContent(state->targetEdit, state->dpi);
    MoveWindow(GetDlgItem(state->hwnd, IDC_FILE), margin + targetW + gap, y,
               chooseFileW, buttonH, TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_FOLDER),
               margin + targetW + gap + chooseFileW + gap, y,
               chooseFolderW, buttonH, TRUE);

    const int listTop = y + buttonH + Scale(state, 8);
    const int bottomArea = Scale(state, 72);
    int listHeight = height - listTop - bottomArea;
    if (listHeight < Scale(state, 150)) listHeight = Scale(state, 150);
    const int listW = width - margin * 2;
    MoveWindow(state->list, margin, listTop, listW, listHeight, TRUE);
    UpdateListColumns(state, listW);

    const int actionsY = listTop + listHeight + Scale(state, 7);
    const int unlockW = Scale(state, 74);
    const int forceW = Scale(state, 96);
    const int terminateW = Scale(state, 84);
    const int destructiveW = Scale(state, 120);
    const int retryW = Scale(state, 92);

    int actionX = margin;
    MoveWindow(GetDlgItem(state->hwnd, IDC_UNLOCK), actionX, actionsY,
               unlockW, buttonH, TRUE);
    actionX += unlockW + gap;
    MoveWindow(GetDlgItem(state->hwnd, IDC_FORCE_UNLOCK), actionX, actionsY,
               forceW, buttonH, TRUE);
    actionX += forceW + gap;
    MoveWindow(GetDlgItem(state->hwnd, IDC_TERMINATE), actionX, actionsY,
               terminateW, buttonH, TRUE);
    actionX += terminateW + gap;
    MoveWindow(GetDlgItem(state->hwnd, IDC_DELETE), actionX, actionsY,
               destructiveW, buttonH, TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_UNLOCK_DELETE), actionX, actionsY,
               destructiveW, buttonH, TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_RETRY), width - margin - retryW, actionsY,
               retryW, buttonH, TRUE);

    const int statusY = actionsY + buttonH + Scale(state, 5);
    const int detailsW = Scale(state, 58);
    const int reportW = Scale(state, 84);
    MoveWindow(state->status, margin, statusY,
               width - margin * 2 - detailsW - reportW - gap * 2, Scale(state, 18), TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_COPY_REPORT),
               width - margin - detailsW - reportW - gap,
               statusY - Scale(state, 2), reportW, Scale(state, 22), TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_DETAILS), width - margin - detailsW,
               statusY - Scale(state, 2), detailsW, Scale(state, 22), TRUE);
}

void CreateControls(AppState* state) {
    state->dpi = GetDpiForWindow(state->hwnd);
    if (state->dpi <= 0) state->dpi = 96;
    RecreateFonts(state);

    CreateWindowW(L"STATIC", L"Genia Unlocker",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_APP_TITLE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"⚙ Settings",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_SETTINGS), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Target",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_TARGET_LABEL), nullptr, nullptr);

    state->targetEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_TARGET), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"File...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_FILE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Folder...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_FOLDER), nullptr, nullptr);

    state->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_LIST), nullptr, nullptr);
    ListView_SetExtendedListViewStyle(state->list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP | LVS_EX_HEADERDRAGDROP);
    RecreateProcessImageList(state);

    struct Column { const wchar_t* text; int width; } columns[] = {
        {L"Process", 135}, {L"PID", 60}, {L"Method", 105}, {L"Locked object", 330}
    };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(columns[i].text);
        col.cx = Scale(state, columns[i].width);
        col.iSubItem = i;
        ListView_InsertColumn(state->list, i, &col);
    }

    CreateWindowW(L"BUTTON", L"Unlock", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_UNLOCK), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Force Unlock", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_FORCE_UNLOCK), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Terminate", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_TERMINATE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_DELETE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Unlock && Delete", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_UNLOCK_DELETE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Rescan", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_RETRY), nullptr, nullptr);

    state->status = CreateWindowW(L"STATIC", L"Choose a file or folder, or drop it here.",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_STATUS), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Copy report", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_COPY_REPORT), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Details", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_DETAILS), nullptr, nullptr);

    ApplyFonts(state);
    UpdateTheme(state);
    DragAcceptFiles(state->hwnd, TRUE);
    SetStatus(state, L"Choose a file or folder, or drop it here.");
    UpdateActionButtons(state);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<AppState*>(cs->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (state && msg == state->taskbarCreated && state->taskbarCreated != 0) {
        AddTrayIcon(state);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        CreateControls(state);
        AddTrayIcon(state);
        return 0;

    case WM_ERASEBKGND:
        if (state && state->windowBrush) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, state->windowBrush);
            return 1;
        }
        break;

    case WM_CTLCOLORSTATIC: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        const int id = GetDlgCtrlID(control);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (id == IDC_STATUS)
                             ? state->palette.muted
                             : state->palette.text);
        if (id == IDC_TARGET) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, state->palette.surface);
            return reinterpret_cast<LRESULT>(state->surfaceBrush);
        }
        return reinterpret_cast<LRESULT>(state->windowBrush);
    }

    case WM_CTLCOLOREDIT: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, state->palette.text);
        SetBkColor(dc, state->palette.surface);
        return reinterpret_cast<LRESULT>(state->surfaceBrush);
    }

    case WM_CTLCOLORBTN: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state->palette.text);
        return reinterpret_cast<LRESULT>(state->windowBrush);
    }

    case WM_DRAWITEM: {
        if (!state) break;
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (!draw || draw->CtlType != ODT_BUTTON) break;
        ModernTheme::ButtonKind kind = ModernTheme::ButtonKind::Secondary;
        if (draw->CtlID == IDC_UNLOCK) {
            kind = ModernTheme::ButtonKind::Primary;
        } else if (draw->CtlID == IDC_TERMINATE ||
                   draw->CtlID == IDC_DELETE ||
                   draw->CtlID == IDC_UNLOCK_DELETE) {
            kind = ModernTheme::ButtonKind::Danger;
        }
        ModernTheme::DrawButton(*draw, state->palette, kind);
        return TRUE;
    }

    case WM_GETMINMAXINFO:
        if (state) {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Scale(state, 720);
            info->ptMinTrackSize.y = Scale(state, 400);
            return 0;
        }
        break;

    case WM_SIZE:
        if (state) {
            LayoutControls(state, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_DPICHANGED:
        if (state) {
            state->dpi = HIWORD(wParam);
            if (state->dpi <= 0) state->dpi = 96;
            RecreateFonts(state);
            ApplyFonts(state);
            RecreateProcessImageList(state);
            PopulateList(state);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RECT client{};
            GetClientRect(hwnd, &client);
            LayoutControls(state, client.right - client.left, client.bottom - client.top);
            UpdateTheme(state);
        }
        return 0;

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        if (state) {
            UpdateTheme(state);
        }
        return 0;

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[32768]{};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(_countof(path))) > 0) {
            SetTarget(state, path);
        }
        DragFinish(drop);
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_FILE || id == ID_TRAY_FILE) {
            std::wstring path = PickPath(hwnd, false);
            if (!path.empty()) SetTarget(state, path);
        } else if (id == IDC_FOLDER) {
            std::wstring path = PickPath(hwnd, true);
            if (!path.empty()) SetTarget(state, path);
        } else if (id == IDC_UNLOCK) {
            GracefulUnlock(state);
        } else if (id == IDC_FORCE_UNLOCK) {
            ForceUnlockCurrent(state);
        } else if (id == IDC_TERMINATE) {
            TerminateSelected(state);
        } else if (id == IDC_DELETE) {
            DeleteOnly(state);
        } else if (id == IDC_UNLOCK_DELETE) {
            UnlockAndDelete(state);
        } else if (id == IDC_RETRY) {
            RetryScan(state);
        } else if (id == IDC_SETTINGS) {
            ShowSettingsWindow(state);
        } else if (id == IDC_DETAILS) {
            const std::wstring details = BuildCurrentDetails(state);
            MessageBoxW(hwnd, details.c_str(), L"Scan details", MB_ICONINFORMATION);
        } else if (id == IDC_COPY_REPORT) {
            const std::wstring report = BuildDiagnosticReport(state);
            if (CopyTextToClipboard(hwnd, report)) {
                SetStatus(state, L"Diagnostic report copied to the clipboard.");
            } else {
                MessageBoxW(hwnd, L"Could not copy the diagnostic report.", kWindowTitle, MB_ICONERROR);
            }
        } else if (id == ID_TRAY_ABOUT) {
            ShowAboutDialog(state);
        } else if (id == ID_TRAY_EXIT) {
            state->exiting = true;
            DestroyWindow(hwnd);
        } else if (id == ID_TRAY_OPEN) {
            ActivateMainWindow(state);
        }
        return 0;
    }

    case WM_NOTIFY: {
        if (state) {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);
            if (header && header->idFrom == IDC_LIST) {
                if (header->code == LVN_ITEMCHANGED) {
                    UpdateActionButtons(state);
                } else if (header->code == NM_DBLCLK) {
                    const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                    if (activate && activate->iItem >= 0) {
                        RevealProcessExecutable(state, activate->iItem);
                    }
                } else if (header->code == NM_RCLICK) {
                    const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                    if (activate && activate->iItem >= 0) {
                        POINT pt{};
                        GetCursorPos(&pt);
                        ShowProcessContextMenu(state, activate->iItem, pt);
                    }
                } else if (header->code == LVN_GETINFOTIPW) {
                    auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                    if (tip && tip->iItem >= 0 &&
                        static_cast<size_t>(tip->iItem) < state->locks.size() &&
                        tip->pszText && tip->cchTextMax > 0) {
                        const std::wstring text = BuildProcessInfoText(
                            state->locks[static_cast<size_t>(tip->iItem)]);
                        wcsncpy_s(tip->pszText, static_cast<size_t>(tip->cchTextMax),
                                  text.c_str(), _TRUNCATE);
                    }
                }
            }
        }
        break;
    }

    case WM_APP_SCAN_DONE: {
        std::unique_ptr<ScanPayload> payload(reinterpret_cast<ScanPayload*>(lParam));
        if (!payload || payload->generation != state->scanGeneration.load()) {
            return 0;
        }

        ScanResult& result = payload->result;
        state->scanInProgress = false;
        state->lastTargetExists = result.targetExists;
        state->lastTargetIsDirectory = result.targetIsDirectory;
        state->lastInaccessibleProcessCount = result.inaccessibleProcessCount;
        state->lastRestartManagerError = result.restartManagerError;
        state->lastInspectedDiskHandleCount = result.inspectedDiskHandleCount;
        state->lastDeleteShareProbeError = result.deleteShareProbeError;
        state->lastHandleTypeFilterAvailable = result.handleTypeFilterAvailable;
        state->lastDeleteShareProbeSucceeded = result.deleteShareProbeSucceeded;
        state->locks = std::move(result.processes);
        PopulateList(state);

        const bool unidentifiedSharingLock =
            state->locks.empty() &&
            result.deleteShareProbeError == ERROR_SHARING_VIOLATION;
        if ((result.inaccessibleProcessCount != 0 || unidentifiedSharingLock) &&
            !IsRunningElevated()) {
            SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY), L"Scan as Admin");
        } else {
            SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY), L"Rescan");
        }
        UpdateActionButtons(state);

        if (HandlePendingActionAfterScan(state, result)) {
            UpdateActionButtons(state);
            return 0;
        }

        if (!result.targetExists) {
            SetStatus(state, L"The selected path no longer exists.");
        } else if (state->locks.empty()) {
            if (result.deleteShareProbeError == ERROR_SHARING_VIOLATION) {
                std::wstring details =
                    L"Windows confirms that delete sharing is blocked for this target, "
                    L"but the owning process was not identified by Restart Manager or the native handle scan.";
                if (result.inaccessibleProcessCount != 0) {
                    details += L" " + std::to_wstring(result.inaccessibleProcessCount) +
                        (result.inaccessibleProcessCount == 1
                            ? L" file-handle owner could not be inspected."
                            : L" file-handle owners could not be inspected.");
                }
                if (!IsRunningElevated()) {
                    details += L" Scan as Admin may identify an elevated owner.";
                    SetStatusWithDetails(state, L"Locked — blocker not identified. Administrator scan recommended.", details);
                } else {
                    SetStatusWithDetails(state, L"Locked — blocker not identified.", details);
                }
            } else if (result.deleteShareProbeSucceeded) {
                std::wstring details;
                if (result.inaccessibleProcessCount != 0) {
                    details = std::to_wstring(result.inaccessibleProcessCount) +
                        (result.inaccessibleProcessCount == 1
                            ? L" unrelated/protected file-handle owner could not be inspected."
                            : L" unrelated/protected file-handle owners could not be inspected.");
                }
                if (!result.handleTypeFilterAvailable) {
                    if (!details.empty()) details += L" ";
                    details += L"Native File ObjectTypeIndex filtering was unavailable, so handle-scan coverage is partial.";
                }
                if (details.empty()) {
                    SetStatus(state, L"No locking processes detected.");
                } else {
                    SetStatusWithDetails(state, L"No locking processes detected.", details);
                }
            } else {
                std::wstring details = L"No blocker was detected, but delete-share verification failed: " +
                    ErrorMessage(result.deleteShareProbeError);
                if (!result.handleTypeFilterAvailable) {
                    details += L" Native handle-scan coverage is partial.";
                }
                SetStatusWithDetails(state, L"No blocker detected — verification incomplete.", details);
            }
        } else {
            std::wstring shortText = L"\u25CF " + std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" locking process" : L" locking processes");
            size_t lockedObjectCount = 0;
            for (const auto& process : state->locks) {
                lockedObjectCount += process.lockedObjects.size();
            }
            std::wstring details = std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" locking process detected." : L" locking processes detected.");
            if (lockedObjectCount != 0) {
                details += L" " + std::to_wstring(lockedObjectCount) +
                    (lockedObjectCount == 1 ? L" locked object path identified."
                                            : L" locked object paths identified.");
            }
            if (result.restartManagerError != ERROR_SUCCESS) {
                details += L" Handle scan completed; Restart Manager did not accept this resource.";
            }
            if (result.inaccessibleProcessCount != 0) {
                details += L" Some protected/elevated file-handle owners could not be inspected.";
            }
            SetStatusWithDetails(state, shortText, details);
        }
        return 0;
    }

    case WM_APP_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ActivateMainWindow(state);
        } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(state);
        }
        return 0;

    case WM_COPYDATA: {
        const COPYDATASTRUCT* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (copy && copy->lpData && copy->cbData >= sizeof(wchar_t)) {
            const wchar_t* incoming = reinterpret_cast<const wchar_t*>(copy->lpData);
            if (*incoming) {
                SetTarget(state, incoming);
            } else {
                ActivateMainWindow(state);
            }
        }
        return TRUE;
    }

    case WM_CLOSE:
        // Portable tray behavior: closing the window does not terminate the helper.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (state) {
            if (state->settingsWindow && IsWindow(state->settingsWindow)) {
                DestroyWindow(state->settingsWindow);
                state->settingsWindow = nullptr;
            }
            RemoveTrayIcon(state);
            if (state->processImageList) {
                ListView_SetImageList(state->list, nullptr, LVSIL_SMALL);
                ImageList_Destroy(state->processImageList);
                state->processImageList = nullptr;
            }
            if (state->font) DeleteObject(state->font);
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->windowBrush) DeleteObject(state->windowBrush);
            if (state->surfaceBrush) DeleteObject(state->surfaceBrush);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ForwardToExistingInstance(const std::wstring& target) {
    HWND existing = FindWindowW(kWindowClass, nullptr);
    if (!existing) {
        return false;
    }

    // When Explorer starts this short-lived forwarding instance, transfer the
    // foreground permission to the already-running Genia Unlocker process.
    DWORD existingPid = 0;
    GetWindowThreadProcessId(existing, &existingPid);
    if (existingPid != 0) {
        AllowSetForegroundWindow(existingPid);
    }

    std::wstring payload = target;
    payload.push_back(L'\0');
    COPYDATASTRUCT copy{};
    copy.dwData = 1;
    copy.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    copy.lpData = payload.data();
    SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy),
                        SMTO_ABORTIFHUNG, 2000, nullptr);
    return true;
}

int HandleCommandOnlyActions(const ParsedArgs& args) {
    std::wstring error;
    if (args.installShell) {
        if (!SetShellIntegrationEnabled(true, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    if (args.uninstallShell) {
        if (!SetShellIntegrationEnabled(false, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    if (args.enableAutostart) {
        if (!SetAutostartEnabled(true, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    if (args.disableAutostart) {
        if (!SetAutostartEnabled(false, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    return -1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    ParsedArgs args = ParseArguments();

    if (!args.elevatedForceUnlockTarget.empty()) {
        const ForceUnlockResult result = ForceUnlockHandles(args.elevatedForceUnlockTarget);
        if (!result.targetExists) {
            return ERROR_FILE_NOT_FOUND;
        }
        // The caller always performs a verification rescan. An elevated pass may
        // still encounter PPL/protected processes; that is not a helper-launch
        // failure and should not discard handles that were successfully closed.
        return ERROR_SUCCESS;
    }

    if (!args.elevatedKillPids.empty()) {
        for (DWORD pid : args.elevatedKillPids) {
            DWORD error = ERROR_SUCCESS;
            if (!TerminateProcessByPid(pid, error)) {
                // The process may have exited while UAC was being approved.
                if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
                    continue;
                }
                return static_cast<int>(error);
            }
        }
        return ERROR_SUCCESS;
    }

    if (args.elevatedKillPid != 0) {
        DWORD error = ERROR_SUCCESS;
        return TerminateProcessByPid(args.elevatedKillPid, error)
            ? ERROR_SUCCESS
            : static_cast<int>(error);
    }

    int commandResult = HandleCommandOnlyActions(args);
    if (commandResult >= 0) {
        return commandResult;
    }

    if (args.showHelp) {
        MessageBoxW(nullptr,
            L"Genia Unlocker\n\n"
            L"--target <path>\n"
            L"--tray\n"
            L"--install-shell / --uninstall-shell\n"
            L"--enable-autostart / --disable-autostart\n"
            L"Portable build: one native EXE, optional GeniaUnlocker.ini beside it.\n"
            L"Force Unlock closes matching file handles and skips critical Windows processes.",
            kWindowTitle, MB_ICONINFORMATION);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // An elevated rescan deliberately uses a separate singleton scope. The
    // non-elevated instance launches it and immediately exits, avoiding a race
    // where the elevated child would forward its target back to the process
    // that lacks permission to inspect the blocker.
    const wchar_t* mutexName = args.elevatedInstance
        ? L"Local\\GeniaUnlocker.Singleton.Elevated.1"
        : kMutexName;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName);
    bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning) {
        if (!args.tray || !args.target.empty()) {
            ForwardToExistingInstance(args.target);
        }
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }

    AppState state;
    state.permanentDeleteDefault = LoadPermanentDeleteDefault();
    state.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                               0, 0, LR_DEFAULTSIZE));
    if (!state.icon) {
        state.icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    state.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = state.icon;
    wc.hIconSm = state.icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 460,
        nullptr, nullptr, instance, &state);
    if (!hwnd) {
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }

    if (!args.target.empty()) {
        SetTarget(&state, args.target, !args.tray);
    }

    if (!args.tray) {
        ShowWindow(hwnd, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
        UpdateWindow(hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
