#pragma once
#include <windows.h>
namespace books {
// Dialog-level shortcut routing before child controls/dialog navigation consume it.
inline constexpr UINT DialogShortcut=WM_APP+95;
void applyTheme(HWND window);
bool usesDarkTheme(HWND window);
// Shared visual state for app-owned controls, including owner-drawn actions.
bool controlHovered(HWND window);
COLORREF interactionColor(HWND window, COLORREF base, bool hot, bool pressed = false);
bool focusCuesVisible(HWND window);
void updateFocusCues(HWND window, bool keyboard);
void drawKeyboardFocus(HWND window, HDC dc, RECT bounds, bool selected = false);
inline void drawFullscreenIcon(HDC dc, RECT bounds, UINT dpi, COLORREF color) {
    auto pen=CreatePen(PS_SOLID,MulDiv(2,dpi,96),color);
    auto previous=SelectObject(dc,pen);
    const int cx=(bounds.left+bounds.right)/2,cy=(bounds.top+bounds.bottom)/2,d=MulDiv(6,dpi,96);
    for(int x=-1;x<=1;x+=2)for(int y=-1;y<=1;y+=2) {
        MoveToEx(dc,cx+x*d,cy+y*(d/2),nullptr);
        LineTo(dc,cx+x*d,cy+y*d);LineTo(dc,cx+x*(d/2),cy+y*d);
    }
    SelectObject(dc,previous);DeleteObject(pen);
}
HBRUSH dialogBackground(HWND window);
HBRUSH panelBackground(HWND window);
int themedMessageBox(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags);
}
