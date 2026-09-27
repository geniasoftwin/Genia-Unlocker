#include "LockScanner.h"

#include <RestartManager.h>
#include <TlHelp32.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Rstrtmgr.lib")

namespace {

constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);
constexpr ULONG kSystemExtendedHandleInformation = 64;

typedef LONG (NTAPI* NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct SYSTEM_HANDLE_INFORMATION_EX_LOCAL {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX_LOCAL Handles[1];
};

USHORT DetectFileObjectTypeIndex(const SYSTEM_HANDLE_INFORMATION_EX_LOCAL* info) {
    if (!info) {
        return 0;
    }

    // SystemExtendedHandleInformation exposes an ObjectTypeIndex but does not
    // provide its name. Open one file handle that belongs to this process and
    // find the matching table entry. This lets the main scan ignore mutexes,
    // events, registry keys, sections, etc. before asking for PROCESS_DUP_HANDLE.
    // Besides being much faster, this prevents harmless protected processes
    // from being reported as an incomplete *file* scan.
    wchar_t modulePath[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(_countof(modulePath)));
    if (length == 0 || length >= _countof(modulePath)) {
        return 0;
    }

    HANDLE probe = CreateFileW(modulePath,
                               FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        return 0;
    }

    const DWORD currentPid = GetCurrentProcessId();
    const ULONG_PTR probeValue = reinterpret_cast<ULONG_PTR>(probe);
    USHORT typeIndex = 0;
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& entry = info->Handles[i];
        if (static_cast<DWORD>(entry.UniqueProcessId) == currentPid &&
            entry.HandleValue == probeValue) {
            typeIndex = entry.ObjectTypeIndex;
            break;
        }
    }

    CloseHandle(probe);
    return typeIndex;
}

struct FileIdentity {
    bool valid{};
    DWORD volumeSerial{};
    DWORD fileIndexHigh{};
    DWORD fileIndexLow{};
};

bool TryGetFileIdentity(HANDLE handle, FileIdentity& identity) {
    identity = {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) {
        return false;
    }
    identity.valid = true;
    identity.volumeSerial = info.dwVolumeSerialNumber;
    identity.fileIndexHigh = info.nFileIndexHigh;
    identity.fileIndexLow = info.nFileIndexLow;
    return true;
}

bool SameFileIdentity(const FileIdentity& a, const FileIdentity& b) {
    return a.valid && b.valid &&
           a.volumeSerial == b.volumeSerial &&
           a.fileIndexHigh == b.fileIndexHigh &&
           a.fileIndexLow == b.fileIndexLow;
}

void EnableDebugPrivilegeBestEffort() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return;
    }

    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    }
    CloseHandle(token);
}

HANDLE OpenTargetForMetadata(const std::wstring& target, bool isDirectory) {
    DWORD flags = isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    return CreateFileW(target.c_str(),
                       0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr,
                       OPEN_EXISTING,
                       flags,
                       nullptr);
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

std::wstring StripExtendedPrefix(std::wstring value) {
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        value = L"\\\\" + value.substr(8);
    } else if (value.rfind(L"\\\\?\\", 0) == 0) {
        value = value.substr(4);
    }
    return value;
}

std::wstring NormalizePath(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }

    DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    std::wstring full;
    if (needed != 0) {
        full.resize(needed);
        DWORD written = GetFullPathNameW(input.c_str(), needed, full.data(), nullptr);
        if (written > 0 && written < full.size()) {
            full.resize(written);
        } else {
            full = input;
        }
    } else {
        full = input;
    }

    full = StripExtendedPrefix(full);
    std::replace(full.begin(), full.end(), L'/', L'\\');

    // Keep the trailing slash for volume roots (C:\), strip it elsewhere.
    while (full.size() > 3 && !full.empty() && full.back() == L'\\') {
        full.pop_back();
    }
    return ToLower(full);
}

bool IsVolumeRootPath(const std::wstring& target) {
    wchar_t volumeRoot[32768]{};
    if (!GetVolumePathNameW(target.c_str(), volumeRoot, static_cast<DWORD>(_countof(volumeRoot)))) {
        return false;
    }
    return NormalizePath(target) == NormalizePath(volumeRoot);
}

bool PathMatchesTarget(const std::wstring& normalizedHandlePath,
                       const std::wstring& normalizedTarget,
                       bool targetIsDirectory) {
    if (normalizedHandlePath == normalizedTarget) {
        return true;
    }
    if (!targetIsDirectory) {
        return false;
    }

    if (normalizedTarget.empty()) {
        return false;
    }

    std::wstring prefix = normalizedTarget;
    if (prefix.back() != L'\\') {
        prefix.push_back(L'\\');
    }
    return normalizedHandlePath.rfind(prefix, 0) == 0;
}

std::wstring QueryFinalPathDisplay(HANDLE handle, DWORD nameMode) {
    const DWORD flags = nameMode | VOLUME_NAME_DOS;
    DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (needed == 0) {
        return {};
    }

    std::wstring buffer(needed + 1, L'\0');
    DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    buffer.resize(written);
    buffer = StripExtendedPrefix(buffer);
    std::replace(buffer.begin(), buffer.end(), L'/', L'\\');
    return buffer;
}

std::wstring GetHandleDisplayPath(HANDLE handle) {
    std::wstring path = QueryFinalPathDisplay(handle, FILE_NAME_NORMALIZED);
    if (path.empty()) {
        path = QueryFinalPathDisplay(handle, FILE_NAME_OPENED);
    }
    return path;
}

std::wstring GetHandlePath(HANDLE handle) {
    const std::wstring display = GetHandleDisplayPath(handle);
    return display.empty() ? std::wstring{} : NormalizePath(display);
}

void MergeProcess(std::unordered_map<DWORD, LockProcess>& map,
                  DWORD pid,
                  bool restartManager,
                  bool handleScan,
                  bool processImageScan = false,
                  const std::wstring& restartManagerName = {},
                  const std::wstring& lockedObject = {}) {
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return;
    }

    auto [it, inserted] = map.try_emplace(pid);
    LockProcess& process = it->second;
    process.pid = pid;
    process.foundByRestartManager = process.foundByRestartManager || restartManager;
    process.foundByHandleScan = process.foundByHandleScan || handleScan;
    process.foundByProcessImageScan = process.foundByProcessImageScan || processImageScan;

    if (process.path.empty()) {
        process.path = QueryProcessImagePath(pid);
    }
    if (process.name.empty()) {
        if (!restartManagerName.empty()) {
            process.name = restartManagerName;
        } else {
            process.name = QueryProcessDisplayName(pid);
        }
    }
    if (process.name.empty()) {
        process.name = L"PID " + std::to_wstring(pid);
    }

    if (!lockedObject.empty() && process.lockedObjects.size() < 64) {
        const std::wstring normalizedCandidate = NormalizePath(lockedObject);
        bool exists = false;
        for (const auto& existing : process.lockedObjects) {
            if (NormalizePath(existing) == normalizedCandidate) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            if (handleScan || processImageScan) {
                process.lockedObjects.insert(process.lockedObjects.begin(), lockedObject);
            } else {
                process.lockedObjects.push_back(lockedObject);
            }
        }
    }
}

DWORD ScanRestartManager(const std::wstring& target, std::unordered_map<DWORD, LockProcess>& processes) {
    DWORD session = 0;
    WCHAR sessionKey[CCH_RM_SESSION_KEY + 1]{};
    DWORD rc = RmStartSession(&session, 0, sessionKey);
    if (rc != ERROR_SUCCESS) {
        return rc;
    }

    LPCWSTR resources[] = {target.c_str()};
    rc = RmRegisterResources(session, 1, resources, 0, nullptr, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        RmEndSession(session);
        return rc;
    }

    UINT needed = 0;
    UINT count = 0;
    DWORD rebootReasons = 0;
    rc = RmGetList(session, &needed, &count, nullptr, &rebootReasons);
    if (rc == ERROR_SUCCESS && needed == 0) {
        RmEndSession(session);
        return ERROR_SUCCESS;
    }
    if (rc != ERROR_MORE_DATA) {
        RmEndSession(session);
        return rc;
    }

    std::vector<RM_PROCESS_INFO> infos(needed);
    count = needed;
    rc = RmGetList(session, &needed, &count, infos.data(), &rebootReasons);
    if (rc == ERROR_SUCCESS) {
        for (UINT i = 0; i < count; ++i) {
            MergeProcess(processes,
                         infos[i].Process.dwProcessId,
                         true,
                         false,
                         false,
                         infos[i].strAppName,
                         target);
        }
    }

    RmEndSession(session);
    return rc;
}


void ScanProcessImagesAndModules(const std::wstring& normalizedTarget,
                                 const std::wstring& normalizedCanonicalTarget,
                                 bool targetIsDirectory,
                                 std::unordered_map<DWORD, LockProcess>& processes) {
    HANDLE processSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processSnapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);
    if (!Process32FirstW(processSnapshot, &processEntry)) {
        CloseHandle(processSnapshot);
        return;
    }

    auto pathMatches = [&](const std::wstring& rawPath) {
        if (rawPath.empty()) {
            return false;
        }
        const std::wstring normalized = NormalizePath(rawPath);
        if (normalized.empty()) {
            return false;
        }
        if (PathMatchesTarget(normalized, normalizedTarget, targetIsDirectory)) {
            return true;
        }
        return !normalizedCanonicalTarget.empty() &&
               normalizedCanonicalTarget != normalizedTarget &&
               PathMatchesTarget(normalized, normalizedCanonicalTarget, targetIsDirectory);
    };

    do {
        const DWORD pid = processEntry.th32ProcessID;
        if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
            continue;
        }

        const std::wstring imagePath = QueryProcessImagePath(pid);
        if (pathMatches(imagePath)) {
            // A running EXE inside the selected folder (or the selected EXE
            // itself) is locked by an image section, not necessarily by an
            // ordinary file handle. This is a very common directory-delete
            // blocker and was the main blind spot in earlier versions.
            MergeProcess(processes, pid, false, false, true, {}, imagePath);
        }

        HANDLE moduleSnapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (moduleSnapshot == INVALID_HANDLE_VALUE) {
            continue;
        }

        MODULEENTRY32W moduleEntry{};
        moduleEntry.dwSize = sizeof(moduleEntry);
        if (Module32FirstW(moduleSnapshot, &moduleEntry)) {
            do {
                if (pathMatches(moduleEntry.szExePath)) {
                    // DLLs and executable images can keep a file/folder from
                    // being deleted after the loader has already closed its
                    // original file handle. Toolhelp module enumeration catches
                    // that case without needing a kernel driver.
                    MergeProcess(processes, pid, false, false, true, {}, moduleEntry.szExePath);
                    break;
                }
            } while (Module32NextW(moduleSnapshot, &moduleEntry));
        }
        CloseHandle(moduleSnapshot);
    } while (Process32NextW(processSnapshot, &processEntry));

    CloseHandle(processSnapshot);
}

void ScanSystemHandles(const std::wstring& normalizedTarget,
                       const std::wstring& normalizedCanonicalTarget,
                       const FileIdentity& targetIdentity,
                       bool targetIsDirectory,
                       DWORD& inaccessibleProcessCount,
                       DWORD& inspectedDiskHandleCount,
                       std::unordered_map<DWORD, LockProcess>& processes) {
    EnableDebugPrivilegeBestEffort();

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return;
    }

    auto ntQuerySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!ntQuerySystemInformation) {
        return;
    }

    ULONG size = 1u << 20;
    std::vector<BYTE> buffer(size);
    ULONG required = 0;
    LONG status = 0;

    for (int attempt = 0; attempt < 8; ++attempt) {
        status = ntQuerySystemInformation(
            kSystemExtendedHandleInformation, buffer.data(), size, &required);
        if (status != kStatusInfoLengthMismatch) {
            break;
        }
        const ULONG doubled = static_cast<ULONG>(size * 2u);
        const ULONG requested = static_cast<ULONG>(required + (1u << 16));
        size = (std::max)(doubled, requested);
        buffer.resize(size);
    }
    if (status < 0) {
        return;
    }

    const auto* info = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX_LOCAL*>(buffer.data());
    const USHORT fileObjectTypeIndex = DetectFileObjectTypeIndex(info);
    std::unordered_map<DWORD, HANDLE> processHandles;
    std::unordered_set<DWORD> inaccessiblePids;

    auto closeAll = [&]() {
        for (auto& [_, handle] : processHandles) {
            if (handle) {
                CloseHandle(handle);
            }
        }
    };

    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& entry = info->Handles[i];
        DWORD pid = static_cast<DWORD>(entry.UniqueProcessId);
        if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
            continue;
        }

        // Once the file object type has been identified, do not attempt to
        // open a protected process merely because it owns some unrelated
        // kernel object. v0.2.4 counted those as "inaccessible processes",
        // which could produce numbers such as 100+ even though they had
        // nothing to do with the selected folder.
        if (fileObjectTypeIndex != 0 && entry.ObjectTypeIndex != fileObjectTypeIndex) {
            continue;
        }

        HANDLE process = nullptr;
        auto it = processHandles.find(pid);
        if (it == processHandles.end()) {
            // PROCESS_QUERY_LIMITED_INFORMATION is intentionally not requested
            // here. Duplication is all the scanner needs, and asking for extra
            // rights caused otherwise inspectable processes to be skipped.
            process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
            if (!process && GetLastError() == ERROR_ACCESS_DENIED) {
                inaccessiblePids.insert(pid);
            }
            processHandles.emplace(pid, process);
        } else {
            process = it->second;
        }
        if (!process) {
            continue;
        }

        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process,
                             reinterpret_cast<HANDLE>(entry.HandleValue),
                             GetCurrentProcess(),
                             &duplicate,
                             0,
                             FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            continue;
        }

        if (GetFileType(duplicate) == FILE_TYPE_DISK) {
            ++inspectedDiskHandleCount;

            bool matched = false;
            if (targetIdentity.valid) {
                FileIdentity candidateIdentity{};
                if (TryGetFileIdentity(duplicate, candidateIdentity) &&
                    SameFileIdentity(targetIdentity, candidateIdentity)) {
                    // This catches the exact same file/directory even when the
                    // process opened it through a junction, symlink, SUBST or
                    // another path alias.
                    matched = true;
                }
            }

            if (!matched) {
                std::wstring path = GetHandlePath(duplicate);
                if (!path.empty()) {
                    matched = PathMatchesTarget(path, normalizedTarget, targetIsDirectory);
                    if (!matched && !normalizedCanonicalTarget.empty() &&
                        normalizedCanonicalTarget != normalizedTarget) {
                        matched = PathMatchesTarget(path, normalizedCanonicalTarget, targetIsDirectory);
                    }
                }
            }

            if (matched) {
                std::wstring displayPath = GetHandleDisplayPath(duplicate);
                if (displayPath.empty()) {
                    displayPath = normalizedTarget;
                }
                MergeProcess(processes, pid, false, true, false, {}, displayPath);
            }
        }
        CloseHandle(duplicate);
    }

    inaccessibleProcessCount = static_cast<DWORD>(inaccessiblePids.size());
    closeAll();
}

bool MatchDuplicatedFileHandle(HANDLE duplicate,
                               const std::wstring& normalizedTarget,
                               const std::wstring& normalizedCanonicalTarget,
                               const FileIdentity& targetIdentity,
                               bool targetIsDirectory) {
    if (!duplicate || duplicate == INVALID_HANDLE_VALUE ||
        GetFileType(duplicate) != FILE_TYPE_DISK) {
        return false;
    }

    if (targetIdentity.valid) {
        FileIdentity candidateIdentity{};
        if (TryGetFileIdentity(duplicate, candidateIdentity) &&
            SameFileIdentity(targetIdentity, candidateIdentity)) {
            return true;
        }
    }

    const std::wstring path = GetHandlePath(duplicate);
    if (path.empty()) {
        return false;
    }
    if (PathMatchesTarget(path, normalizedTarget, targetIsDirectory)) {
        return true;
    }
    return !normalizedCanonicalTarget.empty() &&
           normalizedCanonicalTarget != normalizedTarget &&
           PathMatchesTarget(path, normalizedCanonicalTarget, targetIsDirectory);
}

std::wstring QueryProcessSnapshotName(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {};
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring name;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                name = entry.szExeFile;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return name;
}

BOOL CALLBACK CloseWindowForPids(HWND hwnd, LPARAM param) {
    auto* pids = reinterpret_cast<std::unordered_set<DWORD>*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0 && pids->contains(pid) && IsWindowVisible(hwnd)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

} // namespace

std::wstring QueryProcessImagePath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }

    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        CloseHandle(process);
        return {};
    }
    CloseHandle(process);
    path.resize(size);
    return path;
}

std::wstring QueryProcessDisplayName(DWORD pid) {
    std::wstring path = QueryProcessImagePath(pid);
    if (path.empty()) {
        return {};
    }
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsUnsafeSystemProcess(DWORD pid, std::wstring& reason) {
    reason.clear();
    if (pid == 0 || pid == 4) {
        reason = L"Windows kernel/system process";
        return true;
    }

    std::wstring name = QueryProcessDisplayName(pid);
    if (name.empty()) {
        name = QueryProcessSnapshotName(pid);
    }
    name = ToLower(name);
    static constexpr const wchar_t* kProtectedNames[] = {
        L"system",
        L"registry",
        L"smss.exe",
        L"csrss.exe",
        L"wininit.exe",
        L"winlogon.exe",
        L"services.exe",
        L"lsass.exe"
    };
    for (const wchar_t* protectedName : kProtectedNames) {
        if (name == protectedName) {
            reason = L"critical Windows process";
            return true;
        }
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return false;
    }

    using IsProcessCriticalFn = BOOL (WINAPI*)(HANDLE, PBOOL);
    auto isProcessCritical = reinterpret_cast<IsProcessCriticalFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsProcessCritical"));
    if (isProcessCritical) {
        BOOL critical = FALSE;
        if (isProcessCritical(process, &critical) && critical) {
            CloseHandle(process);
            reason = L"Windows marks this process as critical";
            return true;
        }
    }

    CloseHandle(process);
    return false;
}

ForceUnlockResult ForceUnlockHandles(const std::wstring& target) {
    ForceUnlockResult result;

    DWORD attrs = GetFileAttributesW(target.c_str());
    result.targetExists = attrs != INVALID_FILE_ATTRIBUTES;
    const bool targetIsDirectory =
        result.targetExists && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!result.targetExists) {
        return result;
    }
    if (IsVolumeRootPath(target)) {
        result.failedHandleCount = 1;
        return result;
    }

    const std::wstring normalizedTarget = NormalizePath(target);
    if (normalizedTarget.empty()) {
        return result;
    }

    std::wstring normalizedCanonicalTarget;
    FileIdentity targetIdentity{};
    HANDLE targetHandle = OpenTargetForMetadata(target, targetIsDirectory);
    if (targetHandle != INVALID_HANDLE_VALUE) {
        normalizedCanonicalTarget = GetHandlePath(targetHandle);
        TryGetFileIdentity(targetHandle, targetIdentity);
        CloseHandle(targetHandle);
    }

    EnableDebugPrivilegeBestEffort();

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return result;
    }
    auto ntQuerySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!ntQuerySystemInformation) {
        return result;
    }

    ULONG size = 1u << 20;
    std::vector<BYTE> buffer(size);
    ULONG required = 0;
    LONG status = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        status = ntQuerySystemInformation(
            kSystemExtendedHandleInformation, buffer.data(), size, &required);
        if (status != kStatusInfoLengthMismatch) {
            break;
        }
        const ULONG doubled = static_cast<ULONG>(size * 2u);
        const ULONG requested = static_cast<ULONG>(required + (1u << 16));
        size = (std::max)(doubled, requested);
        buffer.resize(size);
    }
    if (status < 0) {
        return result;
    }

    const auto* info = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX_LOCAL*>(buffer.data());
    const USHORT fileObjectTypeIndex = DetectFileObjectTypeIndex(info);

    std::unordered_map<DWORD, HANDLE> processHandles;
    std::unordered_set<DWORD> inaccessiblePids;
    std::unordered_set<DWORD> protectedPids;
    std::unordered_set<DWORD> safePids;
    std::unordered_set<DWORD> affectedPids;

    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& entry = info->Handles[i];
        const DWORD pid = static_cast<DWORD>(entry.UniqueProcessId);
        if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
            continue;
        }
        if (fileObjectTypeIndex != 0 && entry.ObjectTypeIndex != fileObjectTypeIndex) {
            continue;
        }

        if (protectedPids.contains(pid)) {
            continue;
        }
        if (!safePids.contains(pid)) {
            std::wstring unsafeReason;
            if (IsUnsafeSystemProcess(pid, unsafeReason)) {
                protectedPids.insert(pid);
                continue;
            }
            safePids.insert(pid);
        }

        HANDLE process = nullptr;
        auto processIt = processHandles.find(pid);
        if (processIt == processHandles.end()) {
            process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
            if (!process && GetLastError() == ERROR_ACCESS_DENIED) {
                inaccessiblePids.insert(pid);
            }
            processHandles.emplace(pid, process);
        } else {
            process = processIt->second;
        }
        if (!process) {
            continue;
        }

        // First duplicate without closing the source and verify that the handle
        // still points at the requested target. Only then issue CLOSE_SOURCE.
        HANDLE verification = nullptr;
        if (!DuplicateHandle(process,
                             reinterpret_cast<HANDLE>(entry.HandleValue),
                             GetCurrentProcess(),
                             &verification,
                             0,
                             FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            continue;
        }

        const bool matched = MatchDuplicatedFileHandle(
            verification,
            normalizedTarget,
            normalizedCanonicalTarget,
            targetIdentity,
            targetIsDirectory);
        CloseHandle(verification);
        if (!matched) {
            continue;
        }

        HANDLE localCopy = nullptr;
        if (DuplicateHandle(process,
                            reinterpret_cast<HANDLE>(entry.HandleValue),
                            GetCurrentProcess(),
                            &localCopy,
                            0,
                            FALSE,
                            DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
            ++result.handlesClosed;
            affectedPids.insert(pid);
            if (localCopy) {
                CloseHandle(localCopy);
            }
        } else {
            ++result.failedHandleCount;
        }
    }

    for (auto& [_, process] : processHandles) {
        if (process) {
            CloseHandle(process);
        }
    }

    result.processesAffected = static_cast<DWORD>(affectedPids.size());
    result.inaccessibleProcessCount = static_cast<DWORD>(inaccessiblePids.size());
    result.skippedProtectedProcessCount = static_cast<DWORD>(protectedPids.size());
    return result;
}

ScanResult ScanLocks(const std::wstring& target) {
    ScanResult result;
    result.target = target;

    DWORD attrs = GetFileAttributesW(target.c_str());
    result.targetExists = attrs != INVALID_FILE_ATTRIBUTES;
    result.targetIsDirectory = result.targetExists && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!result.targetExists) {
        return result;
    }

    std::unordered_map<DWORD, LockProcess> processMap;
    result.restartManagerError = ScanRestartManager(target, processMap);

    std::wstring normalizedTarget = NormalizePath(target);
    std::wstring normalizedCanonicalTarget;
    FileIdentity targetIdentity{};

    HANDLE targetHandle = OpenTargetForMetadata(target, result.targetIsDirectory);
    if (targetHandle != INVALID_HANDLE_VALUE) {
        normalizedCanonicalTarget = GetHandlePath(targetHandle);
        TryGetFileIdentity(targetHandle, targetIdentity);
        CloseHandle(targetHandle);
    }

    if (!normalizedTarget.empty()) {
        ScanProcessImagesAndModules(normalizedTarget,
                                    normalizedCanonicalTarget,
                                    result.targetIsDirectory,
                                    processMap);
        ScanSystemHandles(normalizedTarget,
                          normalizedCanonicalTarget,
                          targetIdentity,
                          result.targetIsDirectory,
                          result.inaccessibleProcessCount,
                          result.inspectedDiskHandleCount,
                          processMap);
    }

    result.processes.reserve(processMap.size());
    for (auto& [_, process] : processMap) {
        result.processes.push_back(std::move(process));
    }
    std::sort(result.processes.begin(), result.processes.end(), [](const LockProcess& a, const LockProcess& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.pid < b.pid;
    });
    return result;
}

bool RequestGracefulUnlock(const std::wstring& target, std::wstring& errorText) {
    errorText.clear();
    bool restartManagerSucceeded = false;

    DWORD session = 0;
    WCHAR sessionKey[CCH_RM_SESSION_KEY + 1]{};
    DWORD rc = RmStartSession(&session, 0, sessionKey);
    if (rc == ERROR_SUCCESS) {
        LPCWSTR resources[] = {target.c_str()};
        rc = RmRegisterResources(session, 1, resources, 0, nullptr, 0, nullptr);
        if (rc == ERROR_SUCCESS) {
            rc = RmShutdown(session, 0, nullptr);
            restartManagerSucceeded = (rc == ERROR_SUCCESS);
        }
        RmEndSession(session);
    }

    // Restart Manager does not cover every directory/handle case. Ask any GUI
    // processes discovered by the handle scanner to close as a safe fallback.
    ScanResult scan = ScanLocks(target);
    std::unordered_set<DWORD> pids;
    for (const auto& process : scan.processes) {
        pids.insert(process.pid);
    }
    if (!pids.empty()) {
        EnumWindows(CloseWindowForPids, reinterpret_cast<LPARAM>(&pids));
        return true;
    }

    if (restartManagerSucceeded || scan.processes.empty()) {
        return true;
    }

    errorText = L"Windows could not request a graceful release of the selected resource.";
    if (rc != ERROR_SUCCESS) {
        errorText += L" Error: " + std::to_wstring(rc);
    }
    return false;
}

bool TerminateProcessByPid(DWORD pid, DWORD& win32Error) {
    win32Error = ERROR_SUCCESS;

    std::wstring unsafeReason;
    if (IsUnsafeSystemProcess(pid, unsafeReason)) {
        win32Error = ERROR_ACCESS_DENIED;
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        win32Error = GetLastError();
        return false;
    }

    bool ok = TerminateProcess(process, 1) != FALSE;
    if (!ok) {
        win32Error = GetLastError();
    } else {
        WaitForSingleObject(process, 2000);
    }
    CloseHandle(process);
    return ok;
}