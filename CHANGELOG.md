# Changelog

## 0.4.1 Final — 2026-09-06

### Fixed
- `Unlock & Delete` no longer treats unrelated inaccessible processes as proof that the selected target is still locked.
- Explorer context-menu invocation now restores and foregrounds the existing Genia Unlocker window.
- Numeric `FILEVERSION` and `PRODUCTVERSION` metadata correctly report `0.4.1.0`.

### Improved
- Delete completion is verified after Windows finishes the file operation, preventing false-success reporting while the target still exists.
- Settings uses a compact custom caption with a DPI-aware inset Close button.
- Post-unlock rescans remain authoritative before escalation to Force Unlock or process termination.

### Release status
- Promoted from v0.4.1 RC2 after successful stress-path validation, including administrator elevation and locked-folder deletion.
- No application-code changes were made between the tested RC2 candidate and v0.4.1 Final.
