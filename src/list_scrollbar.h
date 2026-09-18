#pragma once
#include <windows.h>
namespace books { void attachListScrollbar(HWND list,HWND bar); void updateListScrollbar(HWND list,HWND bar); }
namespace books { void attachContentScrollbar(HWND owner,HWND bar); inline constexpr UINT ContentScroll=WM_APP+93; }
