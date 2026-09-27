# Genia Unlocker v0.4.1 Final

**Release date:** 2026-09-06  
**Platform:** Windows 11 / x64  
**Build:** Native Win32, portable, static MSVC runtime (`/MT`)  
**Executable metadata:** 0.4.1.0

## Release decision

v0.4.1 Final is promoted directly from the tested RC2 codebase. No application-code changes were introduced after the successful release-candidate stress test.

The validated stress path included multiple lock scenarios, elevation through UAC/administrator restart, and deletion of a locked folder.

## Key fixes

- Reliable `Unlock & Delete` final stage.
- Real post-delete verification.
- Correct foreground activation when invoked from Explorer.
- Correct 0.4.1.0 file/product version metadata.
- Compact Settings close button with a DPI-aware inset.

## Portable package

After building, run:

```text
package-release.cmd
```

It creates the end-user portable ZIP under:

```text
release\Genia-Unlocker-v0.4.1-Windows-x64-Portable.zip
```

The distributable contains only the executable and release documentation. `GeniaUnlocker.ini` is created beside the EXE only when a portable preference is changed.

## Scope

Genia Unlocker remains a user-mode utility and does not install a kernel driver, service, .NET runtime, Qt or WebView2. Protected-process, kernel/minifilter, antivirus or unusual filesystem locks can still require operating-system-level handling.
