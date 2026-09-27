# Genia Unlocker v0.5.0 Preview 1

Development preview based on the stable v0.4.1 Final codebase.

## New

- New Genia Unlocker application icon optimized for Windows Explorer, taskbar, tray and context-menu use.
- Native **About Genia Unlocker** dialog with version, author, license and official GitHub links.
- **Copy report** action for a complete clipboard diagnostic report including target, elevation state, blockers, PIDs, executable paths, detection methods and locked object paths.
- Short **Delete Retry** grace window before offering delete-on-reboot, improving reliability when a process releases its final handle just after a rescan.
- Updated executable metadata:
  - Company: GeniaSoftWin
  - File version: 0.5.0.1
  - Product version: 0.5.0 Preview 1
  - Copyright: © 2026 GeniaSoftWin

## Unchanged safety model

- Native Win32 x64.
- Portable single EXE.
- No kernel driver or Windows service.
- No .NET, Qt or WebView2 dependency.
- Critical Windows process protection remains enabled.
- Delete success is still verified against the filesystem before reporting success.

## Test focus

Please retest:
1. Normal Unlock.
2. Force Unlock.
3. Unlock & Delete.
4. Administrator elevation.
5. Explorer context menu and the new icon at small sizes.
6. Copy report.
7. About dialog and GitHub/issue links.
