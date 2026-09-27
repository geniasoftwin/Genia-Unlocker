#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct LockProcess {
    DWORD pid{};
    std::wstring name;
    std::wstring path;
    bool foundByRestartManager{};
    bool foundByHandleScan{};
    bool foundByProcessImageScan{};
    // Exact files/directories/modules observed inside the selected target.
    // Kept deduplicated and intentionally bounded by the scanner.
    std::vector<std::wstring> lockedObjects;
};

struct ScanResult {
    std::wstring target;
    bool targetExists{};
    bool targetIsDirectory{};
    DWORD restartManagerError{};
    DWORD inaccessibleProcessCount{};
    DWORD inspectedDiskHandleCount{};
    std::vector<LockProcess> processes;
};

struct ForceUnlockResult {
    bool targetExists{};
    DWORD handlesClosed{};
    DWORD processesAffected{};
    DWORD inaccessibleProcessCount{};
    DWORD failedHandleCount{};
    DWORD skippedProtectedProcessCount{};
};

ScanResult ScanLocks(const std::wstring& target);

// Tries to ask applications using the target to close gracefully.
// This does not force-close arbitrary handles.
bool RequestGracefulUnlock(const std::wstring& target, std::wstring& errorText);

// Force-closes matching file handles in other processes. This is deliberately
// more invasive than Restart Manager and should only be called after explicit
// user confirmation. Loaded image/module mappings cannot be closed this way.
ForceUnlockResult ForceUnlockHandles(const std::wstring& target);

// Returns true for processes that Genia Unlocker intentionally refuses to
// terminate or force-unlock because doing so could destabilize Windows.
bool IsUnsafeSystemProcess(DWORD pid, std::wstring& reason);

bool TerminateProcessByPid(DWORD pid, DWORD& win32Error);
std::wstring QueryProcessImagePath(DWORD pid);
std::wstring QueryProcessDisplayName(DWORD pid);
