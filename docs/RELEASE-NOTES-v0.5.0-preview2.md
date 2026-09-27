# Genia Unlocker v0.5.0 Preview 2

Development preview based on the stable v0.4.1 Final codebase and Preview 1.

## Scanner fixes

- Fixed File ObjectTypeIndex detection in the native system-handle scanner. The probe handle now exists before the system handle snapshot is captured.
- Improved Restart Manager enumeration by retrying if the process list changes between sizing and data calls.
- Added a non-destructive delete-share probe for the selected target.
- A target is no longer treated as uncertain merely because unrelated protected/elevated file-handle owners elsewhere in the system cannot be inspected.
- Microsoft Word documents that Windows refuses to delete while open are now correctly detected through Restart Manager in the tested scenario.
- Diagnostic reports now distinguish target lock state from system-wide inaccessible file-handle owners.

## UI polish

- New themed **About Genia Unlocker** window that follows the application's dark/light palette.
- New themed **Details** window with **Copy report** inside it.
- Removed the permanent Copy report button from the main status row.
- Custom themed ListView header for a consistent dark-mode process table.
- Full target path is available as a tooltip over the Target field.
- Settings shows the current preview version.
- The main destructive action switches between **Delete** and **Unlock & Delete** according to the verified target state.
- Clean scans use a clearer success status indicator.
- Adaptive ICO layers:
  - 16/20/24/32 px use a simplified high-contrast Genia unlock symbol;
  - 40/48/64/128/256 px use the full G + gold key artwork.

## Version metadata

- Company: GeniaSoftWin
- File version: 0.5.0.2
- Product version: 0.5.0 Preview 2
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
1. Open a .docx in Microsoft Word and confirm the process is detected.
2. Normal Unlock followed by a clean verified rescan.
3. Delete versus Unlock & Delete button switching.
4. Force Unlock and Terminate on Test Lab scenarios.
5. Scan as Admin only when the selected target is confirmed sharing-locked but the owner is not identified.
6. Dark/light About and Details windows.
7. Copy report from Details.
8. ListView header in dark mode.
9. Target tooltip for long paths.
10. Tray and Explorer/context-menu icon at 16–32 px.
