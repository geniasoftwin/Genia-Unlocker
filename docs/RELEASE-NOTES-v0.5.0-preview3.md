# Genia Unlocker v0.5.0 Preview 3

Development preview focused on final UI polish before v0.5.0 Final.

## UI polish

- Added native themed confirmation dialogs for:
  - Unlock
  - Force Unlock
  - Terminate
  - Delete / Unlock & Delete
  - administrator rescans
  - delete-on-reboot
- Confirmation windows follow the same dark/light palette as the rest of Genia Unlocker.
- The Rescan button now shows **Scanning…** while a scan is active.
- Scan duration is shown in the main status line where appropriate.
- Details output is organized into:
  - Target
  - Scan Result
  - Diagnostics
  - Blockers
- Copy report remains inside Details to keep the main window compact.

## Portable layout preferences

GeniaUnlocker.ini now also stores:
- main-window width and height;
- process-table column widths.

Values are stored as logical 96-DPI sizes so they can be restored across display scaling changes.

## DPI

Per-monitor DPI handling was hardened for:
- Settings
- About
- Details
- themed confirmation windows

The main window remains Per-Monitor-Aware V2.

## Diagnostics

Diagnostic reports now include scan duration in milliseconds.

## Version metadata

- Company: GeniaSoftWin
- File version: 0.5.0.3
- Product version: 0.5.0 Preview 3
- Copyright: © 2026 GeniaSoftWin

## Safety model

Unchanged:
- Native Win32 x64.
- Portable single EXE.
- No kernel driver or Windows service.
- No .NET, Qt or WebView2 dependency.
- Critical Windows process protection remains enabled.
- Delete success is verified against the filesystem before reporting success.

## Test focus

Please retest:
1. Unlock confirmation in dark and light Windows themes.
2. Force Unlock and Terminate confirmations.
3. Delete and Unlock & Delete confirmation flows.
4. Delete-on-reboot fallback.
5. Scan as Admin confirmation.
6. Main-window size persistence after restart.
7. Process-table column width persistence after restart.
8. 100%, 125%, 150% and 175% display scaling.
9. Details formatting and Copy report.
10. Word locking scenario from Preview 2.
