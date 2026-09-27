#pragma once

#include <windows.h>

namespace ModernTheme {

struct Palette {
    bool dark{};
    COLORREF window{};
    COLORREF surface{};
    COLORREF surfacePressed{};
    COLORREF border{};
    COLORREF text{};
    COLORREF muted{};
    COLORREF accent{};
    COLORREF accentPressed{};
    COLORREF danger{};
    COLORREF dangerPressed{};
};

enum class ButtonKind {
    Secondary,
    Primary,
    Danger
};

Palette QueryPalette();
void ApplyWindowChrome(HWND hwnd, bool dark);
void ApplyControlTheme(HWND hwnd, bool dark);
void DrawButton(const DRAWITEMSTRUCT& draw, const Palette& palette, ButtonKind kind);

} // namespace ModernTheme
