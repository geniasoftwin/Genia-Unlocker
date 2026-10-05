# Genia Unlocker v0.5.0 Final

**Release date:** 2026-10-05  
**Platform:** Windows 11 / x64  
**Build:** Native Win32 C++, portable, static MSVC runtime (`/MT`)  
**Executable metadata:** FileVersion 0.5.0.7 / ProductVersion 0.5.0 Final

## Release decision

v0.5.0 Final is promoted from the Windows-tested RC1 codebase. Functional scanner, unlock and delete logic is unchanged after RC1 acceptance; Final only changes release/version metadata and packaging.

The RC pass included real Microsoft Word/Excel/Access lock scenarios, normal Unlock, Force Unlock, Terminate, Delete / Unlock & Delete, tray tooltip behavior and Flat UX checks.

## Highlights

- New Flat UX for dark and light Windows themes.
- Themed Settings, About, Details and confirmation windows.
- Correct Microsoft Word / Office lock detection through Restart Manager/native scanning.
- Fixed File ObjectTypeIndex detection in the native handle scanner.
- Target-level delete-share verification.
- System-wide inaccessible handles no longer falsely mark the selected target as locked.
- Scan duration and clearer diagnostics.
- Context-sensitive Delete / Unlock & Delete.
- Adaptive Explorer/tray/context-menu icon.
- Portable window-size and table-column persistence.
- Windows 11 tray tooltip fix.
- Per-monitor DPI hardening.

## Force Unlock behavior

Force Unlock closes only matching file handles when possible. The owning application remains running; after a clean rescan the released file can be renamed or deleted without terminating the whole process.

## Portable package

The release ZIP contains:
- `GeniaUnlocker.exe`
- `README.md`
- `CHANGELOG.md`
- `RELEASE-NOTES.md`
- `LICENSE`

No installer, service, driver, .NET runtime, Qt, WebView2 or helper DLL is required.

## SmartScreen / signing

The current build is unsigned. Windows SmartScreen can therefore show an unknown-reputation warning on newly downloaded copies. This is separate from the functional release acceptance and can be addressed later with trusted Authenticode code signing.

## Scope

Genia Unlocker remains a user-mode utility. Protected-process, kernel/minifilter, antivirus or unusual filesystem locks can still require operating-system-level handling.
