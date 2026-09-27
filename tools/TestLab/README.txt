Genia Unlocker PowerShell Test Lab
====================================

Purpose
-------
Creates controlled Windows locking situations for testing Genia Unlocker.

Scenarios
---------
1. Exclusive file handle
2. File permits read/write but denies delete/rename sharing
3. Multiple PowerShell processes hold the same target
4. Open directory handle blocks folder delete/rename
5. Memory-mapped file
6. Stubborn worker attempts to reopen its handle after external Force Unlock
7. Elevated/UAC lock-holder for Retry Admin testing
8. Combined stress pack

Recommended Genia Unlocker checks
---------------------------------
- Scan detects the expected PowerShell PID(s)
- Retry Admin resolves elevated-process visibility
- Unlock releases a normal user-mode handle
- Force Unlock handles tougher cases
- Re-scan removes stale PID entries
- Terminate kills the selected lock-holder
- Unlock & Delete deletes the target after releasing handles
- Folder deletion works with nested locked files
- Recycle Bin vs Permanent Delete behavior is correct
- Delete on reboot works when normal deletion cannot complete

Safety
------
The script creates objects only under:
%TEMP%\GeniaUnlocker-TestLab\<session>

Normal workers are cleaned on normal script exit.
An elevated worker can survive if the non-elevated controller is unable to terminate it.
In that case, terminate it with Genia Unlocker / Task Manager as Administrator.

Run
---
Right-click GeniaUnlocker-TestLab.ps1 -> Run with PowerShell

or:

powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\GeniaUnlocker-TestLab.ps1