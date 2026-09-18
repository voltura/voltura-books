#pragma once
#include <windows.h>
namespace books {
void applyTheme(HWND window);
bool usesDarkTheme(HWND window);
HBRUSH dialogBackground(HWND window);
HBRUSH panelBackground(HWND window);
int themedMessageBox(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags);
}
