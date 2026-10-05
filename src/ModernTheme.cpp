#include "ModernTheme.h"

#include <algorithm>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Uxtheme.lib")

namespace ModernTheme {
namespace {

COLORREF Blend(COLORREF a, COLORREF b, int percentB) {
    const int percentA = 100 - percentB;
    const auto r = (GetRValue(a) * percentA + GetRValue(b) * percentB) / 100;
    const auto g = (GetGValue(a) * percentA + GetGValue(b) * percentB) / 100;
    const auto bl = (GetBValue(a) * percentA + GetBValue(b) * percentB) / 100;
    return RGB(r, g, bl);
}

bool IsAppsDarkMode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &value,
                     &size) == ERROR_SUCCESS) {
        return value == 0;
    }
    return false;
}

} // namespace

Palette QueryPalette() {
    Palette p{};
    p.dark = IsAppsDarkMode();

    const COLORREF systemAccent = GetSysColor(COLOR_HIGHLIGHT);
    if (p.dark) {
        // Flat UX: one quiet window plane, one slightly raised content plane
        // and low-contrast separators instead of heavy card borders.
        p.window = RGB(24, 24, 27);
        p.surface = RGB(31, 32, 36);
        p.surfacePressed = RGB(43, 44, 49);
        p.border = RGB(53, 54, 60);
        p.text = RGB(241, 241, 243);
        p.muted = RGB(170, 171, 177);
        p.accent = Blend(systemAccent, RGB(255, 255, 255), 8);
        p.accentPressed = Blend(p.accent, RGB(0, 0, 0), 16);
        p.danger = RGB(220, 74, 78);
        p.dangerPressed = RGB(184, 57, 61);
    } else {
        p.window = RGB(246, 247, 249);
        p.surface = RGB(255, 255, 255);
        p.surfacePressed = RGB(236, 239, 243);
        p.border = RGB(216, 219, 225);
        p.text = RGB(27, 28, 31);
        p.muted = RGB(92, 95, 102);
        p.accent = systemAccent;
        p.accentPressed = Blend(systemAccent, RGB(0, 0, 0), 16);
        p.danger = RGB(196, 52, 56);
        p.dangerPressed = RGB(161, 39, 43);
    }
    return p;
}

void ApplyWindowChrome(HWND hwnd, bool dark) {
    if (!hwnd) return;

    BOOL darkValue = dark ? TRUE : FALSE;
    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE on supported Windows 10/11 builds.
    constexpr DWORD kUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &darkValue, sizeof(darkValue));

    // Flat UX keeps only a small Windows 11 corner radius and uses a solid
    // background instead of Mica/translucent backdrops.
    constexpr DWORD kWindowCornerPreference = 33;
    constexpr int kRoundSmall = 3; // DWMWCP_ROUNDSMALL
    int corner = kRoundSmall;
    DwmSetWindowAttribute(hwnd, kWindowCornerPreference, &corner, sizeof(corner));

    constexpr DWORD kSystemBackdropType = 38;
    constexpr int kNoSystemBackdrop = 1; // DWMSBT_NONE
    int backdrop = kNoSystemBackdrop;
    DwmSetWindowAttribute(hwnd, kSystemBackdropType, &backdrop, sizeof(backdrop));

    // Keep the native main-window caption on the same visual plane as the
    // client area. This removes the separate grey strip visible in Flat UX.
    const Palette palette = QueryPalette();
    constexpr DWORD kBorderColor = 34;  // DWMWA_BORDER_COLOR
    constexpr DWORD kCaptionColor = 35; // DWMWA_CAPTION_COLOR
    constexpr DWORD kTextColor = 36;    // DWMWA_TEXT_COLOR
    COLORREF borderColor = palette.border;
    COLORREF captionColor = palette.window;
    COLORREF textColor = palette.text;
    DwmSetWindowAttribute(hwnd, kBorderColor, &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(hwnd, kCaptionColor, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, kTextColor, &textColor, sizeof(textColor));
}

void ApplyControlTheme(HWND hwnd, bool dark) {
    if (!hwnd) return;
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

void DrawButton(const DRAWITEMSTRUCT& draw, const Palette& palette, ButtonKind kind) {
    if (draw.CtlType != ODT_BUTTON || !draw.hDC) return;

    RECT rc = draw.rcItem;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool hot = (draw.itemState & ODS_HOTLIGHT) != 0;

    COLORREF fill = palette.window;
    COLORREF border = palette.border;
    COLORREF text = palette.text;

    switch (kind) {
    case ButtonKind::Primary:
        fill = pressed ? palette.accentPressed : palette.accent;
        border = fill;
        text = RGB(255, 255, 255);
        break;

    case ButtonKind::Danger:
        // Destructive actions stay visually quiet until invoked; the red
        // outline communicates meaning without turning the whole toolbar red.
        fill = palette.window;
        border = palette.danger;
        text = palette.danger;
        if (hot) {
            fill = Blend(palette.danger, palette.window, 84);
        }
        if (pressed) {
            fill = palette.dangerPressed;
            border = fill;
            text = RGB(255, 255, 255);
        }
        break;

    case ButtonKind::Secondary:
    default:
        // Secondary controls should still be visible in the light theme.
        // A quiet surface fill gives them shape without returning to raised UX.
        fill = pressed ? palette.surfacePressed
                       : (hot ? palette.surfacePressed : palette.surface);
        break;
    }

    if (disabled) {
        fill = palette.window;
        border = Blend(palette.border, palette.window, 30);
        text = Blend(palette.muted, palette.text, 18);
    }

    HBRUSH background = CreateSolidBrush(palette.window);
    FillRect(draw.hDC, &rc, background);
    DeleteObject(background);

    const int buttonHeight = static_cast<int>(rc.bottom - rc.top);
    const int radius = (std::max)(4, buttonHeight / 6);

    HBRUSH brush = CreateSolidBrush(fill);
    const COLORREF outline = focused && !disabled ? palette.accent : border;
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ oldBrush = SelectObject(draw.hDC, brush);
    HGDIOBJ oldPen = SelectObject(draw.hDC, pen);

    RoundRect(draw.hDC, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1,
              radius, radius);

    SelectObject(draw.hDC, oldPen);
    SelectObject(draw.hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t textBuffer[256]{};
    GetWindowTextW(draw.hwndItem, textBuffer, static_cast<int>(_countof(textBuffer)));
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, text);

    HFONT buttonFont = reinterpret_cast<HFONT>(
        SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (buttonFont) {
        oldFont = SelectObject(draw.hDC, buttonFont);
    }

    RECT textRc = rc;
    if (pressed) {
        OffsetRect(&textRc, 0, 1);
    }
    InflateRect(&textRc, -6, 0);
    DrawTextW(draw.hDC, textBuffer, -1, &textRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (oldFont) {
        SelectObject(draw.hDC, oldFont);
    }
}

} // namespace ModernTheme
