#pragma once

#include <windows.h>
#include <string>
#include <vector>

std::wstring GetExecutablePath();
std::wstring QuoteCommandArgument(const std::wstring& value);

bool IsShellIntegrationEnabled();
bool SetShellIntegrationEnabled(bool enabled, std::wstring& errorText);

bool IsAutostartEnabled();
bool SetAutostartEnabled(bool enabled, std::wstring& errorText);

bool IsRunningElevated();
bool RunElevatedKillHelper(DWORD pid, DWORD& helperExitCode, std::wstring& errorText);
bool RunElevatedForceUnlockHelper(const std::wstring& target,
                                  DWORD& helperExitCode,
                                  std::wstring& errorText);
bool RunElevatedKillListHelper(const std::vector<DWORD>& pids,
                               DWORD& helperExitCode,
                               std::wstring& errorText);

// Starts a new elevated Genia Unlocker instance for a target path. The caller
// can then close its non-elevated UI; no helper DLL/service is installed.
bool RunElevatedScanInstance(const std::wstring& target, std::wstring& errorText);
