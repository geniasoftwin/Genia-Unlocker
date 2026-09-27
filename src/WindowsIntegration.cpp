#include "WindowsIntegration.h"

#include <shellapi.h>
#include <sddl.h>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Genia Unlocker";

const wchar_t* kShellRoots[] = {
    L"Software\\Classes\\*\\shell\\GeniaUnlocker",
    L"Software\\Classes\\Directory\\shell\\GeniaUnlocker",
    L"Software\\Classes\\Drive\\shell\\GeniaUnlocker",
};

std::wstring FormatWin32Error(DWORD code) {
    LPWSTR buffer = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result;
    if (length && buffer) {
        result.assign(buffer, length);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
    } else {
        result = L"Win32 error " + std::to_wstring(code);
    }
    if (buffer) {
        LocalFree(buffer);
    }
    return result;
}

bool SetStringValue(HKEY key, const wchar_t* valueName, const std::wstring& value) {
    const BYTE* bytes = reinterpret_cast<const BYTE*>(value.c_str());
    DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, valueName, 0, REG_SZ, bytes, size) == ERROR_SUCCESS;
}

bool WriteShellKey(const wchar_t* rootPath, const std::wstring& exePath, std::wstring& errorText) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, rootPath, 0, nullptr, 0,
                              KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        errorText = L"Cannot create Explorer integration key: " + FormatWin32Error(rc);
        return false;
    }

    bool ok = SetStringValue(key, nullptr, L"Genia Unlocker") &&
              SetStringValue(key, L"Icon", exePath);
    if (!ok) {
        rc = GetLastError();
        RegCloseKey(key);
        errorText = L"Cannot write Explorer integration: " + FormatWin32Error(rc);
        return false;
    }

    HKEY commandKey = nullptr;
    rc = RegCreateKeyExW(key, L"command", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &commandKey, nullptr);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(key);
        errorText = L"Cannot create Explorer command key: " + FormatWin32Error(rc);
        return false;
    }

    std::wstring command = QuoteCommandArgument(exePath) + L" --target \"%1\"";
    ok = SetStringValue(commandKey, nullptr, command);
    RegCloseKey(commandKey);
    RegCloseKey(key);

    if (!ok) {
        errorText = L"Cannot write Explorer command.";
    }
    return ok;
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD size = 0;
    LONG rc = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }

    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    rc = RegQueryValueExW(key, valueName, nullptr, &type,
                          reinterpret_cast<BYTE*>(buffer.data()), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(buffer.data());
}

} // namespace

std::wstring GetExecutablePath() {
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return buffer;
}

std::wstring QuoteCommandArgument(const std::wstring& value) {
    if (value.empty()) {
        return L"\"\"";
    }

    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

bool IsShellIntegrationEnabled() {
    std::wstring expected = QuoteCommandArgument(GetExecutablePath()) + L" --target \"%1\"";
    for (const wchar_t* root : kShellRoots) {
        std::wstring commandKey = std::wstring(root) + L"\\command";
        std::wstring command = ReadRegistryString(HKEY_CURRENT_USER, commandKey.c_str(), nullptr);
        if (command.empty() || _wcsicmp(command.c_str(), expected.c_str()) != 0) {
            return false;
        }
    }
    return true;
}

bool SetShellIntegrationEnabled(bool enabled, std::wstring& errorText) {
    errorText.clear();
    if (!enabled) {
        bool ok = true;
        for (const wchar_t* root : kShellRoots) {
            LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, root);
            if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
                ok = false;
                errorText = L"Cannot remove Explorer integration: " + FormatWin32Error(rc);
            }
        }
        return ok;
    }

    const std::wstring exePath = GetExecutablePath();
    if (exePath.empty()) {
        errorText = L"Cannot determine Genia Unlocker executable path.";
        return false;
    }

    // Rewrite all entries so moving the portable EXE is automatically repaired
    // when the user re-enables the option.
    for (const wchar_t* root : kShellRoots) {
        RegDeleteTreeW(HKEY_CURRENT_USER, root);
        if (!WriteShellKey(root, exePath, errorText)) {
            return false;
        }
    }
    return true;
}

bool IsAutostartEnabled() {
    std::wstring value = ReadRegistryString(HKEY_CURRENT_USER, kRunKey, kRunValue);
    if (value.empty()) {
        return false;
    }
    std::wstring expected = QuoteCommandArgument(GetExecutablePath()) + L" --tray";
    return _wcsicmp(value.c_str(), expected.c_str()) == 0;
}

bool SetAutostartEnabled(bool enabled, std::wstring& errorText) {
    errorText.clear();
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                              KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        errorText = L"Cannot open Windows startup settings: " + FormatWin32Error(rc);
        return false;
    }

    bool ok = true;
    if (enabled) {
        std::wstring value = QuoteCommandArgument(GetExecutablePath()) + L" --tray";
        ok = SetStringValue(key, kRunValue, value);
        if (!ok) {
            errorText = L"Cannot enable Windows startup.";
        }
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
        if (!ok) {
            errorText = L"Cannot disable Windows startup: " + FormatWin32Error(rc);
        }
    }

    RegCloseKey(key);
    return ok;
}

bool IsRunningElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool RunElevatedKillListHelper(const std::vector<DWORD>& pids,
                               DWORD& helperExitCode,
                               std::wstring& errorText) {
    helperExitCode = ERROR_GEN_FAILURE;
    errorText.clear();

    if (pids.empty()) {
        helperExitCode = ERROR_SUCCESS;
        return true;
    }

    const std::wstring exe = GetExecutablePath();
    if (exe.empty()) {
        errorText = L"Cannot determine Genia Unlocker executable path.";
        return false;
    }

    std::wstring list;
    for (DWORD pid : pids) {
        if (pid == 0) {
            continue;
        }
        if (!list.empty()) {
            list.push_back(L',');
        }
        list += std::to_wstring(pid);
    }
    if (list.empty()) {
        helperExitCode = ERROR_SUCCESS;
        return true;
    }

    const std::wstring parameters = L"--elevated-kill-list " + QuoteCommandArgument(list);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = nullptr;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        errorText = (error == ERROR_CANCELLED)
            ? L"Administrator permission was cancelled."
            : L"Cannot start elevated helper: " + FormatWin32Error(error);
        return false;
    }

    WaitForSingleObject(info.hProcess, INFINITE);
    GetExitCodeProcess(info.hProcess, &helperExitCode);
    CloseHandle(info.hProcess);
    return helperExitCode == ERROR_SUCCESS;
}

bool RunElevatedKillHelper(DWORD pid, DWORD& helperExitCode, std::wstring& errorText) {
    return RunElevatedKillListHelper({pid}, helperExitCode, errorText);
}

bool RunElevatedForceUnlockHelper(const std::wstring& target,
                                  DWORD& helperExitCode,
                                  std::wstring& errorText) {
    helperExitCode = ERROR_GEN_FAILURE;
    errorText.clear();

    const std::wstring exe = GetExecutablePath();
    if (exe.empty()) {
        errorText = L"Cannot determine Genia Unlocker executable path.";
        return false;
    }

    std::wstring parameters = L"--elevated-force-unlock ";
    parameters += QuoteCommandArgument(target);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = nullptr;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        errorText = (error == ERROR_CANCELLED)
            ? L"Administrator permission was cancelled."
            : L"Cannot start elevated force-unlock helper: " + FormatWin32Error(error);
        return false;
    }

    WaitForSingleObject(info.hProcess, INFINITE);
    GetExitCodeProcess(info.hProcess, &helperExitCode);
    CloseHandle(info.hProcess);
    return helperExitCode == ERROR_SUCCESS;
}

bool RunElevatedScanInstance(const std::wstring& target, std::wstring& errorText) {
    errorText.clear();

    const std::wstring exe = GetExecutablePath();
    if (exe.empty()) {
        errorText = L"Cannot determine Genia Unlocker executable path.";
        return false;
    }

    std::wstring parameters = L"--elevated-instance --target ";
    parameters += QuoteCommandArgument(target);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = nullptr;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        errorText = (error == ERROR_CANCELLED)
            ? L"Administrator permission was cancelled."
            : L"Cannot start elevated Genia Unlocker: " + FormatWin32Error(error);
        return false;
    }

    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    return true;
}
