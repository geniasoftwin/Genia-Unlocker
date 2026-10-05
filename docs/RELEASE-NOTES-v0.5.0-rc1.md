# Genia Unlocker v0.5.0 RC1

Release Candidate 1 freezes the v0.5.0 feature and UI set for final Windows regression testing.

## Scanner and verification

- Fixed File ObjectTypeIndex detection by ensuring the probe handle exists before the system handle snapshot.
- Restart Manager enumeration retries when the process list changes between calls.
- Added target-level delete-share verification.
- System-wide inaccessible handle owners are diagnostics only and no longer imply that the selected target is locked.
- Confirmed detection of Microsoft Word in a real .docx deletion-blocking scenario.
- Unlock success is verified by a clean rescan of the target.

## Flat UX

- Unified dark/light Flat UX palette.
- Custom flat process-table header.
- Themed Settings, About, Details and confirmation windows.
- Dark Details body follows the application palette.
- Native caption color aligns with the application background.
- Compact secondary controls and outlined destructive actions.
- Adaptive small-size app icon for Explorer, tray and context-menu use.

## Diagnostics and usability

- Scan duration is shown in status and diagnostic reports.
- Details groups Target, Scan Result, Diagnostics and Blockers.
- Copy report lives in Details.
- Target field exposes the full path via tooltip.
- Main-window size and table-column widths persist in GeniaUnlocker.ini.
- Windows 11 tray tooltip explicitly uses NIF_SHOWTIP.
- Per-monitor DPI handling hardened for tool windows.

## Safety model

Unchanged:
- native Win32 x64;
- portable single EXE;
- no driver or Windows service;
- no .NET, Qt or WebView2;
- critical Windows process protection remains enabled;
- destructive actions remain confirmed;
- delete success is verified against the filesystem;
- delete-on-reboot does not recursively traverse reparse/junction targets.

## RC1 metadata

- File version: 0.5.0.6
- Product version: 0.5.0 RC1
- Company: GeniaSoftWin
- Copyright: © 2026 GeniaSoftWin

## Release gate

RC1 should become v0.5.0 Final only after the final checklist passes on Windows. Code signing / SmartScreen trust is tracked separately from functional acceptance.
