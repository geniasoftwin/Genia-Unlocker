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
        p.window = RGB(28, 28, 30);
        p.surface = RGB(40, 40, 43);
        p.surfacePressed = RGB(54, 54, 58);
        p.border = RGB(67, 67, 72);
        p.text = RGB(245, 245, 247);
        p.muted = RGB(174, 174, 178);
        p.accent = Blend(systemAccent, RGB(255, 255, 255), 12);
        p.accentPressed = Blend(p.accent, RGB(0, 0, 0), 18);
        p.danger = RGB(210, 70, 70);
        p.dangerPressed = RGB(176, 54, 54);
    } else {
        p.window = RGB(246, 247, 249);
        p.surface = RGB(255, 255, 255);
        p.surfacePressed = RGB(235, 237, 241);
        p.border = RGB(215, 218, 224);
        p.text = RGB(28, 28, 30);
        p.muted = RGB(100, 102, 107);
        p.accent = systemAccent;
        p.accentPressed = Blend(systemAccent, RGB(0, 0, 0), 18);
        p.danger = RGB(196, 43, 28);
        p.dangerPressed = RGB(160, 34, 24);
    }
    return p;
}

void ApplyWindowChrome(HWND hwnd, bool dark) {
    if (!hwnd) return;

    BOOL darkValue = dark ? TRUE : FALSE;
    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE on supported Windows 10/11 builds.
    constexpr DWORD kUseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &darkValue, sizeof(darkValue));

    // Rounded corners on Windows 11. Calls safely fail on older builds.
    constexpr DWORD kWindowCornerPreference = 33;
    constexpr int kRound = 2; // DWMWCP_ROUND
    int corner = kRound;
    DwmSetWindowAttribute(hwnd, kWindowCornerPreference, &corner, sizeof(corner));

    // Mica-style main-window backdrop on Windows 11. Safe no-op on unsupported builds.
    constexpr DWORD kSystemBackdropType = 38;
    constexpr int kMainWindowBackdrop = 2; // DWMSBT_MAINWINDOW
    int backdrop = kMainWindowBackdrop;
    DwmSetWindowAttribute(hwnd, kSystemBackdropType, &backdrop, sizeof(backdrop));
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

    COLORREF fill = palette.surface;
    COLORREF border = palette.border;
    COLORREF text = palette.text;

    switch (kind) {
    case ButtonKind::Primary:
        fill = pressed ? palette.accentPressed : palette.accent;
        border = fill;
        text = RGB(255, 255, 255);
        break;
    case ButtonKind::Danger:
        fill = pressed ? palette.dangerPressed : palette.danger;
        border = fill;
        text = RGB(255, 255, 255);
        break;
    case ButtonKind::Secondary:
    default:
        fill = pressed ? palette.surfacePressed : palette.surface;
        break;
    }

    if (disabled) {
        fill = palette.surface;
        border = palette.border;
        text = palette.muted;
    }

    HBRUSH background = CreateSolidBrush(palette.window);
    FillRect(draw.hDC, &rc, background);
    DeleteObject(background);

    // RECT coordinates use LONG while the first argument used to be an int.
    // std::max requires both arguments to have the same type, which MSVC 2026
    // correctly diagnoses as an ambiguous/mismatched template call.
    const int buttonHeight = static_cast<int>(rc.bottom - rc.top);
    const int radius = (std::max)(8, buttonHeight / 3);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? palette.accent : border);
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

    HFONT buttonFont = reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (buttonFont) {
        oldFont = SelectObject(draw.hDC, buttonFont);
    }

    RECT textRc = rc;
    if (pressed) {
        OffsetRect(&textRc, 0, 1);
    }
    DrawTextW(draw.hDC, textBuffer, -1, &textRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) {
        SelectObject(draw.hDC, oldFont);
    }
}

} // namespace ModernTheme
