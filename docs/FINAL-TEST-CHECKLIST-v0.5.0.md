# Genia Unlocker v0.5.0 Final Test Checklist

Use this checklist against **v0.5.0 RC1** before merging `v0.5.0-dev` into `main`.

## 1. Startup and portable behavior

- [ ] Launches normally as a portable EXE.
- [ ] No unexpected DLL/runtime dependency beside the EXE.
- [ ] Closing and reopening restores the saved main-window size.
- [ ] Adjusted process-table column widths survive restart.
- [ ] GeniaUnlocker.ini is only created/updated for portable preferences/layout.
- [ ] Tray icon appears and hover text says **Genia Unlocker**.
- [ ] Tray Open / Select file / About / Exit work.

## 2. Theme and DPI

Test Windows app theme in both Light and Dark.

- [ ] Main window follows theme.
- [ ] Process-table header follows theme.
- [ ] Target and process-list surfaces follow theme.
- [ ] Settings follows theme.
- [ ] About follows theme.
- [ ] Details body follows theme.
- [ ] Confirmation dialogs follow theme.
- [ ] Native title bar visually matches Flat UX.
- [ ] No clipped text or controls at 100%.
- [ ] No clipped text or controls at 125%.
- [ ] No clipped text or controls at 150%.
- [ ] No clipped text or controls at 175%.

## 3. Basic target handling

- [ ] File picker selects a file.
- [ ] Folder picker selects a folder.
- [ ] Drag-and-drop sets the target.
- [ ] Long target path tooltip shows the full path.
- [ ] Missing/deleted target is reported accurately.
- [ ] Context-menu invocation brings the existing instance to foreground.

## 4. Scanner

- [ ] Unlocked ordinary file reports no blocking processes.
- [ ] Test Lab exclusive file handle is detected.
- [ ] Delete-sharing blocker is detected.
- [ ] Multiple PIDs locking one target are detected.
- [ ] Directory handle blocker is detected.
- [ ] Memory-mapped scenario behaves as expected.
- [ ] System-wide inaccessible handle count does not falsely mark a clean target as locked.
- [ ] Scan as Admin appears only when target-level verification warrants it.

## 5. Microsoft Word regression

- [ ] Open a .docx in Word.
- [ ] Confirm Explorer cannot delete the open document.
- [ ] Genia Unlocker identifies Microsoft Word / WINWORD.EXE.
- [ ] Normal Unlock requests graceful release.
- [ ] Rescan verifies that the target is clean when Word releases it.
- [ ] Copy report reflects the post-unlock clean state correctly.

## 6. Unlock actions

- [ ] Unlock confirmation is themed.
- [ ] Normal Unlock performs a rescan before reporting success.
- [ ] Force Unlock confirmation is themed.
- [ ] Force Unlock closes matching handles in Test Lab.
- [ ] Force Unlock never modifies known critical Windows processes.
- [ ] Terminate confirmation is themed.
- [ ] Terminate kills the selected non-critical blocker and rescans.

## 7. Delete actions

- [ ] Clean target shows **Delete**.
- [ ] Locked target shows **Unlock & Delete**.
- [ ] Default delete goes to Recycle Bin.
- [ ] Permanent-delete preference works.
- [ ] Unlock & Delete follows Unlock → rescan → Force Unlock if needed → rescan → optional terminate → rescan → delete.
- [ ] Unrelated inaccessible processes never veto the final Windows delete attempt.
- [ ] Success is never reported while the target still exists.
- [ ] Delete retry handles short release races.
- [ ] Delete-on-reboot is offered only after immediate deletion fails.
- [ ] Delete-on-reboot confirmation is themed.
- [ ] Reparse/junction traversal safety remains intact.

## 8. Details and diagnostics

- [ ] Details opens in the correct theme.
- [ ] TARGET section is correct.
- [ ] SCAN RESULT section is correct.
- [ ] BLOCKERS section is correct.
- [ ] Scan duration is present.
- [ ] Restart Manager state is present.
- [ ] Disk-handle count is present.
- [ ] Handle type filter state is present.
- [ ] Copy report copies valid readable text.

## 9. Explorer / tray integration

- [ ] Enable Explorer context menu.
- [ ] Context menu works for files.
- [ ] Context menu works for folders.
- [ ] Disable Explorer context menu removes it.
- [ ] Enable Start with Windows (tray).
- [ ] Disable Start with Windows removes autostart.
- [ ] Tray tooltip is not an empty bubble.

## 10. Release acceptance

- [ ] Windows CI build succeeds from the exact RC1 commit.
- [ ] RC1 ZIP SHA256 recorded.
- [ ] No functional regression found.
- [ ] No UI blocker found in Light/Dark.
- [ ] No DPI blocker found.
- [ ] SmartScreen behavior understood: unsigned RC builds may show reputation warning.
- [ ] Code-signing plan selected before or immediately after Final publication.

If every blocking item passes, RC1 can be promoted to v0.5.0 Final with only version/release packaging changes.
