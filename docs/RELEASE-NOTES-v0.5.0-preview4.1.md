# Genia Unlocker v0.5.0 Preview 4.1 — Flat UX polish

Preview 4.1 refines the Flat UX introduced in Preview 4. Scanner behavior and safety rules are unchanged.

## Visual fixes

- Process-table header now uses an explicit custom flat renderer so dark mode no longer falls back to a white Windows header.
- Details read-only text surface follows the dark/light application palette.
- Native main-window caption color is aligned with the application background.
- Light theme has stronger separation between the window plane and white content surfaces.
- Secondary buttons use a quiet surface fill instead of disappearing into the background.
- Disabled controls have slightly stronger contrast while remaining clearly inactive.
- About footer spacing and divider placement were adjusted.

## Tray

- Added NIF_SHOWTIP for NOTIFYICON_VERSION_4.
- Reasserts the tray tooltip after NIM_SETVERSION so Windows 11 no longer shows an empty hover bubble.

## Version metadata

- Company: GeniaSoftWin
- File version: 0.5.0.5
- Product version: 0.5.0 Preview 4.1
- Copyright: © 2026 GeniaSoftWin

## Test focus

Please compare dark and light themes for:
1. Process-table header.
2. Details body/background.
3. Native main-window caption.
4. Secondary and disabled buttons.
5. About footer spacing.
6. Tray hover tooltip text.
