#pragma once
#include <windows.h>
#include <string>
namespace books { void drawBookCover(HDC dc, RECT bounds, HBITMAP bitmap, const std::wstring& extension); void drawPlaceholderCover(HDC dc, RECT bounds, const std::wstring& extension); }
