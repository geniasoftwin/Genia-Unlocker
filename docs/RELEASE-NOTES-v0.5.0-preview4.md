# Genia Unlocker v0.5.0 Preview 4 — Flat UX

Preview 4 is a visual refresh of the native Win32 interface. The scanner, unlock workflow and safety model remain unchanged.

## Flat UX

- Dark and light palettes were simplified into a flatter two-plane visual system.
- Mica/translucent system backdrop was disabled in favor of solid application surfaces.
- Windows 11 corner treatment uses the smaller rounded-corner preference.
- Secondary buttons now use quieter flat fills and one-pixel outlines.
- Primary actions keep the Windows accent color.
- Destructive actions use a red outline and red text rather than a permanently filled red button.
- Button corner radii were reduced substantially.
- Main-window spacing, button heights and toolbar widths were tightened.
- The Settings button no longer uses a decorative gear glyph.
- Target, process list and Details text area no longer use the classic recessed client-edge appearance.
- Confirmation dialogs use a flat drawn warning mark instead of the stock 3-D Windows warning icon.
- Tool-window typography was rebalanced for the flatter hierarchy.

## Preserved behavior

Preview 4 keeps the Preview 3 functionality:
- themed confirmation dialogs;
- scan duration reporting;
- structured Details view and Copy report;
- Scanning… activity state;
- persisted main-window size and table-column widths;
- per-monitor DPI handling;
- Word / Restart Manager lock detection fixes;
- target-level delete-share verification;
- adaptive small-size application icon.

## Version metadata

- Company: GeniaSoftWin
- File version: 0.5.0.4
- Product version: 0.5.0 Preview 4
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

Please compare Preview 4 with Preview 3 at:
1. Main window in dark mode.
2. Main window in light mode.
3. Unlock / Force Unlock / Terminate confirmation dialogs.
4. Delete / Unlock & Delete destructive actions.
5. Settings, About and Details.
6. Windows scaling at 100%, 125%, 150% and 175%.
7. Process table selection and header readability.
8. Tray/context-menu icon and normal lock/unlock workflows.
